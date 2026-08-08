/*
 * sandbox.c — JERICHO sample package: no-damage, time-scale, and the
 * Sandbox Menu (a pause-menu overlay that keeps the world running).
 *
 * Demonstrates the full module surface with ZERO vanilla edits:
 *   - hooks   : JER_EVENT_FRAME, JER_EVENT_DRAW_OVERLAY, JER_EVENT_PAUSE_MENU
 *   - override: JER_OVERRIDE_SLOT_SIM (the world step) for time-scale
 *   - globals : externs into the game (car_data, player[], InitCar)
 */
#include "jericho.h"
#include "jer_events.h"

#include "driver2.h"
#include "cars.h"
#include "players.h"
#include "civ_ai.h"
#include "pres.h"
#include "system.h"
#include "mission.h"
#include "sky.h"
#include "debris.h"
#include "models.h"
#include "camera.h"
#include "draw.h"
#include "glaunch.h"
#include "felony.h"
#include "pad.h"
#include "main.h"
#include "map.h"
#include "engine/mdl.h"
#include "engine/cell.h"

#include <string.h>
#include <stdio.h>

/* custom event: firing this toggles no-damage */
#define SANDBOX_CUSTOM_TOGGLE (JER_EVENT_MODULE_CUSTOM + 10)

static JERICHO_CONTEXT* gCtx;
static int gFrameCount;
static int gNoDamage;
static int gTimeScale = 4096;	/* 4096 = 1.0x */
static void (*gOriginalStepSim)(void);

extern void StepSim(void);
extern void SetRightWayUp(int direction);

/* cars.c exports these but does not declare them in cars.h */
extern void DrawCarObject(CAR_MODEL* car, MATRIX* matrix, VECTOR* pos, int palette, CAR_DATA* cp, int detail);
extern void DrawCarWheels(CAR_DATA* cp, MATRIX* rearMatrix, VECTOR* pos, int zclip);
extern DENTUVS* gTempCarUVPtr;

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
/* Spawning helpers                                                    */
/* ------------------------------------------------------------------ */

/* spawn a car with the given internal model in front of the player */
static int SandboxSpawnCarModel(int model)
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

	InitCar(pNewCar, direction, &pos, CONTROL_TYPE_CIV_AI, model, 0, (char*)&civDat);

	return 0;
}

/* find a valid packed-cell slot for spawned objects (damage_object
 * dereferences pcoplist[pad]) */
static int SandboxFindPcopIndex(void)
{
	int i;

	for (i = 0; i < 4096; i++)
	{
		if (pcoplist[i] != NULL)
			return i;
	}

	return -1;
}

