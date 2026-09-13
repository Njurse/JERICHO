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

/* Which vehicle sits where (measured on this install, so nobody re-derives it):
 *   model  8 (`-car slot6`) = fire truck
 *   model 10 (`-car slot8`) = school bus   (data: LEVELS\CHICAGO\CARMODEL_10_clean.dmodel)
 *   model 11 (`-car slot9`) = the slot the game reserves for a content-override
 *                             truck. NO data ships here, and forcing it CRASHES
 *                             the game in load - never unlock it by hand; the
 *                             engine's own data check keeps it off, which is
 *                             correct.
 * The Chicago SEMI TRUCK itself is a scenery prop (model "LORRY", placed as 7
 * events for Caine's Compound in event.c) - it is not a car model at all, so it
 * cannot be made selectable by an availability hack.
 *
 * The stock frontend only offers the extra vehicles once
 * `gFurthestMission == 40 && NumPlayers == 1`. The engine asks modules via
 * JER_EVENT_CAR_AVAILABILITY while it builds each level's list, and this hack
 * answers "unlock them" so they are offered any time, in 1P or 2P. The engine
 * still applies its per-model data checks, so models with no data (11) stay off.
 */

typedef struct CHK_HACK
{
	const char* name;	/* display name */
	const char* key;	/* config key under the "carhacks" section */
	int         def;	/* enabled by default */
} CHK_HACK;

static const CHK_HACK gChkHacks[] =
{
	{ "Unlock extra vehicles", "unlock_extra_vehicles", 1 },
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

/* JER_EVENT_CAR_AVAILABILITY: the frontend is building `level`'s car list and
 * asks whether the normally-locked extra vehicles may be offered. */
static int gChkLoggedLevel = -1;

static int ChkOnCarAvailability(void* ud, void* args)
{
	JER_ARGS_CAR_AVAILABILITY* a = (JER_ARGS_CAR_AVAILABILITY*)args;

	(void)ud;

	if (carhacks_enabled(0))
	{
		a->result = 1;

		/* one line per level-list build is enough (the frontend rebuilds the
		 * list on entry, so this would otherwise repeat every frame) */
		if (gChkLoggedLevel != a->level)
		{
			gChkLoggedLevel = a->level;
			printInfo("[carhacks] extra vehicles unlocked for level %d\n", a->level);
		}
	}

	return JER_RESULT_CONTINUE;
}

void carhacks_register(JERICHO_CONTEXT* ctx)
{
	int i;

	for (i = 0; i < CHK_HACK_COUNT; i++)
		ctx->jer_log(ctx, "[carhacks] hack '%s' (%s) is %s\n",
			gChkHacks[i].name, gChkHacks[i].key, carhacks_enabled(i) ? "on" : "off");

	ctx->jer_register_hook(ctx, JER_EVENT_CAR_AVAILABILITY, ChkOnCarAvailability, NULL, 0);

	ctx->jer_log(ctx, "[carhacks] %d car hack(s) registered\n", CHK_HACK_COUNT);
}
