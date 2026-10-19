/* ------------------------------------------------------------------
 * jer_modstore.c — the mod store (see jer_modstore.h).
 *
 * The whole-game updater's little brother. jer_update.c moves the GAME between
 * releases; this moves ONE MOD between versions and never touches the exe. The
 * work is split the same way the updater splits it: this file decides, the
 * helper script beside the exe does the filesystem work, and the only thing
 * crossing back is a JSON result file and an exit code.
 *
 *   modstore.sh / modstore.bat   fetch, verify SHA256, unpack, copy into
 *                                MODS/<id>/, write the provenance record.
 *   this file                    repos, the index, the menus, the job state.
 *
 * NOTHING here writes into MODS/. That is the point: every operation that can
 * destroy a player's work is one reviewed script applying update_jericho's own
 * rules (verify first, overwrite only, never delete what it did not install),
 * so this side cannot get it wrong behind the script's back.
 *
 * No PSX guard: like jer_update.c beside it, this is desktop-only code, and the
 * PSX build does not carry either file.
 * ------------------------------------------------------------------ */
#include "jer_modstore.h"
#include "jericho.h"
#include "jer_config.h"
#include "jer_frontend.h"
#include "jer_notify.h"
#include "jer_prompt.h"
#include "jer_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

/* The build's own release number (premake defines it for the runtime too).
 * Compared against a repo entry's min_jericho: X.Y.Z is what both mean, and a
 * "-3-gabc1234" suffix is ignored by the comparison. */
#ifndef JERICHO_BUILD_VERSION
#define JERICHO_BUILD_VERSION "0.0.0"
#endif
#define STORE_CORE_VERSION JERICHO_BUILD_VERSION

#if defined(_WIN32)
#define store_stricmp	_stricmp
#else
#define store_stricmp	strcasecmp
#endif

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define STORE_MAX_REPOS		8
#define STORE_MAX_MODS		48
#define STORE_ID_MAX		40	/* matches mod.toml / jerValidId */
#define STORE_VER_MAX		20
#define STORE_NAME_MAX		64
#define STORE_DESC_MAX		200
#define STORE_URL_MAX		400
#define STORE_SHA_MAX		72
#define STORE_PATH_MAX		700
#define STORE_MSG_MAX		220

/* FEmain.c's side of the menu: the dev-only compile gate, the compile prompt, the
 * Credits page and the screen entry all belong to the frontend, not to the store.
 * Declared up here because the test lever (below) uses them too. */
#ifdef __cplusplus
extern "C" {
#endif
int  jer_mods_compile_available(void);
void jer_mods_request_compile(void);
void jer_mods_open_credits(void);
void jer_mods_open_screen(void);
#ifdef __cplusplus
}
#endif

/* The official repo. A plain mods.json on a static host: no API, no rate limit,
 * no token. Preconfigured because a store nobody can point anywhere is not a
 * store; repo2..repo8 in CONFIG/modstore.ini add more. */
#define STORE_OFFICIAL_NAME	"official"
#define STORE_OFFICIAL_URL	"https://raw.githubusercontent.com/Njurse/JERICHO-mods/main/mods.json"

/* Job kinds */
enum
{
	STORE_JOB_NONE = 0,
	STORE_JOB_INDEX,
	STORE_JOB_INSTALL,
	STORE_JOB_REMOVE,
	STORE_JOB_REBAKE
};

/* One mod as a repo advertises it. */
typedef struct STORE_MOD
{
	char id[STORE_ID_MAX];
	char name[STORE_NAME_MAX];
	char version[STORE_VER_MAX];
	char author[STORE_ID_MAX];
	char description[STORE_DESC_MAX];
	char file[STORE_URL_MAX];
	char url[STORE_URL_MAX];	/* resolved against the repo's index URL */
	char sha[STORE_SHA_MAX];
	char requires[STORE_ID_MAX * 2];
	char minCore[STORE_VER_MAX];
	int  size;
	int  defaultEnabled;
	int  bakeVersion;
} STORE_MOD;

/* What the store installed for one id (CONFIG/modstore/installed/<id>.json). */
typedef struct STORE_RECORD
{
	char id[STORE_ID_MAX];
	char name[STORE_NAME_MAX];
	char version[STORE_VER_MAX];
	char runtime[16];
	char url[STORE_URL_MAX];
	char tomlSha[STORE_SHA_MAX];
	char dllSha[STORE_SHA_MAX];
	int  present;
} STORE_RECORD;

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct
{
	int  ready;
	char root[STORE_PATH_MAX];		/* <install>/JERICHO */
	char exeDir[STORE_PATH_MAX];	/* where the game (and the script) live */
	char stateDir[STORE_PATH_MAX];	/* <root>/CONFIG/modstore */
	char modsDir[STORE_PATH_MAX];	/* <root>/MODS */
	char script[STORE_PATH_MAX];
	int  scriptFound;

	/* repos */
	int  repoCount;
	char repoName[STORE_MAX_REPOS][STORE_ID_MAX];
	char repoUrl[STORE_MAX_REPOS][STORE_URL_MAX];
	int  repoSelected;

	/* the selected repo's index */
	STORE_MOD mods[STORE_MAX_MODS];
	int  modCount;
	int  indexState;				/* 0 never fetched, 1 loaded, 2 failed */
	char indexNote[STORE_MSG_MAX];

	/* computed rows for the browser */
	JER_MODSTORE_ITEM items[STORE_MAX_MODS];
	char itemNote[STORE_MAX_MODS][96];
	int  itemCount;
	int  itemsDirty;

	/* records */
	STORE_RECORD records[JER_MAX_MODULES];
	int  recordCount;

	/* the running job */
	int  jobKind;
	int  jobRepo;
	char jobId[STORE_ID_MAX];
	char status[STORE_MSG_MAX];
	int  statusIsError;

	/* the helper's own protocol version, from the last result it wrote */
	int  scriptVersion;

	/* JERICHO_TEST_MODSTORE: drives one flow headlessly (see StoreTestTick) */
	int  testLever;
	char testId[STORE_ID_MAX];
	int  testStage;
	int  drawLogged;
} gStore;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void StoreCopy(char* dst, int max, const char* src)
{
	if (max <= 0)
		return;
	if (src == NULL)
		src = "";
	snprintf(dst, (size_t)max, "%s", src);
}

static int StoreEndsWith(const char* s, const char* tail)
{
	size_t ls, lt;

	if (s == NULL || tail == NULL)
		return 0;
	ls = strlen(s);
	lt = strlen(tail);
	return (ls >= lt && store_stricmp(s + ls - lt, tail) == 0);
}

