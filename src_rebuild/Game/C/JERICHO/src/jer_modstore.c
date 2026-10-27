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
#include "jer_rebake.h"	/* the one Driver 1 re-bake prompt; re-armed after an install */
#include "jer_screen.h"	/* jer_screen_active: never prompt over a presentation screen */
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
#include <sys/stat.h>	/* _stat: the cached index's age */
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
#define STORE_OFFICIAL_URL	"https://raw.githubusercontent.com/Njurse/JERICHO-Repo-Official/main/mods.json"

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
	char interfaceVer[8];		/* the index entry's `interface`: the SDK version it was BUILT
								 * against. Empty = not declared (an entry written before
								 * the key existed). */
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
	int  repoState[STORE_MAX_REPOS];	/* 0 never fetched, 1 loaded, 2 failed */
	int  scriptMismatch;			/* the helper beside the exe speaks another version */
	int  scriptVersionSeen;

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

/* The startup mod-update check and its Yes action (see
 * jer_modstore_frontend_tick and jer_modstore_startup_update_pending). Declared
 * here because the public predicate reads them. */
static int  gStartupUpdate;		/* 0 = unchecked, 1 = fetching the index, 2 = done */
static int  gUpdateAll;			/* the prompt was answered Yes: apply updates in turn */

/* A compiled-in mod is not in the running exe just because its folder landed in
 * MODS/: premake generates the module registry from that folder at build time.
 * So the store offers the rebuild when the player LEAVES the repo browser, where
 * a module that was just downloaded would otherwise sit inert until they find
 * the "Compile Mods" row on their own. */
static int  gCompileAsk;		/* a compiled-in mod was just installed */

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

/* "just now" / "12m" / "3h" / "2d" for the cached index of a repo. */
static const char* StoreIndexAge(int repo)
{
	static char age[4][16];
	static int slot = 0;
	char path[STORE_PATH_MAX];
#if defined(_WIN32)
	struct _stat st;
#else
	struct stat st;
#endif
	time_t now;
	long secs;
	char* out;

	if (repo < 0 || repo >= STORE_MAX_REPOS)
		return "-";

	out = age[slot++ & 3];

	StoreIndexPath(path, sizeof(path), repo);

	/* MSVC keeps the POSIX names behind a macro, so the type and the call differ */
#if defined(_WIN32)
	if (_stat(path, &st) != 0)
#else
	if (stat(path, &st) != 0)
#endif
	{
		snprintf(out, 16, "no index");
		return out;
	}

	now = time(NULL);
	secs = (long)(now - (time_t)st.st_mtime);
	if (secs < 0)
		secs = 0;

	if (secs < 60)
		snprintf(out, 16, "just now");
	else if (secs < 3600)
		snprintf(out, 16, "%ldm old", secs / 60);
	else if (secs < 86400)
		snprintf(out, 16, "%ldh old", secs / 3600);
	else
		snprintf(out, 16, "%ldd old", secs / 86400);

	return out;
}

static const char* StoreRepoStateText(int repo)
{
	if (repo < 0 || repo >= STORE_MAX_REPOS)
		return "?";

	switch (gStore.repoState[repo])
	{
		case 1:  return "fetched";
		case 2:  return "failed";
		default: return "not fetched";
	}
}

const char* jer_modstore_repo_state(int index)
{
	return StoreRepoStateText(index);
}

