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
#include "jer_config.h"
#include "jer_pause_menu.h"

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
#include "overlay.h"
#include "pause.h"
#include "cop_ai.h"
#include "leadai.h"
#include "pedest.h"
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

/* spawn position for the spawn pages: 0 = in front, 1 = at the player's
 * position (warping the player into the car) */
static int gSandboxSpawnMode;

/* hold-to-repeat state for the left/right adjusters */
static int gSandboxHoldDir;		/* -1 left, 1 right, 0 none */
static int gSandboxHoldFrames;	/* frames the direction has been held */
static int gSandboxHoldInterval;	/* current repeat interval (frames) */

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
static int SandboxSpawnCarModel(int model, int palette)
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

	if (gSandboxSpawnMode == 1)
	{
		/* teleport-in: the new car replaces the player's, same spot */
		pos[0] = pc->hd.where.t[0];
		pos[1] = pc->hd.where.t[1];
		pos[2] = pc->hd.where.t[2];

		InitCar(pNewCar, direction, &pos, CONTROL_TYPE_CIV_AI, model, palette, NULL);
		ChangePedPlayerToCar(0, pNewCar);
		PingOutCar(pc);

		return 0;
	}

	/* a couple of car-lengths in front of the player */
	pos[0] = pc->hd.where.t[0] + FIXEDH(RSIN(direction) * 250);
	pos[1] = pc->hd.where.t[1];
	pos[2] = pc->hd.where.t[2] + FIXEDH(RCOS(direction) * 250);

	memset(&civDat, 0, sizeof(civDat));

	InitCar(pNewCar, direction, &pos, CONTROL_TYPE_CIV_AI, model, 0, (char*)&civDat);

	return 0;
}

/* find a valid packed-cell slot for spawned objects (damage_object
 * dereferences pcoplist[pad]; the pad field is a u_char, so 0..255 only) */
