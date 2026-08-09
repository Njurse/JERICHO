/* ------------------------------------------------------------------
 * jer_config.c — lightweight persistent configuration for JERICHO
 * modules (see jer_config.h for the API).
 *
 * Storage: <mods>/config/<modid>.ini, one key=value pair per line.
 * Every set() rewrites the whole file for that module, which is fine
 * for the handful of settings a mod keeps (a dozen lines at most).
 *
 * On PSX there is no filesystem, so everything collapses to the
 * default-returning stubs at the bottom of the file.
 * ------------------------------------------------------------------ */

#include "jer_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#ifndef PSX

#define JER_CONFIG_MAX_ENTRIES 192	/* total key/value slots across all mods */
#define JER_CONFIG_MOD_LEN 24
#define JER_CONFIG_KEY_LEN 32
#define JER_CONFIG_VAL_LEN 64

typedef struct JER_CONFIG_ENTRY
{
	char mod[JER_CONFIG_MOD_LEN];
	char key[JER_CONFIG_KEY_LEN];
	char val[JER_CONFIG_VAL_LEN];
} JER_CONFIG_ENTRY;

static JER_CONFIG_ENTRY gEntries[JER_CONFIG_MAX_ENTRIES];
static int gEntryCount;
static char gModsDir[512];			/* mods dir ("" = inert) */
static int gInitDone;

/* helper: does the module have a file in the cache yet? */
static int jerConfigModLoaded(const char* mod)
{
	int i;

	for (i = 0; i < gEntryCount; i++)
	{
		if (strcmp(gEntries[i].mod, mod) == 0)
			return 1;
	}

	return 0;
}

/* helper: drop every cached entry belonging to a module (its file is then
 * regenerated from scratch on the next set_*) */
static void jerConfigDropEntries(const char* mod)
{
	int i;

	i = 0;
	while (i < gEntryCount)
	{
		if (strcmp(gEntries[i].mod, mod) == 0)
		{
			gEntries[i] = gEntries[gEntryCount - 1];
			gEntryCount--;
		}
		else
		{
			i++;
		}
	}
}

/* helper: index of the slot for (mod,key), or -1 */
static int jerConfigFind(const char* mod, const char* key)
{
	int i;

	for (i = 0; i < gEntryCount; i++)
	{
		if (strcmp(gEntries[i].mod, mod) == 0 &&
			strcmp(gEntries[i].key, key) == 0)
			return i;
	}

	return -1;
}

/* helper: trim leading + trailing whitespace in place; returns the new start */
static char* jerConfigTrim(char* s)
{
	char* end;

	while (*s == ' ' || *s == '\t')
		s++;

	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = 0;

	return s;
}

/* load <mods>/config/<mod>.ini into the cache (last value wins on dupes) */
static void jerConfigLoadMod(const char* mod)
{
	char path[512];
	FILE* f;
	char line[192];

	if (!gInitDone || gModsDir[0] == 0 || mod == NULL || mod[0] == 0)
		return;

	if (jerConfigModLoaded(mod))
		return;

	snprintf(path, sizeof(path), "%s/%s/%s.ini", gModsDir, JER_CONFIG_PATH, mod);

	f = fopen(path, "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL)
	{
		char* eq;
		char* val;
		char* key;
		char* nl;

		/* skip blanks and comments */
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == 0)
			continue;

		nl = strchr(line, '\n');
		if (nl != NULL)
			*nl = 0;
		nl = strchr(line, '\r');
		if (nl != NULL)
			*nl = 0;

		eq = strchr(line, '=');
		if (eq == NULL)
			continue;

		*eq = 0;
		key = jerConfigTrim(line);
		val = jerConfigTrim(eq + 1);

		if (key[0] == 0 || val[0] == 0)
			continue;

		/* last value wins if the file somehow has duplicate keys */
		{
			int i = jerConfigFind(mod, key);

			if (i >= 0)
			{
				snprintf(gEntries[i].val, sizeof(gEntries[i].val), "%s", val);
				continue;
			}
		}

		if (gEntryCount >= JER_CONFIG_MAX_ENTRIES)
			break;

		{
			JER_CONFIG_ENTRY* e = &gEntries[gEntryCount];

			snprintf(e->mod, sizeof(e->mod), "%s", mod);
			snprintf(e->key, sizeof(e->key), "%s", key);
			snprintf(e->val, sizeof(e->val), "%s", val);
			gEntryCount++;
		}
	}

	fclose(f);
}

/* make sure the config folder exists so set() can write into it */
static void jerConfigEnsureDir(void)
{
	char path[512];

	if (!gInitDone || gModsDir[0] == 0)
		return;

	snprintf(path, sizeof(path), "%s/%s", gModsDir, JER_CONFIG_PATH);

#if defined(_WIN32)
	_mkdir(path);
#else
	mkdir(path, 0755);
#endif
}

