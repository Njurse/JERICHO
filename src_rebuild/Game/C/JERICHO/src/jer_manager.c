/*
 * jer_manager.c — the module manager: reads/writes mods/modlist.json
 * (a tiny JSON subset) and exposes enable/disable/order operations for the
 * in-game Mods menu and the Release_dev console. Toggling a module is a
 * runtime operation — no rebuild needed; only install/remove requires
 * touching the build (drop the source folder + one premake line).
 */
#include "jer_internal.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Minimal JSON reader (subset sufficient for our fixed shape):        */
/*                                                                     */
/*   { "modules": [ { "id": "crumple", "enabled": true }, ... ] }     */
/* ------------------------------------------------------------------ */

typedef struct JER_JSON
{
	const char* p;
	int error;
} JER_JSON;

static void jerJsonSkipWs(JER_JSON* j)
{
	while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
		j->p++;
}

static int jerJsonExpect(JER_JSON* j, char c)
{
	jerJsonSkipWs(j);

	if (*j->p != c)
	{
		j->error = 1;
		return 0;
	}

	j->p++;
	return 1;
}

static int jerJsonString(JER_JSON* j, char* out, int outSize)
{
	int n = 0;

	jerJsonSkipWs(j);

	if (*j->p != '"')
	{
		j->error = 1;
		return 0;
	}

	j->p++;

	while (*j->p != '"' && *j->p != 0)
	{
		if (*j->p == '\\' && j->p[1] != 0)
			j->p++;

		if (n + 1 < outSize)
			out[n++] = *j->p;

		j->p++;
	}

	if (*j->p != '"')
	{
		j->error = 1;
		return 0;
	}

	j->p++;
	out[n] = 0;
	return 1;
}

static int jerJsonBool(JER_JSON* j, int* out)
{
	jerJsonSkipWs(j);

	if (strncmp(j->p, "true", 4) == 0)
	{
		j->p += 4;
		*out = 1;
		return 1;
	}

	if (strncmp(j->p, "false", 5) == 0)
	{
		j->p += 5;
		*out = 0;
		return 1;
	}

	j->error = 1;
	return 0;
}

int jer_manager_read(const char* modsDir, JER_MODLIST_STATE* st)
{
	char path[512];
	char buf[8192];
	FILE* f;
	long size;
	JER_JSON j;
	int n = 0;

	memset(st, 0, sizeof(*st));

	if (modsDir == NULL || modsDir[0] == 0)
		return -1;

	snprintf(path, sizeof(path), "%s/%s", modsDir, JER_MODLIST_PATH);

	f = fopen(path, "rb");

	if (f == NULL)
		return 0;	/* no file = default state (everything enabled) */

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || size >= (long)sizeof(buf))
	{
		fclose(f);
		return -1;
	}

	if (fread(buf, 1, (size_t)size, f) != (size_t)size)
	{
		fclose(f);
		return -1;
	}

	fclose(f);
	buf[size] = 0;

	j.p = buf;
	j.error = 0;

	if (!jerJsonExpect(&j, '{') || !jerJsonString(&j, buf, sizeof(buf)) || strcmp(buf, "modules") != 0 || !jerJsonExpect(&j, ':'))
		return -1;

	if (!jerJsonExpect(&j, '['))
		return -1;

	jerJsonSkipWs(&j);

	while (*j.p != ']' && !j.error)
	{
		char key[40];
		JER_MODLIST_ITEM* it;

		if (n >= JER_MAX_MODULES)
			break;

		it = &st->items[n];

		if (!jerJsonExpect(&j, '{'))
			break;

		while (!j.error)
		{
			if (!jerJsonString(&j, key, sizeof(key)))
				break;

			if (!jerJsonExpect(&j, ':'))
				break;

			if (strcmp(key, "id") == 0)
			{
				if (!jerJsonString(&j, it->id, sizeof(it->id)))
					break;
			}
			else if (strcmp(key, "enabled") == 0)
			{
				if (!jerJsonBool(&j, &it->enabled))
					break;
			}
			else
			{
				/* unknown key: skip its value */
				char dummy[64];

				if (!jerJsonString(&j, dummy, sizeof(dummy)))
					break;
			}

			jerJsonSkipWs(&j);

			if (*j.p == ',')
			{
				j.p++;
				continue;
			}

			break;
		}

		if (j.error)
			break;

		if (!jerJsonExpect(&j, '}'))
			break;

		/* keep only items that actually have an id */
		if (it->id[0] != 0)
		{
			n++;
			st->count = n;
		}

		jerJsonSkipWs(&j);

		if (*j.p == ',')
		{
			j.p++;
			continue;
		}

		break;
	}

	if (j.error)
		return -1;

	jerJsonExpect(&j, ']');
	jerJsonExpect(&j, '}');

	return 0;
}

int jer_manager_write(const char* modsDir, const JER_MODLIST_STATE* st)
{
	char path[512];
	FILE* f;
	int i;

	if (modsDir == NULL || modsDir[0] == 0)
		return -1;

	snprintf(path, sizeof(path), "%s/%s", modsDir, JER_MODLIST_PATH);

	f = fopen(path, "wb");

	if (f == NULL)
		return -1;

	fprintf(f, "{\n  \"modules\": [\n");

	for (i = 0; i < st->count; i++)
	{
		fprintf(f, "    { \"id\": \"%s\", \"enabled\": %s }%s\n",
			st->items[i].id,
			st->items[i].enabled ? "true" : "false",
			(i + 1 < st->count) ? "," : "");
	}

	fprintf(f, "  ]\n}\n");
	fclose(f);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Public manager API (jericho.h)                                      */
/* ------------------------------------------------------------------ */

int jer_manager_set_enabled(const char* modsDir, const char* id, int enabled)
{
	JER_MODLIST_STATE st;
	int i;

	if (jer_manager_read(modsDir, &st) != 0)
		return -1;

	for (i = 0; i < st.count; i++)
	{
		if (strcmp(st.items[i].id, id) == 0)
		{
			st.items[i].enabled = enabled;
			return jer_manager_write(modsDir, &st);
		}
	}

	if (st.count < JER_MAX_MODULES)
	{
		snprintf(st.items[st.count].id, sizeof(st.items[st.count].id), "%s", id);
		st.items[st.count].enabled = enabled;
		st.count++;
		return jer_manager_write(modsDir, &st);
	}

	return -1;
}

int jer_manager_move(const char* modsDir, const char* id, int direction)
{
	JER_MODLIST_STATE st;
	int i;

	if (jer_manager_read(modsDir, &st) != 0)
		return -1;

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
				return jer_manager_write(modsDir, &st);
			}

			return -1;
		}
	}

	return -1;
}
