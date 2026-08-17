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

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__unix__) && !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#else
/* emscripten / android: no runtime loading — the loader is a stub */
#define JER_NO_RUNTIME_LOADING 1
#endif

#define JER_MODTOML "mod.toml"

/* ------------------------------------------------------------------ */
/* Tiny mod.toml subset parser (line-based). Understands:              */
/*   id = "example"                                                    */
/*   name = "Example Module"                                           */
/*   version = "0.1.0"                                                 */
/*   author = "You"                                                    */
/*   description = "what it does"                                      */
/*   default-enabled = false   (true/false; default true when absent)   */
/*   dependencies = ["a", "b"]   or   dependencies = "a,b"             */
/* ------------------------------------------------------------------ */

static char* jerTomlTrim(char* s)
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

	v = jerTomlTrim(v);

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

	v = jerTomlTrim(v);
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
	char v[256];

	m->defaultEnabled = 1;

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

		key = jerTomlTrim(line);

		if (key[0] == 0)
			continue;

		eq = strchr(key, '=');

		if (eq == NULL)
			continue;

		*eq = 0;
		key = jerTomlTrim(key);
		val = jerTomlTrim(eq + 1);

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
			val = jerTomlTrim(val);

			if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0 || strcmp(val, "disabled") == 0)
				m->defaultEnabled = 0;
			else
				m->defaultEnabled = 1;
		}
		else if (strcmp(key, "dependencies") == 0)
			jerTomlDeps(val, m->deps, sizeof(m->deps));
	}

	fclose(f);

	/* a missing id falls back to the folder name (set by the caller) */
	(void)v;
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
	char sym[96];
	void* handle;
	void* entry = NULL;

	(void)snprintf(path, sizeof(path), "%s/MODS/%s/%s%s", rootDir, id, id,
#if defined(_WIN32)
		".dll"
#elif defined(__unix__) && !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
		".so"
#else
		".bin"
#endif
	);

	snprintf(sym, sizeof(sym), "jer_module_%s_entry", id);

#if defined(_WIN32)
	handle = (void*)LoadLibraryA(path);

	if (handle != NULL)
		entry = (void*)GetProcAddress((HMODULE)handle, sym);
#elif defined(JER_NO_RUNTIME_LOADING)
	handle = NULL;
#else
	handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);

	if (handle != NULL)
	{
		entry = dlsym(handle, sym);

		if (entry == NULL)
			fprintf(stderr, "[jericho] warning: %s loaded but symbol %s missing: %s\n", path, sym, dlerror());
	}
#endif

	if (handle == NULL || entry == NULL)
	{
		if (handle != NULL)
		{
#if defined(_WIN32)
			FreeLibrary((HMODULE)handle);
#elif !defined(JER_NO_RUNTIME_LOADING)
			dlclose(handle);
#endif
		}

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

int jer_loader_scan(const char* rootDir, JER_MODULE* table, int max)
{
	int count = 0;

	if (rootDir == NULL || rootDir[0] == 0 || table == NULL || max <= 0)
		return 0;

#if defined(_WIN32)
	{
		char pattern[640];
		WIN32_FIND_DATAA fd;
		HANDLE hFind;

		snprintf(pattern, sizeof(pattern), "%s/MODS/*", rootDir);
		hFind = FindFirstFileA(pattern, &fd);

		if (hFind == INVALID_HANDLE_VALUE)
			return 0;

		do
		{
			JER_MODULE* m;
			char modtoml[640];

			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				continue;
			if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
				continue;
			if (!jerValidId(fd.cFileName))
				continue;	/* id must match [A-Za-z_][A-Za-z0-9_]* */
			if (count >= max)
				break;

			snprintf(modtoml, sizeof(modtoml), "%s/MODS/%s/%s", rootDir, fd.cFileName, JER_MODTOML);
			{
				FILE* t = fopen(modtoml, "rb");

				if (t == NULL)
					continue;

				fclose(t);
			}

			m = &table[count];
			memset(m, 0, sizeof(*m));
			m->valid = 1;
			snprintf(m->id, sizeof(m->id), "%s", fd.cFileName);
			snprintf(m->name, sizeof(m->name), "%s", fd.cFileName);
			snprintf(m->version, sizeof(m->version), "?");
			jerParseModToml(modtoml, m);

			if (!jerLoadBinary(rootDir, m->id, &m->handle, &m->entry))
			{
				m->handle = NULL;
				m->entry = NULL;
			}

			count++;
		} while (FindNextFileA(hFind, &fd) != 0);

		FindClose(hFind);
	}
#elif defined(JER_NO_RUNTIME_LOADING)
	(void)rootDir;	/* stub: no runtime loading on this platform */
#else
	{
		char dirPath[640];
		DIR* d;
		struct dirent* de;

		snprintf(dirPath, sizeof(dirPath), "%s/MODS", rootDir);
		d = opendir(dirPath);

		if (d == NULL)
			return 0;

		while ((de = readdir(d)) != NULL)
		{
			JER_MODULE* m;
			char modtoml[640];
			struct stat st;

			if (de->d_name[0] == '.')
				continue;
			if (!jerValidId(de->d_name))
				continue;	/* id must match [A-Za-z_][A-Za-z0-9_]* */

			snprintf(modtoml, sizeof(modtoml), "%s/MODS/%s/%s", rootDir, de->d_name, JER_MODTOML);

			if (stat(modtoml, &st) != 0)
				continue;	/* not a module folder */

			if (count >= max)
				break;

			m = &table[count];
			memset(m, 0, sizeof(*m));
			m->valid = 1;
			snprintf(m->id, sizeof(m->id), "%s", de->d_name);
			snprintf(m->name, sizeof(m->name), "%s", de->d_name);
			snprintf(m->version, sizeof(m->version), "?");
			jerParseModToml(modtoml, m);

			if (!jerLoadBinary(rootDir, m->id, &m->handle, &m->entry))
			{
				m->handle = NULL;
				m->entry = NULL;
			}

			count++;
		}

		closedir(d);
	}
#endif

	return count;
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
#if defined(_WIN32)
			FreeLibrary((HMODULE)table[i].handle);
#elif !defined(JER_NO_RUNTIME_LOADING)
			dlclose(table[i].handle);
#endif
			table[i].handle = NULL;
			table[i].entry = NULL;
		}
	}
}
