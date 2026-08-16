/*
 * jer_manager.c — the module manager: reads/writes the module list
 * (JERICHO/CONFIG/modlist.ini — a tiny flat INI: one "id = 1|0" line per
 * module, file order = load order) and exposes enable/disable/order
 * operations for the in-game Mods menu and the Release_dev console.
 * Toggling a module is a runtime operation — no rebuild needed; only
 * install/remove requires touching the build (drop/remove the folder in
 * JERICHO/MODS + rebuild).
 */
#include "jer_internal.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

/* ------------------------------------------------------------------ */
/* Tiny INI subset sufficient for our fixed shape:                     */
/*                                                                     */
/*   # comment                                                         */
/*   crumple = 1        (1/0; true/false/enabled/disabled/on/off too)  */
/*   d2pl = 0                                                          */
/*                                                                     */
/* Line order is the load order; comments and blanks are ignored.      */
/* ------------------------------------------------------------------ */

static char* jerIniTrim(char* s)
{
	char* end;

	while (*s == ' ' || *s == '\t')
		s++;

	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		*--end = 0;

	return s;
}

/* parse an enabled token: false-ish -> 0, everything else -> 1 */
static int jerIniBool(const char* v)
{
	if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
		strcmp(v, "disabled") == 0 || strcmp(v, "off") == 0)
		return 0;

	return 1;
}

/* make sure JERICHO/CONFIG exists so the manager can write into it */
static void jerManagerEnsureDir(const char* rootDir)
{
	char path[512];

	if (rootDir == NULL || rootDir[0] == 0)
		return;

	snprintf(path, sizeof(path), "%s/%s", rootDir, JER_CONFIG_PATH);

#if defined(_WIN32)
	_mkdir(path);
#else
	mkdir(path, 0755);
#endif
}

int jer_manager_read(const char* rootDir, JER_MODLIST_STATE* st)
{
	char path[512];
	char line[256];
	FILE* f;
	int n = 0;

	memset(st, 0, sizeof(*st));

	if (rootDir == NULL || rootDir[0] == 0)
		return -1;

	snprintf(path, sizeof(path), "%s/%s/%s", rootDir, JER_CONFIG_PATH, JER_MODLIST_PATH);

	f = fopen(path, "rb");

	if (f == NULL)
		return 0;	/* no file = default state (everything enabled) */

	while (fgets(line, sizeof(line), f) != NULL)
	{
		char* eq;
		char* key;
		char* val;
		JER_MODLIST_ITEM* it;

		/* strip comments and blanks */
		{
			char* hash = strchr(line, '#');

			if (hash != NULL)
				*hash = 0;
		}

		key = jerIniTrim(line);

		if (key[0] == 0)
			continue;

		eq = strchr(key, '=');
		if (eq == NULL)
			continue;	/* not a key=value line */

		*eq = 0;
		key = jerIniTrim(key);
		val = jerIniTrim(eq + 1);

		if (key[0] == 0 || val[0] == 0)
			continue;

		if (n >= JER_MAX_MODULES)
			break;

		it = &st->items[n];
		snprintf(it->id, sizeof(it->id), "%s", key);
		it->enabled = jerIniBool(val);
		n++;
	}

	fclose(f);
	st->count = n;

	return 0;
}

int jer_manager_write(const char* rootDir, const JER_MODLIST_STATE* st)
{
	char path[512];
	FILE* f;
	int i;

	if (rootDir == NULL || rootDir[0] == 0)
		return -1;

	jerManagerEnsureDir(rootDir);
	snprintf(path, sizeof(path), "%s/%s/%s", rootDir, JER_CONFIG_PATH, JER_MODLIST_PATH);

	f = fopen(path, "wb");

	if (f == NULL)
		return -1;

	fprintf(f, "# JERICHO module list — load order and enabled state.\n");
	fprintf(f, "# Line order = load order. Values: 1/0 (true/false/enabled/disabled also accepted).\n");

	for (i = 0; i < st->count; i++)
	{
		fprintf(f, "%s = %d\n", st->items[i].id, st->items[i].enabled ? 1 : 0);
	}

	fclose(f);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Public manager API (jericho.h)                                      */
/* ------------------------------------------------------------------ */

int jer_manager_set_enabled(const char* rootDir, const char* id, int enabled)
{
	JER_MODLIST_STATE st;
	int i;

	if (jer_manager_read(rootDir, &st) != 0)
		return -1;

	for (i = 0; i < st.count; i++)
	{
		if (strcmp(st.items[i].id, id) == 0)
		{
			st.items[i].enabled = enabled;
			return jer_manager_write(rootDir, &st);
		}
	}

	if (st.count < JER_MAX_MODULES)
	{
		snprintf(st.items[st.count].id, sizeof(st.items[st.count].id), "%s", id);
		st.items[st.count].enabled = enabled;
		st.count++;
		return jer_manager_write(rootDir, &st);
	}

	return -1;
}

int jer_manager_move(const char* rootDir, const char* id, int direction)
{
	JER_MODLIST_STATE st;
	int i;

	if (jer_manager_read(rootDir, &st) != 0)
		return -1;

	/* unlisted module: append it first (keeps its current state), so a
	 * reorder from the manager works even before the first toggle */
	for (i = 0; i < st.count; i++)
	{
		if (strcmp(st.items[i].id, id) == 0)
			break;
	}

	if (i == st.count)
	{
		if (st.count >= JER_MAX_MODULES)
			return -1;

		snprintf(st.items[st.count].id, sizeof(st.items[st.count].id), "%s", id);
		st.items[st.count].enabled = 1;
		st.count++;
	}

	for (i = 0; i < st.count; i++)
	{
		if (strcmp(st.items[i].id, id) == 0)
		{
			int j = i + direction;

			if (j >= 0 && j < st.count)
			{
				JER_MODLIST_ITEM tmp = st.items[i];
				st.items[i] = st.items[j];
				st.items[j] = tmp;
				return jer_manager_write(rootDir, &st);
			}

			return -1;
		}
	}

	return -1;
}
