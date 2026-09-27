/*
 * jer_loader.c — runtime module loader.
 *
 * Scans <root>/MODS for installed modules (a folder counts when it has a
 * mod.toml), loads each compiled binary (<id>.dll on Windows, <id>.so on
 * Linux) and resolves its JER_MODULE_ENTRY symbol (jer_module_<id>_entry),
 * then fills the runtime module table with the metadata from mod.toml. The
 * exe is never rebuilt for mods: drop a folder (and its compiled binary),
 * reload, done.
 *
 * A folder whose binary is missing is still listed (entry = NULL) so the
 * frontend can show "not compiled"; activation skips it.
 *
 * Every platform-specific operation goes through jer_host (jer_host.h), so
 * this file has no platform branches and no host SDK headers — poring this
 * to a new OS is a jer_host.c back-end, not a change here.
 */
#include "jer_internal.h"
#include "jer_host.h"

#include <stdio.h>
#include <string.h>

#define JER_MODTOML "mod.toml"

/* ------------------------------------------------------------------ */
/* Tiny mod.toml subset parser (line-based). Understands:              */
/*   id = "example"                                                    */
/*   name = "Example Module"                                           */
/*   version = "0.1.0"                                                 */
/*   author = "You"                                                    */
/*   description = "what it does"                                      */
/*   default-enabled = false   (true/false; when the key is ABSENT the module  */
/*                              defaults to DISABLED — it must opt in, never   */
/*                              inherit "on"; see jerParseModToml)             */
/*   dependencies = ["a", "b"]   or   dependencies = "a,b"             */
/* ------------------------------------------------------------------ */

static char* jerTrim(char* s)
{
	char* end;

	while (*s == ' ' || *s == '\t')
		s++;

	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		*--end = 0;

	return s;
}

/* copy a quoted ("...") or bare value into out (bounded) */
static void jerTomlValue(char* v, char* out, size_t outSize)
{
	char* q;

	v = jerTrim(v);

	if (v[0] == '"')
	{
		v++;
		q = strchr(v, '"');

		if (q != NULL)
			*q = 0;
	}

	snprintf(out, outSize, "%s", v);
}

/* parse dependencies: "a,b" or ["a","b"] -> "a,b" (comma separated) */
static void jerTomlDeps(char* v, char* out, size_t outSize)
{
	char tmp[512];
	char* p;
	char* dst = out;

	v = jerTrim(v);
	snprintf(tmp, sizeof(tmp), "%s", v);

	/* strip [ ] and quotes */
	p = tmp;
	while (*p)
	{
		if (*p == '[' || *p == ']' || *p == '"' || *p == '\'')
			memmove(p, p + 1, strlen(p));
		else
			p++;
	}

	{
		char* tok = strtok(tmp, ", \t");
		size_t left = outSize;

		out[0] = 0;

		while (tok != NULL)
		{
			size_t len = strlen(tok);

			if (len >= left)
				break;

			if (dst != out)
			{
				*dst++ = ',';
				left--;
			}

			memcpy(dst, tok, len);
			dst += len;
			left -= len;

			tok = strtok(NULL, ", \t");
		}

		*dst = 0;
	}
}

