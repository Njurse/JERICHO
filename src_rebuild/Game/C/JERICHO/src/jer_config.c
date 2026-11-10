/* ------------------------------------------------------------------
 * jer_config.c — lightweight persistent configuration for JERICHO
 * modules (see jer_config.h for the API).
 *
 * Storage: <root>/CONFIG/<modid>.ini, one key=value pair per line (root
 * is the central JERICHO folder passed to jer_init).
 * Every set() rewrites the whole file for that module, which is fine
 * for the handful of settings a mod keeps (a dozen lines at most).
 *
 * On PSX there is no filesystem, so everything collapses to the
 * default-returning stubs at the bottom of the file.
 * ------------------------------------------------------------------ */

#include "jer_config.h"
#include "jer_internal.h"	/* jerTrim - the shared whitespace trimmer */
#include "jericho.h"		/* jer_log - it is called below and only declared there */

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

/* Value length. 64 was too small: it silently truncated longer values, and
 * because a set rewrites the file from this cache the truncation was written
 * back to disk. A list-like setting (see cainescrossfire's car_list) does not fit in
 * 64 - it came back cut to 63 chars. Anything that needs more than this is
 * better off in a file of its own. */
#define JER_CONFIG_VAL_LEN 256

/* Longest line the loader will read, so a whole "key = value" always arrives
 * in one fgets: a longer line would be split and the tail parsed as garbage. */
#define JER_CONFIG_LINE_LEN (JER_CONFIG_KEY_LEN + JER_CONFIG_VAL_LEN + 8)

typedef struct JER_CONFIG_ENTRY
{
	char mod[JER_CONFIG_MOD_LEN];
	char key[JER_CONFIG_KEY_LEN];
	char val[JER_CONFIG_VAL_LEN];
} JER_CONFIG_ENTRY;

static JER_CONFIG_ENTRY gEntries[JER_CONFIG_MAX_ENTRIES];
static int gEntryCount;
static char gRootDir[512];			/* central JERICHO folder ("" = inert) */
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

/* A value that does not fit is cut, and because the file is rewritten from
 * this cache the cut version replaces it on disk. Say so instead of losing it
 * quietly - that silence is what made a long setting look like it had been
 * parsed as only its first few characters. */
static void jerConfigWarnIfCut(const char* mod, const char* key, const char* val)
{
	if (val != NULL && strlen(val) >= JER_CONFIG_VAL_LEN)
		jer_log("[config] %s: '%s' is %d chars, the store holds %d - use a file\n",
			mod, key, (int)strlen(val), JER_CONFIG_VAL_LEN - 1);
}

