// select/select.c — the CC select flow, built on the NATIVE frontend menus
// (JERICHO/include/jer_frontend.h), so it looks and navigates like the game's
// own screens instead of a floating overlay.
//
//   cc.arena          the four cities (+ Back)
//   cc.veh.<city>     < VEHICLE > - a carousel over that arena's own cars, with
//                     the car's frontend icon, left/right to change, Cross to
//                     start the match
//
// Restricting the vehicle list to the arena's own city is deliberate: the engine
// imports from only ONE foreign city per level (models.c, InitCarImport), so a
// car belongs to the city it is picked in - that is also what makes its
// geometry load from its own city.
//
// Confirming a car launches a Twisted-Metal match: free-roam TAKE A RIDE, single
// player, on the city's multiplayer-map arena 0.
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
#define CC_MENU_VEH_BASE	1	/* the four vehicle menus follow (1..4) */
#define CC_MENU_OPPONENTS	5	/* after the vehicles: how many opponents */

static const char* const gCcArenas[] =
{
	"Chicago", "Havana", "Las Vegas", "Rio"
};

static JER_FE_ITEM gCcArenaItems[5];	/* 4 cities + Back */
static JER_FE_MENU gCcArenaMenu = { "cc.arena", gCcArenaItems, 5, NULL, NULL, NULL };

static const char* const gCcVehIds[4] =
{
	"cc.veh.chicago", "cc.veh.havana", "cc.veh.vegas", "cc.veh.rio"
};

static JER_FE_ITEM gCcVehItems[4][2];	/* the carousel row + Back */
static JER_FE_MENU gCcVehMenu[4] =
{
	{ "cc.veh.chicago", gCcVehItems[0], 0, NULL, NULL, NULL },
	{ "cc.veh.havana",  gCcVehItems[1], 0, NULL, NULL, NULL },
	{ "cc.veh.vegas",   gCcVehItems[2], 0, NULL, NULL, NULL },
	{ "cc.veh.rio",     gCcVehItems[3], 0, NULL, NULL, NULL }
};

// The vehicle menus' cities (must line up with gCcVehIds).
static const int gCcVehCity[4] =
{
	CD2_VEH_CITY_CHICAGO, CD2_VEH_CITY_HAVANA, CD2_VEH_CITY_VEGAS, CD2_VEH_CITY_RIO
};

// The per-city rosters + the carousel cursor.
static int gCcVehList[4][CD2_VEH_COUNT];	// profile ids per city, registry order
static int gCcVehN[4];				// how many cars the city has
static int gCcVehSel[4];			// the carousel cursor

// The opponent menu: a carousel over 0..CD2_AI_MAX, plus what the vehicle screen
// picked (its Cross is what opens this menu).
static JER_FE_ITEM gCcOppItems[2];	/* the carousel row + Back */
static JER_FE_MENU gCcOppMenu = { "cc.opponents", gCcOppItems, 2, NULL, NULL, NULL };
static int gCcOppSel;			// how many opponents the match will field
static int gCcPendingCity;		// the arena the vehicle screen chose
static int gCcPendingCar;		// ...and the roster slot in it

static int cd2SelVehCity(void* ud)
{
	return (int)(size_t)ud;
}

// Launch a match with the car of `city` at roster slot `sel`.
static void cd2SelLaunch(int city, int sel)
{
	const CD2_VEH_PROFILE* p;
	int profile;

	// TEST/DEBUG override, for headless verification of the cross-city path:
	//   CC_FORCE_ARENA=<0..3> CC_FORCE_CAR=<0..CD2_VEH_COUNT-1>
	//   CC_FORCE_OPPONENTS=<0..CD2_AI_MAX>
	if (getenv("CC_FORCE_ARENA") != NULL)
		city = atoi(getenv("CC_FORCE_ARENA"));
	if (getenv("CC_FORCE_CAR") != NULL)
		sel = atoi(getenv("CC_FORCE_CAR"));
	if (getenv("CC_FORCE_OPPONENTS") != NULL)
		gCd2Cfg.aiOpponents = atoi(getenv("CC_FORCE_OPPONENTS"));

	if (city < 0 || city > 3 || gCcVehN[city] <= 0)
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
	// entry would otherwise start. Free-roam TAKE A RIDE, single player, on the
	// city's MULTIPLAYER-MAP arena 0 (gBootMpLevel makes State_GameStart pick
	// the small mp layout, M58.., instead of the full city, M50..).
	GameType = GAME_TAKEADRIVE;
	NumPlayers = 1;
	gWantNight = 0;
	gSubGameNumber = 0;		/* arena 0 (mission M58 + city*2) */
	gBootMpLevel = 1;
	gBootMpArena = 0;

	printInfo("[cainescrossfire] CC select: start %s with %s (model %d) - take-a-ride mp arena 0, %d opponent(s)\n",
		LevelNames[city], cd2VehDisplayName(profile), (p != NULL) ? p->modelSlot : -1,
		gCd2Cfg.aiOpponents);

	SetState(STATE_GAMESTART);
}

// "< HORNET >"
static void cd2SelVehLabel(void* ud, char* out, int max)
{
	int city = cd2SelVehCity(ud);

	if (city < 0 || city > 3 || gCcVehN[city] <= 0)
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

	if (city < 0 || city > 3 || gCcVehN[city] <= 0)
		return 0;

	gCcVehSel[city] = (gCcVehSel[city] + dir + gCcVehN[city]) % gCcVehN[city];

	jer_frontend_refresh();		/* the label AND the icon follow the cursor */
	return 1;
}

static int cd2SelVehActivate(void* ud)
{
	int city = cd2SelVehCity(ud);

	if (city < 0 || city > 3 || gCcVehN[city] <= 0)
		return 0;

	// remember the pick and move on to the opponent screen - the match starts
	// from there, so a player always gets to choose the field size
	gCcPendingCity = city;
	gCcPendingCar = gCcVehSel[city];

	jer_frontend_open(CC_MENU_OPPONENTS);
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

	cd2SelLaunch(gCcPendingCity, gCcPendingCar);
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

	if (c < 0 || c > 3 || gCcVehN[c] <= 0)
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

	for (city = 0; city < 4; city++)
	{
		JER_FE_ITEM* it = &gCcArenaItems[city];

		memset(it, 0, sizeof(*it));
		it->label = gCcArenas[city];
		it->submenu = CC_MENU_VEH_BASE + city;
	}

	memset(&gCcArenaItems[4], 0, sizeof(gCcArenaItems[4]));
	gCcArenaItems[4].label = "Back";
	gCcArenaItems[4].submenu = -1;
	gCcArenaItems[4].is_back = 1;

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

	for (city = 0; city < 4; city++)
	{
		int i;

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

	printInfo("[cainescrossfire] CC select: cars per arena - Chicago %d, Havana %d, Vegas %d, Rio %d\n",
		gCcVehN[0], gCcVehN[1], gCcVehN[2], gCcVehN[3]);
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
		// starts from here instead of on a confirm. Same launch the vehicle
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

	for (city = 0; city < 4; city++)
		gCcVehSel[city] = 0;

	return JER_RESULT_CONTINUE;
}

void cd2SelectRegister(JERICHO_CONTEXT* ctx)
{
	cd2SelBuildMenus();

	// ORDER MATTERS: the arena menu first, so it is index 0; the four vehicle
	// menus follow as CC_MENU_VEH_BASE + city.
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