static int StoreValidId(const char* id)
{
	const char* p;

	if (id == NULL || id[0] == 0 || strlen(id) >= STORE_ID_MAX)
		return 0;
	if (!(isalpha((unsigned char)id[0]) || id[0] == '_'))
		return 0;
	for (p = id; *p != 0; p++)
	{
		if (!(isalnum((unsigned char)*p) || *p == '_'))
			return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Version compare (X.Y.Z, the rule the updater uses)                   */
/* ------------------------------------------------------------------ */

static void StoreVerParts(const char* s, int* out)
{
	int k = 0;

	out[0] = out[1] = out[2] = 0;
	if (s == NULL)
		return;

	while (*s == ' ' || *s == 'v' || *s == 'V')
		s++;

	while (k < 3)
	{
		int v = 0, any = 0;

		while (isdigit((unsigned char)*s))
		{
			v = v * 10 + (*s - '0');
			s++;
			any = 1;
		}
		if (!any)
			break;
		out[k++] = v;
		if (*s == '.')
			s++;
		else
			break;
	}
}

/* -1 when a < b, 0 when equal, +1 when a > b. Unparsable counts as 0.0.0, so a
 * repo cannot win an update by writing nonsense in its version field. */
static int StoreVerCmp(const char* a, const char* b)
{
	int pa[3], pb[3], i;

	StoreVerParts(a, pa);
	StoreVerParts(b, pb);

	for (i = 0; i < 3; i++)
	{
		if (pa[i] < pb[i])
			return -1;
		if (pa[i] > pb[i])
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Tiny JSON reader (the index and the records; no library)             */
/* ------------------------------------------------------------------ */

static const char* JsonSkipWs(const char* p, const char* end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

/* The value that follows "key" : , bounded by end. NULL when absent. */
static const char* JsonFindValue(const char* start, const char* end, const char* key)
{
	size_t klen = strlen(key);
	const char* p = start;

	while (p < end)
	{
		if (*p == '"')
		{
			const char* q = p + 1;
			size_t n = 0;

			while (q + n < end && key[n] != 0 && q[n] == key[n])
				n++;

			if (key[n] == 0 && q + n < end && q[n] == '"')
			{
				const char* r = JsonSkipWs(q + n + 1, end);

				if (r < end && *r == ':')
					return JsonSkipWs(r + 1, end);
			}

			/* skip the string body (an escaped quote does not end it) */
			p++;
			while (p < end && *p != '"')
			{
				if (*p == '\\')
					p++;
				p++;
			}
			p++;
			continue;
		}
		p++;
	}
	return NULL;
}

static int JsonString(const char* p, const char* end, char* out, int max)
{
	int i = 0;

	if (max <= 0)
		return 0;
	out[0] = 0;

	if (p == NULL || p >= end || *p != '"')
		return 0;

	p++;
	while (p < end && *p != '"' && i < max - 1)
	{
		if (*p == '\\' && p + 1 < end)
		{
			p++;
			switch (*p)
			{
				case 'n': out[i++] = '\n'; break;
				case 't': out[i++] = '\t'; break;
				case 'r': break;
				case 'u':
				{
					int k;
					for (k = 0; k < 4 && p + 1 < end; k++)
						p++;
					if (i < max - 1)
						out[i++] = '?';
					break;
				}
				default: out[i++] = *p; break;
			}
			p++;
			continue;
		}
		out[i++] = *p++;
	}
	out[i] = 0;
	return 1;
}

static int JsonInt(const char* p, const char* end, int def)
{
	int v = 0, neg = 0;

	if (p == NULL || p >= end)
		return def;
	if (*p == '-')
	{
		neg = 1;
		p++;
	}
	if (p >= end || !isdigit((unsigned char)*p))
		return def;
	while (p < end && isdigit((unsigned char)*p))
	{
		v = v * 10 + (*p - '0');
		p++;
	}
	return neg ? -v : v;
}

/* The next {...} object in an array. NULL at the end. Fills the object's own
 * bounds so its keys are read inside it, never from a sibling. */
static const char* JsonNextObject(const char* p, const char* end, const char** objStart, const char** objEnd)
{
	int depth = 0, inStr = 0;

	p = JsonSkipWs(p, end);
	/* between array elements: step over the separator, or every index would
	 * stop after its first mod */
	if (p < end && *p == ',')
		p = JsonSkipWs(p + 1, end);
	if (p >= end || *p != '{')
		return NULL;

	*objStart = p;
	for (; p < end; p++)
	{
		char c = *p;

		if (inStr)
		{
			if (c == '\\')
				p++;
			else if (c == '"')
				inStr = 0;
			continue;
		}
		if (c == '"')
		{
			inStr = 1;
			continue;
		}
		if (c == '{')
			depth++;
		else if (c == '}')
		{
			depth--;
			if (depth == 0)
			{
				*objEnd = p + 1;
				return p + 1;
			}
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

static int StoreReadFile(const char* path, char* buf, int max)
{
	FILE* f;
	size_t n;

	if (max <= 0)
		return 0;
	buf[0] = 0;

	f = fopen(path, "rb");
	if (f == NULL)
		return 0;

	n = fread(buf, 1, (size_t)max - 1, f);
	fclose(f);
	buf[n] = 0;
	return (int)n;
}

static void StoreStatePath(char* out, int max, const char* leaf)
{
	snprintf(out, (size_t)max, "%s/%s", gStore.stateDir, leaf);
}

static void StoreIndexPath(char* out, int max, int repo)
{
	char leaf[32];

	snprintf(leaf, sizeof(leaf), "index-%d.json", repo);
	StoreStatePath(out, max, leaf);
}

static void StoreRecordPath(char* out, int max, const char* id)
{
	snprintf(out, (size_t)max, "%s/installed/%s.json", gStore.stateDir, id);
}

/* ------------------------------------------------------------------ */
/* Locating the install and the helper script                          */
/* ------------------------------------------------------------------ */

static void StoreFindExeDir(void)
{
#if defined(_WIN32)
	char exe[MAX_PATH];
	char* slash;

	if (GetModuleFileNameA(NULL, exe, sizeof(exe)) == 0)
	{
		StoreCopy(gStore.exeDir, sizeof(gStore.exeDir), ".");
		return;
	}
	slash = strrchr(exe, '\\');
	if (slash == NULL)
		slash = strrchr(exe, '/');
	if (slash != NULL)
		*slash = 0;
	StoreCopy(gStore.exeDir, sizeof(gStore.exeDir), exe);
#else
	char exe[1024];
	ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	char* slash;

	exe[k > 0 ? k : 0] = 0;
	if (k <= 0)
	{
		StoreCopy(gStore.exeDir, sizeof(gStore.exeDir), ".");
		return;
	}
	slash = strrchr(exe, '/');
	if (slash != NULL)
		*slash = 0;
	StoreCopy(gStore.exeDir, sizeof(gStore.exeDir), exe);
#endif
}

static void StoreFindScript(void)
{
#if defined(_WIN32)
	const char* names[] = { "modstore.bat", "modstore.cmd" };
	int i;
#else
	const char* names[] = { "modstore.sh" };
	int i;
#endif

	gStore.scriptFound = 0;
	gStore.script[0] = 0;

	for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
	{
		char path[STORE_PATH_MAX];
		FILE* f;

		snprintf(path, sizeof(path), "%s/%s", gStore.exeDir, names[i]);
		f = fopen(path, "rb");
		if (f != NULL)
		{
			fclose(f);
			StoreCopy(gStore.script, sizeof(gStore.script), path);
			gStore.scriptFound = 1;
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Repos                                                               */
/* ------------------------------------------------------------------ */

static void StoreReposLoad(void)
{
	int i;

	gStore.repoCount = 0;

	StoreCopy(gStore.repoName[0], STORE_ID_MAX, STORE_OFFICIAL_NAME);
	StoreCopy(gStore.repoUrl[0], STORE_URL_MAX, STORE_OFFICIAL_URL);

	/* A test can point the official slot somewhere local, so the whole flow runs
	 * without the network (JERICHO_TEST_MODSTORE_URL=file:///.../mods.json). */
	{
		const char* overrideUrl = getenv("JERICHO_TEST_MODSTORE_URL");

		if (overrideUrl != NULL && overrideUrl[0] != 0)
			StoreCopy(gStore.repoUrl[0], STORE_URL_MAX, overrideUrl);
	}
	gStore.repoCount = 1;

	/* repo2..repoN, each with an optional repoN_name. The official one is
	 * compiled in (CONFIG/ is preserved across updates, so a shipped default
	 * file would go stale and could never be corrected). */
	for (i = 2; i < STORE_MAX_REPOS; i++)
	{
		char key[16], nameKey[24];
		const char* url;
		char urlCopy[STORE_URL_MAX];

		snprintf(key, sizeof(key), "repo%d", i);
		url = jer_config_get_str("modstore", key, "");
		if (url == NULL || url[0] == 0)
			continue;

		StoreCopy(urlCopy, sizeof(urlCopy), url);

		snprintf(nameKey, sizeof(nameKey), "repo%d_name", i);
		StoreCopy(gStore.repoName[gStore.repoCount], STORE_ID_MAX,
			jer_config_get_str("modstore", nameKey, ""));
		if (gStore.repoName[gStore.repoCount][0] == 0)
			snprintf(gStore.repoName[gStore.repoCount], STORE_ID_MAX, "repo%d", i);

		StoreCopy(gStore.repoUrl[gStore.repoCount], STORE_URL_MAX, urlCopy);
		gStore.repoCount++;
	}

	if (gStore.repoSelected < 0 || gStore.repoSelected >= gStore.repoCount)
		gStore.repoSelected = 0;
}

/* ------------------------------------------------------------------ */
/* Records: what this store installed                                   */
/* ------------------------------------------------------------------ */

static STORE_RECORD* StoreFindRecord(const char* id)
{
	int i;

	for (i = 0; i < gStore.recordCount; i++)
	{
		if (store_stricmp(gStore.records[i].id, id) == 0)
			return &gStore.records[i];
	}
	return NULL;
}

static void StoreRecordParse(const char* json, int len, STORE_RECORD* rec)
{
	const char* start = json;
	const char* end = json + len;

	memset(rec, 0, sizeof(*rec));
	rec->present = 1;

	JsonString(JsonFindValue(start, end, "id"), end, rec->id, sizeof(rec->id));
	JsonString(JsonFindValue(start, end, "name"), end, rec->name, sizeof(rec->name));
	JsonString(JsonFindValue(start, end, "version"), end, rec->version, sizeof(rec->version));
	JsonString(JsonFindValue(start, end, "runtime"), end, rec->runtime, sizeof(rec->runtime));
	JsonString(JsonFindValue(start, end, "repo"), end, rec->url, sizeof(rec->url));
	JsonString(JsonFindValue(start, end, "modtoml_sha256"), end, rec->tomlSha, sizeof(rec->tomlSha));
	JsonString(JsonFindValue(start, end, "dll_sha256"), end, rec->dllSha, sizeof(rec->dllSha));
}

static void StoreRecordsLoad(void)
{
	char dirPath[STORE_PATH_MAX];

	gStore.recordCount = 0;
	snprintf(dirPath, sizeof(dirPath), "%s/installed", gStore.stateDir);

#if defined(_WIN32)
	{
		char pattern[STORE_PATH_MAX + 8];
		WIN32_FIND_DATAA fd;
		HANDLE hFind;

		snprintf(pattern, sizeof(pattern), "%s/*.json", dirPath);
		hFind = FindFirstFileA(pattern, &fd);

		if (hFind == INVALID_HANDLE_VALUE)
			return;

		do
		{
			char buf[4096];
			char path[STORE_PATH_MAX + 64];
			char id[STORE_ID_MAX];
			int len, i;

			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			if (gStore.recordCount >= JER_MAX_MODULES)
				break;

			snprintf(path, sizeof(path), "%s/%s", dirPath, fd.cFileName);
			len = StoreReadFile(path, buf, sizeof(buf));
			if (len <= 0)
				continue;

			/* the id comes from the FILE NAME too, so a record whose body was
			 * clobbered still cannot claim to be another mod */
			StoreCopy(id, sizeof(id), fd.cFileName);
			for (i = 0; id[i] != 0; i++)
			{
				if (id[i] == '.')
				{
					id[i] = 0;
					break;
				}
			}
			if (!StoreValidId(id))
				continue;

			StoreRecordParse(buf, len, &gStore.records[gStore.recordCount]);
			if (gStore.records[gStore.recordCount].id[0] == 0)
				StoreCopy(gStore.records[gStore.recordCount].id, STORE_ID_MAX, id);
			gStore.recordCount++;
		}
		while (FindNextFileA(hFind, &fd));

		FindClose(hFind);
	}
#else
	{
		DIR* d = opendir(dirPath);
		struct dirent* e;

		if (d == NULL)
			return;

		while ((e = readdir(d)) != NULL)
		{
			char buf[4096];
			char path[STORE_PATH_MAX + 64];
			char id[STORE_ID_MAX];
			int len, i;

			if (e->d_name[0] == '.')
				continue;
			if (!StoreEndsWith(e->d_name, ".json"))
				continue;
			if (gStore.recordCount >= JER_MAX_MODULES)
				break;

			snprintf(path, sizeof(path), "%s/%s", dirPath, e->d_name);
			len = StoreReadFile(path, buf, sizeof(buf));
			if (len <= 0)
				continue;

			StoreCopy(id, sizeof(id), e->d_name);
			for (i = 0; id[i] != 0; i++)
			{
				if (id[i] == '.')
				{
					id[i] = 0;
					break;
				}
			}
			if (!StoreValidId(id))
				continue;

			StoreRecordParse(buf, len, &gStore.records[gStore.recordCount]);
			if (gStore.records[gStore.recordCount].id[0] == 0)
				StoreCopy(gStore.records[gStore.recordCount].id, STORE_ID_MAX, id);
			gStore.recordCount++;
		}
		closedir(d);
	}
#endif
}

/* ------------------------------------------------------------------ */
/* The index                                                           */
/* ------------------------------------------------------------------ */

static void StoreResolveUrl(const char* base, const char* file, char* out, int max)
{
	const char* slash;

	out[0] = 0;
	if (file == NULL || file[0] == 0)
		return;

	/* absolute (scheme:// or file:///) */
	if (strstr(file, "://") != NULL)
	{
		StoreCopy(out, max, file);
		return;
	}

	if (file[0] == '/')
	{
		StoreCopy(out, max, file);
		return;
	}

	/* relative: resolve against the index's own directory */
	slash = strrchr(base, '/');
	if (slash == NULL)
	{
		StoreCopy(out, max, file);
		return;
	}
	snprintf(out, (size_t)max, "%.*s/%s", (int)(slash - base), base, file);
}

static int StoreIndexParse(const char* json, int len, int repo)
{
	const char* end = json + len;
	const char* p;
	const char* arr;
	int count = 0;

	gStore.modCount = 0;
	p = JsonFindValue(json, end, "mods");
	if (p == NULL)
		return 0;

	arr = JsonSkipWs(p, end);
	if (arr >= end || *arr != '[')
		return 0;
	arr++;

	p = arr;
	for (;;)
	{
		const char* objStart = NULL;
		const char* objEnd = NULL;
		const char* next = JsonNextObject(p, end, &objStart, &objEnd);
		STORE_MOD* m;

		if (next == NULL || count >= STORE_MAX_MODS)
			break;

		m = &gStore.mods[count];
		memset(m, 0, sizeof(*m));

		JsonString(JsonFindValue(objStart, objEnd, "id"), objEnd, m->id, sizeof(m->id));
		if (!StoreValidId(m->id))
		{
			p = next;
			continue;		/* a row without a usable id is skipped, not fatal */
		}

		JsonString(JsonFindValue(objStart, objEnd, "name"), objEnd, m->name, sizeof(m->name));
		JsonString(JsonFindValue(objStart, objEnd, "version"), objEnd, m->version, sizeof(m->version));
		JsonString(JsonFindValue(objStart, objEnd, "author"), objEnd, m->author, sizeof(m->author));
		JsonString(JsonFindValue(objStart, objEnd, "description"), objEnd, m->description, sizeof(m->description));
		JsonString(JsonFindValue(objStart, objEnd, "file"), objEnd, m->file, sizeof(m->file));
		JsonString(JsonFindValue(objStart, objEnd, "sha256"), objEnd, m->sha, sizeof(m->sha));
		JsonString(JsonFindValue(objStart, objEnd, "requires"), objEnd, m->requires, sizeof(m->requires));
		JsonString(JsonFindValue(objStart, objEnd, "min_jericho"), objEnd, m->minCore, sizeof(m->minCore));

		m->size = JsonInt(JsonFindValue(objStart, objEnd, "size"), objEnd, 0);
		m->defaultEnabled = JsonInt(JsonFindValue(objStart, objEnd, "default_enabled"), objEnd, 0);
		m->bakeVersion = JsonInt(JsonFindValue(objStart, objEnd, "bake_version"), objEnd, 0);

		StoreResolveUrl(gStore.repoUrl[repo], m->file, m->url, sizeof(m->url));

		if (m->name[0] == 0)
			StoreCopy(m->name, sizeof(m->name), m->id);

		count++;
		p = next;
	}

	gStore.modCount = count;
	return count;
}

static int StoreIndexLoad(int repo)
{
	char path[STORE_PATH_MAX];
	char* buf;
	int len, n = 0;
	FILE* f;
	long size;

	StoreIndexPath(path, sizeof(path), repo);

	f = fopen(path, "rb");
	if (f == NULL)
		return 0;

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || size > 512 * 1024)
	{
		fclose(f);
		return 0;
	}

	buf = (char*)malloc((size_t)size + 1);
	if (buf == NULL)
	{
		fclose(f);
		return 0;
	}

	len = (int)fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[len] = 0;

	n = StoreIndexParse(buf, len, repo);
	free(buf);

	gStore.indexState = 1;
	gStore.itemsDirty = 1;
	return n;
}

/* ------------------------------------------------------------------ */
/* Rows for the repo browser                                            */
/* ------------------------------------------------------------------ */

/* Fill one row's state from the index entry, the record and the live module
 * inventory. This is where the ownership rules live: a bundled mod the release
 * ships is updatable, never downgradable, and after a core update it reads as
 * REPLACED rather than as "installed". */
static void StoreRowState(STORE_MOD* m, JER_MODSTORE_ITEM* it, char* note, int noteMax)
{
	STORE_RECORD* rec = StoreFindRecord(m->id);
	JER_MODULE_INFO info[JER_MAX_MODULES];
	int count = jer_module_list(info, JER_MAX_MODULES);
	int i, live = -1;

	note[0] = 0;

	for (i = 0; i < count; i++)
	{
		if (store_stricmp(info[i].id, m->id) == 0)
		{
			live = i;
			break;
		}
	}

	it->id = m->id;
	it->name = m->name;
	it->version = m->version;
	it->author = m->author;
	it->description = m->description;
	it->note = note;
	it->canInstall = 1;
	it->canRemove = 0;
	it->installedVersion = 0;

	if (m->minCore[0] != 0 && StoreVerCmp(STORE_CORE_VERSION, m->minCore) < 0)
	{
		it->state = JER_MODSTORE_ST_NEEDS_CORE;
		snprintf(note, (size_t)noteMax, "needs JERICHO %s or newer", m->minCore);
		it->canInstall = 0;
		return;
	}

	if (rec == NULL)
	{
		if (live < 0)
		{
			it->state = JER_MODSTORE_ST_AVAILABLE;
			return;
		}

		/* present, but not from the store: the release ships it. Updatable only if
		 * the repo is genuinely AHEAD; the same version is simply what is here, and
		 * an older repo entry is never a downgrade this store will make. */
		it->installedVersion = 1;
		switch (StoreVerCmp(m->version, info[live].version))
		{
			case 1:
				it->state = JER_MODSTORE_ST_UPDATE;
				it->canInstall = 1;
				snprintf(note, (size_t)noteMax, "update %s -> %s", info[live].version, m->version);
				break;
			case 0:
				it->state = JER_MODSTORE_ST_INSTALLED;
				it->canInstall = 0;
				snprintf(note, (size_t)noteMax, "shipped with JERICHO %s", info[live].version);
				break;
			default:
				it->state = JER_MODSTORE_ST_BUNDLED_NEWER;
				it->canInstall = 0;
				snprintf(note, (size_t)noteMax, "newer here (%s)", info[live].version);
				break;
		}
		return;
	}

	it->installedVersion = 1;
	it->canRemove = 1;

	if (live >= 0 && StoreVerCmp(info[live].version, rec->version) != 0)
	{
		/* what is on disk is not what we installed: a core update replaced the
		 * folder with the release's copy. Offer the store's version again. */
		it->state = JER_MODSTORE_ST_REPLACED;
		snprintf(note, (size_t)noteMax, "replaced by the release (%s here, %s installed)",
			info[live].version, rec->version);
		it->canInstall = (StoreVerCmp(m->version, rec->version) >= 0);
		return;
	}

	if (StoreVerCmp(m->version, rec->version) > 0)
	{
		it->state = JER_MODSTORE_ST_UPDATE;
		snprintf(note, (size_t)noteMax, "update %s -> %s", rec->version, m->version);
		return;
	}

	if (StoreVerCmp(m->version, rec->version) < 0)
	{
		/* never a downgrade: the release may already be ahead of the repo */
		it->state = JER_MODSTORE_ST_BUNDLED_NEWER;
		snprintf(note, (size_t)noteMax, "%s installed, repo has %s", rec->version, m->version);
		it->canInstall = 0;
		return;
	}

	it->state = JER_MODSTORE_ST_INSTALLED;
	it->canInstall = 0;
}

static void StoreItemsBuild(void)
{
	int i;

	gStore.itemCount = 0;

	for (i = 0; i < gStore.modCount && i < STORE_MAX_MODS; i++)
	{
		StoreRowState(&gStore.mods[i], &gStore.items[i],
			gStore.itemNote[i], (int)sizeof(gStore.itemNote[i]));
		gStore.itemCount++;
	}

	gStore.itemsDirty = 0;
}

/* Test levers (the JERICHO_TEST_* family). One env var drives one flow so the
 * whole path -- fetch an index, install a package, see the module appear -- can
 * be exercised headlessly, which is how it is verified in CI and in the repo's
 * own test scripts. Harmless when unset: every branch is behind a value. */
static int StoreJobRunning(void);
static const char* StoreStateText(int state);
enum
{
	STORE_TEST_NONE = 0,
	STORE_TEST_INDEX,
	STORE_TEST_INSTALL,
	STORE_TEST_REMOVE,
	STORE_TEST_MENU		/* open the table through the real screen hand-over */
};

static void StoreTestParse(void)
{
	const char* v = getenv("JERICHO_TEST_MODSTORE");

	gStore.testLever = STORE_TEST_NONE;
	gStore.testId[0] = 0;
	gStore.testStage = 0;

	if (v == NULL || v[0] == 0)
		return;

	if (store_stricmp(v, "index") == 0 || store_stricmp(v, "refresh") == 0)
	{
		gStore.testLever = STORE_TEST_INDEX;
	}
	else if (store_stricmp(v, "install") == 0)
	{
		gStore.testLever = STORE_TEST_INSTALL;
	}
	else if (strncmp(v, "remove:", 7) == 0)
	{
		gStore.testLever = STORE_TEST_REMOVE;
		StoreCopy(gStore.testId, sizeof(gStore.testId), v + 7);
	}
	else if (store_stricmp(v, "menu") == 0)
	{
		gStore.testLever = STORE_TEST_MENU;
	}
	else if (strncmp(v, "install:", 8) == 0)
	{
		gStore.testLever = STORE_TEST_INSTALL;
		StoreCopy(gStore.testId, sizeof(gStore.testId), v + 8);
	}

	jer_log("[modstore][test] lever %s%s%s\n", v,
		gStore.testId[0] ? " id=" : "", gStore.testId);
}

static void StoreTestTick(void)
{
	if (gStore.testLever == STORE_TEST_NONE || gStore.testStage >= 4)
		return;
	if (StoreJobRunning())
		return;	/* one step at a time; the job's own completion advances us */

	switch (gStore.testLever)
	{
		case STORE_TEST_INDEX:
			if (gStore.testStage == 0)
			{
				gStore.testStage = 1;
				jer_modstore_refresh();
			}
			else if (gStore.testStage >= 1)
			{
				gStore.testStage = 4;
				jer_log("[modstore][test] index: %d mod(s), state=%d, note=\"%s\"\n",
					gStore.modCount, gStore.indexState, gStore.indexNote);
			}
			break;

		case STORE_TEST_INSTALL:
			if (gStore.testStage == 0)
			{
				gStore.testStage = 1;
				jer_modstore_refresh();
				break;
			}
			if (gStore.testStage == 1)
			{
				int rows = jer_modstore_item_count();	/* builds the rows */
				const JER_MODSTORE_ITEM* row = NULL;
				int i, pick = -1;

				gStore.testStage = 2;
				for (i = 0; i < rows; i++)
				{
					const JER_MODSTORE_ITEM* it = jer_modstore_item(i);

					if (it == NULL)
						continue;
					if (gStore.testId[0] != 0)
					{
						if (store_stricmp(it->id, gStore.testId) == 0)
						{
							pick = i;
							break;
						}
					}
					else
					{
						pick = i;
						break;
					}
				}

				if (pick < 0)
				{
					gStore.testStage = 4;
					jer_log("[modstore][test] nothing to install (%d row(s))\n", rows);
					break;
				}

				row = jer_modstore_item(pick);
				jer_log("[modstore][test] installing %s %s (canInstall=%d)\n",
					row->id, row->version, row->canInstall);
				jer_modstore_install(row->id);
				break;
			}
			{
				JER_MODSTORE_MODULE mod;
				int n = 0, i, found = 0;

				gStore.testStage = 4;
				for (i = 0; i < jer_modstore_module_count(); i++)
				{
					if (!jer_modstore_module_at(i, &mod))
						continue;
					n++;
					if (gStore.testId[0] == 0 || store_stricmp(mod.id, gStore.testId) == 0)
						found = 1;
				}

				jer_log("[modstore][test] install finished: %s | %d module(s)%s\n",
					jer_modstore_status(), n, found ? ", the installed mod is in the inventory" : "");
			}
			break;

		case STORE_TEST_REMOVE:
			if (gStore.testStage == 0)
			{
				gStore.testStage = 1;
				jer_log("[modstore][test] removing %s\n", gStore.testId);
				jer_modstore_remove(gStore.testId);
			}
			else
			{
				gStore.testStage = 4;
				jer_log("[modstore][test] remove finished: %s\n", jer_modstore_status());
			}
			break;

		case STORE_TEST_MENU:
			/* enter the Mods screen the way its button does, so the HAND-OVER to the
			 * table menu is what gets exercised, then report what settled */
			if (gStore.testStage == 0)
			{
				gStore.testStage = 1;
				jer_mods_open_screen();
				break;
			}
			if (gStore.testStage == 1)
			{
				int i, rows;

				gStore.testStage = 2;
				jer_log("[modstore][test] screen->menu: current=%d jericho.mods=%d repo=%d modules=%d\n",
					jer_frontend_current_menu(), jer_frontend_find("jericho.mods"),
					jer_frontend_find("jericho.mods.repo"), jer_modstore_module_count());

				rows = jer_modstore_module_count();
				if (rows > 4)
					rows = 4;
				for (i = 0; i < rows; i++)
				{
					JER_MODSTORE_MODULE mod;

					if (jer_modstore_module_at(i, &mod))
						jer_log("[modstore][test]   row %d: %s %s enabled=%d fromStore=%d replaced=%d\n",
							i, mod.name, mod.version, mod.enabled, mod.fromStore, mod.replaced);
				}

				/* fetch the repo, so the browser has rows to lay out */
				jer_modstore_refresh();
				break;
			}
			if (gStore.testStage == 2)
			{
				gStore.testStage = 3;
				jer_log("[modstore][test] index: rows=%d status=\"%s\"\n",
					jer_modstore_item_count(), jer_modstore_status());
				jer_frontend_open(jer_frontend_find("jericho.mods.repo"));
				break;
			}
			{
				int i, rows;

				gStore.testStage = 4;
				jer_log("[modstore][test] repo menu: current=%d (repo=%d) repos=%d rows=%d\n",
					jer_frontend_current_menu(), jer_frontend_find("jericho.mods.repo"),
					jer_modstore_repo_count(), jer_modstore_item_count());

				rows = jer_modstore_item_count();
				if (rows > 4)
					rows = 4;
				for (i = 0; i < rows; i++)
				{
					const JER_MODSTORE_ITEM* it = jer_modstore_item(i);

					if (it != NULL)
						jer_log("[modstore][test]   repo row %d: %s %s state=%s canInstall=%d note=\"%s\"\n",
							i, it->name, it->version, StoreStateText(it->state), it->canInstall, it->note);
				}
			}
			break;

		default:
			break;
	}
}

/* ------------------------------------------------------------------ */
/* Running the helper                                                  */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
static HANDLE gJobProcess;
#else
static pid_t gJobPid;
#endif

static int StoreJobRunning(void)
{
	if (gStore.jobKind == STORE_JOB_NONE)
		return 0;
#if defined(_WIN32)
	return gJobProcess != NULL;
#else
	return gJobPid > 0;
#endif
}

/* POSIX only: the child's stdio is redirected into this file. Windows runs the
 * helper with CREATE_NO_WINDOW and its own modstore.log is the record. */
#if !defined(_WIN32)
static void StoreJobLogPath(char* out, int max)
{
	StoreStatePath(out, max, "job.log");
}
#endif

static int StoreJobStart(int kind, const char* arg1, const char* arg2, const char* arg3)
{
	char cmdTail[3][STORE_URL_MAX];
	const char* argv[6];
	int argc = 1, i;

	if (gStore.jobKind != STORE_JOB_NONE)
		return JER_MODSTORE_BUSY;

	if (!gStore.scriptFound)
	{
		StoreCopy(gStore.status, sizeof(gStore.status),
			"modstore.sh/.bat is not beside the game, so the store cannot install anything");
		gStore.statusIsError = 1;
		return JER_MODSTORE_NO_SCRIPT;
	}

	StoreCopy(cmdTail[0], sizeof(cmdTail[0]), arg1 != NULL ? arg1 : "");
	StoreCopy(cmdTail[1], sizeof(cmdTail[1]), arg2 != NULL ? arg2 : "");
	StoreCopy(cmdTail[2], sizeof(cmdTail[2]), arg3 != NULL ? arg3 : "");

	/* The helper's result file is deleted FIRST: if the script dies before it can
	 * write one (no shell, a full disk), the store must not read the previous
	 * job's answer and call it this job's. */
	{
		char lastPath[STORE_PATH_MAX];

		StoreStatePath(lastPath, sizeof(lastPath), "last.json");
		remove(lastPath);
	}

	switch (kind)
	{
		case STORE_JOB_INDEX:	argv[argc++] = "index";	break;
		case STORE_JOB_INSTALL:	argv[argc++] = "install";	break;
		case STORE_JOB_REMOVE:	argv[argc++] = "remove";	break;
		case STORE_JOB_REBAKE:	argv[argc++] = "rebake";	break;
		default: return JER_MODSTORE_UNAVAILABLE;
	}

	for (i = 0; i < 3; i++)
	{
		if (cmdTail[i][0] != 0)
			argv[argc++] = cmdTail[i];
	}
	argv[argc] = NULL;

#if defined(_WIN32)
	{
		char cmd[2048];
		STARTUPINFOA si;
		PROCESS_INFORMATION pi;
		int n = 0;

		n = snprintf(cmd, sizeof(cmd), "cmd.exe /c \"\"%s\"", gStore.script);
		for (i = 1; i < argc && n > 0 && n < (int)sizeof(cmd); i++)
			n += snprintf(cmd + n, sizeof(cmd) - n, " %s", argv[i]);
		snprintf(cmd + (n < (int)sizeof(cmd) ? n : (int)sizeof(cmd) - 1),
			sizeof(cmd) - (n < (int)sizeof(cmd) ? n : (int)sizeof(cmd) - 1), "\"");

		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;

		jer_log("[modstore] running: %s\n", cmd);

		if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
			NULL, gStore.exeDir, &si, &pi))
		{
			StoreCopy(gStore.status, sizeof(gStore.status), "could not start the mod store helper");
			gStore.statusIsError = 1;
			return JER_MODSTORE_NO_SCRIPT;
		}

		CloseHandle(pi.hThread);
		gJobProcess = pi.hProcess;
	}
#else
	{
		char logPath[STORE_PATH_MAX];
		pid_t pid;

		StoreJobLogPath(logPath, sizeof(logPath));

		pid = fork();
		if (pid < 0)
		{
			StoreCopy(gStore.status, sizeof(gStore.status), "could not start the mod store helper");
			gStore.statusIsError = 1;
			return JER_MODSTORE_NO_SCRIPT;
		}

		if (pid == 0)
		{
			int fd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			setsid();
			if (fd >= 0)
			{
				dup2(fd, 1);
				dup2(fd, 2);
			}

			/* the script runs from the exe's folder: ROOT is its own dir */
			if (chdir(gStore.exeDir) != 0)
				_exit(127);

			execl("/bin/sh", "sh", gStore.script, argv[1], argv[2], argv[3], argv[4], (char*)NULL);
			_exit(127);
		}

		gJobPid = pid;
	}
#endif

	gStore.jobKind = kind;
	gStore.statusIsError = 0;

	if (kind == STORE_JOB_INSTALL)
	{
		StoreCopy(gStore.jobId, sizeof(gStore.jobId), arg3);
		snprintf(gStore.status, sizeof(gStore.status), "installing %s...", arg3);
	}
	else if (kind == STORE_JOB_REMOVE)
	{
		StoreCopy(gStore.jobId, sizeof(gStore.jobId), arg1);
		snprintf(gStore.status, sizeof(gStore.status), "removing %s when the game closes...", arg1);
	}
	else if (kind == STORE_JOB_INDEX)
	{
		gStore.jobId[0] = 0;
		snprintf(gStore.status, sizeof(gStore.status), "contacting %s...", gStore.repoName[gStore.jobRepo]);
	}
	else
	{
		gStore.jobId[0] = 0;
		StoreCopy(gStore.status, sizeof(gStore.status), "re-baking Driver 1 cars...");
	}

	jer_log("[modstore] started %s job%s%s\n",
		kind == STORE_JOB_INDEX ? "index" :
		kind == STORE_JOB_INSTALL ? "install" :
		kind == STORE_JOB_REMOVE ? "remove" : "rebake",
		arg1 != NULL && arg1[0] ? " " : "",
		arg1 != NULL ? arg1 : "");
	return JER_MODSTORE_OK;
}

/* Read the helper's own result file: op/id/ok/msg/script. */
static void StoreJobResult(char* msg, int msgMax, int* ok, int* scriptVersion)
{
	char path[STORE_PATH_MAX];
	char buf[4096];
	int len;

	msg[0] = 0;
	*ok = 0;
	*scriptVersion = 0;

	StoreStatePath(path, sizeof(path), "last.json");
	len = StoreReadFile(path, buf, sizeof(buf));
	if (len <= 0)
	{
		StoreCopy(msg, msgMax, "the helper said nothing (no last.json)");
		return;
	}

	{
		const char* end = buf + len;

		JsonString(JsonFindValue(buf, end, "msg"), end, msg, msgMax);
		*ok = JsonInt(JsonFindValue(buf, end, "ok"), end, 0);
		*scriptVersion = JsonInt(JsonFindValue(buf, end, "script"), end, 0);
	}
}

static void StoreJobFinish(void)
{
	char msg[STORE_MSG_MAX];
	int ok = 0, ver = 0;
	int kind = gStore.jobKind;

	StoreJobResult(msg, sizeof(msg), &ok, &ver);

	gStore.scriptVersion = ver;
	gStore.jobKind = STORE_JOB_NONE;
	gStore.statusIsError = !ok;

	if (msg[0] == 0)
		StoreCopy(msg, sizeof(msg), ok ? "done" : "failed");

	StoreCopy(gStore.status, sizeof(gStore.status), msg);

	jer_log("[modstore] %s finished: %s\n",
		kind == STORE_JOB_INDEX ? "index" :
		kind == STORE_JOB_INSTALL ? "install" :
		kind == STORE_JOB_REMOVE ? "remove" : "rebake", msg);

	if (ver != 0 && ver != JER_MODSTORE_SCRIPT_VERSION)
	{
		jer_log("[modstore] warning: the helper script speaks v%d, this build expects v%d "
			"- a game update and its script have drifted apart\n", ver, JER_MODSTORE_SCRIPT_VERSION);
	}

	switch (kind)
	{
		case STORE_JOB_INDEX:
			if (ok)
			{
				if (StoreIndexLoad(gStore.jobRepo) <= 0)
				{
					gStore.indexState = 2;
					StoreCopy(gStore.indexNote, sizeof(gStore.indexNote), "the repo's index has no mods in it");
				}
				else
				{
					gStore.indexNote[0] = 0;
				}
			}
			else
			{
				gStore.indexState = 2;
				StoreCopy(gStore.indexNote, sizeof(gStore.indexNote), msg);
			}
			break;

		case STORE_JOB_INSTALL:
			if (ok)
			{
				/* installed, and enabled: the prompt the player answered said so */
				if (gStore.jobId[0] != 0)
				{
					jer_manager_set_enabled(jer_root_dir(), gStore.jobId, 1);
					jer_manager_reload(jer_root_dir());
				}
			}
			StoreRecordsLoad();
			gStore.itemsDirty = 1;
			break;

		case STORE_JOB_REMOVE:
			StoreRecordsLoad();
			gStore.itemsDirty = 1;
			if (ok)
				jer_manager_reload(jer_root_dir());
			break;

		default:
			break;
	}

	if (!ok)
		jer_notify(msg, 2, 6);

	/* the rows just changed under the browser, so relayout it if it is on screen */
	if (jer_frontend_current_menu() == jer_frontend_find("jericho.mods.repo"))
		jer_frontend_refresh();
}

static void StoreJobPoll(void)
{
	if (!StoreJobRunning())
		return;

#if defined(_WIN32)
	{
		DWORD code = 0;

		if (WaitForSingleObject(gJobProcess, 0) != WAIT_OBJECT_0)
			return;

		GetExitCodeProcess(gJobProcess, &code);
		CloseHandle(gJobProcess);
		gJobProcess = NULL;
	}
#else
	{
		int status = 0;

		if (waitpid(gJobPid, &status, WNOHANG) != gJobPid)
			return;

		gJobPid = 0;
	}
#endif

	StoreJobFinish();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void jer_modstore_init(const char* rootDir)
{
	char path[STORE_PATH_MAX];

	memset(&gStore, 0, sizeof(gStore));
	gStore.scriptVersion = -1;

	if (rootDir != NULL && rootDir[0] != 0)
		StoreCopy(gStore.root, sizeof(gStore.root), rootDir);
	else
		StoreCopy(gStore.root, sizeof(gStore.root), jer_root_dir());

	snprintf(gStore.stateDir, sizeof(gStore.stateDir), "%s/CONFIG/modstore", gStore.root);
	snprintf(gStore.modsDir, sizeof(gStore.modsDir), "%s/MODS", gStore.root);

#if defined(_WIN32)
	_mkdir(gStore.stateDir);
	snprintf(path, sizeof(path), "%s/installed", gStore.stateDir);
	_mkdir(path);
#else
	mkdir(gStore.stateDir, 0755);
	snprintf(path, sizeof(path), "%s/installed", gStore.stateDir);
	mkdir(path, 0755);
#endif

	StoreFindExeDir();
	StoreFindScript();
	StoreTestParse();
	StoreReposLoad();
	StoreRecordsLoad();

	/* a cached index from the last run, so the browser is not empty on entry */
	StoreIndexPath(path, sizeof(path), gStore.repoSelected);
	{
		FILE* f = fopen(path, "rb");

		if (f != NULL)
		{
			fclose(f);
			StoreIndexLoad(gStore.repoSelected);
		}
		else
		{
			gStore.indexState = 0;
		}
	}

	gStore.ready = 1;

	jer_log("[modstore] initialised: %d repo(s), %d install record(s), helper %s\n",
		gStore.repoCount, gStore.recordCount,
		gStore.scriptFound ? gStore.script : "(not found beside the exe)");

	if (!gStore.scriptFound)
	{
		jer_log("[modstore] modstore.sh/.bat must sit beside the game exe for installs to work\n");
	}
}

int jer_modstore_busy(void)
{
	return StoreJobRunning();
}

const char* jer_modstore_status(void)
{
	if (gStore.status[0] == 0)
		return "Idle";
	return gStore.status;
}

int jer_modstore_repo_count(void)
{
	return gStore.repoCount;
}

const char* jer_modstore_repo_name(int index)
{
	if (index < 0 || index >= gStore.repoCount)
		return "";
	return gStore.repoName[index];
}

const char* jer_modstore_repo_url(int index)
{
	if (index < 0 || index >= gStore.repoCount)
		return "";
	return gStore.repoUrl[index];
}

int jer_modstore_repo_selected(void)
{
	return gStore.repoSelected;
}

void jer_modstore_repo_select(int index)
{
	if (index < 0 || index >= gStore.repoCount)
		return;
	if (index == gStore.repoSelected)
		return;

	gStore.repoSelected = index;
	gStore.modCount = 0;
	gStore.itemCount = 0;
	gStore.indexState = 0;
	gStore.indexNote[0] = 0;

	if (StoreIndexLoad(index) > 0)
		gStore.indexState = 1;
}

int jer_modstore_refresh(void)
{
	char path[STORE_PATH_MAX];

	if (StoreJobRunning())
		return JER_MODSTORE_BUSY;

	if (gStore.repoCount <= 0)
	{
		StoreCopy(gStore.status, sizeof(gStore.status), "no repos are configured");
		gStore.statusIsError = 1;
		return JER_MODSTORE_UNAVAILABLE;
	}

	StoreIndexPath(path, sizeof(path), gStore.repoSelected);
	gStore.jobRepo = gStore.repoSelected;

	return StoreJobStart(STORE_JOB_INDEX, gStore.repoUrl[gStore.repoSelected], path, NULL);
}

int jer_modstore_item_count(void)
{
	if (gStore.itemsDirty)
		StoreItemsBuild();
	return gStore.itemCount;
}

const JER_MODSTORE_ITEM* jer_modstore_item(int index)
{
	if (gStore.itemsDirty)
		StoreItemsBuild();
	if (index < 0 || index >= gStore.itemCount)
		return NULL;
	return &gStore.items[index];
}

int jer_modstore_install(const char* id)
{
	int i, rows;

	if (StoreJobRunning())
		return JER_MODSTORE_BUSY;
	if (!StoreValidId(id))
		return JER_MODSTORE_BAD_ID;

	rows = jer_modstore_item_count();	/* build the rows this reads */

	for (i = 0; i < rows; i++)
	{
		if (store_stricmp(gStore.items[i].id, id) == 0)
		{
			if (!gStore.items[i].canInstall)
			{
				StoreCopy(gStore.status, sizeof(gStore.status),
					gStore.items[i].note != NULL && gStore.items[i].note[0] != 0 ?
					gStore.items[i].note : "there is nothing to install for that mod");
				gStore.statusIsError = 1;
				return JER_MODSTORE_UNAVAILABLE;
			}
			return StoreJobStart(STORE_JOB_INSTALL, gStore.mods[i].url, gStore.mods[i].sha, gStore.mods[i].id);
		}
	}

	return JER_MODSTORE_UNAVAILABLE;
}

int jer_modstore_remove(const char* id)
{
	char pidText[24];

	if (StoreJobRunning())
		return JER_MODSTORE_BUSY;
	if (!StoreValidId(id))
		return JER_MODSTORE_BAD_ID;
	if (StoreFindRecord(id) == NULL)
		return JER_MODSTORE_NOT_OURS;

	/* the DLL is loaded in THIS process, so the delete has to wait for it: the
	 * helper holds the folder until this pid is gone, which is the same trick
	 * update_jericho uses to replace the exe it is running under. */
#if defined(_WIN32)
	snprintf(pidText, sizeof(pidText), "%lu", (unsigned long)GetCurrentProcessId());
#else
	snprintf(pidText, sizeof(pidText), "%d", (int)getpid());
#endif

	return StoreJobStart(STORE_JOB_REMOVE, id, "--wait", pidText);
}

int jer_modstore_rebake(void)
{
	if (StoreJobRunning())
		return JER_MODSTORE_BUSY;

	return StoreJobStart(STORE_JOB_REBAKE, NULL, NULL, NULL);
}

/* ================================================================== */
/* The menus                                                           */
/*                                                                     */
/* Two registered frontend menus, in the shape the mp server browser    */
/* uses (mp_ui.c):                                                     */
/*                                                                     */
/*   jericho.mods       the installed modules, as a table              */
/*   jericho.mods.repo  what a repo offers: install, update, remove    */
/*                                                                     */
/* Rows carry a NULL label and are painted in the draw below: the       */
/* engine still lays them out, highlights them, navigates them and      */
/* pages them, it just has nothing of its own to write in them. That    */
/* is what lets a row be four columns instead of one centred string.    */
/* ================================================================== */

#define MODS_ROWS_MAX	6	/* table rows a page of the mods list shows */
#define REPO_ROWS_MAX	6	/* same for the repo browser */

static int  gModsPage;
static int  gRepoPage;
static int  gModsRows;			/* list rows on the page being shown */
static int  gRepoRows;
static int  gModsReopen;		/* a reload wiped the registry: re-open us */
static char gPendingId[STORE_ID_MAX];	/* the mod the open prompt is about */
static char gPromptText[200];
static char gLabelMore[48];
static char gLabelRepo[STORE_ID_MAX + 16];

static JER_FE_ITEM gModsItems[JER_FE_MAX_ITEMS];
static JER_FE_ITEM gRepoItems[JER_FE_MAX_ITEMS];
static JER_FE_MENU gModsMenu;
static JER_FE_MENU gRepoMenu;
static int gMenusBuilt;

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void StoreAfterReload(void)
{
	/* jer_manager_reload clears the whole menu registry (jer_frontend_reset), so
	 * the menu that is on screen right now no longer exists in it: register it
	 * again and ask the engine to put it back where it was. */
	gModsReopen = 1;
}

static int ModsRowActivate(void* ud)
{
	int idx = (int)(size_t)ud;
	JER_MODSTORE_MODULE mod;

	if (!jer_modstore_module_at(idx, &mod) || mod.id == NULL)
		return 1;

	jer_manager_set_enabled(jer_root_dir(), mod.id, mod.enabled ? 0 : 1);
	jer_manager_reload(jer_root_dir());
	StoreAfterReload();
	return 1;
}

static int ModsRowAdjust(void* ud, int dir)
{
	int idx = (int)(size_t)ud;
	JER_MODSTORE_MODULE mod;

	if (!jer_modstore_module_at(idx, &mod) || mod.id == NULL)
		return 1;

	jer_manager_move(jer_root_dir(), mod.id, dir > 0 ? 1 : -1);
	jer_manager_reload(jer_root_dir());
	StoreAfterReload();
	return 1;
}

static int ModsNextPage(void* ud)
{
	(void)ud;
	gModsPage++;
	jer_frontend_refresh();
	return 1;
}

static int ModsCompile(void* ud)
{
	(void)ud;
	jer_mods_request_compile();
	return 1;
}

static int ModsCredits(void* ud)
{
	(void)ud;
	jer_mods_open_credits();
	return 1;
}

static void StoreInstallYes(void)
{
	jer_modstore_install(gPendingId);
}

static void StoreRemoveYes(void)
{
	jer_modstore_remove(gPendingId);
}

static int RepoRowActivate(void* ud)
{
	int idx = (int)(size_t)ud;
	const JER_MODSTORE_ITEM* it = jer_modstore_item(idx);

	if (it == NULL)
		return 1;

	if (!it->canInstall)
	{
		jer_notify((it->note != NULL && it->note[0] != 0) ? it->note : "there is nothing to install for that mod", 2, 5);
		return 1;
	}

	StoreCopy(gPendingId, sizeof(gPendingId), it->id);

	if (it->state == JER_MODSTORE_ST_UPDATE)
		snprintf(gPromptText, sizeof(gPromptText), "Update %s to %s?", it->name, it->version);
	else if (it->state == JER_MODSTORE_ST_REPLACED)
		snprintf(gPromptText, sizeof(gPromptText), "Reinstall %s %s over the release's copy?", it->name, it->version);
	else
		snprintf(gPromptText, sizeof(gPromptText), "Install %s %s?", it->name, it->version);

	jer_prompt_begin(gPromptText, StoreInstallYes);
	return 1;
}

static int RepoRowAdjust(void* ud, int dir)
{
	int idx = (int)(size_t)ud;
	const JER_MODSTORE_ITEM* it = jer_modstore_item(idx);

	(void)dir;

	if (it == NULL || !it->canRemove)
	{
		if (it != NULL)
			jer_notify("only a mod the store installed can be removed", 2, 4);
		return 1;
	}

	StoreCopy(gPendingId, sizeof(gPendingId), it->id);
	snprintf(gPromptText, sizeof(gPromptText), "Remove %s? It is deleted when JERICHO closes.", it->name);
	jer_prompt_begin(gPromptText, StoreRemoveYes);
	return 1;
}

static int RepoCycle(void* ud, int dir)
{
	int n = jer_modstore_repo_count();

	(void)ud;

	if (n <= 1)
		return 1;

	{
		int next = jer_modstore_repo_selected() + (dir > 0 ? 1 : -1);

		if (next < 0)
			next = n - 1;
		if (next >= n)
			next = 0;

		jer_modstore_repo_select(next);
	}

	gRepoPage = 0;
	jer_modstore_refresh();		/* fetch it, so the table fills by itself */
	jer_frontend_refresh();
	return 1;
}

static int RepoRefresh(void* ud)
{
	(void)ud;
	jer_modstore_refresh();
	jer_frontend_refresh();
	return 1;
}

static int RepoNextPage(void* ud)
{
	(void)ud;
	gRepoPage++;
	jer_frontend_refresh();
	return 1;
}

/* ------------------------------------------------------------------ */
/* Building the rows                                                   */
/* ------------------------------------------------------------------ */

static void ModsMenuEnter(void* ud)
{
	int count = jer_modstore_module_count();
	int pages, start, rows, i, n = 0, controls = 0;
	int repoMenu = jer_frontend_find("jericho.mods.repo");

	(void)ud;

	pages = (count + MODS_ROWS_MAX - 1) / MODS_ROWS_MAX;
	if (pages < 1)
		pages = 1;
	if (gModsPage >= pages)
		gModsPage = pages - 1;
	if (gModsPage < 0)
		gModsPage = 0;

	start = gModsPage * MODS_ROWS_MAX;
	rows = count - start;
	if (rows > MODS_ROWS_MAX)
		rows = MODS_ROWS_MAX;
	if (rows < 0)
		rows = 0;

	gModsRows = rows;

	for (i = 0; i < rows; i++)
	{
		JER_FE_ITEM* it = &gModsItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = NULL;				/* we draw the row */
		it->userdata = (void*)(size_t)(start + i);	/* the module index */
		it->on_activate = ModsRowActivate;
		it->on_adjust = ModsRowAdjust;
		it->submenu = -1;
	}

	if (pages > 1)
	{
		JER_FE_ITEM* it = &gModsItems[n++];

		memset(it, 0, sizeof(*it));
		snprintf(gLabelMore, sizeof(gLabelMore), "More mods  (%d/%d)", gModsPage + 1, pages);
		it->label = gLabelMore;
		it->on_activate = ModsNextPage;
		it->submenu = -1;
	}

	/* controls, pinned to the bottom edge */
	{
		JER_FE_ITEM* it = &gModsItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = "Mod Repo";
		it->submenu = repoMenu;			/* engine navigation */
		controls++;
	}

	if (jer_mods_compile_available())
	{
		JER_FE_ITEM* it = &gModsItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = "Compile Mods";
		it->on_activate = ModsCompile;
		it->submenu = -1;
		controls++;
	}

	{
		JER_FE_ITEM* it = &gModsItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = "Credits";
		it->on_activate = ModsCredits;
		it->submenu = -1;
		controls++;
	}

	{
		JER_FE_ITEM* it = &gModsItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = "Back";
		it->is_back = 1;
		it->submenu = -1;
		controls++;
	}

	gModsMenu.item_count = n;
	gModsMenu.bottom_items = controls;
}

static void RepoMenuEnter(void* ud)
{
	int count = jer_modstore_item_count();
	int pages, start, rows, i, n = 0, controls = 0;

	(void)ud;

	pages = (count + REPO_ROWS_MAX - 1) / REPO_ROWS_MAX;
	if (pages < 1)
		pages = 1;
	if (gRepoPage >= pages)
		gRepoPage = pages - 1;
	if (gRepoPage < 0)
		gRepoPage = 0;

	start = gRepoPage * REPO_ROWS_MAX;
	rows = count - start;
	if (rows > REPO_ROWS_MAX)
		rows = REPO_ROWS_MAX;
	if (rows < 0)
		rows = 0;

	gRepoRows = rows;

	/* the first row names the repo and cycles it: one row, three jobs */
	{
		JER_FE_ITEM* it = &gRepoItems[n++];

		memset(it, 0, sizeof(*it));
		snprintf(gLabelRepo, sizeof(gLabelRepo), "Repo: %s  < >",
			jer_modstore_repo_name(jer_modstore_repo_selected()));
		it->label = gLabelRepo;
		it->on_adjust = RepoCycle;
		it->on_activate = RepoRefresh;
		it->submenu = -1;
	}

	for (i = 0; i < rows; i++)
	{
		JER_FE_ITEM* it = &gRepoItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = NULL;				/* we draw the row */
		it->userdata = (void*)(size_t)(start + i);
		it->on_activate = RepoRowActivate;
		it->on_adjust = RepoRowAdjust;
		it->submenu = -1;
	}

	if (pages > 1)
	{
		JER_FE_ITEM* it = &gRepoItems[n++];

		memset(it, 0, sizeof(*it));
		snprintf(gLabelMore, sizeof(gLabelMore), "More mods  (%d/%d)", gRepoPage + 1, pages);
		it->label = gLabelMore;
		it->on_activate = RepoNextPage;
		it->submenu = -1;
	}

	{
		JER_FE_ITEM* it = &gRepoItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = "Refresh";
		it->on_activate = RepoRefresh;
		it->submenu = -1;
		controls++;
	}

	{
		JER_FE_ITEM* it = &gRepoItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = "Back";
		it->is_back = 1;
		it->submenu = -1;
		controls++;
	}

	gRepoMenu.item_count = n;
	gRepoMenu.bottom_items = controls;
}

/* Register both menus if the registry does not hold them. The registry is wiped
 * on every module reload, so this is not a one-off: it is how the menu comes
 * back after the player toggles a mod. */
static void StoreEnsureMenus(void)
{
	if (!gMenusBuilt)
	{
		memset(&gModsMenu, 0, sizeof(gModsMenu));
		gModsMenu.id = "jericho.mods";
		gModsMenu.items = gModsItems;
		gModsMenu.on_enter = ModsMenuEnter;
		/* NO stock title: it is drawn at (100,150) at 1.5x, exactly where a tall
		 * table's rows get lifted to. The heading is ours, above the header row. */
		gModsMenu.title = NULL;
		gModsMenu.row_w = 560;
		gModsMenu.row_step = 28;
		gModsMenu.row_top = 200;
		gModsMenu.row_bottom = 466;
		gModsMenu.list_align = 0;	/* hang the rows off the top: a table with a
									 * heading reads wrong centred (mp's reason) */

		memset(&gRepoMenu, 0, sizeof(gRepoMenu));
		gRepoMenu.id = "jericho.mods.repo";
		gRepoMenu.items = gRepoItems;
		gRepoMenu.on_enter = RepoMenuEnter;
		gRepoMenu.title = NULL;
		gRepoMenu.row_w = 560;
		gRepoMenu.row_step = 28;
		gRepoMenu.row_top = 200;
		gRepoMenu.row_bottom = 466;
		gRepoMenu.list_align = 0;

		gMenusBuilt = 1;
	}

	if (jer_frontend_find("jericho.mods") < 0)
		jer_frontend_register_menu(&gModsMenu);
	if (jer_frontend_find("jericho.mods.repo") < 0)
		jer_frontend_register_menu(&gRepoMenu);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static int StoreTextX(int x, int w, int pct)
{
	return x + (w * pct) / 100;
}

/* The mods table: NAME / VERSION / STATE / SOURCE, plus the store's last word
 * above it. The headings are drawn even when there are no rows, so an empty list
 * still reads as a table rather than as a broken screen. */
static void ModsDrawTable(void)
{
	int x = 40, y = 210, w = 560, h = 28, i, top;
	char buf[96];

	jer_frontend_row_rect(0, &x, &y, &w, &h);
	top = jer_frontend_list_top();

	/* the store's last word, under the rows and clear of the pinned controls */
	jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 2), 470, jer_modstore_status(), 190, 190, 190);

	/* the heading. The stock menu title is not used (see StoreEnsureMenus): a tall
	 * table is lifted, and the stock title is right where the rows land. */
	jer_text_draw_at(JER_TEXT_FE, 150, StoreTextX(x, w, 2), top - 52, "MODS", 255, 176, 64);

	jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 2), top - 22, "MODULE", 255, 176, 64);
	jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 50), top - 22, "VERSION", 255, 176, 64);
	jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 64), top - 22, "STATE", 255, 176, 64);
	jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 80), top - 22, "SOURCE", 255, 176, 64);

	for (i = 0; i < gModsRows; i++)
	{
		int rx = 0, ry = 0, rw = 0, rh = 0;
		JER_MODSTORE_MODULE mod;

		if (!jer_frontend_row_rect(i, &rx, &ry, &rw, &rh))
			break;

		/* one geometry line per run, so the table's numbers can be checked
		 * headlessly (the test lever only) */
		if (gStore.testLever == STORE_TEST_MENU && i == 0 && !gStore.drawLogged)
		{
			gStore.drawLogged = 1;
			jer_log("[modstore][test] table box x=%d w=%d top=%d; row0 x=%d y=%d w=%d h=%d; cols %d/%d/%d/%d\n",
				x, w, top, rx, ry, rw, rh,
				StoreTextX(rx, rw, 2), StoreTextX(rx, rw, 50), StoreTextX(rx, rw, 64), StoreTextX(rx, rw, 80));
		}
		if (!jer_modstore_module_at(gModsPage * MODS_ROWS_MAX + i, &mod))
			continue;

		jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 2), ry + 4,
			(mod.name != NULL) ? mod.name : mod.id, 255, 255, 255);
		jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 50), ry + 4,
			(mod.version != NULL) ? mod.version : "?", 200, 200, 200);

		if (mod.refusal != NULL)
			jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 64), ry + 4, "BLOCKED", 255, 128, 128);
		else if (mod.enabled)
			jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 64), ry + 4, "ON", 128, 255, 128);
		else
			jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 64), ry + 4, "OFF", 150, 150, 150);

		if (mod.replaced)
			snprintf(buf, sizeof(buf), "store %s (replaced)", (mod.storeVersion != NULL) ? mod.storeVersion : "?");
		else if (mod.fromStore)
			snprintf(buf, sizeof(buf), "store %s", (mod.storeVersion != NULL) ? mod.storeVersion : "");
		else
			snprintf(buf, sizeof(buf), "bundled");

		jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 80), ry + 4, buf, 170, 190, 210);
	}
}

