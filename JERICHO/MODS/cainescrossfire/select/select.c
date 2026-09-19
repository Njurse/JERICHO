// select/select.c — the CC select flow (see select.h).
//
// Stages: 0 = idle, 1 = choose an arena (city), 2 = choose a vehicle (a
// carousel over every registered profile). On the vehicle confirm it sets
// GameLevel + the player's profile + wantedCar and starts the match with
// SetState(STATE_GAMESTART), exactly as the stock frontend start does.
//
// Drawn through the shared JERICHO menu look (jer_menu_draw) from the FRAME
// hook, so it needs no engine-side screen of its own and matches levelhacks'
// approach (a module owning the frontend for a beat).

#include "driver2.h"

#include "jericho.h"
#include "jer_events.h"
#include "jer_menu.h"

#include "players.h"
#include "main.h"
#include "pad.h"
#include "mission.h"		/* GameLevel, wantedCar */
#include "state.h"		/* SetState, STATE_GAMESTART */
#include "system.h"		/* LevelNames[] */

#include "cainescrossfire.h"
#include "profiles/profile.h"

#include "select/select.h"

#include <string.h>
#include <stdlib.h>

extern int wantedCar[2];		/* the player's chosen car model per player */

static int gCcForced;			/* -ccmenu / CC_MENU seen this run */
static int gCcStage;			/* 0 idle, 1 arena, 2 vehicle */
static int gCcCity;			/* chosen arena (LevelNames index) */
static JerMenu gCcMenu;

static const char* const gCcArenas[] =
{
	"Chicago", "Havana", "Las Vegas", "Rio"
};

static const char* gCcVehicles[CD2_VEH_COUNT];	/* filled from the profiles */

int cd2SelectActive(void)
{
	return gCcStage != 0;
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

// JER_EVENT_FRONTEND — the take-a-ride city confirm: while the CC flow owns the
// start, freeze the frontend so our menu takes the pad.
static int cd2SelOnFrontend(void* ud, void* args)
{
	JER_ARGS_FRONTEND* a = (JER_ARGS_FRONTEND*)args;

	(void)ud;

	if (!gCcForced)
		return JER_RESULT_CONTINUE;

	a->defer = 1;		/* the frontend freezes; we start the match ourselves */

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_FRAME — drive + draw the menus while the frontend is frozen.
static int cd2SelOnFrame(void* ud, void* args)
{
	int pad, padNew;
	int i;

	(void)ud;
	(void)args;

	// -ccmenu raises the flow directly, so it does not depend on the player
	// reaching the take-a-ride confirm (which then just freezes the frontend).
	if (gCcForced && gCcStage == 0)
	{
		for (i = 0; i < CD2_VEH_COUNT; i++)
			gCcVehicles[i] = cd2VehDisplayName(i);

		gCcStage = 1;
		jer_menu_list(&gCcMenu, "SELECT ARENA", gCcArenas, 4);

		printInfo("[cainescrossfire] CC select: arena menu up\n");
	}

	if (gCcStage == 0)
		return JER_RESULT_CONTINUE;

	pad = Pads[0].mapped;
	padNew = Pads[0].mapnew;

	if (jer_menu_tick(&gCcMenu, MPAD_D_UP, MPAD_D_DOWN, MPAD_D_LEFT, MPAD_D_RIGHT,
			  MPAD_CROSS, pad, padNew))
	{
		if (gCcStage == 1)
		{
			gCcCity = gCcMenu.cursor;
			gCcStage = 2;		// arena -> vehicle
			jer_menu_carousel(&gCcMenu, "SELECT VEHICLE", gCcVehicles, CD2_VEH_COUNT);
			Pads[0].mapnew = 0;
		}
		else if (gCcStage == 2)
		{
			int profile = gCcMenu.cursor;
			const CD2_VEH_PROFILE* p = cd2VehDef(profile);

			GameLevel = gCcCity;
			cd2VehSetPlayerProfile(profile);

			if (p != NULL && p->modelSlot >= 0)
				wantedCar[0] = p->modelSlot;

			gCcStage = 0;
			gCcForced = 0;

			printInfo("[cainescrossfire] CC select: start %s with %s (model %d)\n",
				LevelNames[gCcCity], cd2VehDisplayName(profile),
				(p != NULL) ? p->modelSlot : -1);

			SetState(STATE_GAMESTART);
			return JER_RESULT_CONTINUE;
		}
	}

	// the shared JERICHO menu look, centred near the top of the frame
	jer_menu_draw(&gCcMenu, 320, 210);

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_GAME_START — a level is starting: the flow is done for this round.
static int cd2SelOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	gCcStage = 0;

	return JER_RESULT_CONTINUE;
}

void cd2SelectRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CMDLINE, cd2SelOnCmdline, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND, cd2SelOnFrontend, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2SelOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2SelOnGameStart, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] CC select flow registered (-ccmenu)\n");
}