const char* jer_modstore_repo_age(int index)
{
	return StoreIndexAge(index);
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

/* Write a commented CONFIG/modstore.ini the first time the store runs. Nothing
 * here is required -- the official repo is compiled in -- but a player who wants
 * a second repo should start from a file that explains itself rather than from an
 * empty one. Never overwrites: CONFIG/ is preserved across updates, so an existing
 * file is the player's. */
static void StoreWriteIniTemplate(void)
{
	char path[STORE_PATH_MAX];
	FILE* f;

	snprintf(path, sizeof(path), "%s/CONFIG/modstore.ini", gStore.root);

	f = fopen(path, "rb");
	if (f != NULL)
	{
		fclose(f);
		return;
	}

	f = fopen(path, "wb");
	if (f == NULL)
		return;

	fprintf(f, "; JERICHO mod store -- which repos the Mod Repo screen talks to.\n"
		";\n"
		"; The official repo is compiled into the game, so this file only matters if\n"
		"; you want another one: a friend's repo, a mod author's, or your own test\n"
		"; repo. This file is preserved by the updater -- your repos survive an\n"
		"; update.\n"
		";\n"
		"; Add as many as you like, numbered from 2:\n"
		";\n"
		";   repo2      = http://127.0.0.1:8099/mods.json\n"
		";   repo2_name = My test repo\n"
		";\n"
		"; `repo` (no number) replaces the OFFICIAL repo instead of adding one, which\n"
		"; is how you point the game at a local test index:\n"
		";\n"
		";   repo = http://127.0.0.1:8099/mods.json\n"
		";\n"
		"; A repo is a URL to a mods.json. The packages it names are downloaded from\n"
		"; the URLs it gives; a package named with no URL is looked for next to the\n"
		"; index. See JERICHO/CORE/modstore/ and docs/mod-repo.md for the format.\n");

	fclose(f);
	jer_log("[modstore] wrote a starter %s\n", path);
}

static void StoreReposLoad(void)
{
	int i;

	gStore.repoCount = 0;

	StoreCopy(gStore.repoName[0], STORE_ID_MAX, STORE_OFFICIAL_NAME);
	StoreCopy(gStore.repoUrl[0], STORE_URL_MAX, STORE_OFFICIAL_URL);

	/* `repo = <url>` replaces the compiled-in official repo. This is the one-line
	 * setup every doc shows, and it used to be ignored: only repo2..repoN were read,
	 * so a player who followed the instructions pointed at nothing. */
	{
		const char* official = jer_config_get_str("modstore", "repo", "");

		if (official != NULL && official[0] != 0)
			StoreCopy(gStore.repoUrl[0], STORE_URL_MAX, official);
	}

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
		/* a number in the index (the packager writes it unquoted), so read it as one
		 * and keep the text for the row note */
		{
			int iface = JsonInt(JsonFindValue(objStart, objEnd, "interface"), objEnd, 0);

			if (iface > 0)
				snprintf(m->interfaceVer, sizeof(m->interfaceVer), "%d", iface);
		}

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

/* ------------------------------------------------------------------ */
/* Dependencies and the Driver 1 bake                                  */
/* ------------------------------------------------------------------ */

static int StoreHasModule(const char* id)
{
	JER_MODULE_INFO info[JER_MAX_MODULES];
	int count = jer_module_list(info, JER_MAX_MODULES);
	int i;

	for (i = 0; i < count; i++)
	{
		if (store_stricmp(info[i].id, id) == 0)
			return 1;
	}
	return 0;
}

/* The first mod named in `requires` that is not installed here, or NULL. The
 * field is a comma or space separated list ("carhacks, mp"), the same shape
 * mod.toml uses, so one idiom covers both. */
static const char* StoreMissingDep(const char* requires, char* buf, int max)
{
	const char* p = requires;
	int n = 0;

	if (requires == NULL || requires[0] == 0)
		return NULL;

	while (*p != 0)
	{
		char id[STORE_ID_MAX];
		int k = 0;

		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		while (*p != 0 && *p != ',' && *p != ' ' && *p != '\t' && *p != '"' && k < STORE_ID_MAX - 1)
			id[k++] = *p++;
		id[k] = 0;

		if (k == 0)
			break;
		if (!StoreValidId(id))
			continue;
		if (!StoreHasModule(id))
		{
			StoreCopy(buf, max, id);
			n = 1;
			break;
		}
	}

	return n ? buf : NULL;
}

/* The first integer in a file, or -1 when there is none. */
static int StoreFileInt(const char* path, const char* afterKey)
{
	char buf[8192];
	const char* p;
	int len;

	len = StoreReadFile(path, buf, sizeof(buf));
	if (len <= 0)
		return -1;

	p = buf;
	if (afterKey != NULL)
	{
		const char* hit = strstr(buf, afterKey);

		if (hit == NULL)
			return -1;
		p = hit + strlen(afterKey);
	}

	while (*p != 0 && !isdigit((unsigned char)*p))
		p++;
	if (*p == 0)
		return -1;

	return atoi(p);
}

/* What the SHIPPED tooling expects, from BAKE_VERSION in the bake script. The
 * script is the record: bumping it is what says "the cars must be baked again". */
static int StoreToolingBakeVersion(void)
{
	const char* names[] =
	{
		"BAKE_D1_CARS.sh", "BAKE_D1_CARS.bat",
		"MODS/mp/tools/pack_lan/BAKE_D1_CARS.sh", "MODS/mp/tools/pack_lan/BAKE_D1_CARS.bat"
	};
	int i;

	for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
	{
		char path[STORE_PATH_MAX];
		int v;

		snprintf(path, sizeof(path), "%s/%s", (i < 2) ? gStore.exeDir : gStore.root, names[i]);
		v = StoreFileInt(path, "BAKE_VERSION");
		if (v >= 0)
			return v;
	}
	return -1;
}

/* Is there anything in this folder? Used for "a bake exists but predates the
 * stamp", which is a real state on machines baked before BAKE_VERSION existed. */
static int StoreDirHasEntries(const char* path)
{
#if defined(_WIN32)
	{
		char pattern[STORE_PATH_MAX + 8];
		WIN32_FIND_DATAA fd;
		HANDLE hFind;
		int found = 0;

		snprintf(pattern, sizeof(pattern), "%s/*", path);
		hFind = FindFirstFileA(pattern, &fd);
		if (hFind == INVALID_HANDLE_VALUE)
			return 0;

		do
		{
			if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
				continue;
			found = 1;
			break;
		}
		while (FindNextFileA(hFind, &fd));

		FindClose(hFind);
		return found;
	}
#else
	{
		DIR* d = opendir(path);
		struct dirent* e;
		int found = 0;

		if (d == NULL)
			return 0;

		while ((e = readdir(d)) != NULL)
		{
			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			found = 1;
			break;
		}
		closedir(d);
		return found;
	}
#endif
}

/* What is baked on THIS machine. -1 = no bake at all (nothing to report), 0 = a
 * bake from before the stamp existed (so older than any tooling that has one). */
static int StoreBakedVersion(void)
{
	char path[STORE_PATH_MAX];
	char dir[STORE_PATH_MAX];
	int v;

	/* <install>/JERICHO is the root, so the baked car data sits in the game's
	 * own tree: JERICHO/MODS/carhacks/D1CARS (carhacks owns it - the blobs are
	 * its modded LEVs, not part of the player's Driver 1 disc). */
	snprintf(dir, sizeof(dir), "%s/MODS/carhacks/D1CARS", gStore.root);
	snprintf(path, sizeof(path), "%s/BAKE_VERSION", dir);

	v = StoreFileInt(path, NULL);
	if (v >= 0)
		return v;

	/* A bake from a build before the move lived in DRIVER/D1CARS. Still report
	 * it (unstamped if it has no stamp), so an update sees an old bake and asks
	 * for a re-bake into the new folder instead of treating it as "no bake". */
	snprintf(dir, sizeof(dir), "%s/../DRIVER/D1CARS", gStore.root);
	snprintf(path, sizeof(path), "%s/BAKE_VERSION", dir);

	v = StoreFileInt(path, NULL);
	if (v >= 0)
		return v;

	/* unstamped: a bake from the tooling that predates the version, which is by
	 * definition older than the tooling beside the game now */
	return StoreDirHasEntries(dir) ? 0 : -1;
}

/* The version the repo's own bake entry asks for (a release that changes the
 * bake data without touching the script bumps this instead). */
static int StoreIndexBakeVersion(const char* id)
{
	int i;

	for (i = 0; i < gStore.modCount; i++)
	{
		if (store_stricmp(gStore.mods[i].id, id) == 0)
			return gStore.mods[i].bakeVersion;
	}
	return 0;
}

/* 1 when this machine has a Driver 1 bake older than what the shipped tooling (or
 * the repo's bake entry) expects. That is the only case worth asking about. */
static int StoreBakeStale(void)
{
	int baked = StoreBakedVersion();
	int wanted = StoreToolingBakeVersion();
	int fromIndex;

	if (baked < 0)
		return 0;			/* nothing baked: nothing to re-bake */

	fromIndex = StoreIndexBakeVersion("bake");
	if (fromIndex > wanted)
		wanted = fromIndex;

	if (wanted < 0)
		return 0;			/* nothing says what is expected */

	return baked < wanted;
}

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

	/* A package built against another INTERFACE cannot be installed at all: the
	 * loader refuses it at activation, so downloading it would only cost the player
	 * a restart and a confusing refusal. This is the store-side half of that check;
	 * jer_system.c makes it at activation, from the manifest. */
	if (m->interfaceVer[0] != 0 && atoi(m->interfaceVer) != JERICHO_SDK_VERSION)
	{
		it->state = JER_MODSTORE_ST_NEEDS_CORE;
		snprintf(note, (size_t)noteMax, "built for interface %s, this build is %d",
			m->interfaceVer, JERICHO_SDK_VERSION);
		it->canInstall = 0;
		return;
	}

	/* a mod that needs another mod, which is not here */
	{
		char missing[STORE_ID_MAX];

		if (StoreMissingDep(m->requires, missing, sizeof(missing)) != NULL)
		{
			it->state = JER_MODSTORE_ST_NEEDS_DEP;
			snprintf(note, (size_t)noteMax, "needs %s", missing);
			it->canInstall = 0;
			return;
		}
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
/* the Mods menu's "Mod Repo (N updates)" label, built on each layout */
static char gLabelRepoItem[64];

static void StoreDrawWrapped(int x, int y, int width, const char* text, int maxLines, int r, int g, int b);
static int StoreJobRunning(void);
static int StoreCountUpdates(void);
static const char* StoreStateText(int state);
/* the menu's row actions, defined with the menus further down; the test lever
 * drives them so the real Cross / Left-Right path is what gets exercised */
static int ModsRowActivate(void* ud);
static int ModsRowAdjust(void* ud, int dir);
enum
{
	STORE_TEST_NONE = 0,
	STORE_TEST_INDEX,
	STORE_TEST_INSTALL,
	STORE_TEST_REMOVE,
	STORE_TEST_MENU,	/* open the table through the real screen hand-over */
	STORE_TEST_RECOMPILE,	/* leave the repo browser after a compiled-in change */
	STORE_TEST_REBUILD	/* the prompt's Yes: exit and hand the build off */
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
	else if (store_stricmp(v, "recompile") == 0)
	{
		gStore.testLever = STORE_TEST_RECOMPILE;
	}
	else if (store_stricmp(v, "rebuild") == 0)
	{
		gStore.testLever = STORE_TEST_REBUILD;
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
	if (gStore.testLever == STORE_TEST_NONE || gStore.testStage >= 7)
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
				gStore.testStage = 7;
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
					gStore.testStage = 7;
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

				gStore.testStage = 7;	/* 7 is the terminal stage: StoreTestTick returns there,
									 * so the report is logged once rather than every tick */
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
				gStore.testStage = 7;
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
				/* the Mods menu advertises waiting updates on its Mod Repo item, so a
				 * player notices without opening the browser */
				jer_log("[modstore][test] Mod Repo item: \"%s\" (updates=%d)\n",
					gLabelRepoItem[0] != 0 ? gLabelRepoItem : "Browse Mod Repo", StoreCountUpdates());

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
			if (gStore.testStage == 3)
			{
				/* one settle tick: the engine switches screens between frames */
				gStore.testStage = 4;
				break;
			}
			if (gStore.testStage == 4)
			{
				int i, rows;

				gStore.testStage = 5;
				jer_log("[modstore][test] repo menu: current=%d (repo=%d) repos=%d rows=%d\n",
					jer_frontend_current_menu(), jer_frontend_find("jericho.mods.repo"),
					jer_modstore_repo_count(), jer_modstore_item_count());

				/* what each repo row says: state and index age are the two things a
				 * player needs to tell "stale", "never fetched" and "unreachable" apart */
				{
					int r;

					for (r = 0; r < jer_modstore_repo_count(); r++)
					{
						jer_log("[modstore][test]   repo %d: %s state=%s age=%s%s\n",
							r, jer_modstore_repo_name(r), jer_modstore_repo_state(r),
							jer_modstore_repo_age(r),
							r == jer_modstore_repo_selected() ? " (selected)" : "");
					}
				}

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

				/* now drive the LIST's own action -- the same callback Cross calls --
				 * so the toggle + reload + re-open path is exercised, not assumed */
				{
					JER_MODSTORE_MODULE before;

					if (jer_modstore_module_at(0, &before))
						jer_log("[modstore][test] toggling row 0 (%s), was enabled=%d\n",
							before.name, before.enabled);
				}
				ModsRowActivate((void*)(size_t)0);
				break;
			}
			if (gStore.testStage == 5)
			{
				JER_MODSTORE_MODULE after;

				if (jer_modstore_module_at(0, &after))
					jer_log("[modstore][test] after toggle: %s enabled=%d; current menu=%d (mods=%d)\n",
						after.name, after.enabled, jer_frontend_current_menu(),
						jer_frontend_find("jericho.mods"));

				/* Left/Right on a row: the same callback the menu wires up */
				jer_log("[modstore][test] moving row 0 down one\n");
				ModsRowAdjust((void*)(size_t)0, 1);
				gStore.testStage = 6;
				break;
			}
			if (gStore.testStage == 6)
			{
				JER_MODSTORE_MODULE m0, m1;

				gStore.testStage = 7;
				if (jer_modstore_module_at(0, &m0) && jer_modstore_module_at(1, &m1))
					jer_log("[modstore][test] after move: row0=%s row1=%s; current menu=%d (mods=%d)\n",
						m0.name, m1.name, jer_frontend_current_menu(),
						jer_frontend_find("jericho.mods"));
			}
			break;

		case STORE_TEST_RECOMPILE:
			if (gStore.testStage == 0)
			{
				gStore.testStage = 1;
				jer_mods_open_screen();		/* the way the Options button enters */
				break;
			}
			if (gStore.testStage == 1)
			{
				/* one settle tick: the engine switches screens between frames */
				gStore.testStage = 2;
				break;
			}
			if (gStore.testStage == 2)
			{
				gStore.testStage = 3;
				jer_frontend_open(jer_frontend_find("jericho.mods.repo"));
				break;
			}
			if (gStore.testStage == 3)
			{
				gStore.testStage = 4;
				break;
			}
			if (gStore.testStage == 4)
			{
				gStore.testStage = 5;
				/* pretend the install that just finished was compiled-in */
				gCompileAsk = 1;
				jer_log("[modstore][test] leaving the repo browser: current=%d (repo=%d)\n",
					jer_frontend_current_menu(), jer_frontend_find("jericho.mods.repo"));
				jer_mods_open_screen();		/* where Back lands: the Mods screen */
				break;
			}
			if (gStore.testStage == 5)
			{
				/* one settle tick: the engine switches screens between frames */
				gStore.testStage = 6;
				break;
			}
			{
				gStore.testStage = 7;
				jer_log("[modstore][test] recompile prompt after leaving: active=%d question=\"%s\"\n",
					jer_prompt_active(),
					jer_prompt_question() != NULL ? jer_prompt_question() : "");
			}
			break;

		case STORE_TEST_REBUILD:
			{
				gStore.testStage = 7;
				jer_log("[modstore][test] answering the rebuild prompt Yes: handing off and exiting\n");
				jer_compile_request();		/* what the prompt's Yes runs */
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

	if (ver != 0)
	{
		gStore.scriptVersionSeen = ver;
		gStore.scriptMismatch = (ver != JER_MODSTORE_SCRIPT_VERSION);

		if (gStore.scriptMismatch)
		{
			jer_log("[modstore] warning: the helper script speaks v%d, this build expects v%d "
				"- a game update and its script have drifted apart\n", ver, JER_MODSTORE_SCRIPT_VERSION);
		}
	}

	switch (kind)
	{
		case STORE_JOB_INDEX:
			if (gStore.jobRepo >= 0 && gStore.jobRepo < STORE_MAX_REPOS)
				gStore.repoState[gStore.jobRepo] = ok ? 1 : 2;

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

			/* an install can carry newer Driver 1 bake tooling: re-arm the ONE
			 * re-bake prompt so it asks again in this session */
			if (ok)
				jer_rebake_rearm();

			StoreRecordsLoad();
			if (ok)
			{
				STORE_RECORD* rec = (gStore.jobId[0] != 0) ? StoreFindRecord(gStore.jobId) : NULL;

				/* `runtime` empty = a compiled-in mod: the folder is installed,
				 * but the module is in the exe only after a rebuild */
				if (rec != NULL && rec->runtime[0] == 0)
				{
					gCompileAsk = 1;
					jer_log("[modstore] installed compiled-in mod %s: will offer the rebuild on exit\n",
						gStore.jobId);
				}
			}
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
	StoreWriteIniTemplate();	/* only if CONFIG/modstore.ini is not there yet */
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
			gStore.repoState[gStore.repoSelected] = 1;
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

int jer_modstore_startup_update_pending(void)
{
	return gStartupUpdate != 2 || gUpdateAll;
}

const char* jer_modstore_status(void)
{
	if (gStore.status[0] == 0)
		return "Idle";
	return gStore.status;
}

/* Non-zero when the helper script beside the exe speaks another protocol version,
 * which the browser shows as a row rather than leaving in the log. */
int jer_modstore_script_mismatch(int* seen, int* expected)
{
	if (seen != NULL)
		*seen = gStore.scriptVersionSeen;
	if (expected != NULL)
		*expected = JER_MODSTORE_SCRIPT_VERSION;

	return gStore.scriptMismatch;
}

/* Re-read CONFIG/modstore.ini (the browser's "Reload repos"). */
void jer_modstore_repos_reload(void)
{
	StoreReposLoad();
	jer_log("[modstore] repos reloaded: %d configured\n", gStore.repoCount);
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

#define MODS_ROWS_MAX	7	/* table rows a page of the mods list shows */
#define REPO_ROWS_MAX	7	/* same for the repo browser */

static int  gModsPage;
static int  gRepoPage;
static int  gRepoPageRows = REPO_ROWS_MAX;	/* mod rows on a page (the budget) */
static int  gRepoFirstRow;				/* first repo row in the menu */
static int  gRepoCountRows;				/* how many repo rows there are */
static int  gRepoModFirstRow;			/* first MOD row in the menu */
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

	/* Core addons are mandatory: the state is CORE, not OFF/ON. */
	if (mod.core)
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

	/* The core suite's order is fixed (carhacks first); never reorder it from
	 * the list. */
	if (mod.core)
		return 1;

	jer_manager_move(jer_root_dir(), mod.id, dir > 0 ? 1 : -1);
	jer_manager_reload(jer_root_dir());
	StoreAfterReload();
	return 1;
}

static int ModsPages(int count)
{
	int pages = (count + MODS_ROWS_MAX - 1) / MODS_ROWS_MAX;

	return (pages < 1) ? 1 : pages;
}

/* The page row follows the mod rows, so its index is the mod-row count on the
 * new page. The controls follow it; the menu builder uses the same count. */
static int ModsPageRowFor(int page, int count)
{
	int start = page * MODS_ROWS_MAX;
	int rows = count - start;

	if (rows > MODS_ROWS_MAX)
		rows = MODS_ROWS_MAX;
	if (rows < 0)
		rows = 0;

	return rows;
}

static int ModsNextPage(void* ud)
{
	int count = jer_modstore_module_count();
	int pages = ModsPages(count);

	(void)ud;

	gModsPage = (gModsPage + 1) % pages;
	jer_frontend_refresh_at(ModsPageRowFor(gModsPage, count));
	return 1;
}

/* Left/Right on the page row: previous/next, wrapping at the ends. One row
 * carries both directions, so the page controls cost no table space. The
 * refresh lands the cursor on the page row again: the row index moves when a
 * page is shorter, and a plain refresh would snap it to the top row. */
static int ModsPageAdjust(void* ud, int dir)
{
	int count = jer_modstore_module_count();
	int pages = ModsPages(count);

	(void)ud;

	gModsPage = (gModsPage + dir + pages) % pages;
	jer_frontend_refresh_at(ModsPageRowFor(gModsPage, count));
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

/* Cross on a repo row: make it the selected one and fetch it. */
static int RepoPickRepo(void* ud)
{
	int index = (int)(size_t)ud;

	jer_modstore_repo_select(index);
	gRepoPage = 0;
	jer_modstore_refresh();
	jer_frontend_refresh();
	return 1;
}

/* Re-read CONFIG/modstore.ini: the file is the way to add a repo, so the browser
 * has to be able to pick up an edit without restarting the game. */
static int RepoReload(void* ud)
{
	(void)ud;

	jer_modstore_repos_reload();
	gRepoPage = 0;
	jer_frontend_refresh();
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

static int RepoPages(int count)
{
	int rows = (gRepoPageRows > 0) ? gRepoPageRows : REPO_ROWS_MAX;
	int pages = (count + rows - 1) / rows;

	return (pages < 1) ? 1 : pages;
}

/* The page row follows the repo rows and the mod rows: repoN + rows. */
static int RepoPageRowFor(int page, int count, int repoN)
{
	int rows = (gRepoPageRows > 0) ? gRepoPageRows : REPO_ROWS_MAX;
	int start = page * rows;
	int n = count - start;

	if (n > rows)
		n = rows;
	if (n < 0)
		n = 0;

	return repoN + n;
}

static int RepoNextPage(void* ud)
{
	int count = jer_modstore_item_count();
	int repoN = jer_modstore_repo_count();
	int pages = RepoPages(count);

	(void)ud;

	if (repoN < 1)
		repoN = 1;

	gRepoPage = (gRepoPage + 1) % pages;
	jer_frontend_refresh_at(RepoPageRowFor(gRepoPage, count, repoN));
	return 1;
}

/* Left/Right on the repo page row: previous/next, wrapping at the ends. Keep
 * the cursor on the page row: its index moves with the last page's length. */
static int RepoPageAdjust(void* ud, int dir)
{
	int count = jer_modstore_item_count();
	int repoN = jer_modstore_repo_count();
	int pages = RepoPages(count);

	(void)ud;

	if (repoN < 1)
		repoN = 1;

	gRepoPage = (gRepoPage + dir + pages) % pages;
	jer_frontend_refresh_at(RepoPageRowFor(gRepoPage, count, repoN));
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
		snprintf(gLabelMore, sizeof(gLabelMore), "Page %d/%d  (< >)", gModsPage + 1, pages);
		it->label = gLabelMore;
		it->on_activate = ModsNextPage;
		it->on_adjust = ModsPageAdjust;
		it->submenu = -1;
	}

	/* controls, pinned to the bottom edge */
	{
		JER_FE_ITEM* it = &gModsItems[n++];

		int ups = StoreCountUpdates();

		memset(it, 0, sizeof(*it));

		/* "Mod Repo (2 updates)" - the store works without anyone opening the
		 * browser, and a quiet store is a store nobody updates */
		if (ups > 0)
		{
			snprintf(gLabelRepoItem, sizeof(gLabelRepoItem), "Browse Mod Repo (%d update%s)",
				ups, ups == 1 ? "" : "s");
			it->label = gLabelRepoItem;
		}
		else
		{
			it->label = "Browse Mod Repo";
		}

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
	int repoN = jer_modstore_repo_count();
	int pages, start, rows, i, n = 0, controls = 0, budget;

	(void)ud;

	if (repoN < 1)
		repoN = 1;

	/* The row budget: the menu holds JER_FE_MAX_ITEMS, and the repos, the pager
	 * and the controls all come out of it before the mods get a row. With
	 * three repos configured that leaves six mod rows, which is still a page. */
	controls = 3;		/* Refresh, Reload repos, Back */
	budget = JER_FE_MAX_ITEMS - repoN - controls - 1;
	if (budget > REPO_ROWS_MAX)
		budget = REPO_ROWS_MAX;
	if (budget < 1)
		budget = 1;

	pages = (count + budget - 1) / budget;
	if (pages < 1)
		pages = 1;
	if (gRepoPage >= pages)
		gRepoPage = pages - 1;
	if (gRepoPage < 0)
		gRepoPage = 0;

	start = gRepoPage * budget;
	rows = count - start;
	if (rows > budget)
		rows = budget;
	if (rows < 0)
		rows = 0;

	gRepoRows = rows;
	gRepoPageRows = budget;

	/* One row per configured repo, each showing its own index state: the old
	 * single cycling row could not say which repo was stale or unreachable, and
	 * with two repos it hid one of them entirely. */
	gRepoFirstRow = n;
	gRepoCountRows = repoN;

	for (i = 0; i < repoN; i++)
	{
		JER_FE_ITEM* it = &gRepoItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = NULL;			/* we draw the row */
		it->userdata = (void*)(size_t)i;
		it->on_activate = RepoPickRepo;
		it->submenu = -1;
	}

	gRepoModFirstRow = n;

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
		snprintf(gLabelMore, sizeof(gLabelMore), "Page %d/%d  (< >)", gRepoPage + 1, pages);
		it->label = gLabelMore;
		it->on_activate = RepoNextPage;
		it->on_adjust = RepoPageAdjust;
		it->submenu = -1;
	}

	{
		JER_FE_ITEM* it = &gRepoItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = "Refresh";
		it->on_activate = RepoRefresh;
		it->submenu = -1;
	}

	{
		JER_FE_ITEM* it = &gRepoItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = "Reload repos";	/* re-read CONFIG/modstore.ini */
		it->on_activate = RepoReload;
		it->submenu = -1;
	}

	{
		JER_FE_ITEM* it = &gRepoItems[n++];

		memset(it, 0, sizeof(*it));
		it->label = "Back";
		it->is_back = 1;
		it->submenu = -1;
	}

	gRepoMenu.item_count = n;
	/* The controls stay PINNED to the bottom edge: their position must not move
	 * with the mod list. The table is clipped to the band above them; a short
	 * list leaves empty space, and the description/hint is drawn there only when
	 * it fits above the controls. */
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
		gModsMenu.row_top = 130;
		gModsMenu.row_bottom = 470;
		gModsMenu.list_align = 0;	/* hang the rows off the top: a table with a
									 * heading reads wrong centred (mp's reason) */

		memset(&gRepoMenu, 0, sizeof(gRepoMenu));
		gRepoMenu.id = "jericho.mods.repo";
		gRepoMenu.items = gRepoItems;
		gRepoMenu.on_enter = RepoMenuEnter;
		gRepoMenu.title = NULL;
		gRepoMenu.row_w = 560;
		gRepoMenu.row_step = 28;
		gRepoMenu.row_top = 130;
		gRepoMenu.row_bottom = 470;
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

	/* The header stack. The stock menu title is not used (see StoreEnsureMenus): a
	 * tall table is lifted, and the stock title is right where the rows land.
	 *
	 * The offsets are measured, not eyeballed: the frontend font's cell is 36px at
	 * scale 200, so the 180-scale title is 32px tall and the 120-scale cells are
	 * 22px. Each line is placed so its BOTTOM clears the next one's top - the
	 * first version put the title 30px above the column headings, which is less
	 * than its own height, and they overlapped. */
	{
		int yStatus = top - 104;
		int yTitle = top - 74;

		if (yStatus >= 2)
		{
			int seen = 0, expected = 0;

			/* A game update and its helper script can drift apart (a package that
			 * carried one but not the other). That outranks the last status line, so
			 * it takes its place rather than crowding a fourth line in above it. */
			if (jer_modstore_script_mismatch(&seen, &expected))
			{
				char msg[128];

				snprintf(msg, sizeof(msg),
					"helper script v%d, this build expects v%d - reinstall the game files",
					seen, expected);

				jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 2), yStatus, msg, 255, 128, 128);
			}
			else if (jer_modstore_busy())
			{
				jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 2), yStatus, jer_modstore_status(), 255, 220, 120);
			}
			else
			{
				jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 2), yStatus, jer_modstore_status(), 190, 190, 190);
			}
		}

		if (yTitle >= 2)
			jer_text_draw_at(JER_TEXT_FE, 160, StoreTextX(x, w, 2), yTitle, "MODS", 255, 176, 64);
	}

	if (top - 34 >= 2)
	{
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 2), top - 34, "MODULE", 255, 176, 64);
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 50), top - 34, "VERSION", 255, 176, 64);
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 64), top - 34, "STATE", 255, 176, 64);
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 80), top - 34, "SOURCE", 255, 176, 64);
	}

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

		jer_text_draw_at(JER_TEXT_FE, 120, StoreTextX(rx, rw, 2), ry + 3,
			(mod.name != NULL) ? mod.name : mod.id, 255, 255, 255);
		jer_text_draw_at(JER_TEXT_FE, 120, StoreTextX(rx, rw, 50), ry + 3,
			(mod.version != NULL) ? mod.version : "?", 200, 200, 200);

		if (mod.refusal != NULL)
			jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 64), ry + 4, "BLOCKED", 255, 128, 128);
		else if (mod.core)
			jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 64), ry + 4, "CORE", 255, 200, 64);
		else if (mod.enabled)
			jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 64), ry + 4, "ON", 128, 255, 128);
		else
			jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 64), ry + 4, "OFF", 150, 150, 150);

		if (mod.replaced)
			snprintf(buf, sizeof(buf), "store %s (replaced)", (mod.storeVersion != NULL) ? mod.storeVersion : "?");
		else if (mod.fromStore)
			snprintf(buf, sizeof(buf), "store %s", (mod.storeVersion != NULL) ? mod.storeVersion : "");
		else
			snprintf(buf, sizeof(buf), "bundled");

		jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 80), ry + 4, buf, 170, 190, 210);
	}
}