static const char* StoreStateText(int state)
{
	switch (state)
	{
		case JER_MODSTORE_ST_INSTALLED:		return "installed";
		case JER_MODSTORE_ST_UPDATE:		return "update";
		case JER_MODSTORE_ST_REPLACED:		return "replaced";
		case JER_MODSTORE_ST_BUNDLED_NEWER:	return "newer here";
		case JER_MODSTORE_ST_NEEDS_CORE:	return "needs newer";
		case JER_MODSTORE_ST_NEEDS_DEP:		return "needs a mod";
		default:							return "available";
	}
}

/* The repo browser: NAME / VERSION / STATE / NOTE, with the job's status above
 * it. An unreachable repo and a repo with nothing in it both say so in a row,
 * so a failure never reads as an empty screen. */
static void StoreDrawRepoTable(void)
{
	int x = 40, y = 210, w = 560, h = 28, i, top;
	const char* status = jer_modstore_status();

	jer_frontend_row_rect(1, &x, &y, &w, &h);
	top = jer_frontend_list_top();

	if (jer_modstore_busy())
		jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 2), 470, status, 255, 220, 120);
	else
		jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 2), 470, status, 190, 190, 190);

	jer_text_draw_at(JER_TEXT_FE, 150, StoreTextX(x, w, 2), top - 52, "MOD REPO", 255, 176, 64);

	jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 2), top - 22, "MOD", 255, 176, 64);
	jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 34), top - 22, "VERSION", 255, 176, 64);
	jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 48), top - 22, "STATE", 255, 176, 64);
	jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 64), top - 22, "NOTE", 255, 176, 64);

	if (gRepoRows == 0)
	{
		const char* msg;

		if (jer_modstore_busy())
			msg = "(contacting the repo...)";
		else if (jer_modstore_item_count() == 0 && jer_modstore_status()[0] != 0)
			msg = "(no mods listed - press Refresh)";
		else
			msg = "(no mods listed)";

		jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(x, w, 2), top + 6, msg, 170, 170, 170);
		return;
	}

	for (i = 0; i < gRepoRows; i++)
	{
		int rx = 0, ry = 0, rw = 0, rh = 0;
		const JER_MODSTORE_ITEM* it;

		/* row 0 is the repo selector, so the table starts at 1 */
		if (!jer_frontend_row_rect(1 + i, &rx, &ry, &rw, &rh))
			break;

		it = jer_modstore_item(gRepoPage * REPO_ROWS_MAX + i);
		if (it == NULL)
			continue;

		jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 2), ry + 4, it->name, 255, 255, 255);
		jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 34), ry + 4, it->version, 200, 200, 200);

		switch (it->state)
		{
			case JER_MODSTORE_ST_INSTALLED:
				jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 48), ry + 4, StoreStateText(it->state), 128, 255, 128);
				break;
			case JER_MODSTORE_ST_UPDATE:
			case JER_MODSTORE_ST_REPLACED:
				jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 48), ry + 4, StoreStateText(it->state), 255, 220, 120);
				break;
			case JER_MODSTORE_ST_NEEDS_CORE:
			case JER_MODSTORE_ST_NEEDS_DEP:
				jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 48), ry + 4, StoreStateText(it->state), 255, 128, 128);
				break;
			default:
				jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 48), ry + 4, StoreStateText(it->state), 190, 190, 190);
				break;
		}

		if (it->note != NULL && it->note[0] != 0)
			jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 64), ry + 4, it->note, 170, 190, 210);
		else
			jer_text_draw_at(JER_TEXT_FE, 100, StoreTextX(rx, rw, 64), ry + 4, it->author, 170, 190, 210);
	}
}

