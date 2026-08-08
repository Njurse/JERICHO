/*
 * sandbox.c — JERICHO sample package: no-damage, time-scale, car spawning.
 *
 * Demonstrates the full module surface with ZERO vanilla edits:
 *   - hooks  : JER_EVENT_FRAME (+ a custom event toggle)
 *   - override: JER_OVERRIDE_SLOT_SIM (the world step) for time-scale
 *   - globals: externs into the game (car_data, player[], InitCar)
 */
#include "jericho.h"

#include "driver2.h"
#include "cars.h"
#include "players.h"
#include "civ_ai.h"

#include <string.h>

/* custom event: firing this toggles no-damage */
#define SANDBOX_CUSTOM_TOGGLE (JER_EVENT_MODULE_CUSTOM + 10)

static JERICHO_CONTEXT* gCtx;
static int gFrameCount;
static int gNoDamage;
static int gTimeScale = 4096;	/* 4096 = 1.0x */
static void (*gOriginalStepSim)(void);

extern void StepSim(void);

/* ------------------------------------------------------------------ */
/* Time-scale: replaces the world-step override slot                   */
/* ------------------------------------------------------------------ */

static void SandboxStepSim(void)
{
	if (gTimeScale <= 0)
		return;		/* world frozen; the game still renders */

	if (gTimeScale >= 4096 || (gFrameCount % (4096 / gTimeScale)) == 0)
	{
		if (gOriginalStepSim != NULL)
			gOriginalStepSim();
		else
			StepSim();
	}
}

/* ------------------------------------------------------------------ */
/* Spawn: find a free slot and init a civ car near the player          */
/* ------------------------------------------------------------------ */

static int SandboxSpawnCar(void)
{
	CAR_DATA* carCnt;
	CAR_DATA* pNewCar = NULL;
	EXTRA_CIV_DATA civDat;
	LONGVECTOR4 pos;
	CAR_DATA* pc;
	int direction;

	if (player[0].playerCarId < 0)
		return -1;

	pc = &car_data[player[0].playerCarId];

	if (pc->controlType == CONTROL_TYPE_NONE)
		return -1;

	carCnt = car_data;

	do
	{
		if (carCnt->controlType == CONTROL_TYPE_NONE)
		{
			pNewCar = carCnt;
			break;
		}

		carCnt++;
	} while (carCnt < &car_data[MAX_CARS]);

	if (pNewCar == NULL)
		return -1;

	direction = pc->hd.direction;

	/* a couple of car-lengths in front of the player */
	pos[0] = pc->hd.where.t[0] + FIXEDH(RSIN(direction) * 2500);
	pos[1] = pc->hd.where.t[1];
	pos[2] = pc->hd.where.t[2] + FIXEDH(RCOS(direction) * 2500);

	memset(&civDat, 0, sizeof(civDat));

	InitCar(pNewCar, direction, &pos, CONTROL_TYPE_CIV_AI, 1, 0, (char*)&civDat);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Frame hook: no-damage                                               */
/* ------------------------------------------------------------------ */

static int SandboxOnFrame(void* userdata, void* args)
{
	CAR_DATA* playerCar;
	int z;

	(void)userdata;
	(void)args;

	gFrameCount++;

	if (gNoDamage && player[0].playerCarId >= 0)
	{
		playerCar = &car_data[player[0].playerCarId];

		/* undo the frame's damage: keep the body pristine */
		playerCar->totalDamage = 0;
		playerCar->ap.needsDenting = 0;

		for (z = 0; z < 6; z++)
			playerCar->ap.damage[z] = 0;
	}

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Custom event: firing SANDBOX_CUSTOM_TOGGLE flips no-damage          */
/* ------------------------------------------------------------------ */

static int SandboxOnToggle(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	gNoDamage ^= 1;

	gCtx->jer_log(gCtx, "[sandbox] no-damage %s\n", gNoDamage ? "ON" : "OFF");
	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Module entry                                                        */
/* ------------------------------------------------------------------ */

JER_MODULE_ENTRY(jer_module_sandbox_entry)(JERICHO_CONTEXT* ctx)
{
	gCtx = ctx;
	gNoDamage = 1;

	ctx->jer_register_module(ctx,
		"sandbox",			/* id */
		"Sandbox",			/* name */
		"0.1.0",			/* version */
		"DeepSeek",			/* author */
		"Demo package: no-damage, time-scale (world-step override), periodic car spawns.",	/* description */
		"",					/* dependencies */
		JERICHO_SDK_VERSION);	/* SDK this module was built against */

	gOriginalStepSim = (void (*)(void))jer_get_override(JER_OVERRIDE_SLOT_SIM);
	ctx->jer_override(ctx, JER_OVERRIDE_SLOT_SIM, (void*)SandboxStepSim);

	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, SandboxOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, SANDBOX_CUSTOM_TOGGLE, SandboxOnToggle, NULL, 0);

	ctx->jer_log(ctx, "[sandbox] registered — no-damage ON, time-scale 1.0x\n");
}
