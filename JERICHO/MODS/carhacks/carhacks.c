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
#include "mplive.h"

/* Engine globals the hacks touch (exported as C++ data symbols; the mod is
 * compiled C++, so a plain extern matches the export). */
extern int FileExists(char* name);
extern int CarAvailability[CITY_COUNT][10];	/* frontend car list: [level][slot] */
extern char carNumLookup[CITY_COUNT][10];	/* frontend slot -> model number */
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
 *
 * This only rewrites the MODEL MAPPING. Which of those slots is selectable is the
 * availability side's business (ChkFillCarAvailability, below the availability
 * hook) - and that matters for a car_list: it lands in tail slots the stock gates
 * leave dark, so before the module owned the table a configured car was written
 * where nothing would offer it. */
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

/* One line per level saying what the car list will OFFER, because "I cannot pick all the
 * slots" is a question a run has to be able to answer about itself. Hidden slots are named
 * with their reason, so a slot that is missing is explained rather than mysterious.
 *
 * "(N the level already has)" is the part that matters when reading a session: a slot the
 * level already has needs nothing imported, and one it does not have has to be brought in on
 * the pick. It is a report, never a gate - see chkImportCanOffer. In the frontend - before a
 * level exists to have told us its list - it reads 0, which is the truth. */
static void ChkLogCarList(void)
{
	char cities[192];
	char hidden[192];
	int city, slot, first = 1;

	cities[0] = 0;
	hidden[0] = 0;

	for (city = 0; city < CHK_CAR_CITY_COUNT; city++)
	{
		int offered, held;

		/* Fill FIRST, here as well as in the availability pass. This report reads
		 * chkImportCanOffer directly rather than the CarAvailability table, so it
		 * showed "MIAMI 0/10" while the table it was describing was fine - the
		 * report was asking a question nobody had answered yet. Calling the fill
		 * at both sites, rather than only the one I assumed ran, is what makes
		 * the report and the table agree by construction. It is idempotent: a
		 * city that already has a list is left alone. */
		chkFillCarTableFromImport(city);

		offered = chkImportOfferedCount(city);
		held = chkImportOfferedHeldCount(city);
		size_t used = strlen(cities);

		if (used < sizeof(cities) - 40)
			snprintf(cities + used, sizeof(cities) - used, "%s%s %d/%d (%d the level already has)",
				(city > 0) ? ", " : "", chkCityName(city), offered, CHK_CAR_SLOT_COUNT, held);

		for (slot = 0; slot < CHK_CAR_SLOT_COUNT; slot++)
		{
			CHK_OFFER why = chkImportCanOffer(city, slot);

			if (why == CHK_OFFER_OK)
				continue;

			used = strlen(hidden);

			if (used >= sizeof(hidden) - 32)
				continue;

			snprintf(hidden + used, sizeof(hidden) - used, "%s%s[%d]=%s",
				first ? "" : " ", chkCityName(city), slot,
				(why == CHK_OFFER_NO_CAR) ? "empty" : "bad");
			first = 0;
		}
	}

	printInfo("[carhacks] car list: %s\n", cities);

	if (hidden[0] != 0)
		printInfo("[carhacks] car list: hidden slots: %s\n", hidden);
}

/* Write the WHOLE car list table - every city, every slot - from chkImportCanOffer, and tell
 * the engine it owns it (a->own_list, see JER_ARGS_CAR_AVAILABILITY).
 *
 * One pass replaces the stock gates for every reader at once: carhacks' own car-select screen,
 * the mp pause picker and the stock screen's cursor walk all read this one table, and the
 * stock version of it only ever describes the level's own row, four cars deep. */
static void ChkFillCarAvailability(void)
{
	extern int CarAvailability[CITY_COUNT][10];
	char line[192];
	int city, slot;

	/* Before deciding what a city can offer, give the cities that have no frontend
 * list of their own the cars their imported data carries. Without this a Driver 1
 * car-data city offers nothing: the frontend table only ever initialised Driver
 * 2 four rows. It fills only an unclaimed row, so a Driver 2 city - which
 * already has a list of its own - cannot be disturbed. */
	for (city = 0; city < CHK_CAR_CITY_COUNT; city++)
		chkFillCarTableFromImport(city);

for (city = 0; city < CHK_CAR_CITY_COUNT; city++)
	{
		for (slot = 0; slot < CHK_CAR_SLOT_COUNT; slot++)
			CarAvailability[city][slot] = (chkImportCanOffer(city, slot) == CHK_OFFER_OK);
	}

	/* READ BACK what was just written, rather than restating the query: this line is the
	 * evidence that the table - the thing every car list reads - really holds the full
	 * list now, and it is the line to compare against "car list: ... 9/10" above. */
	line[0] = 0;

	for (city = 0; city < CHK_CAR_CITY_COUNT; city++)
	{
		int n = 0;
		size_t used = strlen(line);

		for (slot = 0; slot < CHK_CAR_SLOT_COUNT; slot++)
			if (CarAvailability[city][slot] != 0)
				n++;

		if (used < sizeof(line) - 32)
			snprintf(line + used, sizeof(line) - used, "%s%s %d",
				(city > 0) ? ", " : "", chkCityName(city), n);
	}

	printInfo("[carhacks] car list: table now offers %s of %d slot(s) per city (the engine will not re-gate it)\n",
		line, CHK_CAR_SLOT_COUNT);
}

/* JER_EVENT_CAR_AVAILABILITY: the frontend is building `level`'s car list and
 * asks whether the normally-locked extra vehicles may be offered. */
static int gChkLoggedLevel = -1;
static int gChkListLoggedLevel = -1;