/* rewrite <mods>/config/<mod>.ini from the cache (called on every set) */
static void jerConfigSaveMod(const char* mod)
{
	char path[512];
	FILE* f;
	int i;

	if (!gInitDone || gModsDir[0] == 0 || mod == NULL || mod[0] == 0)
		return;

	jerConfigEnsureDir();
	snprintf(path, sizeof(path), "%s/%s/%s.ini", gModsDir, JER_CONFIG_PATH, mod);

	f = fopen(path, "w");
	if (f == NULL)
		return;

	fprintf(f, "# %s config (JERICHO)\n", mod);
	for (i = 0; i < gEntryCount; i++)
	{
		if (strcmp(gEntries[i].mod, mod) == 0)
			fprintf(f, "%s = %s\n", gEntries[i].key, gEntries[i].val);
	}

	fclose(f);
}

/* public API ------------------------------------------------------ */

int jer_config_init(const char* modsDir)
{
	if (modsDir == NULL || modsDir[0] == 0)
	{
		gModsDir[0] = 0;
		gInitDone = 0;
		return 0;
	}

	snprintf(gModsDir, sizeof(gModsDir), "%s", modsDir);
	gInitDone = 1;
	gEntryCount = 0;	/* a re-init (mods menu reload) re-reads fresh */

	jerConfigEnsureDir();

	return 1;
}

int jer_config_get_int(const char* mod, const char* key, int def)
{
	int i;

	jerConfigLoadMod(mod);
	i = jerConfigFind(mod, key);

	if (i < 0)
		return def;

	return atoi(gEntries[i].val);
}

void jer_config_set_int(const char* mod, const char* key, int value)
{
	char buf[24];

	snprintf(buf, sizeof(buf), "%d", value);
	jer_config_set_str(mod, key, buf);
}

int jer_config_get_bool(const char* mod, const char* key, int def)
{
	return jer_config_get_int(mod, key, def ? 1 : 0) != 0;
}

void jer_config_set_bool(const char* mod, const char* key, int value)
{
	jer_config_set_int(mod, key, value ? 1 : 0);
}

const char* jer_config_get_str(const char* mod, const char* key, const char* def)
{
	static char fallback[JER_CONFIG_VAL_LEN];
	int i;

	jerConfigLoadMod(mod);
	i = jerConfigFind(mod, key);

	if (i < 0)
	{
		snprintf(fallback, sizeof(fallback), "%s", def != NULL ? def : "");
		return fallback;
	}

	return gEntries[i].val;
}

void jer_config_clear(const char* mod)
{
	char path[512];

	/* drop the module's file entirely; the next set_* call regenerates it
	 * from whatever the caller re-seeds — used to flush stale profiles
	 * when defaults change */
	jerConfigDropEntries(mod);

	snprintf(path, sizeof(path), "%s/%s/%s.ini", gModsDir, JER_CONFIG_PATH, mod);

	(void)remove(path);	/* the file may not exist yet */
}

void jer_config_set_str(const char* mod, const char* key, const char* value)
{
	int i;
	JER_CONFIG_ENTRY* e;

	if (mod == NULL || mod[0] == 0 || key == NULL || key[0] == 0)
		return;

	jerConfigLoadMod(mod);
	i = jerConfigFind(mod, key);

	if (i < 0)
	{
		if (gEntryCount >= JER_CONFIG_MAX_ENTRIES)
			return;

		e = &gEntries[gEntryCount];
		snprintf(e->mod, sizeof(e->mod), "%s", mod);
		snprintf(e->key, sizeof(e->key), "%s", key);
		snprintf(e->val, sizeof(e->val), "%s", value != NULL ? value : "");
		gEntryCount++;
	}
	else
	{
		snprintf(gEntries[i].val, sizeof(gEntries[i].val), "%s", value != NULL ? value : "");
	}

	jerConfigSaveMod(mod);
}

#else /* PSX: no filesystem — everything returns the default */

int jer_config_init(const char* modsDir)
{
	(void)modsDir;
	return 0;
}

int jer_config_get_int(const char* mod, const char* key, int def)
{
	(void)mod;
	(void)key;
	return def;
}

void jer_config_set_int(const char* mod, const char* key, int value)
{
	(void)mod;
	(void)key;
	(void)value;
}

int jer_config_get_bool(const char* mod, const char* key, int def)
{
	(void)mod;
	(void)key;
	return def;
}

void jer_config_set_bool(const char* mod, const char* key, int value)
{
	(void)mod;
	(void)key;
	(void)value;
}

const char* jer_config_get_str(const char* mod, const char* key, const char* def)
{
	(void)mod;
	(void)key;
	return def != NULL ? def : "";
}

void jer_config_set_str(const char* mod, const char* key, const char* value)
{
	(void)mod;
	(void)key;
	(void)value;
}

void jer_config_clear(const char* mod)
{
	(void)mod;
}

#endif /* PSX */
