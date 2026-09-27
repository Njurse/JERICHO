// select/select.c — the CC select flow, built on the NATIVE frontend menus
// (JERICHO/include/jer_frontend.h), so it looks and navigates like the game's
// own screens instead of a floating overlay.
//
//   cc.arena          every ARENA in the registry (arenas/), then Back
//   cc.veh.<city>     < VEHICLE > - a carousel over that arena's city's own
//                     cars, with the car's frontend icon, left/right to change,
//                     Cross to the opponent screen
//   cc.opponents      < N OPPONENTS > - 0..CD2_AI_MAX, Cross starts the match
//
// The arena list is the REGISTRY, not a hardcoded four: every built-in city is
// there, and any authored arena file under JERICHO/MODS/cainescrossfire/arenas/ adds another
// (optionally on the SAME city - see ARENAS.md). Selecting an arena loads its
// city and its mission layout and makes it the match's current arena, so its
// authored spawn points and its barrier apply.
//
// Restricting the vehicle list to the arena's own city is deliberate: the engine
// imports from only ONE foreign city per level (models.c, InitCarImport), so a
// car belongs to the city it is picked in - that is also what makes its
// geometry load from its own city.
//
// Raised by -ccmenu (or CC_MENU): the flow opens cc.arena, and the main menu's
// Deathmatch entry (the replaced Undercover button) opens it too.

#include "driver2.h"
#include "dr2types.h"		/* GAMETYPE / GAME_TAKEADRIVE */

#include "jericho.h"
#include "jer_events.h"
#include "jer_frontend.h"

#include "players.h"
#include "main.h"
#include "mission.h"		/* GameLevel, wantedCar, GameType, NumPlayers */
#include "glaunch.h"		/* gSubGameNumber, gWantNight */
#include "state.h"		/* SetState, STATE_GAMESTART */
#include "system.h"		/* LevelNames[] */

#include "cainescrossfire.h"
#include "ai/ai.h"		/* CD2_AI_MAX - the opponent cap the menu cycles */
#include "profiles/profile.h"
#include "arenas/profile.h"	/* the arena registry the flow lists */

#include "select/select.h"

#include <string.h>
#include <stdlib.h>

extern int wantedCar[2];		/* the player's chosen car model per player */
extern int gBootMpLevel;		/* main.c: take-a-ride loads the mp-map mission instead of the full city */
extern int gBootMpArena;		/* main.c: which of the two mp layouts (0 or 1) */

static int gCcForced;			/* -ccmenu / CC_MENU seen this run */
static int gCcOpened;			/* we have opened the arena menu once */

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------
#define CC_MENU_ARENA		0
#define CC_MENU_VEH_BASE	1	/* the vehicle menus follow (one per city) */
#define CC_MENU_OPPONENTS	5	/* after the vehicles: how many opponents */

#define CC_CITY_COUNT		4	/* LevelNames: Chicago, Havana, Vegas, Rio */

// The arena menu: one row per registry arena + Back.
static JER_FE_ITEM gCcArenaItems[CD2_ARENA_MAX_ARENAS + 1];
static JER_FE_MENU gCcArenaMenu = { "cc.arena", gCcArenaItems, 0, NULL, NULL, NULL };
static int gCcArenaId[CD2_ARENA_MAX_ARENAS];	/* registry id per menu row */

static const char* const gCcVehIds[CC_CITY_COUNT] =
{
	"cc.veh.chicago", "cc.veh.havana", "cc.veh.vegas", "cc.veh.rio"
};

static JER_FE_ITEM gCcVehItems[CC_CITY_COUNT][2];	/* the carousel row + Back */
static JER_FE_MENU gCcVehMenu[CC_CITY_COUNT] =
{
	{ "cc.veh.chicago", gCcVehItems[0], 0, NULL, NULL, NULL },
	{ "cc.veh.havana",  gCcVehItems[1], 0, NULL, NULL, NULL },
	{ "cc.veh.vegas",   gCcVehItems[2], 0, NULL, NULL, NULL },
	{ "cc.veh.rio",     gCcVehItems[3], 0, NULL, NULL, NULL }
};

// The per-city rosters + the carousel cursor.
static int gCcVehList[CC_CITY_COUNT][CD2_VEH_COUNT];	// profile ids per city, registry order
static int gCcVehN[CC_CITY_COUNT];			// how many cars the city has
static int gCcVehSel[CC_CITY_COUNT];			// the carousel cursor

