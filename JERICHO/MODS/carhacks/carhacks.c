/* carhacks.c — see carhacks.h. Vehicle-availability + cross-city hacks.
 *
 * Only engine globals and the public JERICHO API are used here, so this unit
 * stays host-agnostic (it was lifted out of Caine's Crossfire into this module;
 * see carhacks.h).
 *
 * The cross-city IMPORT SET itself lives in carimport.c; this file owns the hack
 * table and the two engine hooks that drive it.
 */

#include "driver2.h"
#include "system.h"		/* LevelNames[] */
#include "mission.h"		/* GameLevel, residentCarModels[] */
#include "cars.h"		/* MAX_CAR_RESIDENT_MODELS */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"

#include "carhacks.h"
#include "carid.h"
#include "carimport.h"
#include "net.h"

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

/* Named rows, so no handler depends on table order (CHK_HACK_* live in
 * carhacks.h so sibling sources - carselect.c - can name them too). */

static const CHK_HACK gChkHacks[] =
{
	{ "Unlock extra vehicles", "unlock_extra_vehicles", 1 },
	{ "Cross-city vehicles",   "cross_city_vehicles",   0 },	/* opt-in */
	{ "Car select menu",       "car_select_menu",      1 },
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

	if (a == NULL)
		return JER_RESULT_CONTINUE;

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

	/* The car-select menu (carselect.c) takes the stock car screen's place. The
	 * screen has only STARTED its setup here - this hook runs before the code
	 * that turns unlockExtra into CarAvailability - so the menu is only armed
	 * and opens on the next frame, by which time that setup has finished. */
	if (carhacks_enabled(CHK_HACK_CAR_SELECT))
		chkCarSelectArm();

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_CAR_DATA_SOURCE: fires once per level, before any CARMODEL_* file is
 * read. It builds this level's IMPORT SET (carimport.c) and writes it into the
 * engine's arrays: modelSource[slot] names the city a resident slot's
 * CARMODEL_<n> is read from, so a level can use vehicles that belong to a
 * different city.
 *
 * The set has two sources, applied in this order:
 *   1. the [carhacks] config (`import = slot:city:model, ...`, traffic_model /
 *      traffic_slot) - the fallback, and what the launchers and chk_suite.sh
 *      drive. Off unless cross_city_vehicles is on, exactly as it was.
 *   2. the player's PICK from the car-select menu. That one is an explicit
 *      choice of ONE car, so it imports even when the config-driven hack is off.
 *
 * The player's car is still chosen the normal way: wantedCar[] (set by the menu's
 * Ride, or by -car) is what InitPlayer matches against the resident list. This
 * event only decides which city each slot's DATA comes from. */
static int ChkOnCarDataSource(void* ud, void* args)
{
	JER_ARGS_CAR_DATA_SOURCE* a = (JER_ARGS_CAR_DATA_SOURCE*)args;
	int crossCity = carhacks_enabled(CHK_HACK_CROSS_CITY);
	int src;

	(void)ud;

	if (a == NULL)
		return JER_RESULT_CONTINUE;

	/* a fresh set for this level (the player's pick survives the reset: it was
	 * made in the frontend and is consumed by the level it starts) */
	chkImportReset();

	/* re-arm the spawn lever for THIS level: it places its cars once, and a new
	 * level means a new set to place (spawn.c) */
	chkSpawnReset();

	if (crossCity)
	{
		src = jer_config_get_int("carhacks", "source_city", -1);

		if (src >= 0 && src < 4)
		{
			a->sourceLevel = src;

			printInfo("[carhacks] cross-city: level %d will read car data from %s\n",
				a->level, LevelNames[src]);
		}
	}

	if (a->models != NULL && a->modelSource != NULL)
	{
		/* Let the set see which resident slots the ENGINE already holds a model
		 * in, so its slot choosers skip one another module claimed. mp answers
		 * this SAME event at priority 0 -- i.e. before us -- and writes its spare
		 * player slots 5 and 6, which chkImportSlotFree then avoids. */
		chkImportSetEngineModels(a->models, a->count);

		/* The local config is this machine's fallback. On a CLIENT that has already
		 * received the session's agreed set the HOST is authoritative, so the
		 * config stands down; on the host the config IS the authority. */
		if (crossCity && !chkNetHasAgreedSet())
			chkImportLoadConfig("carhacks", a->count);

		chkImportApplyPick(a->level, a->count);

		/* the session's agreed set, if one arrived (no-op without a session) */
		chkNetApplyAgreedSet();

		/* and what OUR players drive: folding a peer's car in here means the level
		 * reads that vehicle, so a peer is drawn as their own car rather than as
		 * whatever this level happens to hold in the slot they were adopted into.
		 * A car from a second foreign city is refused by the one-guest-city rule
		 * (loudly), which is the engine's limit, not a choice made here. */
		chkNetFoldPeerCars();

		chkImportApplyToCarData(a->count, a->models, a->modelSource);

		chkImportSetEngineModels(NULL, 0);	/* hook-only pointer: drop it */
	}

	/* one line for the log, and the thing a peer will want to compare against
	 * (MP_ADAPTER.md) */
	chkImportDump(a->level);

	/* the set is real now: if this machine is hosting, tell the session, so the
	 * clients (still in the menus) load the same cars (net.c) */
	chkNetNotifySetBuilt();

	/* the pick is spent: a later level must not import the same car again */
	chkImportClearPick();

	return JER_RESULT_CONTINUE;
}

/* ---------------------------------------------------------------------------
 * How a peer's car is drawn here
 * ------------------------------------------------------------------------- */

