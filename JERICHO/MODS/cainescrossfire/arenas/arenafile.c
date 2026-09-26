// arenas/arenafile.c — the authored ARENA DATA FILE (`.cca`) reader + writer.
//
// One arena per file, in JERICHO/CONFIG/arenas/. A line-based, diffable text
// format so it can be written by hand, by the in-game editor, or by the Python
// top-down editor (tools/arenaedit.py) — all three read and write exactly this.
//
//   # a comment runs to the end of the line
//   arena: my_docks        # the internal name (the file key; also overrides a
//                          # built-in of the same name, e.g. 'chicago')
//   name: Dockyard Duel    # the on-screen name
//   city: CHICAGO          # CHICAGO|HAVANA|VEGAS|RIO, or 0..3
//   mp: 1 0                # <load the small mp map?> <which layout 0|1>
//   region: x0 z0 x1 z1    # the barrier (world units); 'none' = whole level
//   spawn: x z heading     # repeatable; the FIRST is the player, the rest the
//                          # opponents in order. heading is PSX units (0..4095)
//
// Unknown keys are ignored (forward-compatible). A file with no `arena:` line
// does not register.

#include "driver2.h"
#include "cainescrossfire.h"
#include "arenas/profile.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static char* cd2Trim(char* s)
{
	char* e;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;

	e = s + strlen(s);

	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = 0;

	return s;
}

static int cd2Strcasecmp(const char* a, const char* b)
{
	while (*a != 0 && *b != 0)
	{
		int ca = *a, cb = *b;

		if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';

		if (ca != cb)
			return ca - cb;

		a++;
		b++;
	}

	return (unsigned char)*a - (unsigned char)*b;
}

// A city name (or number) -> LevelNames index, or -1.
static int cd2ArenaCityFromName(const char* s)
{
	if (s == NULL || s[0] == 0)
		return -1;

	if (cd2Strcasecmp(s, "chicago") == 0) return 0;
	if (cd2Strcasecmp(s, "havana") == 0)  return 1;
	if (cd2Strcasecmp(s, "vegas") == 0)   return 2;
	if (cd2Strcasecmp(s, "rio") == 0)     return 3;

	return -1;
}

static void cd2ArenaCopyStr(char* dst, int cap, const char* src)
{
	if (cap <= 0)
		return;

	if (src == NULL)
		src = "";

	strncpy(dst, src, cap - 1);
	dst[cap - 1] = 0;
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------
int cd2ArenaFileLoad(const char* path, CD2_ARENA_PROFILE* out)
{
	FILE* fp;
	char line[512];
	int sawName = 0;

	if (path == NULL || out == NULL)
		return 0;

	fp = fopen(path, "rb");

	if (fp == NULL)
		return 0;

	memset(out, 0, sizeof(*out));
	out->region.bounded = 0;

	while (fgets(line, sizeof(line), fp) != NULL)
	{
		char* hash;
		char* colon;
		char* key;
		char* val;

		/* strip a trailing comment (everything after the first '#') */
		hash = strchr(line, '#');
		if (hash != NULL)
			*hash = 0;

		key = cd2Trim(line);

		if (key[0] == 0)
			continue;

		colon = strchr(key, ':');

		if (colon == NULL)
			continue;

		*colon = 0;
		val = cd2Trim(colon + 1);
		key = cd2Trim(key);

		if (cd2Strcasecmp(key, "arena") == 0)
		{
			cd2ArenaCopyStr(out->internalName, CD2_ARENA_NAME_LEN, val);
			sawName = 1;
		}
		else if (cd2Strcasecmp(key, "name") == 0)
		{
			cd2ArenaCopyStr(out->displayName, CD2_ARENA_DISPLAY_LEN, val);
		}
		else if (cd2Strcasecmp(key, "city") == 0)
		{
			int c = cd2ArenaCityFromName(val);

			if (c < 0)
				c = atoi(val);

			if (c >= 0 && c <= 3)
				out->city = c;
		}
		else if (cd2Strcasecmp(key, "mp") == 0)
		{
			int level = 1, arena = 0;

			if (sscanf(val, "%d %d", &level, &arena) >= 1)
			{
				out->mpLevel = level ? 1 : 0;
				out->mpArena = (arena != 0) ? 1 : 0;
			}
			else if (cd2Strcasecmp(val, "full") == 0 || cd2Strcasecmp(val, "city") == 0)
			{
				out->mpLevel = 0;
				out->mpArena = 0;
			}
		}
		else if (cd2Strcasecmp(key, "region") == 0)
		{
			int x0, z0, x1, z1;

			if (cd2Strcasecmp(val, "none") == 0 || cd2Strcasecmp(val, "whole") == 0)
			{
				out->region.bounded = 0;
			}
			else if (sscanf(val, "%d %d %d %d", &x0, &z0, &x1, &z1) == 4)
			{
				if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
				if (z0 > z1) { int t = z0; z0 = z1; z1 = t; }

				out->region.bounded = 1;
				out->region.x0 = x0;
				out->region.z0 = z0;
				out->region.x1 = x1;
				out->region.z1 = z1;
			}
		}
		else if (cd2Strcasecmp(key, "spawn") == 0)
		{
			int x, z, heading = 0;

			if (sscanf(val, "%d %d %d", &x, &z, &heading) >= 2 &&
			    out->spawnCount < CD2_ARENA_MAX_SPAWNS)
			{
				CD2_ARENA_SPAWN* sp = &out->spawns[out->spawnCount++];

				sp->x = x;
				sp->z = z;
				sp->heading = heading & 0xfff;
			}
		}
	}

	fclose(fp);

	/* a display name defaults to the internal one */
	if (out->displayName[0] == 0)
		cd2ArenaCopyStr(out->displayName, CD2_ARENA_DISPLAY_LEN, out->internalName);

	return sawName;
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------
int cd2ArenaFileSave(const char* path, const CD2_ARENA_PROFILE* a)
{
	FILE* fp;
	int i;

	if (path == NULL || a == NULL)
		return 0;

	fp = fopen(path, "wb");

	if (fp == NULL)
		return 0;

	fprintf(fp, "# cainescrossfire arena -- see ARENAS.md\n");
	fprintf(fp, "arena: %s\n", a->internalName);
	fprintf(fp, "name: %s\n", a->displayName);
	fprintf(fp, "city: %d\n", a->city);
	fprintf(fp, "mp: %d %d\n", a->mpLevel ? 1 : 0, a->mpArena ? 1 : 0);

	if (a->region.bounded)
		fprintf(fp, "region: %d %d %d %d\n",
			a->region.x0, a->region.z0, a->region.x1, a->region.z1);
	else
		fprintf(fp, "region: none\n");

	fprintf(fp, "# spawn: x z heading  (first = player, rest = opponents)\n");

	for (i = 0; i < a->spawnCount && i < CD2_ARENA_MAX_SPAWNS; i++)
		fprintf(fp, "spawn: %d %d %d\n",
			a->spawns[i].x, a->spawns[i].z, a->spawns[i].heading);

	fclose(fp);

	return 1;
}
