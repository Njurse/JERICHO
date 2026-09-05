/*
 * levelhacks — Take-a-Ride level hacks.
 *
 * Hooks the frontend's take-a-ride city confirm (JER_EVENT_FRONTEND) and
 * asks Singleplayer or Multiplayer. Singleplayer continues the stock city
 * start; Multiplayer shows the four cities' multiplayer maps and starts the
 * chosen map in SINGLEPLAYER take-a-ride.
 *
 * The small multiplayer maps (MLEVELS/MNLEVELS) are selected by the mission
 * file's `region` field (region != 0 -> gMultiplayerLevels -> CITYTYPE_MULTI),
 * and LoadMission() recomputes that flag from the mission header — so setting
 * gMultiplayerLevels in the frontend is overwritten. Instead the mod swaps the
 * mission number at JER_EVENT_LEVEL_LAUNCH (fired after State_GameStart has
 * finalised it): single-player take-a-ride missions M50..M57 are the full
 * city, and M58..M65 are the same city/night as the small multiplayer map.
 * NumPlayers stays 1, so the small levels are testable without a second pad.
 */

#include "driver2.h"

#include "jericho.h"
#include "jer_events.h"
#include "jer_menu.h"

#include "players.h"
#include "main.h"
#include "pad.h"
#include "draw.h"
#include "mission.h"
#include "state.h"
#include "pres.h"

#include <string.h>
#include <stdio.h>

static JerMenu gLhMenu;
static int gLhStage;	/* 0 = idle, 1 = SP/MP prompt, 2 = MP map list */
static int gLhDone;
static int gLhUseMp;	/* pending: launch the take-a-ride MP-map mission */

static int LevelhacksOnDraw(void* userdata, void* args);	/* the mod started the game: don't re-defer this round */

static const char* const gLhPromptItems[] = {
	"Singleplayer",
	"Multiplayer"
};

static const char* const gLhMpItems[] = {
	"MP Chicago",
	"MP Havana",
	"MP Las Vegas",
	"MP Rio"
};

/* JER_EVENT_FRONTEND — the take-a-ride city confirm: intercept it */
static int LevelhacksOnFrontend(void* userdata, void* args)
{
	JER_ARGS_FRONTEND* a = (JER_ARGS_FRONTEND*)args;

	(void)userdata;

	if (a->gameType != GAME_TAKEADRIVE || gLhDone)
		return JER_RESULT_CONTINUE;

	/* defer on EVERY confirm until the mod starts the game — the stock
	 * take-a-ride would otherwise slip through on the next CROSS */
	if (gLhStage == 0)
	{
		jer_menu_begin(&gLhMenu, gLhPromptItems, 2);
		gLhStage = 1;
	}

	a->defer = 1;	/* the frontend freezes; the mod starts the game */

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_FRAME — drive the menu + start the game once a choice is made */
static int LevelhacksOnFrame(void* userdata, void* args)
{
	int pad = Pads[0].mapped;
	int padNew = Pads[0].mapnew;

	(void)userdata;
	(void)args;

	if (gLhStage == 0 || gLhDone)
		return JER_RESULT_CONTINUE;

	LevelhacksOnDraw(userdata, args);

	if (gLhStage == 1)
	{
		if (jer_menu_update(&gLhMenu, MPAD_D_UP, MPAD_D_DOWN, MPAD_CROSS, pad, padNew))
		{
			if (gLhMenu.cursor == 0)
			{
				/* Singleplayer: the stock city take-a-ride */
				gLhDone = 1;
				gLhStage = 0;
				SetState(STATE_GAMESTART);
			}
			else
			{
				/* Multiplayer: pick one of the maps */
				jer_menu_begin(&gLhMenu, gLhMpItems, 4);
				gLhStage = 2;
			}
		}
	}
	else if (gLhStage == 2)
	{
		if (jer_menu_update(&gLhMenu, MPAD_D_UP, MPAD_D_DOWN, MPAD_CROSS, pad, padNew))
		{
			/* the multiplayer map, in singleplayer take-a-ride: remember to
			 * swap the mission for the multiplayer-map variant at launch */
			GameLevel = gLhMenu.cursor;
			NumPlayers = 1;
			gLhUseMp = 1;

			gLhDone = 1;
			gLhStage = 0;
			SetState(STATE_GAMESTART);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_DRAW_OVERLAY — the menu text */
static int LevelhacksOnDraw(void* userdata, void* args)
{
	int i;

	(void)userdata;
	(void)args;

	if (gLhStage == 0)
		return JER_RESULT_CONTINUE;

	SetTextColour(255, 255, 0);
	PrintString(gLhStage == 1 ? "Take a Ride:" : "Multiplayer Map:", 20, 80);

	SetTextColour(255, 255, 255);

	for (i = 0; i < gLhMenu.count; i++)
	{
		char line[40];

		sprintf(line, "%s%s", i == gLhMenu.cursor ? "> " : "  ", gLhMenu.items[i]);
		PrintString(line, 24, 92 + i * 12);
	}

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_GAME_START — a level is starting: re-arm the prompt for the
 * next visit to the frontend */
static int LevelhacksOnGameStart(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	gLhStage = 0;
	gLhDone = 0;

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_LEVEL_LAUNCH — the pending mission number is finalised but the
 * level hasn't loaded yet. If an MP map is pending, swap to the take-a-ride
 * multiplayer-map mission: the single-player variants are M50..M57 (full
 * city, region == 0) and the multiplayer variants are M58..M65 (the small
 * MLEVELS map, region != 0). +8 maps M50+city*2+night -> M58+city*2+night
 * for the same city/night. NumPlayers stays 1, so it is still single-player. */
static int LevelhacksOnLevelLaunch(void* userdata, void* args)
{
	JER_ARGS_LEVEL_LAUNCH* a = (JER_ARGS_LEVEL_LAUNCH*)args;

	(void)userdata;

	if (!gLhUseMp)
		return JER_RESULT_CONTINUE;

	gLhUseMp = 0;

	if (a->gameType != GAME_TAKEADRIVE || a->numPlayers != 1)
		return JER_RESULT_CONTINUE;

	a->missionNumber += 8;

	return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_levelhacks_entry)(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND, LevelhacksOnFrontend, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, LevelhacksOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, LevelhacksOnDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, LevelhacksOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_LEVEL_LAUNCH, LevelhacksOnLevelLaunch, NULL, 0);
}
