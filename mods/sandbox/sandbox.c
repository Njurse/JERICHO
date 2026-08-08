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
#include "overlay.h"
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
	pos[0] = pc->hd.where.t[0] + FIXEDH(RSIN(direction) * 250);
	pos[1] = pc->hd.where.t[1];
	pos[2] = pc->hd.where.t[2] + FIXEDH(RCOS(direction) * 250);

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
	SBX_PAGE_COUNT
};

#define SBX_MAIN_ITEMS 5	/* Vehicle, Spawn, World, Cheats, Close */
#define SBX_VEHICLE_ITEMS 5	/* Repair, Upright, Set Damage, Set Felony, Player AI Mode */
#define SBX_SPAWN_ITEMS 3	/* Car, Object, AI Car */
#define SBX_WORLD_ITEMS 2	/* Time of Day, Weather */
#define SBX_AICAR_ITEMS 5	/* Vehicle, Mode, Param, Spawn, Back */
#define SBX_CHEATS_ITEMS 8	/* invincibility, immunity, secret car, jericho, mini, bonus, unlock all, back */

static const char* const gSandboxMainItems[SBX_MAIN_ITEMS] = {
	"Vehicle", "Spawn", "World", "Cheats", "Close"
};
static const char* const gSandboxVehicleItems[SBX_VEHICLE_ITEMS] = {
	"Repair Car", "Make Car Upright", "Set Damage", "Set Felony", "Player AI Mode"
};
static const char* const gSandboxSpawnItems[SBX_SPAWN_ITEMS] = {
	"Spawn Car", "Spawn Object", "AI Car"
};
static const char* const gSandboxWorldItems[SBX_WORLD_ITEMS] = {
	"Set Time of Day", "Set Weather"
};
static const char* const gSandboxCheatsItems[SBX_CHEATS_ITEMS] = {
	"Invincibility", "Immunity", "Secret Car", "Play as Jericho",
	"Mini Cars", "Bonus Cars", "Unlock All", "Back"
};

static const char* const gSandboxPageTitles[SBX_PAGE_COUNT] = {
	"SANDBOX", "VEHICLE", "SPAWN", "WORLD", "CHEATS", "AI CAR", "SPAWN OBJECT"
};

static int gSandboxMenuOpen;
static int gSandboxPage;		/* current page */
static int gSandboxSubPage;		/* sub-page within the current page */
static int gSandboxCursor;		/* cursor (absolute item index) */
static int gSandboxObjectPage;	/* object-list page (12 per page) */
static int gSandboxObjectCursor;	/* object-list cursor */
static int gSandboxAdjust;		/* 1 = adjusting a SET item */
static int gSandboxPreviewModel;	/* car model shown in the preview (future car page) */

/* AI car submenu state */
static int gSandboxAIModel;		/* internal car model to spawn */
static int gSandboxAIMode;		/* 0 = civilian, 1 = cop, 2 = lead */
static int gSandboxAIParam;		/* mode-specific parameter */

static const char* const gSandboxAIModeNames[3] = { "Civilian", "Cop", "Lead" };

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
	case SBX_PAGE_MAIN: return gSandboxMainItems[cursor];
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
			ActiveCheats.cheat7 | ActiveCheats.cheat8) ? "Bonus Cars: ON" : "Bonus Cars: OFF";
	case 6: return "Unlock All";
	default: return "Back";
	}
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
	case 0:	/* Manual */
		pc->controlType = CONTROL_TYPE_PLAYER;
		break;

	case 1:	/* Traffic AI */
	{
		EXTRA_CIV_DATA civDat;

		memset(&civDat, 0, sizeof(civDat));
		InitCivState(pc, &civDat);
		break;
	}

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
	case 3: return "Spawn";
	default: return "Back";
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
		case 1: gSandboxPage = SBX_PAGE_SPAWN; gSandboxCursor = 0; gSandboxSubPage = 0; break;
		case 2: gSandboxPage = SBX_PAGE_WORLD; gSandboxCursor = 0; gSandboxSubPage = 0; break;
		case 3: gSandboxPage = SBX_PAGE_CHEATS; gSandboxCursor = 0; gSandboxSubPage = 0; break;
		case 4: SandboxMenuClose(); break;
		}
		break;

	case SBX_PAGE_VEHICLE:
		switch (cursor)
		{
		case 0: SandboxMenuRepair(); break;
		case 1: SetRightWayUp(1); break;
		case 2: case 3: gSandboxAdjust = 1; break;
		case 4: SandboxSetPlayerAiMode((g_PlayerControlMode + 1) & 3); break;
		}
		break;

	case SBX_PAGE_SPAWN:
		switch (cursor)
		{
		case 0:
			/* spawn a copy of the player's own car model */
			SandboxSpawnCarModel(pc != NULL ? pc->ap.model : 0);
			break;
		case 1:
			SandboxMenuBuildObjectList();
			gSandboxObjectPage = 0;
			gSandboxObjectCursor = 0;
			gSandboxPage = SBX_PAGE_OBJECTS;
			break;
		case 2:
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
		case 6: SandboxMenuUnlockAll(); break;
		default: gSandboxPage = SBX_PAGE_MAIN; gSandboxCursor = 0; gSandboxSubPage = 0; break;
		}
		break;

	case SBX_PAGE_AICAR:
		switch (cursor)
		{
		case 3: SandboxSpawnAICar(); break;
		case 4: gSandboxPage = SBX_PAGE_SPAWN; gSandboxCursor = 0; gSandboxSubPage = 0; break;
		default: break;	/* items 0-2 adjust via left/right */
		}
		break;

	default:
		break;
	}

	(void)pc;
}