// The opponent menu: a carousel over 0..CD2_AI_MAX, plus what the vehicle screen
// picked (its Cross is what opens this menu).
static JER_FE_ITEM gCcOppItems[2];	/* the carousel row + Back */
static JER_FE_MENU gCcOppMenu = { "cc.opponents", gCcOppItems, 2, NULL, NULL, NULL };
static int gCcOppSel;			// how many opponents the match will field
static int gCcChosenArena = CD2_ARENA_NONE;	// the arena the vehicle screen serves
static int gCcPendingCar;		// ...and the roster slot in its city

static int cd2SelVehCity(void* ud)
{
	return (int)(size_t)ud;
}

// Launch a match on `arenaId` with the car of that arena's city at roster slot
// `sel`.
static void cd2SelLaunch(int arenaId, int sel)
{
	const CD2_ARENA_PROFILE* a;
	const CD2_VEH_PROFILE* p;
	int city, profile;

	// TEST/DEBUG override, for headless verification:
	//   CC_FORCE_ARENA=<arena id> CC_FORCE_CAR=<0..CD2_VEH_COUNT-1>
	//   CC_FORCE_OPPONENTS=<0..CD2_AI_MAX>
	if (getenv("CC_FORCE_ARENA") != NULL)
		arenaId = atoi(getenv("CC_FORCE_ARENA"));
	if (getenv("CC_FORCE_CAR") != NULL)
		sel = atoi(getenv("CC_FORCE_CAR"));
	if (getenv("CC_FORCE_OPPONENTS") != NULL)
		gCd2Cfg.aiOpponents = atoi(getenv("CC_FORCE_OPPONENTS"));

	a = cd2ArenaDef(arenaId);

	if (a == NULL)
	{
		printInfo("[cainescrossfire] CC select: arena %d not found, launch refused\n", arenaId);
		return;
	}

	city = a->city;

	if (city < 0 || city >= CC_CITY_COUNT || gCcVehN[city] <= 0)
		return;

	if (sel < 0 || sel >= gCcVehN[city])
		sel = 0;

	profile = gCcVehList[city][sel];
	p = cd2VehDef(profile);

	GameLevel = city;
	cd2VehSetPlayerProfile(profile);

	if (p != NULL && p->modelSlot >= 0)
		wantedCar[0] = p->modelSlot;

	// A TWISTED-METAL MATCH, not the story campaign the frontend's Undercover
	// entry would otherwise start. The arena says which mission layout: a
	// built-in arena is the city's small multiplayer map, a custom one may be a
	// cordoned corner of the full city (mp 0).
	GameType = GAME_TAKEADRIVE;
	NumPlayers = 1;
	gWantNight = 0;
	gSubGameNumber = 0;
	gBootMpLevel = a->mpLevel ? 1 : 0;
	gBootMpArena = a->mpArena ? 1 : 0;

	// the match's current arena: its authored spawns + barrier apply from here
	cd2ArenaSetCurrent(arenaId);

	printInfo("[cainescrossfire] CC select: start arena '%s' (%s) with %s (model %d) - mp=%d/%d, %d opponent(s)\n",
		a->internalName, a->displayName, cd2VehDisplayName(profile),
		(p != NULL) ? p->modelSlot : -1, a->mpLevel, a->mpArena, gCd2Cfg.aiOpponents);

	SetState(STATE_GAMESTART);
}

// "< HORNET >"
static void cd2SelVehLabel(void* ud, char* out, int max)
{
	int city = cd2SelVehCity(ud);

	if (city < 0 || city >= CC_CITY_COUNT || gCcVehN[city] <= 0)
	{
		snprintf(out, max, "< none >");
		return;
	}

	snprintf(out, max, "< %s >", cd2VehDisplayName(gCcVehList[city][gCcVehSel[city]]));
}

// Left/Right cycles the carousel.
static int cd2SelVehAdjust(void* ud, int dir)
{
	int city = cd2SelVehCity(ud);

	if (city < 0 || city >= CC_CITY_COUNT || gCcVehN[city] <= 0)
		return 0;

	gCcVehSel[city] = (gCcVehSel[city] + dir + gCcVehN[city]) % gCcVehN[city];

	jer_frontend_refresh();		/* the label AND the icon follow the cursor */
	return 1;
}