/* spawn a smashable object (level model) in front of the player */
static int SandboxSpawnObject(int modelIdx)
{
	CAR_DATA* pc;
	CELL_OBJECT cop;
	VECTOR vel;
	int pad;

	if (player[0].playerCarId < 0)
		return -1;

	pc = &car_data[player[0].playerCarId];

	if (pc->controlType == CONTROL_TYPE_NONE)
		return -1;

	pad = SandboxFindPcopIndex();

	if (pad < 0)
		return -1;

	memset(&cop, 0, sizeof(cop));

	cop.pos.vx = pc->hd.where.t[0] + FIXEDH(RSIN(pc->hd.direction) * 2000);
	cop.pos.vy = pc->hd.where.t[1] - 800;
	cop.pos.vz = pc->hd.where.t[2] + FIXEDH(RCOS(pc->hd.direction) * 2000);
	cop.type = (u_short)modelIdx;
	cop.pad = (u_char)pad;

	vel.vx = 0;
	vel.vy = 200;
	vel.vz = 0;

	damage_object(&cop, &vel);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Sandbox overlay menu                                                */
/* ------------------------------------------------------------------ */

#define SBX_ITEMS 11

enum
{
	SBX_SPAWN_CAR = 0,
	SBX_SPAWN_OBJECT,
	SBX_SPAWN_AI_CAR,
	SBX_REPAIR_CAR,
	SBX_MAKE_UPRIGHT,
	SBX_SET_DAMAGE,
	SBX_SET_FELONY,
	SBX_SET_TOD,
	SBX_SET_WEATHER,
	SBX_SET_CHEATS,
	SBX_CLOSE
};

static const char* const gSandboxItems[SBX_ITEMS] = {
	"Spawn Car",
	"Spawn Object",
	"Spawn AI Car",
	"Repair Car",
	"Make Car Upright",
	"Set Damage",
	"Set Felony",
	"Set Time of Day",
	"Set Weather",
	"Set Cheats",
	"Close"
};

static int gSandboxMenuOpen;
static int gSandboxPage;		/* 0 = main, 1 = spawn-object list */
static int gSandboxCursor;		/* main-page cursor */
static int gSandboxObjectCursor;	/* object-list cursor */
static int gSandboxAdjust;		/* 1 = adjusting a SET item */
static int gSandboxPreviewModel;	/* car model shown in the preview (future car page) */

/* the level-object list for the spawn page (smashable[] with a name) */
#define SBX_SMASHABLE_COUNT 37	/* sizeof(smashable)/sizeof(smashable[0]) in objanim.c */

static int gSandboxObjectCount;
static int gSandboxObjectIdx[SBX_SMASHABLE_COUNT];
static const char* gSandboxObjectNames[SBX_SMASHABLE_COUNT];

static void SandboxMenuClose(void)
{
	gSandboxMenuOpen = 0;
	gSandboxAdjust = 0;
	gStopPadReads = 0;
}

static CAR_DATA* SandboxPlayerCar(void)
{
	if (player[0].playerCarId < 0)
		return NULL;

	if (car_data[player[0].playerCarId].controlType == CONTROL_TYPE_NONE)
		return NULL;

	return &car_data[player[0].playerCarId];
}

/* rebuild the object list from the level's smashable table */
static void SandboxMenuBuildObjectList(void)
{
	int i;

	gSandboxObjectCount = 0;

	/* skip entry 0 (the unnamed default) */
	for (i = 1; i < SBX_SMASHABLE_COUNT; i++)
	{
		if (smashable[i].name != NULL && smashable[i].name[0] != 0)
		{
			gSandboxObjectIdx[gSandboxObjectCount] = i;
			gSandboxObjectNames[gSandboxObjectCount] = smashable[i].name;
			gSandboxObjectCount++;
		}
	}
}

static void SandboxMenuRepair(void)
{
	CAR_DATA* pc = SandboxPlayerCar();

	if (pc == NULL)
		return;

	pc->totalDamage = 0;
	pc->ap.needsDenting = 0;
	memset(pc->ap.damage, 0, sizeof(pc->ap.damage));

	/* tell the crumple package to reset its per-car state */
	jer_fire(JER_EVENT_RESET_CAR, &player[0].playerCarId);
}

static void SandboxMenuSetFelony(int delta)
{
	CAR_DATA* pc = SandboxPlayerCar();

	if (pc != NULL)
		pc->felonyRating += delta;
	else if (player[0].playerCarId < 0)
		pedestrianFelony += delta;
}

static void SandboxMenuDoAction(int item)
{
	CAR_DATA* pc = SandboxPlayerCar();

	switch (item)
	{
	case SBX_SPAWN_CAR:
	{
		CAR_DATA* pc = SandboxPlayerCar();

		/* spawn a copy of the player's own car model */
		SandboxSpawnCarModel(pc != NULL ? pc->ap.model : 0);
		break;
	}

	case SBX_SPAWN_OBJECT:
		SandboxMenuBuildObjectList();
		gSandboxObjectCursor = 0;
		gSandboxPage = 1;
		break;

	case SBX_SPAWN_AI_CAR:
		SandboxSpawnCarModel(1);
		break;

	case SBX_REPAIR_CAR:
		SandboxMenuRepair();
		break;

	case SBX_MAKE_UPRIGHT:
		SetRightWayUp(1);
		break;

	case SBX_SET_DAMAGE:
	case SBX_SET_FELONY:
	case SBX_SET_TOD:
	case SBX_SET_WEATHER:
	case SBX_SET_CHEATS:
		gSandboxAdjust = 1;
		break;

	case SBX_CLOSE:
	default:
		SandboxMenuClose();
		break;
	}

	(void)pc;
}

/* pad input for the overlay menu (world keeps running underneath) */
static void SandboxMenuInput(int padnew)
{
	if (gSandboxPage == 1)
	{
		if (padnew & MPAD_TRIANGLE)
		{
			gSandboxPage = 0;
			return;
		}

		if (padnew & (MPAD_D_UP | MPAD_D_LEFT))
		{
			if (gSandboxObjectCursor > 0)
				gSandboxObjectCursor--;
		}

		if (padnew & (MPAD_D_DOWN | MPAD_D_RIGHT))
		{
			if (gSandboxObjectCursor < gSandboxObjectCount - 1)
				gSandboxObjectCursor++;
		}

		if (padnew & MPAD_CROSS)
		{
			int idx = gSandboxObjectIdx[gSandboxObjectCursor];
			int modelIdx = FindModelIdxWithName(smashable[idx].name);

			SandboxSpawnObject(modelIdx);
		}

		return;
	}

	if (padnew & MPAD_TRIANGLE)
	{
		SandboxMenuClose();
		return;
	}

	if (gSandboxAdjust)
	{
		/* adjusting a SET item: left/right change the value, cross exits */
		CAR_DATA* pc = SandboxPlayerCar();

		if (padnew & MPAD_D_LEFT)
		{
			switch (gSandboxCursor)
			{
			case SBX_SET_DAMAGE:
				if (pc != NULL && pc->totalDamage >= 512)
					pc->totalDamage -= 512;
				break;
			case SBX_SET_FELONY:
				SandboxMenuSetFelony(-64);
				break;
			case SBX_SET_TOD:
				if (gTimeOfDay > TIME_DAWN)
				{
					gTimeOfDay--;
					wantedTimeOfDay = gTimeOfDay;
					LoadSky();
				}
				break;
			case SBX_SET_WEATHER:
				if (gWeather > WEATHER_NONE)
					gWeather--;
				wantedWeather = gWeather;
				break;
			case SBX_SET_CHEATS:
				ActiveCheats.cheat1 = 0;
				break;
			}
		}

		if (padnew & MPAD_D_RIGHT)
		{
			switch (gSandboxCursor)
			{
			case SBX_SET_DAMAGE:
				if (pc != NULL)
					pc->totalDamage += 512;
				break;
			case SBX_SET_FELONY:
				SandboxMenuSetFelony(64);
				break;
			case SBX_SET_TOD:
				if (gTimeOfDay < TIME_NIGHT)
				{
					gTimeOfDay++;
					wantedTimeOfDay = gTimeOfDay;
					LoadSky();
				}
				break;
			case SBX_SET_WEATHER:
				if (gWeather < WEATHER_WET)
					gWeather++;
				wantedWeather = gWeather;
				break;
			case SBX_SET_CHEATS:
				ActiveCheats.cheat1 = 1;
				break;
			}
		}

		if (padnew & (MPAD_CROSS | MPAD_TRIANGLE))
			gSandboxAdjust = 0;

		return;
	}

	if (padnew & MPAD_D_UP)
	{
		if (gSandboxCursor > 0)
			gSandboxCursor--;
		else
			gSandboxCursor = SBX_ITEMS - 1;
	}

	if (padnew & MPAD_D_DOWN)
	{
		if (gSandboxCursor < SBX_ITEMS - 1)
			gSandboxCursor++;
		else
			gSandboxCursor = 0;
	}

	if (padnew & MPAD_CROSS)
		SandboxMenuDoAction(gSandboxCursor);
}

/* ------------------------------------------------------------------ */
/* Frame hook: no-damage + sandbox-menu input                          */
/* ------------------------------------------------------------------ */

static int SandboxOnFrame(void* userdata, void* args)
{
	CAR_DATA* playerCar;
	int z;

	(void)userdata;
	(void)args;

	gFrameCount++;

	if (gSandboxMenuOpen)
		SandboxMenuInput(Pads[0].mapnew);

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
/* Preview: the player's car, drawn with the FINAL geometry            */
/* (crumple-deformed verts + damage UVs) and a turntable camera,       */
/* straight into the display buffer like the pause menu                */
/* ------------------------------------------------------------------ */

static void SandboxDrawPreview(void)
{
	CAR_DATA* cp = SandboxPlayerCar();
	CAR_MODEL* CarModelPtr;
	MODEL* objModel;
	MATRIX turntable;
	VECTOR pos;
	int angle;
	OTTYPE* savedOt;

	/* draw the preview into the UI layer (near the 2D text, over the
	 * world) by shifting the render context's OT base for the call */
	savedOt = current->ot;
	current->ot = savedOt - 26;

	if (cp == NULL && gSandboxPage == 0)
	{
		current->ot = savedOt;
		return;		/* on foot: no car preview */
	}

	if (gSandboxPage == 1)
	{
		/* object preview: the level object model */
		int idx = gSandboxObjectIdx[gSandboxObjectCursor];
		int modelIdx = FindModelIdxWithName(smashable[idx].name);

		objModel = modelpointers[modelIdx];

		if (objModel == NULL)
		{
			current->ot = savedOt;
			return;
		}

		InitMatrix(turntable);
		angle = (gFrameCount * 24) & 4095;
		RotMatrixY(angle, &turntable);

		pos.vx = 0;
		pos.vy = -200;
		pos.vz = 1400;

		gte_SetRotMatrix(&turntable);
		gte_SetTransVector(&pos);

		RenderModel(objModel, NULL, NULL, 0, PLOT_NO_CULL, 1, 0);

		current->ot = savedOt;
		return;
	}

	if (cp == NULL)
	{
		current->ot = savedOt;
		return;
	}

	InitMatrix(turntable);
	angle = (gFrameCount * 24) & 4095;
	RotMatrixY(angle, &turntable);

	pos.vx = 0;
	pos.vy = -250;
	pos.vz = 1500;

	/* the same final-geometry setup DrawCar uses */
	CarModelPtr = &NewCarModel[cp->ap.model];
	CarModelPtr->vlist = gTempCarVertDump[cp->id];
	CarModelPtr->nlist = gTempCarVertDump[cp->id];
	gTempCarUVPtr = gTempHDCarUVDump[cp->id];

	DrawCarObject(CarModelPtr, &turntable, &pos, cp->ap.palette, cp, 1);
	DrawCarWheels(cp, &turntable, &pos, 0);

	current->ot = savedOt;
}

/* ------------------------------------------------------------------ */
/* Draw-overlay hook: the menu 2D text + the live preview              */
/* ------------------------------------------------------------------ */

static int SandboxOnDrawOverlay(void* userdata, void* args)
{
	int i;
	char text[64];
	CAR_DATA* pc;

	(void)userdata;
	(void)args;

	if (!gSandboxMenuOpen)
		return JER_RESULT_CONTINUE;

	pc = SandboxPlayerCar();

	if (gSandboxPage == 1)
	{
		/* spawn-object list (paginated) */
		int scroll = (gSandboxObjectCursor / 12) * 12;
		int shown = gSandboxObjectCount - scroll;

		SetTextColour(255, 255, 255);
		PrintString("SPAWN OBJECT", 10, 80);
		PrintString("(Triangle: back)", 10, 92);

		if (shown > 12)
			shown = 12;

		for (i = 0; i < shown; i++)
		{
			sprintf(text, "%s %s", i == gSandboxObjectCursor - scroll ? ">" : " ", gSandboxObjectNames[scroll + i]);
			SetTextColour(i == gSandboxObjectCursor - scroll ? 255, 255, 0 : 255, 255, 255);
			PrintString(text, 10, 108 + i * 12);
		}

		SandboxDrawPreview();
		return JER_RESULT_CONTINUE;
	}

	/* header + current-vehicle readouts */
	SetTextColour(255, 255, 0);
	PrintString("SANDBOX", 10, 74);

	if (pc != NULL)
	{
		sprintf(text, "DAMAGE: %d", pc->totalDamage);
		SetTextColour(255, 255, 255);
		PrintString(text, 10, 86);

		sprintf(text, "FELONY: %d", pc->felonyRating);
		PrintString(text, 10, 96);
	}
	else
	{
		PrintString("(on foot)", 10, 86);
	}

	/* items */
	for (i = 0; i < SBX_ITEMS; i++)
	{
		sprintf(text, "%s %s", i == gSandboxCursor ? ">" : " ", gSandboxItems[i]);

		if (i == gSandboxCursor)
			SetTextColour(255, 255, 0);
		else if (gSandboxAdjust && i == gSandboxCursor)
			SetTextColour(0, 255, 255);
		else
			SetTextColour(255, 255, 255);

		PrintString(text, 10, 112 + i * 12);
	}

	SandboxDrawPreview();
	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Pause-menu hook: the 'Sandbox Menu' item                           */
/* ------------------------------------------------------------------ */

static int SandboxOnPauseMenu(void* userdata, void* args)
{
	JER_ARGS_PAUSE_MENU* jerArgs = (JER_ARGS_PAUSE_MENU*)args;

	(void)userdata;

	if (jerArgs->action == JER_PAUSE_SANDBOX_GET_LABEL)
	{
		jerArgs->result = (void*)"Sandbox Menu";
		return JER_RESULT_CONTINUE;
	}

	if (jerArgs->action == JER_PAUSE_SANDBOX_OPEN)
	{
		gSandboxMenuOpen = 1;
		gSandboxPage = 0;
		gSandboxCursor = 0;
		gSandboxAdjust = 0;
		gStopPadReads = 1;	/* the pad drives the menu, not the car */

		return JER_RESULT_STOP;	/* claim the press: unpause + world runs */
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
		"0.2.0",			/* version */
		"DeepSeek",			/* author */
		"Demo package: no-damage, time-scale (world-step override), and the Sandbox Menu overlay.",	/* description */
		"",					/* dependencies */
		JERICHO_SDK_VERSION);	/* SDK this module was built against */

	gOriginalStepSim = (void (*)(void))jer_get_override(JER_OVERRIDE_SLOT_SIM);
	ctx->jer_override(ctx, JER_OVERRIDE_SLOT_SIM, (void*)SandboxStepSim);

	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, SandboxOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, SandboxOnDrawOverlay, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PAUSE_MENU, SandboxOnPauseMenu, NULL, 0);
	ctx->jer_register_hook(ctx, SANDBOX_CUSTOM_TOGGLE, SandboxOnToggle, NULL, 0);

	ctx->jer_log(ctx, "[sandbox] registered — no-damage ON, time-scale 1.0x\n");
}
