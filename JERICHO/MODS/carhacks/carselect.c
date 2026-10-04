/* carhacks/carselect.c — the carhacks car-select menu (see carhacks.h).
 *
 * A JERICHO frontend menu that REPLACES the stock "Choose A Ride" car-select
 * screen for a single-player Take a Ride. The reason is one row the stock screen
 * has no room for: WHICH CITY'S ROSTER the car list is drawn from.
 *
 *   < CAR >       left/right cycles the car inside the roster below
 *   < CITY: X >   left/right cycles CHICAGO/HAVANA/VEGAS/RIO - the roster (NEW)
 *   Ride          start the level with the picked car
 *   Back          back to the Day/Night screen
 *   Triangle      the same back, from any row (JER_FE_MENU.on_back)
 *
 * The roster row sits directly BELOW the car row, as asked. The stock screen only
 * ever shows the LEVEL's own car list (carNumLookup[GameLevel]); this one lets
 * the player browse any of the four cities' lists. The icon stays the engine's
 * own car-select art for the car's OWN city (JER_FE_MENU.get_preview), so a
 * Havana car previews with Havana's art while the level is Chicago.
 *
 * Nothing about the engine's ASSET LOADING is changed. The stock screen still
 * runs its setup - which is what computes CarAvailability and loads the icon
 * background/palette - and we then show this menu in its place; Ride makes the
 * same call the stock screen's Select makes (SetState(STATE_GAMESTART)) with
 * wantedCar[] set the same way. Only the CHOICE is ours.
 *
 * With a multiplayer session live the stock car screen is NOT replaced: that
 * screen is where mp seats players and claims the START (JER_EVENT_MP_FRONTEND),
 * so overriding it from here would break the session. See MP_ADAPTER.md.
 */

#include "driver2.h"
#include "dr2types.h"		/* GAMETYPE / GAME_TAKEADRIVE */

#include "jericho.h"
#include "jer_events.h"
#include "jer_frontend.h"
#include "jer_net.h"		/* jer_net_is_active - a live session owns the car screen */

#include "system.h"		/* LevelNames[] */
#include "mission.h"		/* GameLevel, GameType, NumPlayers, wantedCar */
#include "state.h"		/* SetState, STATE_GAMESTART */

#include "carhacks.h"
#include "carid.h"
#include "carimport.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Engine globals the menu reads (exported as C++ data symbols; the mod is
 * compiled C++, so a plain extern matches the export - the same pattern
 * carhacks.c already uses for these two). */
extern int  CarAvailability[4][10];	/* frontend car list: [level][slot] */
extern char carNumLookup[4][10];	/* frontend slot -> model number */

#define CHK_CITY_COUNT		4	/* LevelNames: CHICAGO, HAVANA, VEGAS, RIO */
#define CHK_SLOTS		10	/* frontend slots per city (carNumLookup[city][0..9]) */
#define CHK_ROSTER_MAX		CHK_SLOTS

/* The stock frontend screen indices this flow is part of. They come from the
 * runtime screen table (DATA/SCRS.BIN): 0 main menu, 1 city, 3 time of day,
 * 14 car select - the "Take a Ride" chain. */
#define CHK_FE_SCREEN_MAIN	0
#define CHK_FE_SCREEN_TIMEOFDAY	3
#define CHK_FE_SCREEN_CAR	14

/* Menu rows. */
#define CHK_ROW_CAR		0
#define CHK_ROW_CITY		1
#define CHK_ROW_RIDE		2
#define CHK_ROW_BACK		3

typedef struct CHK_ROSTER_ENTRY
{
	int slot;		/* frontend slot 0..9 */
	int model;		/* the model number that slot maps to */
} CHK_ROSTER_ENTRY;

/* ---------------------------------------------------------------------------
 * Menu state
 * ------------------------------------------------------------------------- */

static int gChkRosterCity;	/* which city's roster the car list is drawn from */
static int gChkCarIdx;		/* cursor into that roster */

static int gChkArmed;		/* the stock car screen has run its setup: show ours */
static int gChkWalked;		/* harness: the walk to the car screen happened */
static int gChkInFrontend;	/* a level has not started since we entered the menus */
static int gChkFrontendFrames;	/* frames since entering the menus (harness settle) */
static int gChkRode;		/* the harness has ridden once */

static int gChkForceCity = -1;	/* harness: CHK_FORCE_ROSTER_CITY */
static int gChkForceCar = -1;	/* harness: CHK_FORCE_CAR */
static int gChkForceLevel = -1;	/* harness: CHK_FORCE_LEVEL (the synthetic level city) */
static int gChkForceMenu;	/* harness: CHK_FORCE_MENU - walk to the car screen */