static int cd2SelVehActivate(void* ud)
{
	int city = cd2SelVehCity(ud);

	if (city < 0 || city >= CC_CITY_COUNT || gCcVehN[city] <= 0)
		return 0;

	// remember the pick and move on to the opponent screen - the match starts
	// from there, so a player always gets to choose the field size
	gCcPendingCar = gCcVehSel[city];

	jer_frontend_open(CC_MENU_OPPONENTS);
	return 1;
}

// An arena row was confirmed: remember the arena its vehicle screen will serve,
// then open that city's vehicle menu.
static int cd2SelArenaActivate(void* ud)
{
	int idx = (int)(size_t)ud;
	const CD2_ARENA_PROFILE* a = cd2ArenaDef(gCcArenaId[idx]);

	if (a == NULL)
		return 0;

	gCcChosenArena = gCcArenaId[idx];
	jer_frontend_open(CC_MENU_VEH_BASE + a->city);
	return 1;
}

// ---------------------------------------------------------------------------
// Opponents: 0..CD2_AI_MAX, cycled left/right, Cross starts the match.
// ---------------------------------------------------------------------------
static void cd2SelOppLabel(void* ud, char* out, int max)
{
	(void)ud;

	if (gCcOppSel == 0)
		snprintf(out, max, "< NO OPPONENTS >");
	else if (gCcOppSel == 1)
		snprintf(out, max, "< 1 OPPONENT >");
	else
		snprintf(out, max, "< %d OPPONENTS >", gCcOppSel);
}

static int cd2SelOppAdjust(void* ud, int dir)
{
	int n = CD2_AI_MAX + 1;

	(void)ud;

	gCcOppSel = (gCcOppSel + dir + n) % n;
	jer_frontend_refresh();
	return 1;
}

static int cd2SelOppActivate(void* ud)
{
	(void)ud;

	// the match setting the AI reads at spawn (cd2MatchOpponents)
	gCd2Cfg.aiOpponents = gCcOppSel;

	cd2SelLaunch(gCcChosenArena, gCcPendingCar);
	return 1;
}

// The car icon for the selected car: its OWN city + model (so a Chicago car
// shows Chicago's art, whatever arena it is being picked in).
static void cd2SelVehPreview(void* ud, int* city, int* model)
{
	int c = cd2SelVehCity(ud);
	const CD2_VEH_PROFILE* p;

	*city = -1;
	*model = -1;

	if (c < 0 || c >= CC_CITY_COUNT || gCcVehN[c] <= 0)
		return;

	p = cd2VehDef(gCcVehList[c][gCcVehSel[c]]);

	if (p != NULL)
	{
		*city = p->originCity;
		*model = p->modelSlot;
	}
}