/* pad input for the overlay menu (world keeps running underneath) */
static void SandboxMenuInput(int padnew)
{
	int pageCount;
	int itemCount;

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
			gSandboxPage = SBX_PAGE_SPAWN;
			gSandboxCursor = 0;
			gSandboxSubPage = 0;
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

			SandboxSpawnObject(modelIdx);
		}

		return;
	}

	if (gSandboxPage == SBX_PAGE_AICAR)
	{
		/* AI car page: left/right adjusts the top three items */
		if (padnew & (MPAD_D_LEFT | MPAD_D_RIGHT))
		{
			int step = (padnew & MPAD_D_LEFT) ? -1 : 1;

			switch (gSandboxCursor)
			{
			case 0:
				gSandboxAIModel += step;

				if (gSandboxAIModel < 0)
					gSandboxAIModel = MAX_CAR_RESIDENT_MODELS - 1;
				else if (gSandboxAIModel >= MAX_CAR_RESIDENT_MODELS)
					gSandboxAIModel = 0;
				break;

			case 1:
				gSandboxAIMode = (gSandboxAIMode + step + 3) % 3;
				break;

			case 2:
			{
				int maxParam = (gSandboxAIMode == 0) ? 7
					: (gSandboxAIMode == 1) ? 2048 : 1024;
				int minParam = (gSandboxAIMode == 1) ? 0 : 0;

				gSandboxAIParam += step * 64;

				if (gSandboxAIParam < minParam)
					gSandboxAIParam = maxParam;
				else if (gSandboxAIParam > maxParam)
					gSandboxAIParam = minParam;
				break;
			}
			}

			return;
		}

		/* up/down/cross fall through to the standard navigation */
	}

	if (gSandboxAdjust)
	{
		/* adjusting a SET item: left/right change the value, cross exits */
		CAR_DATA* pc = SandboxPlayerCar();

		/* left/right on the Player AI Mode item cycles without adjust */
		if (gSandboxPage == SBX_PAGE_VEHICLE && gSandboxCursor == 4)
		{
			if (padnew & (MPAD_D_LEFT | MPAD_D_RIGHT))
				SandboxSetPlayerAiMode((g_PlayerControlMode + 1) & 3);

			return;
		}

		if (padnew & MPAD_D_LEFT)
		{
			if (gSandboxPage == SBX_PAGE_VEHICLE && gSandboxCursor == 2)
			{
				if (pc != NULL && pc->totalDamage >= 512)
					pc->totalDamage -= 512;
			}
			else if (gSandboxPage == SBX_PAGE_VEHICLE && gSandboxCursor == 3)
				SandboxMenuSetFelony(-64);
			else if (gSandboxPage == SBX_PAGE_WORLD && gSandboxCursor == 0)
			{
				if (gTimeOfDay > TIME_DAWN)
				{
					gTimeOfDay--;
					wantedTimeOfDay = gTimeOfDay;
					LoadSky();
				}
			}
			else if (gSandboxPage == SBX_PAGE_WORLD && gSandboxCursor == 1)
			{
				if (gWeather > WEATHER_NONE)
					gWeather--;
				wantedWeather = gWeather;
			}
		}

		if (padnew & MPAD_D_RIGHT)
		{
			if (gSandboxPage == SBX_PAGE_VEHICLE && gSandboxCursor == 2)
			{
				if (pc != NULL)
					pc->totalDamage += 512;
			}
			else if (gSandboxPage == SBX_PAGE_VEHICLE && gSandboxCursor == 3)
				SandboxMenuSetFelony(64);
			else if (gSandboxPage == SBX_PAGE_WORLD && gSandboxCursor == 0)
			{
				if (gTimeOfDay < TIME_NIGHT)
				{
					gTimeOfDay++;
					wantedTimeOfDay = gTimeOfDay;
					LoadSky();
				}
			}
			else if (gSandboxPage == SBX_PAGE_WORLD && gSandboxCursor == 1)
			{
				if (gWeather < WEATHER_WET)
					gWeather++;
				wantedWeather = gWeather;
			}
		}

		if (padnew & (MPAD_CROSS | MPAD_TRIANGLE))
			gSandboxAdjust = 0;

		return;
	}

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

