/*
 * driver1 — Driver 1 cities in Take-A-Ride.
 *
 * Adds a "Driver 1 Cities…" entry to the take-a-ride city screen (only when
 * the Driver 1 assets are present) that opens a second page listing the
 * four Driver 1 cities — Miami, San Francisco, Los Angeles, New York.
 * Confirming one starts take-a-ride with GameLevel 4-7; the engine's
 * gDriver1Level flag (set in State_GameStart from GameLevel >= 4) then
 * routes the whole level load through the jer_d1 library.
 *
 * The engine changes are thin hook points only; every Driver-1-specific
 * decision lives here or in the jer_d1 library.
 */

#include "driver2.h"

#include "jericho.h"
#include "jer_events.h"
#include "jer_d1.h"

#include "mission.h"	/* GameType */
#include "glaunch.h"	/* gWantNight */
#include "main.h"	/* gDriver1Level, gWantNight */

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* frontend screen structs (FEmain.c locals — mirrored so the module   */
/* can inject a button into the live take-a-ride city screen)          */
/* ------------------------------------------------------------------ */

typedef struct D1PSXBUTTON
{
	short x, y, w, h;
	u_char l, r;
	u_char u, d;
	int userDrawFunctionNum;
	short s_x, s_y;
	int action, var;
	char Name[32];
} D1PSXBUTTON;

typedef struct D1PSXSCREEN
{
	u_char index;
	u_char numButtons;
	u_char userFunctionNum;
	D1PSXBUTTON buttons[8];
} D1PSXSCREEN;

#define D1_FE_MAKEVAR(code, value)	(((code & 0xffff) << 8) | (value & 0xff))
#define D1_BTN_NEXT_SCREEN	1
#define D1_BTN_START_GAME	2
#define D1_BTN_PREVIOUS_SCREEN	4

#define D1_SCREEN_SLOT		42	/* must match JERICHO_D1_SCREEN in FEmain.c */
#define D1_ENTRY_NAME		"Driver 1 Cities"

/* ------------------------------------------------------------------ */
/* availability + injection                                            */
/* ------------------------------------------------------------------ */

static int gD1Available = -1;

static int Driver1Available(void)
{
	if (gD1Available < 0)
	{
		gD1Available = jer_d1_available();
		printInfo("[driver1] Driver 1 data %s\n", gD1Available ? "found" : "NOT found");
	}

	return gD1Available;
}

/* JER_EVENT_FRONTEND_SCREEN — the take-a-ride city screen was set up.
 * Inject the "Driver 1 Cities…" entry as the last button and rewire the
 * navigation ring (first city's up and last city's down wrap through it). */
static int Driver1OnFrontendScreen(void* userdata, void* args)
{
	JER_ARGS_FRONTEND_SCREEN* a = (JER_ARGS_FRONTEND_SCREEN*)args;
	D1PSXSCREEN* scr;
	int numButtons;
	int idx;

	(void)userdata;

	if (a == NULL || a->screen == NULL || !a->bSetup)
		return JER_RESULT_CONTINUE;

	if (GameType != GAME_TAKEADRIVE)
		return JER_RESULT_CONTINUE;

	if (!Driver1Available())
		return JER_RESULT_CONTINUE;

	scr = (D1PSXSCREEN*)a->screen;
	numButtons = scr->numButtons;

	if (numButtons < 1 || numButtons >= 8)
		return JER_RESULT_CONTINUE;

	/* already injected? */
	idx = numButtons - 1;

	if (scr->buttons[idx].var == -1 &&
		(scr->buttons[idx].action >> 8) == D1_BTN_NEXT_SCREEN &&
		strncmp(scr->buttons[idx].Name, D1_ENTRY_NAME, 8) == 0)
	{
		return JER_RESULT_CONTINUE;
	}

	/* append the entry below the last city */
	idx = numButtons;

	scr->buttons[idx] = scr->buttons[numButtons - 1];	/* copy geometry */
	strcpy(scr->buttons[idx].Name, D1_ENTRY_NAME);
	scr->buttons[idx].y = (short)(scr->buttons[numButtons - 1].y + 40);
	scr->buttons[idx].u = (u_char)numButtons;		/* up -> last city */
	scr->buttons[idx].d = 1;				/* down -> first city */
	scr->buttons[idx].action = D1_FE_MAKEVAR(D1_BTN_NEXT_SCREEN, D1_SCREEN_SLOT);
	scr->buttons[idx].var = -1;

	/* navigation ring: first city's up and previous-last city's down now
	 * wrap through the D1 entry (u/d are "button index + 1") */
	scr->buttons[0].u = (u_char)(idx + 1);
	scr->buttons[numButtons - 1].d = (u_char)(idx + 1);

	scr->numButtons = (u_char)(idx + 1);

	if (a->numButtons < scr->numButtons)
		a->numButtons = scr->numButtons;

	printInfo("[driver1] take-a-ride city screen: injected '%s' at button %d\n",
		D1_ENTRY_NAME, idx);

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_LEVELFILE — SetCityType resolved the level path. Driver 1
 * cities live under their own data folder (DRIVER\ by default), so rewrite
 * the path for GameLevel 4-7. If the D1 data is missing, veto the load. */
static int Driver1OnLevelFile(void* userdata, void* args)
{
	JER_ARGS_LEVELFILE* a = (JER_ARGS_LEVELFILE*)args;

	(void)userdata;

	if (a == NULL || a->gameLevel < 4 || a->gameLevel >= 4 + JER_D1_CITY_COUNT)
		return JER_RESULT_CONTINUE;

	jer_d1_log("[d1] LEVELFILE hook: level=%d\n", a->gameLevel);

	if (!Driver1Available())
	{
		jer_d1_log("[d1] LEVELFILE veto: D1 data missing\n");
		a->result = -1;	/* veto: the caller should have bailed already */
		return JER_RESULT_CONTINUE;
	}

	snprintf(a->filename, a->filenameSize, "%s%s",
		jer_d1_folder(), jer_d1_level_file(a->gameLevel - 4));

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_FRONTEND — a city was confirmed. For D1 cities force the
 * conditions Driver 1 needs (single player, day). The engine sets
 * gDriver1Level itself in State_GameStart. */
static int Driver1OnFrontend(void* userdata, void* args)
{
	JER_ARGS_FRONTEND* a = (JER_ARGS_FRONTEND*)args;

	(void)userdata;

	if (a == NULL || a->gameLevel < 4)
		return JER_RESULT_CONTINUE;

	a->numPlayers = 1;
	gWantNight = 0;

	printInfo("[driver1] take-a-ride: Driver 1 city %d\n", a->gameLevel);

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_GAME_START — a level is starting; refresh the availability
 * gate (in case the data folder changed) and log the D1 mode. */
static int Driver1OnGameStart(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	gD1Available = -1;
	Driver1Available();

	if (gDriver1Level)
		printInfo("[driver1] Driver 1 level %d loading (folder '%s')\n",
			GameLevel, jer_d1_folder());

	return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_driver1_entry)(JERICHO_CONTEXT* ctx)
{
	(void)ctx;

	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_SCREEN, Driver1OnFrontendScreen, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND, Driver1OnFrontend, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_LEVELFILE, Driver1OnLevelFile, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, Driver1OnGameStart, NULL, 0);
}