static int ChkOnCarAvailability(void* ud, void* args)
{
	JER_ARGS_CAR_AVAILABILITY* a = (JER_ARGS_CAR_AVAILABILITY*)args;

	(void)ud;

	if (a == NULL)
		return JER_RESULT_CONTINUE;

	if (carhacks_enabled(CHK_HACK_UNLOCK_EXTRA))
	{
		a->result = 1;

		/* and the whole table, not just this level's row: the player should be able to
		 * pick any car the game has, in any of the four cities. a->own_list stops the
		 * engine's stock gates (which run after this hook) from overwriting it. */
		a->own_list = 1;

		if (gChkLoggedLevel != a->level)
		{
			gChkLoggedLevel = a->level;
			printInfo("[carhacks] extra vehicles unlocked for level %d\n", a->level);
		}
	}

	/* The car_list remap and the availability write both work on the same table, and in this
	 * order: the remap names the models, the fill then says which of them can be offered. */
	if (carhacks_enabled(CHK_HACK_CROSS_CITY))
		ChkApplyCarList(a->level);

	/* Claiming the list means the PICKS are ours to serve, so it is only claimed when our own
	 * car-select screen is the one in use.
	 *
	 * Why that matters: this level's row is the one the STOCK screen's cursor walks, and a
	 * stock Select writes wantedCar[0] straight out (FEmain.c:2810) without recording a pick
	 * for this module - there is no event on a car being chosen, and chkImportSetLocalPick has
	 * exactly one caller, our own screen. So on a stock screen an offered car the level cannot
	 * load is a car nothing will import, i.e. the load crash the stock content check exists to
	 * prevent. With our screen in place (the default) every tail slot is ours to serve, so the
	 * whole list can be offered. */
	if (a->own_list && !carhacks_enabled(CHK_HACK_CAR_SELECT))
	{
		a->own_list = 0;

		if (gChkListLoggedLevel != a->level)
		{
			gChkListLoggedLevel = a->level;
			printInfo("[carhacks] car list: the stock car screen is in use, so its gated list is left alone (enable the car select menu to offer every slot)\n");
		}
	}

	if (a->own_list)
		ChkFillCarAvailability();

	/* After the list is applied, so the counts are what the player will actually see. Once
	 * per level, because this hook runs every time the car screen sets up. */
	if (gChkListLoggedLevel != a->level)
	{
		gChkListLoggedLevel = a->level;
		ChkLogCarList();
	}

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

	/* ...and the per-player slot records and deferred releases that described the OLD set
	 * (net.c). The fold below runs inside this hook, and a record left over from the last
	 * level would otherwise be "released" against the new one. */
	chkNetOnLevelReset();

	/* re-arm the spawn lever for THIS level: it places its cars once, and a new
	 * level means a new set to place (spawn.c) */
	chkSpawnReset();

	if (crossCity)
	{
		src = jer_config_get_int("carhacks", "source_city", -1);

		/* A PICK from the car-select menu that names another city is the player's own
		 * decision about where this level reads its car data; source_city is a
		 * launcher/test lever. This value (sourceLevel) is the one that IS level-wide
		 * - which city's LEVELS folder is read; the per-slot cities are separate
		 * (a->modelSource), so the two no longer conflict and the pick simply wins for
		 * the level-wide value. */
		{
			int pickCity = chkImportLocalPickCity();

			if (pickCity >= 0 && pickCity < 4 && pickCity != a->level)
				src = pickCity;
		}

		if (src >= 0 && src < CHK_CITY_COUNT_LIMIT)
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
		 * config stands down; on the host the config IS the authority.
		 *
		 * The player's pick is applied AFTERWARDS and claims only ITS OWN slot, so a
		 * pick from one city and config entries from another coexist. They used to
		 * conflict: a set could name only one foreign city, so whichever claimed it
		 * first refused the other ("this level already reads cars from CHICAGO" for a
		 * level the player had just picked a HAVANA car in). */
		if (crossCity && !chkNetHasAgreedSet())
			chkImportLoadConfig("carhacks", a->count, 0);

		chkImportApplyPick(a->level, a->count);

		/* the session's agreed set, if one arrived (no-op without a session) */
		chkNetApplyAgreedSet();

		/* and what OUR players drive: folding a peer's car in here means the level
		 * reads that vehicle, so a peer is drawn as their own car rather than as
		 * whatever this level happens to hold in the slot they were adopted into.
		 * A peer's car may come from any city -- the set is per-slot, so it adds to
		 * whatever the config and the pick already named. */
		chkNetFoldPeerCars();

		chkImportApplyToCarData(a->count, a->models, a->modelSource);

		chkImportSetEngineModels(NULL, 0);	/* hook-only pointer: drop it */
	}

	/* one line for the log, and the thing a peer will want to compare against
	 * (MP_ADAPTER.md) */
	chkImportDump(a->level);

	/* ...and what a car list will offer for this level. The same question the frontend
	 * hook logs, asked from the level-load path too, because that is the path a headless
	 * session takes: a pair run never opens the car screen, so "which slots can I pick?"
	 * has to be answerable from the run's own log. */
	if (gChkListLoggedLevel != a->level)
	{
		gChkListLoggedLevel = a->level;
		ChkLogCarList();
	}

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

	/* the live "Change car" bridge (mplive.c): answers mp's custom events asking
	 * which cities the session can offer, and asking for one car to be made
	 * available mid-match. Nothing runs without a live mp session. */
	chkMpLiveRegister(ctx);

	/* the "see the imported cars" measurement lever (spawn.c): places one car per
	 * imported city in a line ahead of the player, once per level. Off unless
	 * spawn_imports = 1. */
	chkSpawnRegister(ctx);

	ctx->jer_log(ctx, "[carhacks] %d car hack(s) registered\n", CHK_HACK_COUNT);
}