/* semi-transparent panel behind the whole sandbox overlay (same style as the
 * pause menu's box: dark, half-transparent). Sized to cover the normal HUD
 * region and the preview, and added at OT level 3 so it draws behind the
 * menu text (OT 0) AND the spinning preview (OT 2). */
static void SandboxDrawPanel(void)
{
	POLY_F4* poly;

	poly = (POLY_F4*)current->primptr;

	setPolyF4(poly);

	poly->x0 = 6;
	poly->y0 = 6;
	poly->x1 = 294;
	poly->y1 = 6;
	poly->x2 = 6;
	poly->y2 = 238;
	poly->x3 = 294;
	poly->y3 = 238;

	poly->r0 = 16;
	poly->g0 = 16;
	poly->b0 = 16;

	setSemiTrans(poly, 1);

	addPrim(current->ot + 3, poly);
	current->primptr += sizeof(POLY_F4);
}

/* ------------------------------------------------------------------ */
/* Preview: the player's car, drawn with the FINAL geometry            */
/* (crumple-deformed verts + damage UVs) and a turntable camera,       */
/* straight into the display buffer like the pause menu                */
/* ------------------------------------------------------------------ */

static void SandboxDrawPreview(void)
{
	CAR_DATA* cp = NULL;
	CAR_MODEL* CarModelPtr;
	MODEL* model = NULL;
	MATRIX turntable;
	VECTOR pos;
	int angle;
	OTTYPE* savedOt;

	/* draw the preview into the UI layer (near the 2D text, over the
	 * world) by shifting the render context's OT base for the call */
	savedOt = current->ot;
	current->ot = savedOt - 26;

	/* The preview is persistent: while the sandbox menu is open, every page
	 * shows a spinning model — the player's car (final crumpled geometry),
	 * the player ped when on foot, or the selected spawn model on the spawn
	 * pages. Every lookup is bounds- and NULL-checked; an unsafe lookup
	 * simply skips the preview instead of drawing garbage. */
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
		{
			current->ot = savedOt;
			return;
		}

		InitMatrix(turntable);
		angle = (gFrameCount * 24) & 4095;
		RotMatrixY(angle, &turntable);

		pos.vx = 0;
		pos.vy = -400;
		pos.vz = 2400;

		gte_SetRotMatrix(&turntable);
		gte_SetTransVector(&pos);

		RenderModel(model, NULL, NULL, 0, PLOT_NO_CULL, 1, 0);

		current->ot = savedOt;
		return;
	}

	if (gSandboxPage == SBX_PAGE_AICAR)
	{
		/* the selected spawn car model */
		if (gSandboxAIModel >= 0 && gSandboxAIModel < MAX_CAR_RESIDENT_MODELS)
			model = gCarCleanModelPtr[gSandboxAIModel];

		if (model != NULL)
		{
			InitMatrix(turntable);
			angle = (gFrameCount * 24) & 4095;
			RotMatrixY(angle, &turntable);

			pos.vx = 0;
			pos.vy = -250;
			pos.vz = 1500;

			gte_SetRotMatrix(&turntable);
			gte_SetTransVector(&pos);

			RenderModel(model, NULL, NULL, 0, PLOT_NO_CULL, 1, 0);
		}

		current->ot = savedOt;
		return;
	}

	cp = SandboxPlayerCar();

	if (cp != NULL)
	{
		/* the player's car with its final (crumpled) geometry */
		InitMatrix(turntable);
		angle = (gFrameCount * 24) & 4095;
		RotMatrixY(angle, &turntable);

		pos.vx = 0;
		pos.vy = -250;
		pos.vz = 1500;

		CarModelPtr = &NewCarModel[cp->ap.model];
		CarModelPtr->vlist = gTempCarVertDump[cp->id];
		CarModelPtr->nlist = gTempCarVertDump[cp->id];
		gTempCarUVPtr = gTempHDCarUVDump[cp->id];

		DrawCarObject(CarModelPtr, &turntable, &pos, cp->ap.palette, cp, 1);
		DrawCarWheels(cp, &turntable, &pos, 0);

		current->ot = savedOt;
		return;
	}

	/* on foot: the player ped model (torso), if loaded */
	model = pmTannerModels[0];

	if (model != NULL)
	{
		InitMatrix(turntable);
		angle = (gFrameCount * 24) & 4095;
		RotMatrixY(angle, &turntable);

		pos.vx = 0;
		pos.vy = -250;
		pos.vz = 1000;

		gte_SetRotMatrix(&turntable);
		gte_SetTransVector(&pos);

		RenderModel(model, NULL, NULL, 0, PLOT_NO_CULL, 1, 0);
	}

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

	/* the backdrop: one full-size panel behind everything */
	SandboxDrawPanel();

	if (gSandboxPage == SBX_PAGE_OBJECTS)
	{
		/* spawn-object list (page-based, 12 per page) */
		int pageStart = gSandboxObjectPage * 12;
		int shown = gSandboxObjectCount - pageStart;
		char pageText[24];

		SetTextColour(255, 255, 255);
		SandboxPrint(10, 14, "SPAWN OBJECT");

		sprintf(pageText, "(L1/R1: page %d/%d)", gSandboxObjectPage + 1,
			(gSandboxObjectCount + 11) / 12);
		SandboxPrint(10, 26, pageText);
		SandboxPrint(10, 36, "(Triangle: back)");

		if (shown > 12)
			shown = 12;

		for (i = 0; i < shown; i++)
		{
			sprintf(text, "%s %s", i == gSandboxObjectCursor - pageStart ? ">" : " ", gSandboxObjectNames[pageStart + i]);
			SetTextColour(i == gSandboxObjectCursor - pageStart ? 255, 255, 0 : 255, 255, 255);
			SandboxPrint(10, 48 + i * SBX_LINE, text);
		}

		SandboxDrawPreview();
		return JER_RESULT_CONTINUE;
	}

	/* header + current-vehicle readouts */
	SetTextColour(255, 255, 0);
	sprintf(text, "%s", gSandboxPageTitles[gSandboxPage]);
	SandboxPrint(10, 14, text);

	if (pc != NULL)
	{
		sprintf(text, "DAMAGE: %d", pc->totalDamage);
		SetTextColour(255, 255, 255);
		SandboxPrint(10, 26, text);

		sprintf(text, "FELONY: %d", pc->felonyRating);
		SandboxPrint(10, 36, text);
	}
	else
	{
		SandboxPrint(10, 26, "(on foot)");
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
			else
				label = SandboxMenuItemLabel(gSandboxPage, idx);

			sprintf(text, "%s %s", idx == gSandboxCursor ? ">" : " ", label);

			if (idx == gSandboxCursor)
				SetTextColour(gSandboxAdjust ? 0 : 255, 255, gSandboxAdjust ? 255 : 0);
			else
				SetTextColour(255, 255, 255);

			SandboxPrint(10, 50 + i * SBX_LINE, text);
		}

		if (gSandboxPage != SBX_PAGE_MAIN)
		{
			int subPages = SandboxPageCount(gSandboxPage);

			if (subPages > 1)
				sprintf(text, "(L1/R1: page %d/%d, Triangle: back)", gSandboxSubPage + 1, subPages);
			else
				sprintf(text, "(L1/R1: menu page %d/4, Triangle: back)", gSandboxPage - SBX_PAGE_VEHICLE + 1);

			SetTextColour(180, 180, 180);
			SandboxPrint(10, 50 + itemCount * SBX_LINE + 4, text);
		}
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
		gSandboxSubPage = 0;
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