static JER_FE_ITEM gChkItems[4];
static JER_FE_MENU gChkMenu =
{
	"chk.car",		/* id */
	gChkItems,		/* items */
	4,			/* item_count */
	NULL,			/* on_enter */
	NULL,			/* userdata */
	"SELECT CAR",		/* title */
	NULL,			/* get_preview (set below, keeps the initializer readable) */
	NULL			/* on_back (set below, with get_preview) */
};

/* ---------------------------------------------------------------------------
 * The roster: the city's OWN frontend car list, filtered to what is available.
 *
 * The stock car screen builds CarAvailability for the level (its bSetup), which
 * is why this menu is only shown once that setup has run - reading it any
 * earlier would hand back the previous level's list.
 * ------------------------------------------------------------------------- */

static int chkRosterBuild(int city, CHK_ROSTER_ENTRY* out, int max)
{
	int n = 0, slot;

	if (city < 0 || city >= CHK_CITY_COUNT)
		return 0;

	for (slot = 0; slot < CHK_SLOTS; slot++)
	{
		if (CarAvailability[city][slot] == 0)
			continue;

		if (out != NULL && n < max)
		{
			out[n].slot = slot;
			out[n].model = (int)(unsigned char)carNumLookup[city][slot];
		}

		n++;
	}

	return n;
}

static int chkRosterCount(int city)
{
	return chkRosterBuild(city, NULL, 0);
}

static void chkClampCursor(void)
{
	int n = chkRosterCount(gChkRosterCity);

	if (n <= 0)
		gChkCarIdx = 0;
	else if (gChkCarIdx >= n)
		gChkCarIdx = n - 1;
	else if (gChkCarIdx < 0)
		gChkCarIdx = 0;
}

/* ---------------------------------------------------------------------------
 * Menu rows
 * ------------------------------------------------------------------------- */

/* "< CAR 3 >" - the position in the roster; the icon shows which car. */
static void chkCarLabel(void* ud, char* out, int max)
{
	(void)ud;

	if (chkRosterCount(gChkRosterCity) <= 0)
	{
		snprintf(out, max, "< no cars >");
		return;
	}

	snprintf(out, max, "< CAR %d >", gChkCarIdx + 1);
}

/* "< CITY: CHICAGO >" - the new row, directly below the car row. */
static void chkCityLabel(void* ud, char* out, int max)
{
	(void)ud;

	snprintf(out, max, "< CITY: %s >", chkCityName(gChkRosterCity));
}

/* Left/Right: the car row moves the cursor, the city row moves the roster. */
static int chkSelAdjust(void* ud, int dir)
{
	int row = (int)(size_t)ud;

	if (row == CHK_ROW_CITY)
	{
		gChkRosterCity = (gChkRosterCity + dir + CHK_CITY_COUNT) % CHK_CITY_COUNT;
		chkClampCursor();

		printInfo("[carhacks] car select: roster city %s (%d car(s) available)\n",
			chkCityName(gChkRosterCity), chkRosterCount(gChkRosterCity));
	}
	else if (row == CHK_ROW_CAR)
	{
		int n = chkRosterCount(gChkRosterCity);

		if (n <= 0)
			return 0;

		gChkCarIdx = (gChkCarIdx + dir + n) % n;

		{
			CHK_ROSTER_ENTRY list[CHK_ROSTER_MAX];

			chkRosterBuild(gChkRosterCity, list, CHK_ROSTER_MAX);

			printInfo("[carhacks] car select: %s slot %d -> model %d\n",
				chkCityName(gChkRosterCity), list[gChkCarIdx].slot, list[gChkCarIdx].model);
		}
	}
	else
	{
		return 0;
	}

	/* the label AND the icon follow the cursor */
	jer_frontend_refresh();
	return 1;
}

/* The car icon: the picked car's OWN city + model, so the engine draws that
 * city's art whatever level we are in. */
static void chkSelPreview(void* ud, int* city, int* model)
{
	CHK_ROSTER_ENTRY list[CHK_ROSTER_MAX];
	int n;

	(void)ud;

	*city = -1;
	*model = -1;

	n = chkRosterBuild(gChkRosterCity, list, CHK_ROSTER_MAX);

	if (gChkCarIdx < 0 || gChkCarIdx >= n)
		return;

	*city = gChkRosterCity;
	*model = list[gChkCarIdx].model;
}

/* ---------------------------------------------------------------------------
 * Ride
 * ------------------------------------------------------------------------- */