/* How many rows the repo is offering as an update (or as a bundled copy the
 * store replaced), so the Mods menu can say so without the browser being open. */
static int StoreCountUpdates(void)
{
	int i, n = 0;

	for (i = 0; i < jer_modstore_item_count(); i++)
	{
		const JER_MODSTORE_ITEM* it = jer_modstore_item(i);

		if (it == NULL)
			continue;

		if (it->state == JER_MODSTORE_ST_UPDATE || it->state == JER_MODSTORE_ST_REPLACED)
			n++;
	}

	return n;
}

/* An update the store can actually apply. REPLACED is included: the release's copy
 * overwrote what the store installed, so the repo's version is an update, not a
 * reinstall of something newer. */
static int StoreIsUpdateRow(const JER_MODSTORE_ITEM* it)
{
	return it != NULL && it->canInstall &&
		(it->state == JER_MODSTORE_ST_UPDATE || it->state == JER_MODSTORE_ST_REPLACED);
}

static int StoreCountInstallableUpdates(void)
{
	int i, n = 0;

	for (i = 0; i < jer_modstore_item_count(); i++)
		if (StoreIsUpdateRow(jer_modstore_item(i)))
			n++;

	return n;
}

static const char* StoreNextUpdateId(void)
{
	int i;

	for (i = 0; i < jer_modstore_item_count(); i++)
	{
		const JER_MODSTORE_ITEM* it = jer_modstore_item(i);

		if (StoreIsUpdateRow(it))
			return it->id;
	}

	return NULL;
}