static int SandboxFindPcopIndex(void)
{
	int i;

	for (i = 0; i < 256; i++)
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

/* pages: main categories + their submenus + the object list */
enum
{
	SBX_PAGE_MAIN = 0,
	SBX_PAGE_VEHICLE,
	SBX_PAGE_SPAWN,
	SBX_PAGE_WORLD,
	SBX_PAGE_CHEATS,
	SBX_PAGE_AICAR,
	SBX_PAGE_OBJECTS,
	SBX_PAGE_TUNE,
	SBX_PAGE_COUNT
};

#define SBX_MAIN_ITEMS 7	/* Vehicle, Spawn, World, Cheats, Replace Pause, Open Pause, Close */
#define SBX_VEHICLE_ITEMS 6	/* Repair, Upright, Set Damage, Set Felony, Player AI Mode, Tune Car */
#define SBX_SPAWN_ITEMS 5	/* Position, Car ID, Spawn Car, Object, AI Car */
#define SBX_WORLD_ITEMS 2	/* Time of Day, Weather */
#define SBX_AICAR_ITEMS 7	/* Vehicle, Mode, Param, Position, Spawn, Remove, Back */
#define SBX_CHEATS_ITEMS 9	/* invincibility, immunity, secret car, jericho, mini, bonus, buddha, unlock all, back */
#define SBX_TUNE_ITEMS 12	/* power, traction, mass, suspension, cog x/y/z, twist x/y/z, wheelbase, track */

static const char* const gSandboxMainItems[SBX_MAIN_ITEMS] = {
	"Vehicle Properties", "Spawn Menu", "World Settings", "Cheats Menu",
	"Replace Pause Menu: OFF", "Open Pause Menu", "Close"
};
static const char* const gSandboxVehicleItems[SBX_VEHICLE_ITEMS] = {
	"Repair Car", "Make Car Upright", "Set Damage", "Set Felony", "Player AI Mode", "Tune Car"
};
static const char* const gSandboxSpawnItems[SBX_SPAWN_ITEMS] = {
	"Spawn Position", "Car ID", "Spawn Car", "Spawn Object", "AI Car"
};
static const char* const gSandboxWorldItems[SBX_WORLD_ITEMS] = {
	"Set Time of Day", "Set Weather"
};
static const char* const gSandboxCheatsItems[SBX_CHEATS_ITEMS] = {
	"Invincibility", "Immunity", "Secret Car", "Play as Jericho",
	"Mini Cars", "Unlock Bonus Cars", "Buddha Mode", "Unlock All", "Back"
};
static const char* const gSandboxTuneItems[SBX_TUNE_ITEMS] = {
	"Power Ratio", "Traction", "Mass", "Suspension", "COG X", "COG Y", "COG Z",
	"Twist X", "Twist Y", "Twist Z", "Wheelbase", "Track Width"
};

static const char* const gSandboxPageTitles[SBX_PAGE_COUNT] = {
	"SANDBOX", "VEHICLE", "SPAWN", "WORLD", "CHEATS", "AI CAR", "SPAWN OBJECT", "TUNE"
};

static int gSandboxMenuOpen;
static int gSandboxReplacePause;	/* START opens the sandbox overlay instead of the engine pause (persisted) */
static int gSandboxPage;		/* current page */
static int gSandboxSubPage;		/* sub-page within the current page */
static int gSandboxCursor;		/* cursor (absolute item index) */
static int gSandboxObjectPage;	/* object-list page (12 per page) */
static int gSandboxObjectCursor;	/* object-list cursor */
static int gSandboxAdjust;		/* 1 = adjusting a SET item */
static int gSandboxPreviewModel;	/* car model shown in the preview (future car page) */

/* HUD elements hidden while the sandbox menu is open (restored on close) */
static int gSandboxSavedShowMap;
static int gSandboxSavedDoOverlays;

/* input debounce: the press that opened the menu must not select anything */
static int gSandboxInputDebounce;

/* AI car submenu state */
static int gSandboxSpawnModel = -1;	/* car model the Spawn Car button creates
				   (the Car ID item cycles this; -1 = first use,
				   defaults to the player's own car) */
static int gSandboxAIModel;		/* internal car model to spawn */
static int gSandboxAIMode;		/* 0 = civilian, 1 = cop, 2 = lead */
static int gSandboxAIParam;		/* mode-specific parameter */

/* per-car tuning: a temporary CAR_COSMETICS copy so edits never touch the
 * shared car_cosmetics[model] that every car of the same model uses */
static CAR_COSMETICS gSandboxTunedCos;
static CAR_COSMETICS* gSandboxOrigCos;
static int gSandboxTunedActive;

/* a private OT for the preview: the model renders on its own layer (between
 * the menu text at ot+0 and the background panel at ot+3) instead of at
 * world depths, where the world geometry cut it off and the panel's
 * semi-transparency flickered over it */
static OTTYPE gSandboxPreviewOt[2048];
static DENTUVS gSandboxCleanUv[MAX_DENTING_UVS];	/* zeroed damage-UV
				   table for clean previews (one entry per denting UV slot —
				   plotCarPoly indexes damageLevel[src->originalindex] for
				   EVERY poly, so a single DENTUVS would read OOB) */
static CAR_DATA gSandboxPreviewCar;	/* scratch car for palette'd model previews */

/* point the scratch car at a model + palette (clean geometry, no damage) */
static void SandboxPreviewScratchCar(int model, int palette)
{
	extern CAR_COSMETICS car_cosmetics[];

	memset(&gSandboxPreviewCar, 0, sizeof(gSandboxPreviewCar));

	gSandboxPreviewCar.id = -1;	/* no crumple bend/damage lookup */
	gSandboxPreviewCar.ap.model = (u_char)model;
	gSandboxPreviewCar.ap.palette = (u_char)palette;
	gSandboxPreviewCar.ap.carCos = &car_cosmetics[model];
}

static const char* const gSandboxAIModeNames[3] = { "Civilian", "Cop", "Lead" };

/* the level-object list for the spawn page (smashable[] with a name) */
#define SBX_SMASHABLE_COUNT 37	/* sizeof(smashable)/sizeof(smashable[0]) in objanim.c */

static int gSandboxObjectCount;
static int gSandboxObjectIdx[SBX_SMASHABLE_COUNT];
static const char* gSandboxObjectNames[SBX_SMASHABLE_COUNT];

/* the per-car tuning helpers (defined below) — the close path needs them */
static void SandboxTuneEnter(void);
static void SandboxTuneRestore(void);

/* first visit to the spawn page: default the Car ID to the player's own
 * car (the historical Spawn Car behavior) */
static void SandboxSpawnPageEnter(CAR_DATA* pc)
{
	if (gSandboxSpawnModel < 0)
		gSandboxSpawnModel = pc != NULL ? pc->ap.model : 0;

	gSandboxPage = SBX_PAGE_SPAWN;
	gSandboxCursor = 0;
	gSandboxSubPage = 0;
}
static void SandboxMenuClose(void)
{
	gSandboxMenuOpen = 0;
	gSandboxAdjust = 0;
	gStopPadReads = 0;

	SandboxTuneRestore();

	/* bring the map + damage/felony bars back */
	gShowMap = gSandboxSavedShowMap;
	gDoOverlays = gSandboxSavedDoOverlays;
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
	int val;

	if (pc != NULL)
	{
		val = pc->felonyRating + delta;

		if (val < 0)
			val = 0;
		else if (val > FELONY_MAX_VALUE)
			val = FELONY_MAX_VALUE;

		pc->felonyRating = (short)val;
	}
	else if (player[0].playerCarId < 0)
	{
		val = pedestrianFelony + delta;

		if (val < 0)
			val = 0;
		else if (val > FELONY_MAX_VALUE)
			val = FELONY_MAX_VALUE;

		pedestrianFelony = (short)val;
	}
}

/* remove every non-player AI car (the ones the sandbox can spawn) */
static void SandboxMenuRemoveAICars(void)
{
	CAR_DATA* pc;
	CAR_DATA* cp;
	int i;

	pc = SandboxPlayerCar();

	for (i = 0; i < MAX_CARS; i++)
	{
		cp = &car_data[i];

		if (cp == pc)
			continue;

		if (cp->controlType == CONTROL_TYPE_CIV_AI ||
			cp->controlType == CONTROL_TYPE_PURSUER_AI ||
			cp->controlType == CONTROL_TYPE_LEAD_AI)
		{
			/* keep the spawner counters in sync: PingOutCar only decrements
			 * them for civilian cars — without this, cops and traffic would
			 * stop respawning for the rest of the level */
			if (cp->controlType == CONTROL_TYPE_PURSUER_AI)
			{
				numCopCars--;

				if (cp->ai.p.dying == 0)
					numActiveCops--;
			}

			PingOutCar(cp);
			cp->controlType = CONTROL_TYPE_NONE;
		}
	}
}

/* ---- per-car tuning: the temp cosmetic copy ---- */
static void SandboxTuneEnter(void)
{
	CAR_DATA* pc = SandboxPlayerCar();

	if (pc == NULL)
		return;

	gSandboxOrigCos = pc->ap.carCos;
	gSandboxTunedCos = *gSandboxOrigCos;
	pc->ap.carCos = &gSandboxTunedCos;
	gSandboxTunedActive = 1;
}

static void SandboxTuneRestore(void)
{
	CAR_DATA* pc;

	if (!gSandboxTunedActive)
		return;

	/* re-point at the model's shared cosmetics (the car may have changed) */
	extern CAR_COSMETICS car_cosmetics[];

	pc = SandboxPlayerCar();

	if (pc != NULL)
		pc->ap.carCos = &car_cosmetics[pc->ap.model];

	gSandboxTunedActive = 0;
}

static void SandboxTuneAdjust(CAR_DATA* pc, int cursor, int step)
{
	short* p;
	int min;
	int max;

	if (pc == NULL || gSandboxTunedActive == 0)
		return;

	switch (cursor)
	{
	case 0: p = &gSandboxTunedCos.powerRatio; min = 0; max = 4096; break;
	case 1: p = &gSandboxTunedCos.traction; min = 0; max = 4096; break;
	case 2: p = &gSandboxTunedCos.mass; min = 16; max = 4096; break;
	case 3: p = &gSandboxTunedCos.susCoeff; min = 0; max = 4096; break;
	case 4: p = &gSandboxTunedCos.cog.vx; min = -1024; max = 1024; break;
	case 5: p = &gSandboxTunedCos.cog.vy; min = -1024; max = 1024; break;
	case 6: p = &gSandboxTunedCos.cog.vz; min = -1024; max = 1024; break;
	case 7: p = &gSandboxTunedCos.twistRateX; min = 0; max = 4096; break;
	case 8: p = &gSandboxTunedCos.twistRateY; min = 0; max = 4096; break;
	case 9: p = &gSandboxTunedCos.twistRateZ; min = 0; max = 4096; break;
	case 10: p = &gSandboxTunedCos.wheelDisp[0].vz; min = 0; max = 4096; break;
	case 11: p = &gSandboxTunedCos.wheelDisp[0].vx; min = 0; max = 4096; break;
	default: return;
	}

	if (cursor == 10)
	{
		/* wheelbase: both axles spread apart symmetrically (front +z, rear -z) */
		gSandboxTunedCos.wheelDisp[0].vz += step * 16;
		gSandboxTunedCos.wheelDisp[1].vz -= step * 16;
		gSandboxTunedCos.wheelDisp[2].vz += step * 16;
		gSandboxTunedCos.wheelDisp[3].vz -= step * 16;
		return;
	}

	if (cursor == 11)
	{
		/* track width: left wheels out, right wheels out */
		gSandboxTunedCos.wheelDisp[0].vx -= step * 8;
		gSandboxTunedCos.wheelDisp[1].vx -= step * 8;
		gSandboxTunedCos.wheelDisp[2].vx += step * 8;
		gSandboxTunedCos.wheelDisp[3].vx += step * 8;
		return;
	}

	*p += step * 32;

	if (*p < min)
		*p = max;
	else if (*p > max)
		*p = min;
}

/* count of items on a page (main pages only; the object page is special) */
static int SandboxMenuItemCount(int page)
{
	switch (page)
	{
	case SBX_PAGE_MAIN: return SBX_MAIN_ITEMS;
	case SBX_PAGE_VEHICLE: return SBX_VEHICLE_ITEMS;
	case SBX_PAGE_SPAWN: return SBX_SPAWN_ITEMS;
	case SBX_PAGE_WORLD: return SBX_WORLD_ITEMS;
	case SBX_PAGE_CHEATS: return SBX_CHEATS_ITEMS;
	case SBX_PAGE_AICAR: return SBX_AICAR_ITEMS;
	case SBX_PAGE_TUNE: return SBX_TUNE_ITEMS;
	default: return 0;
	}
}

/* pagination: every submenu is capped at SBX_PAGE_ITEMS visible items */
#define SBX_PAGE_ITEMS 6

static int SandboxPageCount(int page)
{
	int total = SandboxMenuItemCount(page);

	return (total + SBX_PAGE_ITEMS - 1) / SBX_PAGE_ITEMS;
}

static int SandboxPageVisibleStart(int page)
{
	return gSandboxSubPage * SBX_PAGE_ITEMS;
}

static int SandboxPageVisibleCount(int page)
{
	int total = SandboxMenuItemCount(page);
	int start = SandboxPageVisibleStart(page);
	int count = total - start;

	if (count > SBX_PAGE_ITEMS)
		count = SBX_PAGE_ITEMS;

	return count;
}

static const char* SandboxMenuItemLabel(int page, int cursor)
{
	switch (page)
	{
	case SBX_PAGE_MAIN:
		/* the Replace Pause Menu toggle shows its live state */
		if (cursor == 4)
			return gSandboxReplacePause ? "Replace Pause Menu: ON" : "Replace Pause Menu: OFF";
		return gSandboxMainItems[cursor];
	case SBX_PAGE_VEHICLE: return gSandboxVehicleItems[cursor];
	case SBX_PAGE_SPAWN: return gSandboxSpawnItems[cursor];
	case SBX_PAGE_WORLD: return gSandboxWorldItems[cursor];
	case SBX_PAGE_CHEATS: return gSandboxCheatsItems[cursor];
	default: return "";
	}
}

/* cheats page label: the toggle items show their live state */
static const char* SandboxCheatLabel(int cursor)
{
	switch (cursor)
	{
	case 0: return ActiveCheats.cheat3 ? "Invincibility: ON" : "Invincibility: OFF";
	case 1: return ActiveCheats.cheat4 ? "Immunity: ON" : "Immunity: OFF";
	case 2: return ActiveCheats.cheat10 ? "Secret Car: ON" : "Secret Car: OFF";
	case 3: return ActiveCheats.cheat12 ? "Play as Jericho: ON" : "Play as Jericho: OFF";
	case 4: return ActiveCheats.cheat13 ? "Mini Cars: ON" : "Mini Cars: OFF";
	case 5: return (ActiveCheats.cheat5 | ActiveCheats.cheat6 |
			ActiveCheats.cheat7 | ActiveCheats.cheat8) ? "Unlock Bonus Cars: ON" : "Unlock Bonus Cars: OFF";
	case 6: return gNoDamage ? "Buddha Mode: ON" : "Buddha Mode: OFF";
	case 7: return "Unlock All";
	default: return "Back";
	}
}

/* tune page label: each item shows its live value */
static const char* SandboxTuneLabel(int cursor)
{
	static char buf[32];

	switch (cursor)
	{
	case 0: sprintf(buf, "Power Ratio: %d", gSandboxTunedCos.powerRatio); break;
	case 1: sprintf(buf, "Traction: %d", gSandboxTunedCos.traction); break;
	case 2: sprintf(buf, "Mass: %d", gSandboxTunedCos.mass); break;
	case 3: sprintf(buf, "Suspension: %d", gSandboxTunedCos.susCoeff); break;
	case 4: sprintf(buf, "COG X: %d", gSandboxTunedCos.cog.vx); break;
	case 5: sprintf(buf, "COG Y: %d", gSandboxTunedCos.cog.vy); break;
	case 6: sprintf(buf, "COG Z: %d", gSandboxTunedCos.cog.vz); break;
	case 7: sprintf(buf, "Twist X: %d", gSandboxTunedCos.twistRateX); break;
	case 8: sprintf(buf, "Twist Y: %d", gSandboxTunedCos.twistRateY); break;
	case 9: sprintf(buf, "Twist Z: %d", gSandboxTunedCos.twistRateZ); break;
	case 10: sprintf(buf, "Wheelbase: %d", gSandboxTunedCos.wheelDisp[0].vz - gSandboxTunedCos.wheelDisp[1].vz); break;
	default: sprintf(buf, "Track: %d", gSandboxTunedCos.wheelDisp[2].vx - gSandboxTunedCos.wheelDisp[0].vx); break;
	}

	return buf;
}

/* unlock every bonus track/car/mission in the save (persists on next save) */
static void SandboxMenuUnlockAll(void)
{
	extern ACTIVE_CHEATS AvailableCheats;
	extern int gFurthestMission;

	AvailableCheats.cheat1 = 1;
	AvailableCheats.cheat2 = 1;
	AvailableCheats.cheat3 = 1;
	AvailableCheats.cheat4 = 1;
	AvailableCheats.cheat5 = 1;
	AvailableCheats.cheat6 = 1;
	AvailableCheats.cheat7 = 1;
	AvailableCheats.cheat8 = 1;
	gFurthestMission = 40;
}

/* switch the player's car between Manual and the AI modes */
static void SandboxSetPlayerAiMode(int mode)
{
	CAR_DATA* pc = SandboxPlayerCar();

	if (pc == NULL)
		return;

	g_PlayerControlMode = mode;

	switch (mode)
	{
	case 0:	/* Manual — restore everything the AI inits clobbered: the union
		 * (ai.padid is read as a char* pointer by the player path, main.c:999;
		 * the LEAD/CIV/COP bytes made it a garbage pointer like 0x00FF0003,
		 * the ~0xFFFFFF access violation) and the handling profile */
		pc->controlType = CONTROL_TYPE_PLAYER;
		pc->hndType = 0;
		pc->ai.padid = &player[0].padid;
		break;

	case 1:	/* Traffic AI */
		/* NULL lets InitCivState query the surface under the car (a zeroed
		 * EXTRA_CIV_DATA would feed surfInd=0, an arbitrary surface) */
		InitCivState(pc, NULL);
		break;

	case 2:	/* Cop AI */
		InitCopState(pc, NULL);
		break;

	case 3:	/* Lead AI */
		InitLead(pc);
		break;
	}
}

/* vehicle page label: the Player AI Mode item shows the live mode */
static const char* SandboxVehicleLabel(int cursor)
{
	if (cursor == 4)
	{
		switch (g_PlayerControlMode)
		{
		case 1: return "Player AI Mode: Traffic";
		case 2: return "Player AI Mode: Cop";
		case 3: return "Player AI Mode: Lead";
		default: return "Player AI Mode: Manual";
		}
	}

	return SandboxMenuItemLabel(SBX_PAGE_VEHICLE, cursor);
}

/* AI car page labels (items 0-2 are dynamic) */
static const char* SandboxAICarLabel(int cursor)
{
	static char buf[32];

	switch (cursor)
	{
	case 0: sprintf(buf, "Vehicle: Car %d", gSandboxAIModel); return buf;
	case 1: sprintf(buf, "Mode: %s", gSandboxAIModeNames[gSandboxAIMode]); return buf;
	case 2:
		switch (gSandboxAIMode)
		{
		case 0: sprintf(buf, "Palette: %d", gSandboxAIParam); break;
		case 1: sprintf(buf, "Desired Speed: %d", gSandboxAIParam); break;
		default: sprintf(buf, "Road Pos: %d", gSandboxAIParam); break;
		}
		return buf;
	case 3: return gSandboxSpawnMode ? "Spawn Position: Teleport In" : "Spawn Position: In Front";
	case 4: return "Spawn AI Car";
	case 5: return "Remove AI Cars";
	default: return "Back";
	}
}

/* spawn page label: the position item shows the live mode, the Car ID
 * item shows the cycled model index */
static const char* SandboxSpawnLabel(int cursor)
{
	static char buf[24];

	switch (cursor)
	{
	case 0:
		return gSandboxSpawnMode ? "Spawn Position: Teleport In" : "Spawn Position: In Front";
	case 1:
		sprintf(buf, "Car ID: %d", gSandboxSpawnModel);
		return buf;
	default:
		return SandboxMenuItemLabel(SBX_PAGE_SPAWN, cursor);
	}
}

/* spawn an AI car with the chosen model, mode and parameter */
static void SandboxSpawnAICar(void)
{
	CAR_DATA* carCnt;
	CAR_DATA* pNewCar = NULL;
	CAR_DATA* pc;
	EXTRA_CIV_DATA civDat;
	LONGVECTOR4 pos;
	int direction;
	int control;

	pc = SandboxPlayerCar();

	if (pc == NULL)
		return;

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
		return;

	direction = pc->hd.direction;

	if (gSandboxSpawnMode == 1)
	{
		/* teleport-in: the new car spawns at the player's position and the
		 * player is warped inside — the chosen AI routine takes over */
		pos[0] = pc->hd.where.t[0];
		pos[1] = pc->hd.where.t[1];
		pos[2] = pc->hd.where.t[2];

		InitCar(pNewCar, direction, &pos, CONTROL_TYPE_CIV_AI, gSandboxAIModel,
			gSandboxAIMode == 0 ? gSandboxAIParam : 0, NULL);

		ChangePedPlayerToCar(0, pNewCar);
		PingOutCar(pc);

		if (gSandboxAIMode == 0)
			InitCivState(pNewCar, NULL);
		else if (gSandboxAIMode == 1)
			InitCopState(pNewCar, NULL);
		else
			InitLead(pNewCar);

		return;
	}

	pos[0] = pc->hd.where.t[0] + FIXEDH(RSIN(direction) * 2500);
	pos[1] = pc->hd.where.t[1];
	pos[2] = pc->hd.where.t[2] + FIXEDH(RCOS(direction) * 2500);

	control = (gSandboxAIMode == 1) ? CONTROL_TYPE_PURSUER_AI
		: (gSandboxAIMode == 2) ? CONTROL_TYPE_LEAD_AI
		: CONTROL_TYPE_CIV_AI;

	if (gSandboxAIMode == 0)
	{
		memset(&civDat, 0, sizeof(civDat));
		civDat.palette = (u_char)gSandboxAIParam;
	}

	InitCar(pNewCar, direction, &pos, (u_char)control, gSandboxAIModel,
		gSandboxAIMode == 0 ? gSandboxAIParam : 0,
		gSandboxAIMode == 0 ? (char*)&civDat : NULL);

	/* mode-specific parameters applied after the init */
	if (gSandboxAIMode == 1)
		pNewCar->ai.p.desiredSpeed = (short)gSandboxAIParam;
	else if (gSandboxAIMode == 2)
		pNewCar->ai.l.roadPosition = gSandboxAIParam;
}

static void SandboxMenuDoAction(int page, int cursor)
{
	CAR_DATA* pc = SandboxPlayerCar();

	switch (page)
	{
	case SBX_PAGE_MAIN:
		switch (cursor)
		{
		case 0: gSandboxPage = SBX_PAGE_VEHICLE; gSandboxCursor = 0; gSandboxSubPage = 0; break;
		case 1:
			SandboxSpawnPageEnter(pc);
			break;
		case 2: gSandboxPage = SBX_PAGE_WORLD; gSandboxCursor = 0; gSandboxSubPage = 0; break;
		case 3: gSandboxPage = SBX_PAGE_CHEATS; gSandboxCursor = 0; gSandboxSubPage = 0; break;
		case 4:
			/* toggle whether START opens the sandbox overlay instead of the
			 * engine pause menu (persisted via the JERICHO config API) */
			gSandboxReplacePause ^= 1;
			jer_config_set_bool("sandbox", "replace_pause", gSandboxReplacePause);
			break;
		case 5:
			/* close the sandbox overlay and open the normal pause menu */
			SandboxMenuClose();
			ShowPauseMenu(PAUSEMODE_PAUSE);
			break;
		case 6: SandboxMenuClose(); break;
		}
		break;

	case SBX_PAGE_VEHICLE:
		switch (cursor)
		{
		case 0: SandboxMenuRepair(); break;
		case 1: SetRightWayUp(1); break;
		case 2: gSandboxAdjust = 1; gNoDamage = 0; break;	/* Set Damage — switch the no-damage protection off so it sticks */
		case 3: gSandboxAdjust = 1; break;				/* Set Felony */
		case 4: SandboxSetPlayerAiMode((g_PlayerControlMode + 1) & 3); break;
		case 5:	/* Tune Car — edit a private cosmetic copy of this car */
			SandboxTuneEnter();
			gSandboxPage = SBX_PAGE_TUNE;
			gSandboxCursor = 0;
			gSandboxSubPage = 0;
			break;
		}
		break;

	case SBX_PAGE_SPAWN:
		switch (cursor)
		{
		case 0: gSandboxSpawnMode ^= 1; break;	/* spawn position */
		case 1: break;				/* Car ID — cycled with L/R */
		case 2:
			/* spawn the car selected with the Car ID item, clean, in the
			 * player's palette (matches the preview — the same NULL guard
			 * so the button can't spawn a non-resident model the preview
			 * hides) */
			if (gSandboxSpawnModel >= 0 &&
				gSandboxSpawnModel < MAX_CAR_RESIDENT_MODELS &&
				gCarCleanModelPtr[gSandboxSpawnModel] != NULL)
			{
				SandboxSpawnCarModel(gSandboxSpawnModel,
					pc != NULL ? pc->ap.palette : 0);
			}
			break;
		case 3:
			SandboxMenuBuildObjectList();
			gSandboxObjectPage = 0;
			gSandboxObjectCursor = 0;
			gSandboxPage = SBX_PAGE_OBJECTS;
			break;
		case 4:
			gSandboxPage = SBX_PAGE_AICAR;
			gSandboxCursor = 0;
			gSandboxSubPage = 0;
			break;
		}
		break;

	case SBX_PAGE_CHEATS:
		switch (cursor)
		{
		case 0: ActiveCheats.cheat3 ^= 1; break;	/* invincibility */
		case 1: ActiveCheats.cheat4 ^= 1; break;	/* immunity */
		case 2: ActiveCheats.cheat10 ^= 1; break;	/* secret car */
		case 3: ActiveCheats.cheat12 ^= 1; break;	/* play as Jericho */
		case 4: ActiveCheats.cheat13 ^= 1; break;	/* mini cars */
		case 5:	/* bonus cars */
		{
			int on = !(ActiveCheats.cheat5 | ActiveCheats.cheat6 |
				ActiveCheats.cheat7 | ActiveCheats.cheat8);

			ActiveCheats.cheat5 = (u_char)on;
			ActiveCheats.cheat6 = (u_char)on;
			ActiveCheats.cheat7 = (u_char)on;
			ActiveCheats.cheat8 = (u_char)on;
			break;
		}
		case 6: gNoDamage ^= 1; break;	/* Buddha mode */
		case 7: SandboxMenuUnlockAll(); break;
		default: gSandboxPage = SBX_PAGE_MAIN; gSandboxCursor = 0; gSandboxSubPage = 0; break;
		}
		break;

	case SBX_PAGE_AICAR:
		switch (cursor)
		{
		case 3: gSandboxSpawnMode ^= 1; break;	/* spawn position */
		case 4: SandboxSpawnAICar(); break;
		case 5: SandboxMenuRemoveAICars(); break;
		case 6: SandboxSpawnPageEnter(SandboxPlayerCar()); break;
		default: break;	/* items 0-2 adjust via left/right */
		}
		break;

	default:
		break;
	}

	(void)pc;
}

/* left/right adjust for the current page + cursor. Shared by the edge
 * press and the hold-to-repeat path. */
static void SandboxAdjustItem(int dir)
{
	CAR_DATA* pc = SandboxPlayerCar();

	if (gSandboxPage == SBX_PAGE_TUNE)
	{
		SandboxTuneAdjust(pc, gSandboxCursor, dir);
		return;
	}

	if (gSandboxPage == SBX_PAGE_AICAR && gSandboxCursor <= 3)
	{
		switch (gSandboxCursor)
		{
		case 0:
			gSandboxAIModel += dir;

			if (gSandboxAIModel < 0)
				gSandboxAIModel = MAX_CAR_RESIDENT_MODELS - 1;
			else if (gSandboxAIModel >= MAX_CAR_RESIDENT_MODELS)
				gSandboxAIModel = 0;
			break;

		case 1:
			gSandboxAIMode = (gSandboxAIMode + dir + 3) % 3;
			break;

		case 2:
		{
			int maxParam = (gSandboxAIMode == 0) ? 7
				: (gSandboxAIMode == 1) ? 2048 : 1024;
			int minParam = 0;
			int stepSize = (gSandboxAIMode == 0) ? 1 : 64;	/* palette is 0..7 */

			gSandboxAIParam += dir * stepSize;

			if (gSandboxAIParam < minParam)
				gSandboxAIParam = maxParam;
			else if (gSandboxAIParam > maxParam)
				gSandboxAIParam = minParam;
			break;
		}

		case 3:	/* spawn position */
			gSandboxSpawnMode ^= 1;
			break;
		}

		return;
	}

	if (gSandboxPage == SBX_PAGE_SPAWN && gSandboxCursor == 1)
	{
		/* Car ID: cycle the model to spawn (left/right) */
		gSandboxSpawnModel += dir;

		if (gSandboxSpawnModel < 0)
			gSandboxSpawnModel = MAX_CAR_RESIDENT_MODELS - 1;
		else if (gSandboxSpawnModel >= MAX_CAR_RESIDENT_MODELS)
			gSandboxSpawnModel = 0;
		return;
	}

	if (gSandboxPage == SBX_PAGE_VEHICLE && gSandboxCursor == 4)
	{
		/* Player AI Mode cycles on either direction */
		SandboxSetPlayerAiMode((g_PlayerControlMode + 1) & 3);
		return;
	}

	if (!gSandboxAdjust)
		return;

	if (gSandboxPage == SBX_PAGE_VEHICLE && gSandboxCursor == 2)
	{
		int nd;

		if (pc != NULL)
		{
			nd = pc->totalDamage + dir * 512;

			if (nd < 0)
				nd = 0;
			else if (nd > 65535)
				nd = 65535;

			pc->totalDamage = (u_short)nd;
		}

		return;
	}

	if (gSandboxPage == SBX_PAGE_VEHICLE && gSandboxCursor == 3)
	{
		SandboxMenuSetFelony(dir * 64);
		return;
	}

	if (gSandboxPage == SBX_PAGE_WORLD && gSandboxCursor == 0)
	{
		if (dir < 0)
		{
			if (gTimeOfDay > TIME_DAWN)
			{
				gTimeOfDay--;
				wantedTimeOfDay = gTimeOfDay;
				LoadSky();
			}
		}
		else if (gTimeOfDay < TIME_NIGHT)
		{
			gTimeOfDay++;
			wantedTimeOfDay = gTimeOfDay;
			LoadSky();
		}

		return;
	}

	if (gSandboxPage == SBX_PAGE_WORLD && gSandboxCursor == 1)
	{
		if (dir < 0)
		{
			if (gWeather > WEATHER_NONE)
				gWeather--;
		}
		else if (gWeather < WEATHER_WET)
			gWeather++;

		wantedWeather = gWeather;
	}
}

/* pad input for the overlay menu (world keeps running underneath) */
static void SandboxMenuInput(int padnew)
{
	int pageCount;
	int itemCount;

	/* ignore the pad for a couple of frames after opening — the press that
	 * selected 'Sandbox Menu' in the pause menu must not also navigate */
	if (gSandboxInputDebounce > 0)
	{
		gSandboxInputDebounce--;
		return;
	}
	if (gSandboxPage == SBX_PAGE_TUNE)
	{
		/* tuning page: left/right adjust (via SandboxAdjustItem below),
		 * up/down move (generic navigation), cross/triangle exits back to
		 * the vehicle page and restores the shared cosmetics */
		CAR_DATA* pc = SandboxPlayerCar();

		if (gSandboxTunedActive == 0 && pc != NULL)
			SandboxTuneEnter();	/* belt: entered some other way */

		if (padnew & (MPAD_CROSS | MPAD_TRIANGLE))
		{
			SandboxTuneRestore();
			gSandboxPage = SBX_PAGE_VEHICLE;
			gSandboxCursor = 0;
			gSandboxSubPage = 0;
			return;
		}
	}
	if (gSandboxPage == SBX_PAGE_OBJECTS)
	{
		/* object list: L1/R1 flip pages, up/down/left/right move, cross spawns */
		int pageSize = 12;

		if (padnew & MPAD_L1)
		{
			if (gSandboxObjectPage > 0)
			{
				gSandboxObjectPage--;
				gSandboxObjectCursor = gSandboxObjectPage * pageSize;
			}
		}

		if (padnew & MPAD_R1)
		{
			if ((gSandboxObjectPage + 1) * pageSize < gSandboxObjectCount)
			{
				gSandboxObjectPage++;
				gSandboxObjectCursor = gSandboxObjectPage * pageSize;
			}
		}

		if (padnew & MPAD_TRIANGLE)
		{
			SandboxSpawnPageEnter(SandboxPlayerCar());
			return;
		}

		if (padnew & (MPAD_D_UP | MPAD_D_LEFT))
		{
			if (gSandboxObjectCursor > gSandboxObjectPage * pageSize)
				gSandboxObjectCursor--;
		}

		if (padnew & (MPAD_D_DOWN | MPAD_D_RIGHT))
		{
			int pageEnd = gSandboxObjectPage * pageSize + pageSize;

			if (gSandboxObjectCursor < gSandboxObjectCount - 1 && gSandboxObjectCursor < pageEnd - 1)
				gSandboxObjectCursor++;
		}

		if (padnew & MPAD_CROSS)
		{
			int idx = gSandboxObjectIdx[gSandboxObjectCursor];
			int modelIdx = FindModelIdxWithName(smashable[idx].name);

			/* never feed a missing model (FindModelIdxWithName returns -1)
			 * into the object spawner — cop.type would become 0xFFFF and the
			 * sprite draw would index modelpointers out of bounds */
			if (modelIdx >= 0 && modelIdx < MAX_MODEL_SLOTS)
				SandboxSpawnObject(modelIdx);
		}

		return;
	}

	/* left/right edge: adjust the current item (Tune, AI-car params, the
	 * SET items, the AI-mode cycle) */
	if (padnew & (MPAD_D_LEFT | MPAD_D_RIGHT))
		SandboxAdjustItem((padnew & MPAD_D_LEFT) ? -1 : 1);

	/* page navigation: L1/R1 first flip this page's sub-pages, then cycle
	 * the category pages; Triangle goes back */
	if (gSandboxPage == SBX_PAGE_MAIN)
	{
		if (padnew & MPAD_TRIANGLE)
		{
			SandboxMenuClose();
			return;
		}
	}
	else
	{
		if (padnew & MPAD_TRIANGLE)
		{
			gSandboxPage = SBX_PAGE_MAIN;
			gSandboxCursor = 0;
			gSandboxSubPage = 0;
			return;
		}

		if (padnew & (MPAD_L1 | MPAD_R1))
		{
			int subPageCount = SandboxPageCount(gSandboxPage);

			if (subPageCount > 1)
			{
				if (padnew & MPAD_L1)
				{
					if (gSandboxSubPage > 0)
						gSandboxSubPage--;
				}
				else if (gSandboxSubPage < subPageCount - 1)
					gSandboxSubPage++;

				gSandboxCursor = SandboxPageVisibleStart(gSandboxPage);
			}
			else
			{
				pageCount = SBX_PAGE_OBJECTS - SBX_PAGE_VEHICLE;	/* 4 submenus */

				if (padnew & MPAD_L1)
					gSandboxPage = SBX_PAGE_VEHICLE + ((gSandboxPage - SBX_PAGE_VEHICLE - 1 + pageCount) % pageCount);
				else
					gSandboxPage = SBX_PAGE_VEHICLE + ((gSandboxPage - SBX_PAGE_VEHICLE + 1) % pageCount);

				gSandboxCursor = 0;
				gSandboxSubPage = 0;
			}

			return;
		}
	}

	itemCount = SandboxPageVisibleCount(gSandboxPage);
	{
		int start = SandboxPageVisibleStart(gSandboxPage);

		if (padnew & MPAD_D_UP)
		{
			if (gSandboxCursor > start)
				gSandboxCursor--;
			else
				gSandboxCursor = start + itemCount - 1;
		}

		if (padnew & MPAD_D_DOWN)
		{
			if (gSandboxCursor < start + itemCount - 1)
				gSandboxCursor++;
			else
				gSandboxCursor = start;
		}
	}

	if (padnew & MPAD_CROSS)
		SandboxMenuDoAction(gSandboxPage, gSandboxCursor);

	/* hold-to-repeat: a held left/right starts repeating after ~1s and the
	 * interval shrinks (8 -> 6 -> 4 -> 2 frames) so big value swings are
	 * fast. Uses the pad's held (level) state, not the edge. */
	{
		int heldLeft = Pads[0].mapped & MPAD_D_LEFT;
		int heldRight = Pads[0].mapped & MPAD_D_RIGHT;
		int dir = heldLeft ? -1 : (heldRight ? 1 : 0);

		if (dir != 0)
		{
			if (gSandboxHoldDir == dir)
				gSandboxHoldFrames++;
			else
			{
				gSandboxHoldDir = dir;
				gSandboxHoldFrames = 0;
				gSandboxHoldInterval = 8;
			}

			if (gSandboxHoldFrames > 60)
			{
				if (gSandboxHoldFrames > 180)
					gSandboxHoldInterval = 2;
				else if (gSandboxHoldFrames > 120)
					gSandboxHoldInterval = 4;
				else if (gSandboxHoldFrames > 90)
					gSandboxHoldInterval = 6;

				if (gSandboxHoldFrames % gSandboxHoldInterval == 0)
					SandboxAdjustItem(dir);
			}
		}
		else
		{
			gSandboxHoldDir = 0;
			gSandboxHoldFrames = 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Game-start hook: the level (re)launched — the sandbox menu must not   */
/* carry over, and the pad/HUD state must be restored                  */
/* ------------------------------------------------------------------ */

static int SandboxOnGameStart(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	if (gSandboxMenuOpen)
	{
		gSandboxMenuOpen = 0;
		gSandboxAdjust = 0;
		gSandboxInputDebounce = 0;
		gStopPadReads = 0;

		SandboxTuneRestore();

		/* bring the map + damage/felony bars back */
		gShowMap = gSandboxSavedShowMap;
		gDoOverlays = gSandboxSavedDoOverlays;
	}

	return JER_RESULT_CONTINUE;
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

/* sandbox menu text: scaled HQ font so much more fits per page */
#define SBX_TEXT_SCALE 0.20f
#define SBX_LINE 9		/* line step in PSX pixels */

static void SandboxPrint(int x, int y, const char* text)
{
	PrintStringHiresScaled((char*)text, x, y, SBX_TEXT_SCALE);
}

/* HQ font metrics (defined in pres.c) — used to centre the scaled title */
struct FONT_QUAD
{
	float x0, y0, s0, t0;
	float x1, y1, s1, t1;
};
extern void GetHiresBakedQuadScaled(int char_index, float* xpos, float* ypos, struct FONT_QUAD* q, float scale);

/* print a scaled line centred on the 320-wide viewport */
static void SandboxPrintCentred(int y, const char* text)
{
	const char* p = text;
	struct FONT_QUAD q;
	float fx = 0.0f;
	float fy = 0.0f;
	u_char chr;

	while ((chr = (u_char)*p++) != 0)
	{
		if (chr >= 128 && chr <= 138)
			fx += 24.0f * SBX_TEXT_SCALE;
		else
			GetHiresBakedQuadScaled((chr >= 32 && chr < 128 || chr > 138) ? chr : '?', &fx, &fy, &q, SBX_TEXT_SCALE);
	}

	SandboxPrint((int)(160.0f - fx * 0.5f), y, text);
}

/* semi-transparent panel behind the whole sandbox overlay (same style as the
 * pause menu's box: dark, half-transparent). A centered box capped at ~66%
 * of the screen height, at OT level 3 so it draws behind the menu text. */
static void SandboxDrawPanel(void)
{
	POLY_F4* poly;

	poly = (POLY_F4*)current->primptr;

	setPolyF4(poly);

	/* centered on the 320-wide PSX viewport; vertically centered too (the
	 * 240-tall screen, capped at ~66% height) */
	poly->x0 = 30;
	poly->y0 = 42;
	poly->x1 = 290;
	poly->y1 = 42;
	poly->x2 = 30;
	poly->y2 = 198;
	poly->x3 = 290;
	poly->y3 = 198;

	poly->r0 = 6;
	poly->g0 = 6;
	poly->b0 = 6;

	setSemiTrans(poly, 1);

	addPrim(current->ot + 3, poly);
	current->primptr += sizeof(POLY_F4);
}

/* ------------------------------------------------------------------ */
/* Preview: the player's car, drawn with the FINAL geometry            */
/* (crumple-deformed verts + damage UVs) and a turntable camera,       */
/* straight into the display buffer like the pause menu                */
/* ------------------------------------------------------------------ */

/* the common 'draw a 3D model in HUD space' helper. Callers say WHAT
 * (a MODEL*, or a CAR_DATA* for a palette'd/damaged car — pass one, NULL
 * the other), WHERE (the target screen point) and HOW BIG (target pixels
 * + the model's extent). It computes the auto-centred view-space position,
 * the spinning turntable, draws into the private OT (spliced over the
 * world, under the menu text), and — for preview cars — calculates the
 * vehicle lights (a preview car lives OUTSIDE car_data, so the per-frame
 * FX loop never lit it). */
/* measure a model's actual vertex bbox: the extent (largest X/Y span)
 * and the Y center offset. The model ORIGIN is rarely the visual center
 * (the head model pivots at the neck and hangs down), so the auto-centre
 * must offset by it or the preview lands off-target. */
static void SandboxModelBBox(MODEL* model, int* centerY, int* extent)
{
	SVECTOR* verts = GET_MODEL_DATA(SVECTOR, model, vertices);
	int n = model->num_vertices;
	int i;
	int minY = 32767;
	int maxY = -32768;
	int minX = 32767;
	int maxX = -32768;

	*centerY = 0;
	*extent = 90;

	if (verts == NULL || n <= 0)
		return;

	for (i = 0; i < n; i++)
	{
		if (verts[i].vx < minX) minX = verts[i].vx;
		if (verts[i].vx > maxX) maxX = verts[i].vx;
		if (verts[i].vy < minY) minY = verts[i].vy;
		if (verts[i].vy > maxY) maxY = verts[i].vy;
	}

	*extent = (maxX - minX) > (maxY - minY) ? (maxX - minX) : (maxY - minY);
	*centerY = (minY + maxY) / 2;
}

static void SandboxDrawHudModel(MODEL* model, CAR_DATA* car, int screenX, int screenY, int targetPixels, int extent, int scratch)
{
	MATRIX turntable;
	VECTOR pos;
	OTTYPE* savedOt;
	int z;
	int angle;

	if (extent < 16)
		extent = 16;
	if (targetPixels < 8)
		targetPixels = 8;

	z = (256 * extent) / targetPixels;

	if (z < 512)
		z = 512;

	pos.vx = ((screenX - 160) * z) / 256;
	pos.vy = -((screenY - 120) * z) / 256;	/* view Y is up */
	pos.vz = z;

	InitMatrix(turntable);
	angle = (gFrameCount * 24) & 4095;
	RotMatrixY(angle, &turntable);

	savedOt = current->ot;

	ClearOTagR((u_long*)gSandboxPreviewOt, 2048);
	current->ot = gSandboxPreviewOt;

	if (car != NULL)
	{
		CAR_MODEL* CarModelPtr = &NewCarModel[car->ap.model];

		if (scratch)
		{
			/* clean geometry + zeroed damage UVs (the spawn target).
			 * NOTE: the preview car's lights are NOT calculated here —
			 * the scratch car is outside car_data, so AddNightLights would
			 * position its polys from the zeroed hd.where (the world
			 * origin) and smear them across the screen. The preview is
			 * lit by the frame's own light matrices (the DrawCarObject
			 * shading); the headlight/brake polys are skipped. */
			CarModelPtr->vlist = GET_MODEL_DATA(SVECTOR, gCarCleanModelPtr[car->ap.model], vertices);
			CarModelPtr->nlist = GET_MODEL_DATA(SVECTOR, gCarCleanModelPtr[car->ap.model], vertices);
			gTempCarUVPtr = gSandboxCleanUv;
		}
		else
		{
			/* the player's car with its final (crumpled) geometry */
			CarModelPtr->vlist = gTempCarVertDump[car->id];
			CarModelPtr->nlist = gTempCarVertDump[car->id];
			gTempCarUVPtr = gTempHDCarUVDump[car->id];
		}

		DrawCarObject(CarModelPtr, &turntable, &pos, car->ap.palette, car, 1);

		/* the wheels are skipped for the scratch car: CAR_INDEX() derives
		 * the slot from cp - car_data, and the scratch static is not in
		 * car_data (a wild index into the wheel-rotation arrays) */
		if (!scratch)
			DrawCarWheels(car, &turntable, &pos, 0);
	}
	else
	{
		/* the GTE is already set to the turntable + view-space trans;
		 * RenderModel skips its matrix setup */
		gte_SetRotMatrix(&turntable);
		gte_SetTransVector(&pos);
		RenderModel(model, NULL, NULL, 0, PLOT_NO_CULL, 1, 0);
	}

	current->ot = savedOt;

	/* Splice the private OT into the main chain at bucket 0 — the NEAREST
	 * layer: the world (any depth) and the panel (bucket 3) draw before
	 * it, so the model can never sit behind world geometry. The main
	 * entry points at the preview's HEAD (entry 2047 — ClearOTagR threads
	 * i -> i-1, so the head is the far end, drawn first), and the TAIL
	 * (entry 0) carries on into the ORIGINAL bucket-0 chain (the menu
	 * text, captured BEFORE the splice overwrites the entry) so the
	 * labels render on top — and the walk never loops. */
	{
		OTTYPE* textChain = (OTTYPE*)getaddr(&savedOt[0]);

		setaddr(&savedOt[0], &gSandboxPreviewOt[2047]);
		setaddr(&gSandboxPreviewOt[0], textChain);
	}
}

static void SandboxDrawPreview(void)
{
	CAR_DATA* cp = NULL;
	CAR_MODEL* CarModelPtr;
	MODEL* model = NULL;
	MATRIX turntable;
	VECTOR pos;
	OTTYPE* savedOt;
	int angle;
	int scratch = 0;	/* 1 = the scratch car (clean model + palette) */

	/* The preview is persistent: while the sandbox menu is open, every page
	 * shows a spinning model — the player's car (final crumpled geometry),
	 * the player ped when on foot, the object/car the spawn pages will
	 * create (clean, in the chosen palette). Every lookup is bounds- and
	 * NULL-checked; an unsafe lookup simply skips the preview instead of
	 * drawing garbage. */
	if (gSandboxPage == SBX_PAGE_OBJECTS)
	{
		/* the selected spawn object */
		if (gSandboxObjectCursor >= 0 && gSandboxObjectCursor < gSandboxObjectCount)
		{
			int sIdx = gSandboxObjectIdx[gSandboxObjectCursor];
			int mIdx;

			if (sIdx >= 0 && sIdx < SBX_SMASHABLE_COUNT &&
				smashable[sIdx].name != NULL && smashable[sIdx].name[0] != 0)
			{
				mIdx = FindModelIdxWithName(smashable[sIdx].name);

				if (mIdx >= 0 && mIdx < MAX_MODEL_SLOTS)
					model = modelpointers[mIdx];
			}
		}

		if (model == NULL)
			return;
	}
	else if (gSandboxPage == SBX_PAGE_AICAR)
	{
		/* the AI car about to be spawned: model + chosen palette */
		if (gSandboxAIModel >= 0 && gSandboxAIModel < MAX_CAR_RESIDENT_MODELS &&
			gCarCleanModelPtr[gSandboxAIModel] != NULL)
		{
			SandboxPreviewScratchCar(gSandboxAIModel,
				gSandboxAIMode == 0 ? gSandboxAIParam : 0);
			cp = &gSandboxPreviewCar;
			scratch = 1;
		}
		else
			return;
	}
	else if (gSandboxPage == SBX_PAGE_SPAWN)
	{
		/* the car the Spawn Car button will create: the model selected with
		 * the Car ID item, clean, in the player's palette */
		CAR_DATA* pc = SandboxPlayerCar();
		int model = gSandboxSpawnModel;
		int palette = pc != NULL ? pc->ap.palette : 0;

		if (model < 0 || model >= MAX_CAR_RESIDENT_MODELS ||
			gCarCleanModelPtr[model] == NULL)
			return;

		SandboxPreviewScratchCar(model, palette);
		cp = &gSandboxPreviewCar;
		scratch = 1;
	}
	else
	{
		cp = SandboxPlayerCar();

		if (cp == NULL)
		{
			/* on foot: the player ped's HEAD model (pmTannerModels[1];
			 * index 0 is the torso/hips) — the head reads best at a glance */
			model = pmTannerModels[1];

			if (model == NULL)
				return;
		}
	}

	/* WHAT/WHERE/HOW-BIG: hand the selection to the common HUD-model
	 * helper (auto-centre + turntable + lights + OT splice). The car
	 * preview sits centre-right inside the panel at ~72px; the head at
	 * ~44px. */
	{
		int extent = 90;		/* the head model ~90 units */
		int targetPixels = 44;

		if (cp != NULL && cp->ap.carCos != NULL)
		{
			/* the car's full extent: the collision box is the FULL box
			 * (the engine halves it for the half-lengths) */
			int hx = cp->ap.carCos->colBox.vx;
			int hy = cp->ap.carCos->colBox.vy;
			int hz = cp->ap.carCos->colBox.vz;

			extent = hx > hy ? (hx > hz ? hx : hz) : (hy > hz ? hy : hz);
			targetPixels = 72;	/* the car ~72px tall inside the panel */
		}

		if (cp != NULL)
			SandboxDrawHudModel(model, cp, 220, 112, targetPixels, extent, scratch);
		else
		{
			/* the ped head: measure its real bbox so the auto-centre lands on
			 * the head's VISUAL center (its origin is the neck pivot) */
			int centerY = 0;

			SandboxModelBBox(model, &centerY, &extent);
			SandboxDrawHudModel(model, NULL, 220, 112, 44, extent, 0);

			(void)centerY;
		}
	}
}

static int SandboxOnDrawOverlay(void* userdata, void* args)
{
	int i;
	char text[64];
	CAR_DATA* pc;

	(void)userdata;

	/* the overlay draws ONLY while the menu is open — the DRAW_OVERLAY
	 * hook fires every frame, so without this gate the menu text and the
	 * live preview would render over the game constantly and never go
	 * away (the close button only flips gSandboxMenuOpen) */
	if (!gSandboxMenuOpen)
		return JER_RESULT_CONTINUE;

	/* the semi-transparent shadow backdrop behind the whole overlay */
	SandboxDrawPanel();

	pc = SandboxPlayerCar();

	if (gSandboxPage == SBX_PAGE_OBJECTS || gSandboxPage == SBX_PAGE_TUNE)
	{
		SandboxDrawPreview();
		return JER_RESULT_CONTINUE;
	}

	/* header + current-vehicle readouts (mockup: model / damage / felony /
	 * AI mode), shown on the main page so it reads as a status block */
	SetTextColour(255, 255, 0);
	sprintf(text, "%s", gSandboxPageTitles[gSandboxPage]);
	SandboxPrintCentred(50, text);

	if (pc != NULL)
	{
		sprintf(text, "Car %d", pc->ap.model);
		SetTextColour(255, 255, 255);
		SandboxPrint(34, 60, text);

		sprintf(text, "DAMAGE: %d", pc->totalDamage);
		SandboxPrint(34, 69, text);

		sprintf(text, "FELONY: %d", pc->felonyRating);
		SandboxPrint(34, 78, text);

		sprintf(text, "AI: %s",
			g_PlayerControlMode == 1 ? "Traffic" :
			g_PlayerControlMode == 2 ? "Cop" :
			g_PlayerControlMode == 3 ? "Lead" : "Manual");
		SandboxPrint(34, 87, text);
	}
	else
	{
		SandboxPrint(34, 60, "(on foot)");
	}

	/* items */
	{
		int itemCount = SandboxPageVisibleCount(gSandboxPage);
		int start = SandboxPageVisibleStart(gSandboxPage);

		for (i = 0; i < itemCount; i++)
		{
			int idx = start + i;
			const char* label;

			if (gSandboxPage == SBX_PAGE_CHEATS)
				label = SandboxCheatLabel(idx);
			else if (gSandboxPage == SBX_PAGE_VEHICLE)
				label = SandboxVehicleLabel(idx);
			else if (gSandboxPage == SBX_PAGE_AICAR)
				label = SandboxAICarLabel(idx);
			else if (gSandboxPage == SBX_PAGE_SPAWN)
				label = SandboxSpawnLabel(idx);
			else if (gSandboxPage == SBX_PAGE_TUNE)
				label = SandboxTuneLabel(idx);
			else
				label = SandboxMenuItemLabel(gSandboxPage, idx);

			sprintf(text, "%s %s", idx == gSandboxCursor ? ">" : " ", label);

			if (idx == gSandboxCursor)
				SetTextColour(gSandboxAdjust ? 0 : 255, 255, gSandboxAdjust ? 255 : 0);
			else
				SetTextColour(255, 255, 255);

			SandboxPrint(34, 98 + i * SBX_LINE, text);
		}

		if (gSandboxPage != SBX_PAGE_MAIN)
		{
			int subPages = SandboxPageCount(gSandboxPage);

			if (subPages > 1)
				sprintf(text, "(L1/R1: page %d/%d, Triangle: back)", gSandboxSubPage + 1, subPages);
			else
				sprintf(text, "(L1/R1: menu page %d/5, Triangle: back)", gSandboxPage - SBX_PAGE_VEHICLE + 1);

			SetTextColour(180, 180, 180);
			SandboxPrint(34, 98 + itemCount * SBX_LINE + 4, text);
		}
	}

	SandboxDrawPreview();
	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Pause-menu hook: the 'Sandbox Menu' item                           */
/* ------------------------------------------------------------------ */

/* open the sandbox overlay: hide the HUD, grab the pad, run the world */
static void SandboxOpenOverlay(void)
{
	gSandboxSavedShowMap = gShowMap;
	gSandboxSavedDoOverlays = gDoOverlays;
	gShowMap = 0;
	gDoOverlays = 0;

	gSandboxMenuOpen = 1;
	gSandboxInputDebounce = 4;
	gSandboxPage = 0;
	gSandboxSubPage = 0;
	gSandboxCursor = 0;
	gSandboxAdjust = 0;
	gStopPadReads = 1;	/* the pad drives the menu, not the car */
}

static int SandboxOnPauseMenu(void* userdata, void* args)
{
	JER_ARGS_PAUSE_MENU* jerArgs = (JER_ARGS_PAUSE_MENU*)args;

	(void)userdata;

	if (jerArgs->action == JER_PAUSE_SANDBOX_GET_LABEL)
	{
		jerArgs->result = (void*)"Sandbox Menu";
		return JER_RESULT_CONTINUE;
	}

	/* START pressed: when "replace pause menu" is on, open the overlay
	 * instead of the engine pause (claimed with JER_RESULT_STOP so the
	 * engine pause never opens) */
	if (jerArgs->action == JER_PAUSE_OPEN)
	{
		if (gSandboxReplacePause)
		{
			SandboxOpenOverlay();
			return JER_RESULT_STOP;
		}

		return JER_RESULT_CONTINUE;
	}

	if (jerArgs->action == JER_PAUSE_SANDBOX_OPEN)
	{
		SandboxOpenOverlay();

		return JER_RESULT_STOP;	/* claim the press: unpause + world runs */
	}

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Pause menu item — opens the Sandbox overlay. Registered via         */
/* jer_pause_menu.h, so nothing is hardcoded into the game's pause.    */
/* ------------------------------------------------------------------ */

static int SandboxActivateMenu(void* userdata, int direction)
{
	JER_ARGS_PAUSE_MENU jerArgs;

	(void)userdata;
	(void)direction;

	jerArgs.action = JER_PAUSE_SANDBOX_OPEN;
	jerArgs.result = NULL;
	jerArgs.value = 0;

	/* if the overlay opened (module claimed the event), unpause so the
	 * world keeps running; otherwise stay in the pause menu */
	if (gCtx->jer_fire(gCtx, JER_EVENT_PAUSE_MENU, &jerArgs) == JER_RESULT_STOP)
		return JER_PAUSE_QUIT_CONTINUE;

	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM SandboxMenuItems[] =
{
	{ "Open Sandbox Menu", NULL, SandboxActivateMenu, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU SandboxPauseMenu =
{ "Sandbox", SandboxMenuItems, 1 };

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
	gNoDamage = 0;	/* the Buddha-mode no-damage toggle must be OFF by default:
			   the frame hook otherwise zeroes the player car's damage
			   EVERY frame, which reads as default invulnerability in a
			   car. It is only applied once the player toggles it in the
			   sandbox menu (or fires the toggle event). */

	/* persisted setting: does START open the sandbox overlay? */
	gSandboxReplacePause = jer_config_get_bool("sandbox", "replace_pause", 0);

	/* the sandbox must never default to opening: clear a stale persisted
	 * flag once so START opens the engine pause, not this overlay */
	jer_config_set_bool("sandbox", "replace_pause", gSandboxReplacePause);

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
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, SandboxOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, SandboxOnDrawOverlay, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PAUSE_MENU, SandboxOnPauseMenu, NULL, 0);
	ctx->jer_register_hook(ctx, SANDBOX_CUSTOM_TOGGLE, SandboxOnToggle, NULL, 0);

	/* pause menu: provided by this module, not hardcoded in the game */
	jer_pause_menu_register(&SandboxPauseMenu);

	ctx->jer_log(ctx, "[sandbox] registered — no-damage ON, time-scale 1.0x\n");
}
