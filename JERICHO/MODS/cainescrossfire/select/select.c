// select/select.c — the CC select flow, built on the NATIVE frontend menus
// (JERICHO/include/jer_frontend.h), so it looks and navigates like the game's
// own screens instead of a floating overlay.
//
//   cc.arena          the four cities (+ Back)
//   cc.veh.<city>     the vehicles whose origin city is that arena
//
// Pick an arena, then pick one of ITS vehicles, and the match starts with that
// GameLevel + wantedCar + the player's profile. Restricting the vehicle list to
// the arena's own city is deliberate: the engine imports from only ONE foreign
// city per level (models.c, InitCarImport), so a car belongs to the city it is
// picked in - that is also what makes its geometry load from its own city.
//
// Raised by -ccmenu (or CC_MENU): the flow opens cc.arena, and the main menu's
// entry is routed there too as a fallback.

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
#include "profiles/profile.h"

#include "select/select.h"

#include <string.h>
#include <stdlib.h>

extern int wantedCar[2];		/* the player's chosen car model per player */
extern int gBootMpLevel;		/* main.c: take-a-ride loads the mp-map mission instead of the full city */
extern int gBootMpArena;		/* main.c: which of the two mp layouts (0 or 1) */

static int gCcForced;			/* -ccmenu / CC_MENU seen this run */
static int gCcOpened;			/* we have opened the arena menu once */

// A vehicle press: the item's userdata packs (city * 100 + profileId).
static int cd2SelActVehicle(void* ud)
{
	int packed = (int)(size_t)ud;
	int city = packed / 100;
	int profile = packed % 100;
	const CD2_VEH_PROFILE* p = cd2VehDef(profile);

	GameLevel = city;
	cd2VehSetPlayerProfile(profile);

	if (p != NULL && p->modelSlot >= 0)
		wantedCar[0] = p->modelSlot;

	// A TWISTED-METAL MATCH, not the story campaign the frontend's Undercover
	// entry would otherwise start. Free-roam TAKE A RIDE, single player, on the
	// city's MULTIPLAYER-MAP arena 0 (gBootMpLevel makes State_GameStart pick
	// the small mp layout, M58.., instead of the full city, M50..; arena 0 is
	// gSubGameNumber 0).
	GameType = GAME_TAKEADRIVE;
	NumPlayers = 1;
	gWantNight = 0;
	gSubGameNumber = 0;		/* arena 0 (mission M58 + city*2) */
	gBootMpLevel = 1;
	gBootMpArena = 0;

	printInfo("[cainescrossfire] CC select: start %s with %s (model %d) - take-a-ride mp arena 0\n",
		LevelNames[city], cd2VehDisplayName(profile), (p != NULL) ? p->modelSlot : -1);

	SetState(STATE_GAMESTART);
	return 1;
}

// ---------------------------------------------------------------------------
// The arena menu (index 0) — the four cities, each opening its vehicle menu.
// ---------------------------------------------------------------------------
#define CC_MENU_ARENA		0
#define CC_MENU_VEH_BASE	1	/* the four vehicle menus follow */

static const char* const gCcArenas[] =
{
	"Chicago", "Havana", "Las Vegas", "Rio"
};

static JER_FE_ITEM gCcArenaItems[5];	/* 4 cities + Back */
static JER_FE_MENU gCcArenaMenu = { "cc.arena", gCcArenaItems, 5, NULL, NULL };

// ---------------------------------------------------------------------------
// The vehicle menus (one per city).
// ---------------------------------------------------------------------------
static const char* const gCcVehIds[4] =
{
	"cc.veh.chicago", "cc.veh.havana", "cc.veh.vegas", "cc.veh.rio"
};

static JER_FE_ITEM gCcVehItems[4][JER_FE_MAX_ITEMS];
static JER_FE_MENU gCcVehMenu[4] =
{
	{ "cc.veh.chicago", gCcVehItems[0], 0, NULL, NULL },
	{ "cc.veh.havana",  gCcVehItems[1], 0, NULL, NULL },
	{ "cc.veh.vegas",   gCcVehItems[2], 0, NULL, NULL },
	{ "cc.veh.rio",     gCcVehItems[3], 0, NULL, NULL }
};

// The menu id list above must line up with this (kept explicit, checked at boot).
static const int gCcVehCity[4] =
{
	CD2_VEH_CITY_CHICAGO, CD2_VEH_CITY_HAVANA, CD2_VEH_CITY_VEGAS, CD2_VEH_CITY_RIO
};

// Build the arena + vehicle item tables from the registry (once, at register).
static void cd2SelBuildMenus(void)
{
	int city, n;

	for (city = 0; city < 4; city++)
	{
		JER_FE_ITEM* it = &gCcArenaItems[city];

		memset(it, 0, sizeof(*it));
		it->label = gCcArenas[city];
		it->submenu = CC_MENU_VEH_BASE + city;
	}

	// Back
	memset(&gCcArenaItems[4], 0, sizeof(gCcArenaItems[4]));
	gCcArenaItems[4].label = "Back";
	gCcArenaItems[4].submenu = -1;
	gCcArenaItems[4].is_back = 1;

	for (city = 0; city < 4; city++)
	{
		int i;

		n = 0;

		for (i = 0; i < CD2_VEH_COUNT && n < JER_FE_MAX_ITEMS - 1; i++)
		{
			const CD2_VEH_PROFILE* p = cd2VehDef(i);

			if (p == NULL || p->originCity != gCcVehCity[city])
				continue;

			memset(&gCcVehItems[city][n], 0, sizeof(gCcVehItems[city][n]));
			gCcVehItems[city][n].label = p->displayName;
			gCcVehItems[city][n].userdata = (void*)(size_t)(gCcVehCity[city] * 100 + i);
			gCcVehItems[city][n].on_activate = cd2SelActVehicle;
			gCcVehItems[city][n].submenu = -1;
			n++;
		}

		// Back
		memset(&gCcVehItems[city][n], 0, sizeof(gCcVehItems[city][n]));
		gCcVehItems[city][n].label = "Back";
		gCcVehItems[city][n].submenu = -1;
		gCcVehItems[city][n].is_back = 1;
		n++;

		gCcVehMenu[city].item_count = n;
		gCcVehMenu[city].id = gCcVehIds[city];
	}
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
	}

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

	// a fallback route: the main menu's entry opens the arena menu
	jer_frontend_set_main_entry("cc.arena");

	ctx->jer_register_hook(ctx, JER_EVENT_CMDLINE, cd2SelOnCmdline, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2SelOnFrame, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] CC select flow registered (-ccmenu)\n");
}
