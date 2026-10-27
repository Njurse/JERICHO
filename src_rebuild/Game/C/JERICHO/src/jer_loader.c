/*
 * jer_loader.c — runtime module loader.
 *
 * Scans <root>/MODS for installed modules (a folder counts when it has a
 * mod.toml), loads each compiled binary (<id>.dll on Windows, <id>.so on
 * Linux) with LoadLibrary/dlopen, resolves its JER_MODULE_ENTRY symbol
 * (jer_module_<id>_entry) and fills the runtime module table with the
 * metadata from mod.toml. The exe is never rebuilt for mods: drop a folder
 * (and its compiled binary), reload, done.
 *
 * A folder whose binary is missing is still listed (entry = NULL) so the
 * frontend can show "not compiled"; activation skips it.
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
/*   interface = 2                 the SDK version this mod was built against;  */
/*                              an exact mismatch is refused, like the runtime  */
/*                              check (jericho.h: JERICHO_SDK_VERSION)          */
/*   dependencies = ["a", "b"]   or   dependencies = "a,b"             */
/*   requires = [...]          the same thing; the repo index calls it that */
/*   incompatible = ["x", "y"]   or   incompatible = "x,y"   (mods this one */
/*                              must NOT run beside; see jer_system.c)   */
/* ------------------------------------------------------------------ */

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

static int jerValidId(const char* id);

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
		{
			/* the id builds the binary path and the entry symbol, so it gets the
			 * same check as a folder name: a manifest saying id = "../x" must not
			 * open a library outside its folder. An invalid id is ignored and the
			 * folder's (already validated) name stands. */
			char idv[sizeof(m->id)];

			jerTomlValue(val, idv, sizeof(idv));

			if (jerValidId(idv))
				snprintf(m->id, sizeof(m->id), "%s", idv);
		}
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
		else if (strcmp(key, "interface") == 0)
		{
			/* the SDK version this mod was built against. A number, not a range:
			 * the host refuses an exact mismatch at registration (jericho.h says to
			 * bump JERICHO_SDK_VERSION on any change to this header), so the manifest
			 * says the same thing the runtime gate does. Absent or 0 = not declared,
			 * which is how every mod predating this key reads. */
			char norm[16];

			jerTomlValue(val, norm, sizeof(norm));
			m->interfaceVersion = atoi(norm);
		}
		else if (strcmp(key, "host_api") == 0)
		{
			/* the JER_HOST_API generation the addon was built against. 0 is
			 * accepted for manifests older than this key; a newer generation is
			 * refused in jer_system.c before the module entry can call a function
			 * the host does not have. */
			char norm[16];

			jerTomlValue(val, norm, sizeof(norm));
			m->hostApiVersion = atoi(norm);
		}
		/* `requires` is the spelling a REPO INDEX uses for the same idea (see
		 * mods.json.example), so a manifest that copies an index entry reads the way
		 * it was written instead of being silently ignored. */
		else if (strcmp(key, "dependencies") == 0 || strcmp(key, "requires") == 0)
			jerTomlDeps(val, m->deps, sizeof(m->deps));
		else if (strcmp(key, "incompatible") == 0 || strcmp(key, "incompatible-with") == 0)
			jerTomlDeps(val, m->incompat, sizeof(m->incompat));
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

/* The dependency list a COMPILED-IN (deep) module declares in its mod.toml.
 *
 * The generated registry can only carry id/entry/default-enabled, and the loader's
 * own manifest parse below is reached only by the runtime-DLL scan - so a deep
 * module's `dependencies` was read by NOTHING. A module could declare what it needs
 * in its manifest and the check would never see it: every deep module looked
 * dependency-free, which is why the refusal never fired for anything.
 *
 * Reads the manifest for that one field and nothing else (into a scratch module, so
 * none of jerParseModToml's other fields leak into the live table).
 * Returns 1 when a list was found. */
/* Read ONE list field (the dependency list, or the incompatibility list) out of a
 * module's own mod.toml. Shared by jer_loader_read_deps and _read_incompat.
 *
 * Reads that one field and nothing else (into a scratch module, so none of
 * jerParseModToml's other fields leak into the live table). Returns 1 when a list
 * was found. */
static int jerManifestList(const char* rootDir, const char* id, char* out, int max, int incompat)
{
	JER_MODULE scratch;
	char path[512];

	if (rootDir == NULL || id == NULL || out == NULL || max <= 0)
		return 0;

	out[0] = 0;

	memset(&scratch, 0, sizeof(scratch));
	snprintf(path, sizeof(path), "%s/MODS/%s/%s", rootDir, id, JER_MODTOML);

	jerParseModToml(path, &scratch);

	if (incompat)
	{
		if (scratch.incompat[0] == 0)
			return 0;

		snprintf(out, (size_t)max, "%s", scratch.incompat);
	}
	else
	{
		if (scratch.deps[0] == 0)
			return 0;

		snprintf(out, (size_t)max, "%s", scratch.deps);
	}

	return 1;
}

/* Read the manifest's DESCRIPTIVE fields (name, version, author) for one module.
 *
 * The compiled-in half of the module table is built from the game's registry,
 * and the registry carries none of these: a deep mod used to show its folder name
 * as its name and "?" as its version, which left the store with nothing to
 * compare against a repo's entry. Reads into a scratch module, so nothing else in
 * the manifest leaks into the live table. */