/* "Update 3 mods now? carhacks, crumple, bake" - the question is short enough
 * for the prompt box, and names what is about to change. */
static void StoreBuildUpdateQuestion(int n)
{
	char names[110];
	int i, used = 0, listed = 0;

	names[0] = 0;
	for (i = 0; i < jer_modstore_item_count() && listed < 3; i++)
	{
		const JER_MODSTORE_ITEM* it = jer_modstore_item(i);
		int wrote;

		if (!StoreIsUpdateRow(it))
			continue;

		wrote = snprintf(names + used, sizeof(names) - used, "%s%s",
			listed ? ", " : "", it->id);
		if (wrote > 0)
			used += wrote;
		if (used >= (int)sizeof(names))
			used = (int)sizeof(names) - 1;
		listed++;
	}

	if (n > listed)
		snprintf(names + used, sizeof(names) - used, " and %d more", n - listed);

	snprintf(gPromptText, sizeof(gPromptText),
		"Update %d installed mod%s now? %s", n, n == 1 ? "" : "s", names);
}

static void StoreUpdateAllYes(void)
{
	gUpdateAll = 1;
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
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 2), top - 104, status, 255, 220, 120);
	else
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 2), top - 104, status, 190, 190, 190);

	/* A game update and its helper script can drift apart (a package that carried
	 * one but not the other). The store then talks a protocol the script does not
	 * speak, so say it here instead of leaving it in the log. */
	{
		int seen = 0, expected = 0;

		if (jer_modstore_script_mismatch(&seen, &expected))
		{
			char msg[128];

			snprintf(msg, sizeof(msg),
				"helper script v%d, this build expects v%d - reinstall the game files",
				seen, expected);

			jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 2), top - 104, msg, 255, 128, 128);
		}
	}

	/* the same measured stack as the mods table: 180-scale title, then the
	 * 110-scale column line, each clearing the next by a few pixels */
	if (top - 74 >= 2)
		jer_text_draw_at(JER_TEXT_FE, 160, StoreTextX(x, w, 2), top - 74, "MOD REPO", 255, 176, 64);

	if (top - 34 >= 2)
	{
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 2), top - 34, "MOD", 255, 176, 64);
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 34), top - 34, "VERSION", 255, 176, 64);
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 48), top - 34, "STATE", 255, 176, 64);
		jer_text_draw_at(JER_TEXT_FE, 110, StoreTextX(x, w, 64), top - 34, "NOTE", 255, 176, 64);
	}

	/* The highlighted entry's description, under the table but clipped to the
	 * band above the PINNED controls. The controls never move with the list; a
	 * page with no room for the description simply omits it rather than drawing
	 * over Refresh/Reload/Back. */
	{
		int sel = jer_frontend_selected() - gRepoModFirstRow;
		const JER_MODSTORE_ITEM* it = NULL;
		int controlsTop = gRepoMenu.row_bottom - gRepoMenu.bottom_items * gRepoMenu.row_step;
		int lx = 0, ly = 0, lw = 0, lh = 0;

		if (sel >= 0 && sel < gRepoRows)
			it = jer_modstore_item(gRepoPage * gRepoPageRows + sel);

		/* the rect first: the fit test needs its numbers, and asking before the
		 * call meant the guard tested zeros and always passed */
		if (gRepoRows > 0 &&
			jer_frontend_row_rect(gRepoModFirstRow + gRepoRows - 1, &lx, &ly, &lw, &lh) &&
			ly + lh + 8 + 56 <= controlsTop)
		{
			if (it != NULL && it->description != NULL && it->description[0] != 0)
				StoreDrawWrapped(StoreTextX(lx, lw, 2), ly + lh + 8, 470, it->description, 2, 150, 150, 150);
			else
				StoreDrawWrapped(StoreTextX(lx, lw, 2), ly + lh + 8, 470,
					"Select a mod to see its description.", 2, 150, 150, 150);
		}
	}

	if (gRepoRows == 0)
	{
		const char* msg;

		if (jer_modstore_busy())
			msg = "(contacting the repo...)";
		else if (jer_modstore_item_count() == 0 && jer_modstore_status()[0] != 0)
			msg = "(no mods listed - press Refresh)";
		else
			msg = "(no mods listed)";

		jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(x, w, 2), top + 6, msg, 170, 170, 170);
		return;
	}

	/* the configured repos, each with its own index state and age: which one is
	 * stale or unreachable is the first thing worth knowing here, and the old
	 * single cycling row could not show it (nor show more than one repo) */
	for (i = 0; i < gRepoCountRows; i++)
	{
		int rx = 0, ry = 0, rw = 0, rh = 0;
		int selected = (i == jer_modstore_repo_selected());
		const char* state = jer_modstore_repo_state(i);
		const char* age = jer_modstore_repo_age(i);
		char mark[64];

		if (!jer_frontend_row_rect(gRepoFirstRow + i, &rx, &ry, &rw, &rh))
			break;

		snprintf(mark, sizeof(mark), "%s %s", selected ? ">" : " ", jer_modstore_repo_name(i));

		jer_text_draw_at(JER_TEXT_FE, 120, StoreTextX(rx, rw, 2), ry + 3, mark,
			selected ? 255 : 200, selected ? 220 : 200, selected ? 120 : 200);

		jer_text_draw_at(JER_TEXT_FE, 120, StoreTextX(rx, rw, 48), ry + 3, state,
			state[1] == 'e' ? 255 : (state[1] == 'o' ? 170 : 150),
			state[1] == 'e' ? 200 : 200,
			state[1] == 'e' ? 128 : 190);

		jer_text_draw_at(JER_TEXT_FE, 120, StoreTextX(rx, rw, 64), ry + 3, age, 180, 180, 180);
	}

	for (i = 0; i < gRepoRows; i++)
	{
		int rx = 0, ry = 0, rw = 0, rh = 0;
		const JER_MODSTORE_ITEM* it;

		if (!jer_frontend_row_rect(gRepoModFirstRow + i, &rx, &ry, &rw, &rh))
			break;

		it = jer_modstore_item(gRepoPage * gRepoPageRows + i);
		if (it == NULL)
			continue;

		jer_text_draw_at(JER_TEXT_FE, 120, StoreTextX(rx, rw, 2), ry + 3, it->name, 255, 255, 255);
		jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 34), ry + 4, it->version, 200, 200, 200);

		switch (it->state)
		{
			case JER_MODSTORE_ST_INSTALLED:
				jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 48), ry + 4, StoreStateText(it->state), 128, 255, 128);
				break;
			case JER_MODSTORE_ST_UPDATE:
			case JER_MODSTORE_ST_REPLACED:
				jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 48), ry + 4, StoreStateText(it->state), 255, 220, 120);
				break;
			case JER_MODSTORE_ST_NEEDS_CORE:
			case JER_MODSTORE_ST_NEEDS_DEP:
				jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 48), ry + 4, StoreStateText(it->state), 255, 128, 128);
				break;
			default:
				jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 48), ry + 4, StoreStateText(it->state), 190, 190, 190);
				break;
		}

		if (it->note != NULL && it->note[0] != 0)
			jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 64), ry + 4, it->note, 170, 190, 210);
		else
			jer_text_draw_at(JER_TEXT_FE, 130, StoreTextX(rx, rw, 64), ry + 4, it->author, 170, 190, 210);
	}
}