/* Start the level with the car at `idx` of `city`'s roster.
 *
 * This is the stock car screen's Select, exactly: wantedCar[] is what the engine
 * matches against residentCarModels[] to decide which slot to spawn the player
 * in (players.c, InitPlayer). Nothing else about the launch changes, so the
 * level's asset loading is untouched - which is the whole reason the choice can
 * be replaced without touching the load. */
static int chkRideWith(int city, int idx)
{
	CHK_ROSTER_ENTRY list[CHK_ROSTER_MAX];
	int n;

	if (city < 0 || city >= CHK_CITY_COUNT)
		return 0;

	n = chkRosterBuild(city, list, CHK_ROSTER_MAX);

	if (idx < 0 || idx >= n)
	{
		/* Player-facing: the pick could not be started, so tell them (the menu
		 * normally prevents this, but a refused Ride must not be silent). */
		jer_error("[carhacks] car select: ride refused - no car %d in %s's roster (%d available)",
			idx, chkCityName(city), n);
		return 0;
	}

	gChkRosterCity = city;
	chkClampCursor();

	/* the pick is the module's single source of truth for "the player's car":
	 * carhacks.c turns it into the level's cross-city import (carimport.c) */
	chkImportSetLocalPick(city, list[idx].model);

	/* and tell a session, so the host can fold it into the agreed set (net.c) */
	chkNetAdvertisePick(city, list[idx].model);

	wantedCar[0] = list[idx].model;

	printInfo("[carhacks] car select: RIDE %s slot %d -> model %d (level city %s) wantedCar=%d\n",
		chkCityName(city), list[idx].slot, list[idx].model,
		chkCityName(GameLevel), wantedCar[0]);

	/* A LIVE SESSION OWNS THE LAUNCH. mp seats players on its own car screen and
	 * claims the frontend's START, so starting the level from here would load it
	 * without the session's level, car agreement and spawn. Firing the same event
	 * the stock frontend raises hands the press to mp, which launches on our
	 * behalf (MpOnMpFrontend: MpClientLaunch for a client, MpStartMatch for the
	 * host) and picks the pick up from wantedCar[0] on the way. Nothing is
	 * launched twice: mp is the only caller of SetState in this path. */
	if (jer_net_is_active())
	{
		JER_ARGS_MP_FRONTEND fe;

		fe.action = JER_MP_FE_START;
		fe.query = 0;
		fe.claimed = 0;
		fe.passthrough = 0;

		jer_fire(JER_EVENT_MP_FRONTEND, &fe);

		if (fe.claimed)
		{
			printInfo("[carhacks] car select: the session took the launch (player %d, model %d)\n",
				jer_net_local_player(), wantedCar[0]);
			return 1;
		}

		/* The session exists but would not launch (a refused join is the real
		 * case). Player-facing, because the alternative is a Ride that looks
		 * accepted and does nothing. */
		jer_error("[carhacks] car select: the session did not take the launch - starting the level directly");
	}

	SetState(STATE_GAMESTART);
	return 1;
}

/* BACK, as this menu defines it - reached from the Back row (Cross on it) and from
 * the menu's on_back (Triangle, from ANY row). One function, so the two cannot drift
 * apart.
 *
 * The stock Take-a-Ride chain is main(0) -> city(1) -> day/night(3) -> car(14).
 * "Back" must land on the Day/Night screen, NOT on the stack (which holds the stock
 * car screen we replaced): returning there would re-run its setup, re-arm this menu
 * and trap the player in a loop.
 *
 * IN A SESSION none of that applies: this screen was pushed by mp's own chain
 * (mp.lobby -> the stock car screen), and the stock city/day-night screens are not
 * part of it. Returning 0 hands the press to the engine's own is_back row, which is
 * exactly the previous-screen pop mp expects - and it cannot re-arm anything,
 * because the screen it returns to is mp's, not the car screen. */
static int chkSelBack(void* ud)
{
	(void)ud;

	if (jer_net_is_active())
	{
		printInfo("[carhacks] car select: back (in a session - the engine pops to mp's own screen)\n");
		return 0;
	}

	printInfo("[carhacks] car select: back to the day/night screen\n");
	jer_frontend_goto(CHK_FE_SCREEN_TIMEOFDAY);
	return 1;
}

/* Cross on a row. */
static int chkSelActivate(void* ud)
{
	int row = (int)(size_t)ud;

	if (row == CHK_ROW_RIDE)
		return chkRideWith(gChkRosterCity, gChkCarIdx);

	if (row == CHK_ROW_BACK)
		return chkSelBack(ud);

	return 0;
}