int jer_loader_read_meta(const char* rootDir, const char* id,
	char* name, int nameMax, char* version, int versionMax, char* author, int authorMax)
{
	JER_MODULE scratch;
	char path[512];

	if (rootDir == NULL || id == NULL)
		return 0;

	memset(&scratch, 0, sizeof(scratch));
	snprintf(path, sizeof(path), "%s/MODS/%s/%s", rootDir, id, JER_MODTOML);
	jerParseModToml(path, &scratch);

	/* only overwrite with what the manifest actually says: a module with no
	 * mod.toml keeps the id/name the registry gave it */
	if (name != NULL && nameMax > 0 && scratch.name[0] != 0)
		snprintf(name, (size_t)nameMax, "%s", scratch.name);
	if (version != NULL && versionMax > 0 && scratch.version[0] != 0)
		snprintf(version, (size_t)versionMax, "%s", scratch.version);
	if (author != NULL && authorMax > 0 && scratch.author[0] != 0)
		snprintf(author, (size_t)authorMax, "%s", scratch.author);

	return 1;
}

int jer_loader_read_deps(const char* rootDir, const char* id, char* out, int max)
{
	return jerManifestList(rootDir, id, out, max, 0);
}

int jer_loader_read_incompat(const char* rootDir, const char* id, char* out, int max)
{
	return jerManifestList(rootDir, id, out, max, 1);
}

/* ------------------------------------------------------------------ */
/* Binary loading                                                      */
/* ------------------------------------------------------------------ */

/* load <root>/MODS/<id>/<id>.dll(.so) and resolve the entry symbol.
 * Returns 1 on success. */
static int jerValidId(const char* id)
{
	const char* p = id;

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

static int jerLoadBinary(const char* rootDir, const char* id, void** outHandle, JER_MODULE_ENTRY* outEntry)
{
	char path[640];
	char leaf[128];
	char sym[96];
	void* handle = NULL;
	void* entry = NULL;
	int variant;

	snprintf(sym, sizeof(sym), "jer_module_%s_entry", id);

	/* Walk the platform's on-disk names (<id>.dll, <id>.so, lib<id>.so, ...).
	 * The first one that loads and resolves the entry symbol wins. */
	for (variant = 0; jer_host_lib_variant(id, variant, leaf, sizeof(leaf)); variant++)
	{
		snprintf(path, sizeof(path), "%s/MODS/%s/%s", rootDir, id, leaf);

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

typedef struct
{
	const char* rootDir;
	JER_MODULE* table;
	int max;
	int count;
} JER_LOAD_SCAN;

static void jerLoadScanEntry(const char* name, int isDir, void* user)
{
	JER_LOAD_SCAN* s = (JER_LOAD_SCAN*)user;
	JER_MODULE* m;
	char modtoml[640];

	if (!isDir || s->count >= s->max)
		return;
	if (name[0] == '.')
		return;
	if (!jerValidId(name))
		return;	/* id must match [A-Za-z_][A-Za-z0-9_]* */

	snprintf(modtoml, sizeof(modtoml), "%s/MODS/%s/%s", s->rootDir, name, JER_MODTOML);

	if (!jer_host_file_exists(modtoml))
		return;	/* not a module folder */

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
	char mods[640];
	JER_LOAD_SCAN scan;

	if (rootDir == NULL || rootDir[0] == 0 || table == NULL || max <= 0)
		return 0;

	snprintf(mods, sizeof(mods), "%s/MODS", rootDir);

	scan.rootDir = rootDir;
	scan.table = table;
	scan.max = max;
	scan.count = 0;

	jer_host_dir_scan(mods, jerLoadScanEntry, &scan);

	return scan.count;
}

/*
 * jer_deep_mod_list — the installed modules that are compiled INTO the game
 * (mod.toml without runtime = "dll"). Same directory scan as jer_loader_scan
 * but it never loads anything, so it is safe to call before/after activation.
 */
typedef struct
{
	const char* rootDir;
	JER_DEEP_MOD* out;
	int max;
	int count;
} JER_DEEP_SCAN;

static void jerDeepScanEntry(const char* name, int isDir, void* user)
{
	JER_DEEP_SCAN* s = (JER_DEEP_SCAN*)user;
	JER_MODULE m;
	char modtoml[640];

	if (!isDir || s->count >= s->max)
		return;
	if (name[0] == '.')
		return;
	if (!jerValidId(name))
		return;

	snprintf(modtoml, sizeof(modtoml), "%s/MODS/%s/%s", s->rootDir, name, JER_MODTOML);

	if (!jer_host_file_exists(modtoml))
		return;

	memset(&m, 0, sizeof(m));
	snprintf(m.id, sizeof(m.id), "%s", name);
	snprintf(m.name, sizeof(m.name), "%s", name);
	snprintf(m.version, sizeof(m.version), "?");
	jerParseModToml(modtoml, &m);

	if (m.isDllAddon)
		return;	/* loadable addon: no rebuild needed */

	snprintf(s->out[s->count].id, sizeof(s->out[s->count].id), "%s", m.id);
	snprintf(s->out[s->count].name, sizeof(s->out[s->count].name), "%s", m.name);
	s->count++;
}

int jer_deep_mod_list(JER_DEEP_MOD* out, int max)
{
	const char* rootDir = jer_root_dir();
	char mods[640];
	JER_DEEP_SCAN scan;

	if (rootDir == NULL || rootDir[0] == 0 || out == NULL || max <= 0)
		return 0;

	snprintf(mods, sizeof(mods), "%s/MODS", rootDir);

	scan.rootDir = rootDir;
	scan.out = out;
	scan.max = max;
	scan.count = 0;

	jer_host_dir_scan(mods, jerDeepScanEntry, &scan);

	return scan.count;
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