/* Wrap `text` to `width` and draw at most `maxLines` lines from (x, y). Our own
 * wrapper rather than the frontend's: this is module-side text at table scale. */
static void StoreDrawWrapped(int x, int y, int width, const char* text, int maxLines, int r, int g, int b)
{
	char line[192];
	const char* p = text;
	int drawn = 0;

	if (text == NULL)
		return;

	while (p != NULL && *p != 0 && drawn < maxLines)
	{
		const char* cut = p;
		const char* best = p;
		size_t len;

		while (*p == ' ')
			p++;

		for (;;)
		{
			const char* wordEnd = cut;

			while (*wordEnd != 0 && *wordEnd != ' ')
				wordEnd++;

			len = (size_t)(wordEnd - p);
			if (len > sizeof(line) - 1)
				len = sizeof(line) - 1;

			memcpy(line, p, len);
			line[len] = 0;

			if (best != p && jer_text_width(JER_TEXT_FE, 110, line) > width)
				break;

			best = wordEnd;
			cut = wordEnd;

			if (*wordEnd == 0)
				break;

			while (*cut == ' ')
				cut++;
		}

		len = (size_t)(best - p);
		if (len > sizeof(line) - 1)
			len = sizeof(line) - 1;

		memcpy(line, p, len);
		line[len] = 0;

		while (len > 0 && line[len - 1] == ' ')
			line[--len] = 0;

		jer_text_draw_at(JER_TEXT_FE, 110, x, y + drawn * 22, line, r, g, b);

		drawn++;
		p = best;
	}
}