static void StoreDraw(void)
{
	int menu = jer_frontend_current_menu();
	int mods = jer_frontend_find("jericho.mods");
	int repo = jer_frontend_find("jericho.mods.repo");

	if (menu < 0)
		return;

	if (menu == mods)
		ModsDrawTable();
	else if (menu >= 0 && menu == repo)
		StoreDrawRepoTable();
}

int jer_modstore_menu_active(void)
{
	int menu = jer_frontend_find("jericho.mods");

	return menu >= 0 && jer_frontend_current_menu() == menu;
}

void jer_modstore_open_menu(void)
{
	int menu;

	StoreEnsureMenus();
	menu = jer_frontend_find("jericho.mods");

	if (menu < 0)
		return;

	jer_frontend_open_replace(menu);
}

int jer_modstore_module_count(void)
{
	JER_MODULE_INFO info[JER_MAX_MODULES];

	return jer_module_list(info, JER_MAX_MODULES);
}

int jer_modstore_module_at(int index, JER_MODSTORE_MODULE* out)
{
	JER_MODULE_INFO info[JER_MAX_MODULES];
	int count = jer_module_list(info, JER_MAX_MODULES);
	STORE_RECORD* rec;

	if (out == NULL || index < 0 || index >= count)
		return 0;

	rec = StoreFindRecord(info[index].id);

	out->id = info[index].id;
	out->name = info[index].name;
	out->version = info[index].version;
	out->enabled = info[index].enabled;
	out->refusal = info[index].refusal;
	out->fromStore = (rec != NULL);
	out->storeVersion = (rec != NULL) ? rec->version : NULL;
	out->replaced = (rec != NULL && info[index].version != NULL && rec->version[0] != 0 &&
		StoreVerCmp(info[index].version, rec->version) != 0);
	return 1;
}

void jer_modstore_frontend_tick(void)
{
	int reopen;

	StoreJobPoll();

	/* the registry is cleared by every module reload, so this is not a one-off */
	StoreEnsureMenus();
	StoreTestTick();

	/* after DrawScreen (FEmain.c calls this next to the updater's own tick), which
	 * is where a module's text belongs -- JER_EVENT_FRAME fires before the box. */
	StoreDraw();

	/* a reload inside an ACTION wiped the registry under our feet: put the menu
	 * back in the screen's place and re-lay it out */
	reopen = gModsReopen;
	if (reopen)
	{
		int idx = jer_frontend_find("jericho.mods");

		gModsReopen = 0;
		if (idx >= 0)
		{
			jer_frontend_open_replace(idx);
			jer_frontend_refresh();
		}
	}
}