/* load <root>/CONFIG/<mod>.ini into the cache (last value wins on dupes) */
static void jerConfigLoadMod(const char* mod)
{
	char path[512];
	FILE* f;
	char line[JER_CONFIG_LINE_LEN];

	if (!gInitDone || gRootDir[0] == 0 || mod == NULL || mod[0] == 0)
		return;

	if (jerConfigModLoaded(mod))
		return;

	snprintf(path, sizeof(path), "%s/%s/%s.ini", gRootDir, JER_CONFIG_PATH, mod);

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
		key = jerTrim(line);
		val = jerTrim(eq + 1);

		/* An EMPTY VALUE is a value, not a missing key: `master_url =` has to mean
		 * "no master" rather than "absent, use the default". Dropping the line made
		 * it impossible to switch a defaulted setting off from the file. */
		if (key[0] == 0)
			continue;

		/* last value wins if the file somehow has duplicate keys */
		{
			int i = jerConfigFind(mod, key);

			jerConfigWarnIfCut(mod, key, val);

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

	if (!gInitDone || gRootDir[0] == 0)
		return;

	snprintf(path, sizeof(path), "%s/%s", gRootDir, JER_CONFIG_PATH);

#if defined(_WIN32)
	_mkdir(path);
#else
	mkdir(path, 0755);
#endif
}

/* rewrite <root>/CONFIG/<mod>.ini from the cache (called on every set) */
static void jerConfigSaveMod(const char* mod)
{
	char path[512];
	FILE* f;
	int i;

	if (!gInitDone || gRootDir[0] == 0 || mod == NULL || mod[0] == 0)
		return;

	jerConfigEnsureDir();
	snprintf(path, sizeof(path), "%s/%s/%s.ini", gRootDir, JER_CONFIG_PATH, mod);

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

int jer_config_init(const char* rootDir)
{
	if (rootDir == NULL || rootDir[0] == 0)
	{
		gRootDir[0] = 0;
		gInitDone = 0;
		return 0;
	}

	snprintf(gRootDir, sizeof(gRootDir), "%s", rootDir);
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

	/* same inert guard as the other API — don't touch disk before init */
	if (!gInitDone || gRootDir[0] == 0 || mod == NULL || mod[0] == 0)
		return;

	/* drop the module's file entirely; the next set_* call regenerates it
	 * from whatever the caller re-seeds — used to flush stale profiles
	 * when defaults change */
	jerConfigDropEntries(mod);

	snprintf(path, sizeof(path), "%s/%s/%s.ini", gRootDir, JER_CONFIG_PATH, mod);

	(void)remove(path);	/* the file may not exist yet */
}

void jer_config_set_str(const char* mod, const char* key, const char* value)
{
	int i;
	JER_CONFIG_ENTRY* e;

	if (mod == NULL || mod[0] == 0 || key == NULL || key[0] == 0)
		return;

	if (value != NULL && strlen(value) >= JER_CONFIG_VAL_LEN)
		jer_log("[config] %s: '%s' is %d chars, the store holds %d - use a file\n",
			mod, key, (int)strlen(value), JER_CONFIG_VAL_LEN - 1);

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

/* One key in <root>/CONFIG/<mod>.ini, changed IN PLACE: every other line of
 * the file -- comments, blank lines, keys this store never read -- is kept
 * byte for byte. jer_config_set_* rewrites the whole file from the cache, which
 * drops the comments a hand-documented file (modstore.ini, rebake.ini) ships
 * with; a player's answer to a prompt must not cost them their notes. The first
 * `key = ...` line is replaced (a later duplicate is left as it was); no such
 * line appends one. The cache is updated too, so get_* reads the new value. */
int jer_config_set_line(const char* mod, const char* key, const char* value)
{
	char path[512];
	static char buf[65536];
	char outLine[JER_CONFIG_LINE_LEN];
	size_t n = 0;
	int done = 0;
	FILE* f;

	if (!gInitDone || gRootDir[0] == 0 || mod == NULL || mod[0] == 0 || key == NULL || key[0] == 0)
		return 0;
	if (value == NULL)
		value = "";

	jerConfigEnsureDir();
	snprintf(path, sizeof(path), "%s/%s/%s.ini", gRootDir, JER_CONFIG_PATH, mod);
	snprintf(outLine, sizeof(outLine), "%s=%s", key, value);

	f = fopen(path, "rb");
	if (f != NULL)
	{
		n = fread(buf, 1, sizeof(buf) - 1, f);
		if (!feof(f))
		{
			/* bigger than we can hold: refuse rather than truncate the file */
			fclose(f);
			jer_log("[config] %s is too big to edit in place; %s not saved\n", path, key);
			return 0;
		}
		fclose(f);
	}
	buf[n] = 0;

	f = fopen(path, "wb");
	if (f == NULL)
		return 0;

	{
		const char* p = buf;

		while (*p != 0)
		{
			const char* eol = strchr(p, '\n');
			size_t len = eol != NULL ? (size_t)(eol - p) + 1 : strlen(p);

			if (!done)
			{
				char line[JER_CONFIG_LINE_LEN];
				size_t cl = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
				char* eq;

				memcpy(line, p, cl);
				line[cl] = 0;
				eq = strchr(line, '=');
				if (eq != NULL && line[0] != '#' && line[0] != ';')
				{
					*eq = 0;
					if (strcmp(jerTrim(line), key) == 0)
					{
						/* keep the line's own ending (CRLF from a Windows editor) */
						int crlf = (len >= 2 && p[len - 2] == '\r');

						fputs(outLine, f);
						fputs(eol != NULL ? (crlf ? "\r\n" : "\n") : "\n", f);
						done = 1;
						p += len;
						continue;
					}
				}
			}

			fwrite(p, 1, len, f);
			p += len;
		}
	}

	if (!done)
	{
		/* a new line in the file's own style (CRLF when it already has one) */
		const char* nl = strstr(buf, "\r\n") != NULL ? "\r\n" : "\n";

		if (n > 0 && buf[n - 1] != '\n')
			fputs(nl, f);
		fputs(outLine, f);
		fputs(nl, f);
	}
	fclose(f);

	/* the cache, without jerConfigSaveMod (that is the whole-file rewrite) */
	jerConfigLoadMod(mod);
	{
		int i = jerConfigFind(mod, key);

		if (i >= 0)
			snprintf(gEntries[i].val, sizeof(gEntries[i].val), "%s", value);
		else if (gEntryCount < JER_CONFIG_MAX_ENTRIES)
		{
			JER_CONFIG_ENTRY* e = &gEntries[gEntryCount];

			snprintf(e->mod, sizeof(e->mod), "%s", mod);
			snprintf(e->key, sizeof(e->key), "%s", key);
			snprintf(e->val, sizeof(e->val), "%s", value);
			gEntryCount++;
		}
	}

	return 1;
}

#else /* PSX: no filesystem — everything returns the default */

int jer_config_init(const char* rootDir)
{
	(void)rootDir;
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

int jer_config_set_line(const char* mod, const char* key, const char* value)
{
	(void)mod;
	(void)key;
	(void)value;
	return 0;
}

#endif /* PSX */
