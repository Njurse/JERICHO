/* carhacks.c — see carhacks.h. Vehicle-availability hacks for Driver 2.
 *
 * Only engine globals (already exported to mods in exports.def) and the public
 * JERICHO API are used here, so this file moves to its own module unchanged.
 */

#include "driver2.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"

#include "carhacks.h"

/* Engine globals the hacks touch (exported as C++ data symbols; the mod is
 * compiled C++, so a plain extern matches the export). */
extern int FileExists(char* name);		/* engine filesystem check */
extern int CarAvailability[4][10];		/* frontend car list: [level][slot] */

#define CHK_LEVEL_CHICAGO	0

/* The Chicago truck. IMPORTANT, measured on this install:
 *   - the extra vehicle is car MODEL 10 (frontend list index `carNumLookup
 *     [chicago][7]`, i.e. `-car slot8`). Its content-override data ships as
 *     LEVELS\CHICAGO\CARMODEL_10_clean.dmodel and it loads + drives fine.
 *   - model 11 (`-car slot9`, the one FEmain's `i == 8` line gates) has NO data
 *     on this install: forcing it crashes the game during load. So this hack
 *     targets 10 and must never unlock 11.
 * In the frontend the truck is only offered once gFurthestMission == 40 (and
 * NumPlayers == 1); the whole point here is to offer it any time.
 *
 * Caveats for extending this: the extra car reaches the level through the
 * special resident slot (SPECIAL_CAR_SLOT / MAX_CAR_RESIDENT_MODELS), and
 * InitSpecSpool switches special spooling OFF for mission 7 ("Caine's Compound
 * semi trucks") and whenever residentCarModels[SPECIAL_CAR_SLOT] < 8 - so a
 * level where spooling is off needs another route to make a car resident. */
#define CHK_CHICAGO_TRUCK_SLOT	7	/* CarAvailability[chicago][7] = model 10 */

typedef struct CHK_HACK
{
	const char* name;	/* display name */
	const char* key;	/* config key under the "carhacks" section */
	int         def;	/* enabled by default */
	int         level;	/* GameLevel it applies to */
	int         slot;	/* index into CarAvailability[level][slot] */
} CHK_HACK;

static const CHK_HACK gChkHacks[] =
{
	{ "Chicago truck", "chicago_truck", 1, CHK_LEVEL_CHICAGO, CHK_CHICAGO_TRUCK_SLOT },
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

/* Offer every enabled hack's vehicle for `level`, so it is a normal entry in
 * that level's vehicle list. Idempotent; re-asserted per level launch because
 * the frontend recomputes CarAvailability when it builds the select screen. */
static void ChkApplyForLevel(int level)
{
	int i;

	if (level < 0 || level >= 4)
		return;

	for (i = 0; i < CHK_HACK_COUNT; i++)
	{
		const CHK_HACK* h = &gChkHacks[i];

		if (h->level != level || !carhacks_enabled(i))
			continue;

		if (CarAvailability[level][h->slot] == 0)
		{
			CarAvailability[level][h->slot] = 1;

			printInfo("[carhacks] %s: enabled slot %d for level %d\n",
				h->name, h->slot, level);
		}
	}
}

static int ChkOnBoot(void* ud, void* args)
{
	(void)ud;
	(void)args;

	ChkApplyForLevel(CHK_LEVEL_CHICAGO);
	return JER_RESULT_CONTINUE;
}

/* Fired at the end of State_GameStart, before the level loads, with the
 * pending level in/out. */
static int ChkOnLevelLaunch(void* ud, void* args)
{
	JER_ARGS_LEVEL_LAUNCH* a = (JER_ARGS_LEVEL_LAUNCH*)args;

	(void)ud;

	ChkApplyForLevel(a->gameLevel);
	return JER_RESULT_CONTINUE;
}

void carhacks_register(JERICHO_CONTEXT* ctx)
{
	int i;

	for (i = 0; i < CHK_HACK_COUNT; i++)
		ctx->jer_log(ctx, "[carhacks] hack '%s' (%s, level %d, slot %d) is %s\n",
			gChkHacks[i].name, gChkHacks[i].key, gChkHacks[i].level, gChkHacks[i].slot,
			carhacks_enabled(i) ? "on" : "off");

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, ChkOnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_LEVEL_LAUNCH, ChkOnLevelLaunch, NULL, 0);

	ctx->jer_log(ctx, "[carhacks] %d car hack(s) registered\n", CHK_HACK_COUNT);
}