/* ---------------------------------------------------------------------------
 * The intercept: replace the stock car screen with this menu
 * ------------------------------------------------------------------------- */

/* A padless run needs a few frontend frames before the screens are usable - the
 * same reason jer_frontend_open is retried until the engine reports the menu. */
#define CHK_HARNESS_SETTLE	8

static int chkOurMenuOnScreen(void)
{
	int idx = jer_frontend_find(gChkMenu.id);

	return (idx >= 0) && (jer_frontend_current_menu() == idx);
}

/* Called from carhacks.c's JER_EVENT_CAR_AVAILABILITY hook: the stock car screen
 * has just STARTED its setup. That setup is what computes CarAvailability, so
 * the menu is opened on the next frame rather than from here. */
void chkCarSelectArm(void)
{
	int inSession = jer_net_is_active();

	/* A single-player Take a Ride is the ordinary case: 2-player split-screen and
	 * a mission's own car pick keep the stock screen.
	 *
	 * INSIDE A SESSION those gates say nothing. The LAN flow leaves NumPlayers at
	 * 2 (its split-screen chain sets it) and a joiner's GameType is still
	 * GAME_MISSION until mp launches -- so the old condition bailed every time a
	 * session was live, silently, which is why a joining player never saw the
	 * roster. With a session up, the only question that matters is whether the
	 * session wants the pick, and mp's own screen is what we replace -- so the
	 * menu is offered here and Ride hands the launch back (see chkRideWith). */
	if (!inSession && (GameType != GAME_TAKEADRIVE || NumPlayers != 1))
		return;

	/* the roster opens on the level's own city, i.e. exactly the stock list */
	gChkRosterCity = (GameLevel >= 0 && GameLevel < CHK_CITY_COUNT) ? GameLevel : 0;
	gChkCarIdx = 0;
	gChkArmed = 1;

	printInfo("[carhacks] car select: armed for level %s (%s)\n",
		chkCityName(gChkRosterCity),
		inSession ? "in a session - the roster is offered, Ride hands the launch to mp"
			: "its car screen is showing");
}

/* JER_EVENT_FRAME - fires every frame in the frontend AND in a level; every
 * branch below is gated so gameplay is never touched. */