static void jerParseModToml(const char* path, JER_MODULE* m)
{
	FILE* f;
	char line[512];

	/* Fail closed: a module whose mod.toml omits default-enabled is DISABLED
	 * unless modlist.ini explicitly enables it. The old fallback was 1, which
	 * meant a module could be silently ON simply by not mentioning the key —
	 * that is how collisiondevil/cainescrossfire/d2pl ran unannounced. Opt in only. */
	m->defaultEnabled = 0;

	f = fopen(path, "rb");

	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL)
	{
		char* eq;
		char* key;
		char* val;

		{
			char* hash = strchr(line, '#');

			if (hash != NULL)
				*hash = 0;
		}

		key = jerTrim(line);

		if (key[0] == 0)
			continue;

		eq = strchr(key, '=');

		if (eq == NULL)
			continue;

		*eq = 0;
		key = jerTrim(key);
		val = jerTrim(eq + 1);

		if (strcmp(key, "id") == 0)
			jerTomlValue(val, m->id, sizeof(m->id));
		else if (strcmp(key, "name") == 0)
			jerTomlValue(val, m->name, sizeof(m->name));
		else if (strcmp(key, "version") == 0)
			jerTomlValue(val, m->version, sizeof(m->version));
		else if (strcmp(key, "author") == 0)
			jerTomlValue(val, m->author, sizeof(m->author));
		else if (strcmp(key, "description") == 0)
			jerTomlValue(val, m->description, sizeof(m->description));
		else if (strcmp(key, "default-enabled") == 0)
		{
			/* Allow-list, mirroring premake5.lua: only an explicit
			 * true / 1 / enabled turns a module ON; anything else — empty, a
			 * typo, an unwrapped quoted string — leaves it OFF. A malformed
			 * value must never be the reason a module is silently enabled. */
			char norm[16];

			jerTomlValue(val, norm, sizeof(norm));

			if (strcmp(norm, "true") == 0 || strcmp(norm, "1") == 0 || strcmp(norm, "enabled") == 0)
				m->defaultEnabled = 1;
			else
				m->defaultEnabled = 0;
		}
		else if (strcmp(key, "dependencies") == 0)
			jerTomlDeps(val, m->deps, sizeof(m->deps));
		else if (strcmp(key, "runtime") == 0)
		{
			/* runtime = "dll" means a loadable addon; anything else (including
			 * an absent key) is a deep mod, which is compiled into the game. */
			char norm[16];

			jerTomlValue(val, norm, sizeof(norm));
			m->isDllAddon = (strcmp(norm, "dll") == 0);
		}
	}

	fclose(f);

	/* a missing id falls back to the folder name (set by the caller) */
}

/* ------------------------------------------------------------------ */
/* Binary loading                                                      */
/* ------------------------------------------------------------------ */

/* a module id must be a plain C identifier (it is used as a file name and
 * as jer_module_<id>_entry) */
static int jerValidId(const char* id)
{
	const char* p;

	if (id[0] == 0)
		return 0;

	if (!((id[0] >= 'A' && id[0] <= 'Z') || (id[0] >= 'a' && id[0] <= 'z') || id[0] == '_'))
		return 0;

	for (p = id + 1; *p; p++)
	{
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
			(*p >= '0' && *p <= '9') || *p == '_'))
			return 0;
	}

	return 1;
}

/* load <root>/MODS/<id>/<id>.dll|.so|.dylib and resolve the entry symbol.
 * The host lists the plausible file names (jer_host_lib_variant), so this is
 * platform-neutral. Returns 1 on success. */