/* JER_EVENT_CAR_PEER_DRAW (fired by mp): a remote player's car is about to be
 * drawn with the data its owner sent.
 *
 * mp's wire carries a resident SLOT, not a (city, model), so the car this machine
 * draws for a peer may be a completely different vehicle - and then the owner's
 * palette means nothing here, because the civ_clut rows a car samples are built
 * per city (cars.c). carhacks knows every player's (city, model) [net.c], so it is
 * the only thing that can tell "this IS their car" from "this only looks like it".
 *
 * A mismatch is answered with palette 0 - the base colours every resident model
 * always has - so a car that is not the owner's is never painted with colours
 * that belong to a vehicle that is not here. A car that IS the owner's (the
 * ordinary case, and the only one where their palette is meaningful) is left
 * alone, so mp's owner-authoritative colour still works.
 *
 * This is the never-garbled FALLBACK, not the fix: where the peer's real vehicle
 * can be loaded locally the machine should do that instead (MP_ADAPTER.md's
 * hotload step), and then this hook stops correcting that player. */

static signed char gChkPeerDrawn[CHK_NET_MAX_PLAYERS];	/* -1 unknown, 0 not theirs, 1 theirs */
static CHK_CAR_ID gChkPeerLast[CHK_NET_MAX_PLAYERS];	/* what the last line was about */
static signed char gChkPeerLastSet[CHK_NET_MAX_PLAYERS];

static int ChkOnCarPeerDraw(void* ud, void* args)
{
	JER_ARGS_CAR_PEER_DRAW* a = (JER_ARGS_CAR_PEER_DRAW*)args;
	CHK_CAR_ID peer;
	int slot, drawnModel, drawnCity, peerCity, theirs, relog;

	(void)ud;

	if (a == NULL || a->player < 0 || a->player >= CHK_NET_MAX_PLAYERS)
		return JER_RESULT_CONTINUE;

	/* with no reported identity there is nothing to check against: leave mp's
	 * owner-authoritative palette alone rather than guess */
	if (!chkNetPeerCar(a->player, &peer))
		return JER_RESULT_CONTINUE;

	/* a->model is the resident SLOT mp adopted, not a model number - resolve it
	 * before comparing anything against the owner's reported model */
	slot = a->model;
	drawnModel = (slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS) ? residentCarModels[slot] : -1;
	drawnCity = (a->sourceCity >= 0) ? a->sourceCity : GameLevel;
	peerCity = chkCarIdCity(peer);

	theirs = (drawnModel >= 0) && (drawnModel == chkCarIdModel(peer)) &&
		(peerCity < 0 || peerCity == drawnCity);

	relog = (gChkPeerDrawn[a->player] != (signed char)theirs) ||
		!gChkPeerLastSet[a->player] || !chkCarIdEqual(gChkPeerLast[a->player], peer);

	if (relog)
	{
		gChkPeerDrawn[a->player] = (signed char)theirs;
		gChkPeerLast[a->player] = peer;
		gChkPeerLastSet[a->player] = 1;

		if (theirs)
			printInfo("[carhacks/net] peer %d drives %s model %d and this machine draws exactly that "
				"(slot %d) - their own colours\n",
				a->player, chkNetCityName(drawnCity), drawnModel, slot);
		else
			printInfo("[carhacks/net] peer %d drives %s model %d, but this machine draws %s model %d in slot %d"
				" - using that car's own colours (theirs is not loaded here)\n",
				a->player, chkNetCityName(peerCity), (int)chkCarIdModel(peer),
				chkNetCityName(drawnCity), drawnModel, slot);
	}

	if (!theirs && a->paletteIn != 0)
	{
		a->paletteOut = 0;
		a->handled = 1;
	}

	return JER_RESULT_CONTINUE;
}

void carhacks_register(JERICHO_CONTEXT* ctx)
{
	int i;

	for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
	{
		gChkPeerDrawn[i] = -1;
		gChkPeerLastSet[i] = 0;
	}

	for (i = 0; i < CHK_HACK_COUNT; i++)
		ctx->jer_log(ctx, "[carhacks] hack '%s' (%s) is %s\n",
			gChkHacks[i].name, gChkHacks[i].key, carhacks_enabled(i) ? "on" : "off");

	ctx->jer_register_hook(ctx, JER_EVENT_CAR_AVAILABILITY, ChkOnCarAvailability, NULL, 0);
	/* Priority 10: run AFTER mp (which answers this at 0 and writes its spare
	 * player slots 5/6). A later handler still runs before the engine consumes
	 * the value (ProcessCarModelLump follows the event), so carhacks gets to see
	 * mp's claims and skip them instead of overwriting them. */
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DATA_SOURCE, ChkOnCarDataSource, NULL, 10);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_PEER_DRAW, ChkOnCarPeerDraw, NULL, 0);

	/* the car-select menu (carselect.c): a JERICHO frontend menu that replaces
	 * the stock Take-a-Ride car screen so it can carry the city-roster row */
	if (carhacks_enabled(CHK_HACK_CAR_SELECT))
		chkCarSelectRegister(ctx);

	/* the multiplayer adapter (net.c): carhacks' own channel over the JERICHO
	 * addon net bridge, so a session can agree which city each machine reads its
	 * car data from. Every call inside is a no-op with no session. */
	chkNetRegister(ctx);

	/* the "see the imported cars" measurement lever (spawn.c): places one car per
	 * imported city in a line ahead of the player, once per level. Off unless
	 * spawn_imports = 1. */
	chkSpawnRegister(ctx);

	ctx->jer_log(ctx, "[carhacks] %d car hack(s) registered\n", CHK_HACK_COUNT);
}