// Build the arena + vehicle menus from the registry (once, at register).
static void cd2SelBuildMenus(void)
{
	int city;
	int n = cd2ArenaCount();
	int i;

	if (n > CD2_ARENA_MAX_ARENAS)
		n = CD2_ARENA_MAX_ARENAS;

	// one row per arena, pointing at its city's vehicle menu
	for (i = 0; i < n; i++)
	{
		const CD2_ARENA_PROFILE* a = cd2ArenaDef(i);
		JER_FE_ITEM* it = &gCcArenaItems[i];

		memset(it, 0, sizeof(*it));
		it->label = (a != NULL) ? a->displayName : "?";
		it->userdata = (void*)(size_t)i;
		it->on_activate = cd2SelArenaActivate;
		it->submenu = -1;

		gCcArenaId[i] = i;
	}

	memset(&gCcArenaItems[n], 0, sizeof(gCcArenaItems[n]));
	gCcArenaItems[n].label = "Back";
	gCcArenaItems[n].submenu = -1;
	gCcArenaItems[n].is_back = 1;

	gCcArenaMenu.item_count = n + 1;
	gCcArenaMenu.title = "SELECT ARENA";

	// the opponent menu: one carousel row + Back
	memset(&gCcOppItems[0], 0, sizeof(gCcOppItems[0]));
	gCcOppItems[0].get_label = cd2SelOppLabel;
	gCcOppItems[0].on_adjust = cd2SelOppAdjust;
	gCcOppItems[0].on_activate = cd2SelOppActivate;
	gCcOppItems[0].submenu = -1;

	memset(&gCcOppItems[1], 0, sizeof(gCcOppItems[1]));
	gCcOppItems[1].label = "Back";
	gCcOppItems[1].submenu = -1;
	gCcOppItems[1].is_back = 1;

	gCcOppMenu.title = "SELECT OPPONENTS";

	// open on whatever the match setting already is
	gCcOppSel = gCd2Cfg.aiOpponents;

	if (gCcOppSel < 0)
		gCcOppSel = 0;
	if (gCcOppSel > CD2_AI_MAX)
		gCcOppSel = CD2_AI_MAX;

	for (city = 0; city < CC_CITY_COUNT; city++)
	{
		gCcVehSel[city] = 0;

		// the carousel shows EVERY car, whatever the arena: only one car is ever
		// picked, so at most one foreign city is imported for it (the engine's
		// one-guest-city rule), and the arena only sets the level.
		for (i = 0; i < CD2_VEH_COUNT; i++)
			gCcVehList[city][i] = i;

		gCcVehN[city] = CD2_VEH_COUNT;

		// the carousel row: one row, left/right moves it
		memset(&gCcVehItems[city][0], 0, sizeof(gCcVehItems[city][0]));
		gCcVehItems[city][0].get_label = cd2SelVehLabel;
		gCcVehItems[city][0].userdata = (void*)(size_t)city;
		gCcVehItems[city][0].on_adjust = cd2SelVehAdjust;
		gCcVehItems[city][0].on_activate = cd2SelVehActivate;
		gCcVehItems[city][0].submenu = -1;

		memset(&gCcVehItems[city][1], 0, sizeof(gCcVehItems[city][1]));
		gCcVehItems[city][1].label = "Back";
		gCcVehItems[city][1].submenu = -1;
		gCcVehItems[city][1].is_back = 1;

		// a city with no cars shows just Back
		if (gCcVehN[city] > 0)
		{
			gCcVehMenu[city].item_count = 2;
			gCcVehMenu[city].get_preview = cd2SelVehPreview;
		}
		else
		{
			gCcVehItems[city][0] = gCcVehItems[city][1];
			memset(&gCcVehItems[city][1], 0, sizeof(gCcVehItems[city][1]));
			gCcVehMenu[city].item_count = 1;
			gCcVehMenu[city].get_preview = NULL;
		}

		gCcVehMenu[city].id = gCcVehIds[city];
		gCcVehMenu[city].userdata = (void*)(size_t)city;
		gCcVehMenu[city].title = "SELECT CAR";
	}

	printInfo("[cainescrossfire] CC select: %d arena(s) in the menu\n", n);
}

int cd2SelectActive(void)
{
	return gCcForced;
}

