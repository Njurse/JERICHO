/* carhacks.c — see carhacks.h. Vehicle-availability + cross-city hacks.
 *
 * Only engine globals (already exported to mods in exports.def) and the public
 * JERICHO API are used here, so this file moves to its own module unchanged.
 */

#include "driver2.h"
#include "system.h"		/* LevelNames[] */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"

#include "carhacks.h"

/* Engine globals the hacks touch (exported as C++ data symbols; the mod is
 * compiled C++, so a plain extern matches the export). */
extern int FileExists(char* name);
extern int CarAvailability[4][10];	/* frontend car list: [level][slot] */
extern char carNumLookup[4][10];	/* frontend slot -> model number */
extern int wantedCar[2];		/* the player's chosen car model per player */

#define CHK_LEVEL_CHICAGO	0

/* Which Chicago vehicle sits where (measured, so nobody re-derives it):
 *   model  8 (`-car slot6`) = fire truck
 *   model 10 (`-car slot8`) = school bus   (LEVELS\CHICAGO\CARMODEL_10_clean.dmodel)
 *   model 11 (`-car slot9`) = the slot reserved for a content-override truck;
 *                             NO data ships here and forcing it CRASHES in load.
 * The Chicago SEMI TRUCK is a scenery prop (model "LORRY", 7 events for Caine's
 * Compound in event.c) - not a car model, so it can't be made selectable.
 */

typedef struct CHK_HACK
{
	const char* name;	/* display name */
	const char* key;	/* config key under the "carhacks" section */
	int         def;	/* enabled by default */
} CHK_HACK;

/* Named rows, so the handlers don't depend on table order. */
#define CHK_HACK_UNLOCK_EXTRA	0
#define CHK_HACK_CROSS_CITY	1

static const CHK_HACK gChkHacks[] =
{
	{ "Unlock extra vehicles", "unlock_extra_vehicles", 1 },
	{ "Cross-city vehicles",   "cross_city_vehicles",   0 },	/* opt-in */
};

#define CHK_HACK_COUNT ((int)(sizeof(gChkHacks) / sizeof(gChkHacks[0])))

int carhacks_count(void)
{
	return CHK_HACK_COUNT;
}

const char* carhacks_name(int index)
{
	return (index >= 0 && index < CHK_HACK_COUNT) ? gChkHacks[index].name : "";
}

int carhacks_enabled(int index)
{
	if (index < 0 || index >= CHK_HACK_COUNT)
		return 0;

	return jer_config_get_int("carhacks", gChkHacks[index].key, gChkHacks[index].def) != 0;
}

/* Offer the extended vehicle pool in the frontend's car-select list. The stock
 * list is ten slots mapped through carNumLookup; `car_list = 8,9,10` replaces
 * the LAST slots with those model numbers, so the extra vehicles can be picked.
 * CarAvailability is left to the unlock query (result = 1 makes the slots
 * selectable); only the model mapping is rewritten here. */
static void ChkApplyCarList(int level)
{
	const char* list = jer_config_get_str("carhacks", "car_list", "");
	int models[10];
	int n = 0, slot, i;
	const char* p;

	if (level < 0 || level > 3 || list == NULL || *list == '\0')
		return;

	for (p = list; *p != '\0' && n < 10; )
	{
		int v = 0;

		while (*p >= '0' && *p <= '9')
		{
			v = v * 10 + (*p - '0');
			p++;
		}

		models[n++] = v;

		while (*p != '\0' && (*p < '0' || *p > '9'))
			p++;
	}

	slot = 10 - n;

	for (i = 0; i < n; i++)
		carNumLookup[level][slot + i] = (char)models[i];

	printInfo("[carhacks] level %d car list: slots %d..%d -> %s\n", level, slot, slot + n - 1, list);
}

/* JER_EVENT_CAR_AVAILABILITY: the frontend is building `level`'s car list and
 * asks whether the normally-locked extra vehicles may be offered. */
static int gChkLoggedLevel = -1;

static int ChkOnCarAvailability(void* ud, void* args)
{
	JER_ARGS_CAR_AVAILABILITY* a = (JER_ARGS_CAR_AVAILABILITY*)args;

	(void)ud;

	if (carhacks_enabled(CHK_HACK_UNLOCK_EXTRA))
	{
		a->result = 1;

		if (gChkLoggedLevel != a->level)
		{
			gChkLoggedLevel = a->level;
			printInfo("[carhacks] extra vehicles unlocked for level %d\n", a->level);
		}
	}

	if (carhacks_enabled(CHK_HACK_CROSS_CITY))
		ChkApplyCarList(a->level);

	return JER_RESULT_CONTINUE;
}

/* Pull imported vehicles out of "import = slot:city:model, ..." and write them
 * into the resident list together with their source city. The engine reads both
 * back after this event: it builds that slot's geometry from the named city's
 * level file and takes its colours from that city's .LCF. Comma-separated, so a
 * level can pull several vehicles from one or more cities.
 *
 * e.g. import = 5:0:10, 6:0:0    two Chicago vehicles into resident slots 5/6 */