static int chkSelOnFrame(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (gChkInFrontend)
		gChkFrontendFrames++;

	/* the real path: the stock car screen's setup has finished, so show ours in
	 * its place (one frame late - the setup computes CarAvailability) */
	if (gChkArmed)
	{
		int idx = jer_frontend_find(gChkMenu.id);

		gChkArmed = 0;

		if (idx >= 0)
		{
			jer_frontend_open(idx);
			printInfo("[carhacks] car select: opening the menu over the stock car screen (level %s, %d car(s) in its roster)\n",
				chkCityName(gChkRosterCity), chkRosterCount(gChkRosterCity));
		}
	}

	/* harness, step 1: walk to the car screen - reaching it is what arms the
	 * menu, so this is how the intercept is exercised without a pad */
	if (gChkForceMenu && !gChkWalked && gChkInFrontend && gChkFrontendFrames >= CHK_HARNESS_SETTLE)
	{
		gChkWalked = 1;

		GameType = GAME_TAKEADRIVE;
		NumPlayers = 1;

		if (gChkForceLevel >= 0 && gChkForceLevel < CHK_CITY_COUNT)
			GameLevel = gChkForceLevel;

		printInfo("[carhacks] car select: harness - walking to the car screen (level %s)\n",
			chkCityName(GameLevel));

		jer_frontend_goto(CHK_FE_SCREEN_CAR);
		return JER_RESULT_CONTINUE;
	}

	/* harness, step 2: ride without a pad. Through the menu when it is on screen
	 * (the faithful path); directly when it is not, so the Ride path itself can
	 * be exercised on its own. */
	if ((gChkForceCity >= 0 || gChkForceCar >= 0) && !gChkRode && gChkInFrontend &&
		gChkFrontendFrames >= CHK_HARNESS_SETTLE)
	{
		int city = (gChkForceCity >= 0) ? gChkForceCity : gChkRosterCity;
		int car = (gChkForceCar >= 0) ? gChkForceCar : gChkCarIdx;

		if (!chkOurMenuOnScreen() && gChkForceMenu)
			return JER_RESULT_CONTINUE;	/* wait for the menu to come up */

		gChkRode = 1;
		gChkInFrontend = 0;

		GameType = GAME_TAKEADRIVE;
		NumPlayers = 1;

		if (gChkForceLevel >= 0 && gChkForceLevel < CHK_CITY_COUNT)
			GameLevel = gChkForceLevel;

		gChkRosterCity = city;
		chkClampCursor();

		chkRideWith(city, car);
	}

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_FRONTEND_ENTERED - back in the menus (or here for the first time):
 * allow the flow again, drop a half-made selection, and give the cross-city PLACEMENT back.
 *
 * Why release here and not only when a session ends: those are paged-in car textures and
 * CLUT rows, and the menus do not need any of them. Without it the placement of the level
 * we just left sits behind the frontend until some later level load, which is the "they pile
 * up" report. A live session is the exception - there the placement is what the players
 * are driving, and a joiner's car select is itself a frontend screen. The pick survives in
 * both cases (chkImportPurgePlacement), because the level the pick starts still needs it. */
static int chkSelOnFrontendEntered(void* ud, void* args)
{
	(void)ud;
	(void)args;

	gChkInFrontend = 1;
	gChkFrontendFrames = 0;
	gChkCarIdx = 0;

	if (!jer_net_is_active())
		chkImportPurgePlacement();

	return JER_RESULT_CONTINUE;
}

/* ---------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

/* The harness levers, read once: a padless run cannot drive a module menu (the
 * screens ignore input with no pad), so these let a headless run pick the
 * roster city / car the same way CC_FORCE_* does for the CC select. */
static void chkReadHarness(void)
{
	const char* e;

	e = getenv("CHK_FORCE_ROSTER_CITY");

	if (e != NULL)
		gChkForceCity = atoi(e);

	e = getenv("CHK_FORCE_CAR");

	if (e != NULL)
		gChkForceCar = atoi(e);

	e = getenv("CHK_FORCE_LEVEL");

	if (e != NULL)
		gChkForceLevel = atoi(e);

	e = getenv("CHK_FORCE_MENU");

	if (e != NULL)
		gChkForceMenu = (atoi(e) != 0);
}

int chkCarSelectPickCity(void)
{
	return chkImportLocalPickCity();
}

int chkCarSelectPickModel(void)
{
	return chkImportLocalPickModel();
}

void chkCarSelectRegister(JERICHO_CONTEXT* ctx)
{
	memset(gChkItems, 0, sizeof(gChkItems));

	/* < CAR > */
	gChkItems[CHK_ROW_CAR].get_label = chkCarLabel;
	gChkItems[CHK_ROW_CAR].userdata = (void*)(size_t)CHK_ROW_CAR;
	gChkItems[CHK_ROW_CAR].on_adjust = chkSelAdjust;
	gChkItems[CHK_ROW_CAR].submenu = -1;

	/* < CITY > - the extra row, below the car row */
	gChkItems[CHK_ROW_CITY].get_label = chkCityLabel;
	gChkItems[CHK_ROW_CITY].userdata = (void*)(size_t)CHK_ROW_CITY;
	gChkItems[CHK_ROW_CITY].on_adjust = chkSelAdjust;
	gChkItems[CHK_ROW_CITY].submenu = -1;

	/* Ride */
	gChkItems[CHK_ROW_RIDE].label = "Ride";
	gChkItems[CHK_ROW_RIDE].userdata = (void*)(size_t)CHK_ROW_RIDE;
	gChkItems[CHK_ROW_RIDE].on_activate = chkSelActivate;
	gChkItems[CHK_ROW_RIDE].submenu = -1;

	/* Back */
	gChkItems[CHK_ROW_BACK].label = "Back";
	gChkItems[CHK_ROW_BACK].userdata = (void*)(size_t)CHK_ROW_BACK;
	gChkItems[CHK_ROW_BACK].on_activate = chkSelActivate;
	gChkItems[CHK_ROW_BACK].submenu = -1;

	gChkMenu.get_preview = chkSelPreview;

	/* Triangle, from any row: the same back the Back row performs. */
	gChkMenu.on_back = chkSelBack;

	chkReadHarness();
	chkClampCursor();

	/* register the menu once; the registry is cleared on every reload, so this
	 * runs again on a re-activation and must stay idempotent */
	jer_frontend_register_menu(&gChkMenu);

	/* the intercept + the frontend re-entry reset */
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, chkSelOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, chkSelOnFrontendEntered, NULL, 0);

	ctx->jer_log(ctx, "[carhacks] car select: menu '%s' registered (%d rows: car, city, ride, back; %s)\n",
		gChkMenu.id, gChkMenu.item_count,
		(gChkForceCity >= 0 || gChkForceCar >= 0 || gChkForceMenu) ? "harness overrides set" : "no harness overrides");
}

const char* chkCarSelectMenuId(void)
{
	return gChkMenu.id;
}