// JER_EVENT_CMDLINE — pick up our own shortcut.
static int cd2SelOnCmdline(void* ud, void* args)
{
	JER_ARGS_CMDLINE* a = (JER_ARGS_CMDLINE*)args;
	int i;

	(void)ud;

	for (i = 1; i < a->argc; i++)
	{
		if (a->argv[i] != NULL && strcmp(a->argv[i], "-ccmenu") == 0)
		{
			gCcForced = 1;
			printInfo("[cainescrossfire] -ccmenu: CC select flow armed\n");
		}
	}

	if (getenv("CC_MENU") != NULL)
		gCcForced = 1;

	/* DIRECT BOOT into an arena. The engine's own -level/-mp/-car boots straight
	 * into a level with no frontend; with CC_FORCE_ARENA set we make it an
	 * ARENA here (the registry was loaded at module entry, before CMDLINE) so
	 * its spawns, barrier and - with CC_EDITOR=1 - the in-game editor apply.
	 * This is the path the Python editor's "Launch in game" button uses.
	 *
	 * We do NOT touch the level/gametype/mission: -level and -mp already chose
	 * those (the launcher derives them from the arena's `city`/`mp`). Only the
	 * arena, its car profile and the opponent count are ours to set. */
	if (!gCcForced && getenv("CC_FORCE_ARENA") != NULL)
	{
		int aid = atoi(getenv("CC_FORCE_ARENA"));
		const CD2_ARENA_PROFILE* ar = cd2ArenaDef(aid);

		if (ar != NULL)
		{
			const char* carEnv = getenv("CC_FORCE_CAR");
			const char* oppEnv = getenv("CC_FORCE_OPPONENTS");

			cd2ArenaSetCurrent(aid);

			if (oppEnv != NULL)
			{
				int n = atoi(oppEnv);

				if (n < 0) n = 0;
				if (n > CD2_AI_MAX) n = CD2_AI_MAX;

				gCd2Cfg.aiOpponents = n;
			}

			printInfo("[cainescrossfire] direct boot: arena '%s' (id %d, city=%d mp=%d/%d), %d opponent(s)\n",
				ar->internalName, aid, ar->city, ar->mpLevel, ar->mpArena, gCd2Cfg.aiOpponents);

			/* the car: the engine's -car picks the model; also adopt the arena
			 * city's profile for slot `CC_FORCE_CAR` so the module's stats match */
			if (carEnv != NULL)
			{
				int city = ar->city;
				int sel = atoi(carEnv);

				if (city >= 0 && city < CC_CITY_COUNT && gCcVehN[city] > 0)
				{
					if (sel < 0 || sel >= gCcVehN[city])
						sel = 0;

					cd2VehSetPlayerProfile(gCcVehList[city][sel]);
				}
			}
		}
		else
		{
			printInfo("[cainescrossfire] direct boot: arena %d not found, ignored\n", aid);
		}
	}

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_FRONTEND_MAIN_MENU — the title screen's first row is the stock
// "Undercover" (the story campaign). Replace it with "Deathmatch" and point it
// at our arena menu: the total-conversion entry point.
static int cd2SelOnMainMenu(void* ud, void* args)
{
	JER_ARGS_FRONTEND_ENTRY* e = (JER_ARGS_FRONTEND_ENTRY*)args;

	(void)ud;

	if (e->index == 0)
	{
		snprintf(e->label, sizeof(e->label), "%s", "Deathmatch");
		e->openMenu = CC_MENU_ARENA;
	}

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_FRAME — open the arena menu once the frontend is up.
static int cd2SelOnFrame(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (!gCcForced || gCcOpened)
		return JER_RESULT_CONTINUE;

	// jer_frontend_open pushes the current screen, so open exactly once; the
	// engine reports -1 until the frontend is actually on our menu.
	jer_frontend_open(CC_MENU_ARENA);

	if (jer_frontend_current_menu() == CC_MENU_ARENA)
	{
		gCcOpened = 1;
		gCcForced = 0;
		printInfo("[cainescrossfire] CC select: arena menu open\n");

		// Harness launch: a padless run cannot drive the menus (the module
		// screens ignore input with no pad), so with the overrides set the match
		// starts from here instead of on a confirm. Same launch the opponent
		// screen's confirm runs. Scripted runs only - a pad still gets the menus.
		if (getenv("CC_FORCE_ARENA") != NULL && getenv("CC_FORCE_CAR") != NULL)
		{
			printInfo("[cainescrossfire] CC select: harness launch (no menu input)\n");
			cd2SelLaunch(atoi(getenv("CC_FORCE_ARENA")), atoi(getenv("CC_FORCE_CAR")));
		}
	}

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_FRONTEND_ENTERED — back in the menus. Re-arm the -ccmenu flow so it
// can open again, and drop any half-made selection.
static int cd2SelOnFrontendEntered(void* ud, void* args)
{
	int city;

	(void)ud;
	(void)args;

	gCcOpened = 0;

	for (city = 0; city < CC_CITY_COUNT; city++)
		gCcVehSel[city] = 0;

	return JER_RESULT_CONTINUE;
}

void cd2SelectRegister(JERICHO_CONTEXT* ctx)
{
	cd2SelBuildMenus();

	// ORDER MATTERS: the arena menu first, so it is index 0; the per-city
	// vehicle menus follow as CC_MENU_VEH_BASE + city.
	jer_frontend_register_menu(&gCcArenaMenu);
	jer_frontend_register_menu(&gCcVehMenu[0]);
	jer_frontend_register_menu(&gCcVehMenu[1]);
	jer_frontend_register_menu(&gCcVehMenu[2]);
	jer_frontend_register_menu(&gCcVehMenu[3]);
	jer_frontend_register_menu(&gCcOppMenu);

	// a fallback route: the main menu's entry opens the arena menu
	jer_frontend_set_main_entry("cc.arena");

	ctx->jer_register_hook(ctx, JER_EVENT_CMDLINE, cd2SelOnCmdline, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_MAIN_MENU, cd2SelOnMainMenu, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2SelOnFrontendEntered, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2SelOnFrame, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] CC select flow registered (-ccmenu)\n");
}