static int jerLoadBinary(const char* rootDir, const char* id, void** outHandle, JER_MODULE_ENTRY* outEntry)
{
	char path[640];
	char name[128];
	char sym[96];
	void* handle = NULL;
	void* entry = NULL;
	int v;

	snprintf(sym, sizeof(sym), "jer_module_%s_entry", id);

	for (v = 0; jer_host_lib_variant(id, v, name, sizeof(name)); v++)
	{
		snprintf(path, sizeof(path), "%s/MODS/%s/%s", rootDir, id, name);

		handle = jer_host_lib_open(path);

		if (handle == NULL)
			continue;

		entry = jer_host_lib_sym(handle, sym);

		if (entry != NULL)
			break;

		fprintf(stderr, "[jericho] warning: %s loaded but symbol %s missing: %s\n",
			path, sym, jer_host_last_error());

		jer_host_lib_close(handle);
		handle = NULL;
	}

	if (handle == NULL || entry == NULL)
	{
		if (handle != NULL)
			jer_host_lib_close(handle);

		*outHandle = NULL;
		*outEntry = NULL;
		return 0;
	}

	*outHandle = handle;
	*outEntry = (JER_MODULE_ENTRY)entry;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Directory scan                                                      */
/* ------------------------------------------------------------------ */

/* collector for jer_loader_scan: one module per MODS sub-folder */
typedef struct JER_SCAN_CTX
{
	const char* rootDir;
	JER_MODULE* table;
	int max;
	int count;
} JER_SCAN_CTX;

static void jerScanEntry(const char* name, int isDir, void* user)
{
	JER_SCAN_CTX* s = (JER_SCAN_CTX*)user;
	JER_MODULE* m;
	char modtoml[640];
	FILE* t;

	if (!isDir)
		return;
	if (!jerValidId(name))
		return;		/* id must match [A-Za-z_][A-Za-z0-9_]* */
	if (s->count >= s->max)
		return;

	snprintf(modtoml, sizeof(modtoml), "%s/MODS/%s/%s", s->rootDir, name, JER_MODTOML);

	t = fopen(modtoml, "rb");

	if (t == NULL)
		return;		/* not a module folder */

	fclose(t);

	m = &s->table[s->count];
	memset(m, 0, sizeof(*m));
	m->valid = 1;
	snprintf(m->id, sizeof(m->id), "%s", name);
	snprintf(m->name, sizeof(m->name), "%s", name);
	snprintf(m->version, sizeof(m->version), "?");
	jerParseModToml(modtoml, m);

	if (!jerLoadBinary(s->rootDir, m->id, &m->handle, &m->entry))
	{
		m->handle = NULL;
		m->entry = NULL;
	}

	s->count++;
}

int jer_loader_scan(const char* rootDir, JER_MODULE* table, int max)
{
	char modsPath[640];
	JER_SCAN_CTX s;

	if (rootDir == NULL || rootDir[0] == 0 || table == NULL || max <= 0)
		return 0;

	snprintf(modsPath, sizeof(modsPath), "%s/MODS", rootDir);

	s.rootDir = rootDir;
	s.table = table;
	s.max = max;
	s.count = 0;

	if (jer_host_dir_scan(modsPath, jerScanEntry, &s) < 0)
		return 0;

	return s.count;
}

/*
 * jer_deep_mod_list — the installed modules that are compiled INTO the game
 * (mod.toml without runtime = "dll"). Same directory scan as jer_loader_scan
 * but it never loads anything, so it is safe to call before/after activation.
 */
typedef struct JER_DEEP_CTX
{
	const char* rootDir;
	JER_DEEP_MOD* out;
	int max;
	int count;
} JER_DEEP_CTX;

static void jerDeepEntry(const char* name, int isDir, void* user)
{
	JER_DEEP_CTX* s = (JER_DEEP_CTX*)user;
	JER_MODULE m;
	char modtoml[640];
	FILE* t;

	if (!isDir)
		return;
	if (name[0] == '.')
		return;
	if (!jerValidId(name))
		return;
	if (s->count >= s->max)
		return;

	snprintf(modtoml, sizeof(modtoml), "%s/MODS/%s/%s", s->rootDir, name, JER_MODTOML);

	t = fopen(modtoml, "rb");

	if (t == NULL)
		return;		/* not a module folder */

	fclose(t);

	memset(&m, 0, sizeof(m));
	snprintf(m.id, sizeof(m.id), "%s", name);
	snprintf(m.name, sizeof(m.name), "%s", name);
	jerParseModToml(modtoml, &m);

	if (m.isDllAddon)
		return;		/* loadable addon: no rebuild needed */

	snprintf(s->out[s->count].id, sizeof(s->out[s->count].id), "%s", m.id);
	snprintf(s->out[s->count].name, sizeof(s->out[s->count].name), "%s", m.name);
	s->count++;
}

int jer_deep_mod_list(JER_DEEP_MOD* out, int max)
{
	const char* rootDir = jer_root_dir();
	char modsPath[640];
	JER_DEEP_CTX s;

	if (rootDir == NULL || rootDir[0] == 0 || out == NULL || max <= 0)
		return 0;

	snprintf(modsPath, sizeof(modsPath), "%s/MODS", rootDir);

	s.rootDir = rootDir;
	s.out = out;
	s.max = max;
	s.count = 0;

	if (jer_host_dir_scan(modsPath, jerDeepEntry, &s) < 0)
		return 0;

	return s.count;
}

void jer_loader_unload(JER_MODULE* table, int count)
{
	int i;

	if (table == NULL)
		return;

	for (i = 0; i < count; i++)
	{
		if (table[i].handle != NULL)
		{
			jer_host_lib_close(table[i].handle);
			table[i].handle = NULL;
			table[i].entry = NULL;
		}
	}
}