static void ChkApplyImports(JER_ARGS_CAR_DATA_SOURCE* a)
{
	const char* list = jer_config_get_str("carhacks", "import", "");
	const char* p = list;
	int n = 0;

	if (list == NULL || *list == '\0' || a->models == NULL || a->modelSource == NULL)
		return;

	while (*p != '\0' && n < 8)
	{
		int vals[3];
		int v;

		/* Skip separators and any spacing between entries. Without this a
		 * "slot:city:model, slot:city:model" list parsed only its first entry:
		 * the next one began with a space, so the number scan found nothing and
		 * the entry was rejected as malformed. */
		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;

		if (*p == '\0')
			break;

		for (v = 0; v < 3; v++)
		{
			int got = 0;

			vals[v] = 0;

			while (*p >= '0' && *p <= '9')
			{
				vals[v] = vals[v] * 10 + (*p - '0');
				p++;
				got = 1;
			}

			if (!got)
			{
				vals[v] = -1;
				break;
			}

			if (v < 2)
			{
				if (*p == ':')
					p++;
				else
				{
					vals[0] = -1;
					break;
				}
			}
		}

		while (*p != '\0' && *p != ',')		/* next entry */
			p++;

		if (*p == ',')
			p++;

		n++;

		if (vals[0] < 0 || vals[0] >= a->count || vals[1] < 0 || vals[1] > 3 || vals[2] < 0 || vals[2] > 12)
		{
			printInfo("[carhacks] import entry %d ignored (want slot:city:model)\n", n);
			continue;
		}

		a->models[vals[0]] = vals[2];
		a->modelSource[vals[0]] = vals[1];

		printInfo("[carhacks] import: slot %d <- model %d from %s\n", vals[0], vals[2], LevelNames[vals[1]]);
	}
}

/* JER_EVENT_CAR_DATA_SOURCE: fires once per level, before any CARMODEL_* file is
 * read. Points the loader at another city's LEVELS folder and writes the
 * "import = slot:city:model" roster, so a level can use vehicles that belong to
 * a different city. Off unless the cross_city_vehicles hack is on.
 *
 * It does NOT choose the player's car. That is the player's decision and it is
 * made on the command line: -car <model|slotN> sets wantedCar, the engine's own
 * pass then spawns the player in whichever resident slot holds that model (see
 * InitPlayer). For an imported body that is the slot the `import` line put it
 * in - e.g. import = 5:3:9 plus -car 9 makes resident slot 5 (RIO model 9) the
 * player's car. Hardcoding a player car here is what this used to do, and it
 * silently overrode the command line. */
static int ChkOnCarDataSource(void* ud, void* args)
{
	JER_ARGS_CAR_DATA_SOURCE* a = (JER_ARGS_CAR_DATA_SOURCE*)args;
	int src;

	(void)ud;

	if (!carhacks_enabled(CHK_HACK_CROSS_CITY))
		return JER_RESULT_CONTINUE;

	src = jer_config_get_int("carhacks", "source_city", -1);

	if (src >= 0 && src < 4)
	{
		a->sourceLevel = src;

		printInfo("[carhacks] cross-city: level %d will read car data from %s\n",
			a->level, LevelNames[src]);
	}

	/* Put a foreign vehicle into the level for real. Ambient traffic picks its
	 * model from resident slots 0..4 (modelRandomList in civ_ai.c), so a model
	 * written into one of those shows up as traffic. */
	if (a->models != NULL)
	{
		int tmodel = jer_config_get_int("carhacks", "traffic_model", -1);
		int tslot = jer_config_get_int("carhacks", "traffic_slot", 2);

		if (tmodel >= 0 && tmodel < 40 && tslot >= 0 && tslot < a->count)
		{
			a->models[tslot] = tmodel;

			printInfo("[carhacks] cross-city: resident slot %d -> model %d (traffic)\n", tslot, tmodel);
		}
	}

	/* Real cross-city imports: geometry AND colours from another city's data */
	ChkApplyImports(a);

	return JER_RESULT_CONTINUE;
}

void carhacks_register(JERICHO_CONTEXT* ctx)
{
	int i;

	for (i = 0; i < CHK_HACK_COUNT; i++)
		ctx->jer_log(ctx, "[carhacks] hack '%s' (%s) is %s\n",
			gChkHacks[i].name, gChkHacks[i].key, carhacks_enabled(i) ? "on" : "off");

	ctx->jer_register_hook(ctx, JER_EVENT_CAR_AVAILABILITY, ChkOnCarAvailability, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DATA_SOURCE, ChkOnCarDataSource, NULL, 0);

	ctx->jer_log(ctx, "[carhacks] %d car hack(s) registered\n", CHK_HACK_COUNT);
}