static void StoreDraw(void)
{
	int menu = jer_frontend_current_menu();
	int mods = jer_frontend_find("jericho.mods");
	int repo = jer_frontend_find("jericho.mods.repo");

	if (menu < 0)
		return;

	/* A host prompt owns the screen while it is up: the frontend paints the
	 * question in the engine's own prompt style, and painting our table under it
	 * is what made a prompt look like text dropped on a live menu. */
	if (jer_prompt_active())
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
	out->core = 0;
	{
		int c;

		for (c = 0; c < jer_manager_core_count(); c++)
		{
			const char* coreId = jer_manager_core_id(c);

			if (coreId != NULL && strcmp(coreId, info[index].id) == 0)
			{
				out->core = 1;
				break;
			}
		}
	}
	out->refusal = info[index].refusal;
	out->fromStore = (rec != NULL);
	out->storeVersion = (rec != NULL) ? rec->version : NULL;
	out->replaced = (rec != NULL && info[index].version != NULL && rec->version[0] != 0 &&
		StoreVerCmp(info[index].version, rec->version) != 0);
	return 1;
}

void jer_modstore_frontend_tick(int onMainMenu)
{
	int reopen;

	StoreJobPoll();

	/* the registry is cleared by every module reload, so this is not a one-off */
	StoreEnsureMenus();
	StoreTestTick();

	/* a reload inside an ACTION wiped the registry under our feet -- including our
	 * own menus, which no module re-registers for us. Register again, then put the
	 * menu back in the screen's place; the flag clears only once that worked, or the
	 * player would be left on a screen slot that no longer holds a menu. */
	reopen = gModsReopen;
	if (reopen)
	{
		int idx;

		gModsReopen = 0;
		StoreEnsureMenus();

		idx = jer_frontend_find("jericho.mods");
		if (idx >= 0)
		{
			gModsReopen = 0;
			jer_frontend_open_replace(idx);
			jer_frontend_refresh();
		}
		else
		{
			gModsReopen = 1;	/* try again next frame */
		}
	}

	/* ---- startup mod-update check -------------------------------------
	 *
	 * Ask BEFORE the Driver 1 re-bake prompt, and only when the repo really
	 * offers an update for something installed: an update can carry newer bake
	 * tooling, so re-baking first would use the tools this question is about to
	 * replace. jer_rebake.c waits on jer_modstore_startup_update_pending(), so
	 * the order holds whichever tick FEmain calls first.
	 *
	 * The index is refreshed first: "no update" from a days-old cache is the one
	 * wrong answer. The helper runs in the background, so the menu stays live.
	 */
	if (gUpdateAll && !StoreJobRunning())
	{
		const char* id = StoreNextUpdateId();

		if (id != NULL)
		{
			if (jer_modstore_install(id) != JER_MODSTORE_OK)
				gUpdateAll = 0;		/* the reason is already in status/notify */
		}
		else
		{
			gUpdateAll = 0;
			jer_notify("Mods updated. Restart JERICHO to use them.", 1, 6);
		}
	}

	if (gStartupUpdate != 2 && !StoreJobRunning() && onMainMenu && !jer_screen_active() &&
		!jer_prompt_active() && gStore.testLever == STORE_TEST_NONE)
	{
		if (gStartupUpdate == 0)
		{
			int r = jer_modstore_refresh();

			gStartupUpdate = (r == JER_MODSTORE_OK || r == JER_MODSTORE_BUSY) ? 1 : 2;
		}
		else
		{
			int n = StoreCountInstallableUpdates();

			gStartupUpdate = 2;
			if (n > 0)
			{
				StoreBuildUpdateQuestion(n);
				jer_log("[modstore] startup: %d mod update(s) available\n", n);
				jer_prompt_begin(gPromptText, StoreUpdateAllYes);
			}
		}
	}

	/* ---- the rebuild offer, once the repo browser is behind the player -----
	 *
	 * A compiled-in mod is linked INTO the exe: its folder under MODS/ alone is
	 * not enough, because the module registry was generated by premake at build
	 * time. The store used to leave that to the "Compile Mods" row, which a player
	 * who just downloaded something has no reason to find. Ask here instead, but
	 * not while the browser is still up (the question would cover the table they
	 * are using) and not in the middle of the startup update-all run.
	 */
	if (gCompileAsk != 0 && gUpdateAll == 0 && !StoreJobRunning() &&
		jer_mods_compile_available() && !jer_prompt_active() && !jer_screen_active() &&
		(gStore.testLever == STORE_TEST_NONE || gStore.testLever == STORE_TEST_RECOMPILE))
	{
		int repoMenu = jer_frontend_find("jericho.mods.repo");

		if (repoMenu < 0 || jer_frontend_current_menu() != repoMenu)
		{
			gCompileAsk = 0;
			jer_log("[modstore] left the repo browser after a compiled-in install: asking to recompile\n");
			jer_prompt_begin("Recompile the game to load the new mod? JERICHO closes, rebuilds and restarts.",
				jer_compile_request);
		}
	}

	/* The Driver 1 re-bake question is asked by jer_rebake.c, ONCE, and it waits
	 * for this startup update check to finish. It used to have a twin here, so
	 * answering one prompt still left the other to answer - do not add a second
	 * prompt path. A mod install re-arms the one in jer_rebake.c
	 * (jer_rebake_rearm) so newer bake tooling can still be noticed mid-session. */

	/* after DrawScreen (FEmain.c calls this next to the updater's own tick), which
	 * is where a module's text belongs -- JER_EVENT_FRAME fires before the box. */
	StoreDraw();
}
