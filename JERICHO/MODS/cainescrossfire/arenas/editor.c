// arenas/editor.c — the IN-GAME ARENA EDITOR (a gated debug mode).
//
// Enabled by `-cceditor` on the command line or `CC_EDITOR=1` in the
// environment. The player's car IS the cursor: drive to a spot and press a
// button to drop a spawn point there (its position and heading are the car's),
// or to mark the region's corners. Everything is drawn as ghost markers with
// the engine's line primitives and a small on-screen readout, and the file is
// saved/reloaded on demand - so this and the Python editor (tools/arenaedit.py)
// are two views of one `.cca` file (see ARENAS.md).
//
// Buttons (pad 0, EDGES - a press, not a hold):
//   L1      place / move the SELECTED spawn at the car
//   R1      cycle which spawn slot is selected
//   L2      delete the spawn nearest the car
//   R2      mark the region: first press = corner A, second = the rect (car = the other corner)
//   SELECT  save the arena to CONFIG/arenas/<name>.cca (and update the live arena)
//   START   reload the arena from its file
//
// The shoulders also carry weapon prev/next/fire, so while editing, park the
// car before tapping them. This is a build/debug tool, not a play mode.

#include "driver2.h"
#include "cars.h"
#include "pad.h"		/* Pads[], MPAD_* */
#include "jericho.h"
#include "jer_events.h"
#include "jer_hud.h"		/* jer_hud_panel - the readout */
#include "cainescrossfire.h"
#include "cainescrossfire_internal.h"	/* cd2DbgPadMask - the injected pad mask */
#include "arenas/profile.h"
#include "weapons/core/weapon_internal.h"	/* cd2WpnLine - the ghost markers */

#include <string.h>
#include <stdio.h>

int cd2ArenaFileLoad(const char* path, CD2_ARENA_PROFILE* out);
int cd2ArenaFileSave(const char* path, const CD2_ARENA_PROFILE* a);

#define CD2_ED_PANEL		3	/* the HUD slot the readout owns */
#define CD2_ED_MARK		90	/* ghost bar height (y-up) */
#define CD2_ED_Y			0	/* the ground plane the ghosts stand on */

static int gEditorOn;			/* -cceditor / CC_EDITOR */
static int gEditorReady;		/* the working copy is loaded */
static int gSelSlot;
static int gCornerState;		/* 0 none, 1 A marked, 2 rect set */
static int gCornerAx, gCornerAz;
static unsigned short gLastPad;
static CD2_ARENA_PROFILE gWork;
static int gWorkId = CD2_ARENA_NONE;

int cd2EditorActive(void)
{
	return gEditorOn;
}

// Load the match's current arena into the working copy.
static int cd2EditorLoad(void)
{
	const CD2_ARENA_PROFILE* a;

	gWorkId = CD2_ARENA_NONE;

	/* find the current arena's id by name (the registry has no "current id"
	 * getter for the editor, and the profile is what we edit) */
	a = cd2ArenaCurrent();

	if (a == NULL)
		return 0;

	gWork = *a;
	gWorkId = a->id;
	gEditorReady = 1;
	gSelSlot = 0;
	gCornerState = 0;

	printInfo("[cainescrossfire] arena editor: editing '%s' (%d spawns, %d pickups)\n",
		gWork.internalName, gWork.spawnCount, gWork.pickupCount);

	return 1;
}

// The player's car, or NULL. The car IS the editor cursor.
static CAR_DATA* cd2EditorCar(void)
{
	CAR_DATA* cp = NULL;

	if (!cd2WpnPlayerCar(&cp))
		return NULL;

	return cp;
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------
static void cd2EditorBar(int x, int z, int y, int r, int g, int b, int big)
{
	VECTOR base, tip, a, bb;
	int w = big ? 120 : 70;
	int i;
	static const int dir[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };

	base.vx = x; base.vy = y; base.vz = z;
	tip = base; tip.vy = y + CD2_ED_MARK;

	cd2WpnLine(&base, &tip, r, g, b);

	/* a diamond at the top (a diamond = spawn; the region is a plain rectangle) */
	for (i = 0; i < 4; i++)
	{
		int j = (i + 1) & 3;

		a.vx = tip.vx + dir[i][0] * w; a.vy = tip.vy; a.vz = tip.vz + dir[i][1] * w;
		bb.vx = tip.vx + dir[j][0] * w; bb.vy = tip.vy; bb.vz = tip.vz + dir[j][1] * w;
		cd2WpnLine(&a, &bb, r, g, b);
	}
}

static void cd2EditorRect(int x0, int z0, int x1, int z1, int y, int r, int g, int b)
{
	int i;
	int px[4] = { x0, x1, x1, x0 };
	int pz[4] = { z0, z0, z1, z1 };

	for (i = 0; i < 4; i++)
	{
		VECTOR a, c;
		int j = (i + 1) & 3;

		a.vx = px[i]; a.vy = y; a.vz = pz[i];
		c.vx = px[j]; c.vy = y; c.vz = pz[j];
		cd2WpnLine(&a, &c, r, g, b);
	}
}

static void cd2EditorDraw(void)
{
	CAR_DATA* cp = cd2EditorCar();
	int y = (cp != NULL) ? cp->hd.where.t[1] : CD2_ED_Y;
	int i;

	if (!gEditorReady)
		return;

	/* the region: a plain rectangle, brighter once both corners are set */
	if (gWork.region.bounded)
		cd2EditorRect(gWork.region.x0, gWork.region.z0, gWork.region.x1, gWork.region.z1,
			y, 255, 200, 60);

	/* spawn 0 is the player: green; the rest red; the selected one big/white */
	for (i = 0; i < gWork.spawnCount && i < CD2_ARENA_MAX_SPAWNS; i++)
	{
		int sel = (i == gSelSlot);

		if (i == 0)
			cd2EditorBar(gWork.spawns[i].x, gWork.spawns[i].z, y,
				sel ? 255 : 80, 255, sel ? 255 : 120, sel);
		else
			cd2EditorBar(gWork.spawns[i].x, gWork.spawns[i].z, y,
				255, sel ? 255 : 120, sel ? 255 : 120, sel);
	}

	/* a single corner mark while A is pending */
	if (gCornerState == 1)
		cd2EditorBar(gCornerAx, gCornerAz, y, 255, 255, 120, 1);
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
static void cd2EditorPlace(void)
{
	CAR_DATA* cp = cd2EditorCar();

	if (cp == NULL || !gEditorReady)
		return;

	if (gSelSlot < 0)
		gSelSlot = 0;
	if (gSelSlot >= CD2_ARENA_MAX_SPAWNS)
		gSelSlot = CD2_ARENA_MAX_SPAWNS - 1;

	gWork.spawns[gSelSlot].x = cp->hd.where.t[0];
	gWork.spawns[gSelSlot].z = cp->hd.where.t[2];
	gWork.spawns[gSelSlot].heading = cp->hd.direction & 0xfff;

	if (gSelSlot + 1 > gWork.spawnCount)
		gWork.spawnCount = gSelSlot + 1;

	printInfo("[cainescrossfire] arena editor: spawn %d = (%d,%d) heading %d\n",
		gSelSlot, gWork.spawns[gSelSlot].x, gWork.spawns[gSelSlot].z,
		gWork.spawns[gSelSlot].heading);
}

static void cd2EditorDeleteNearest(void)
{
	CAR_DATA* cp = cd2EditorCar();
	long long best = 0;
	int i, bestI = -1;

	if (cp == NULL || gWork.spawnCount == 0)
		return;

	for (i = 0; i < gWork.spawnCount; i++)
	{
		long long dx = (long long)cp->hd.where.t[0] - gWork.spawns[i].x;
		long long dz = (long long)cp->hd.where.t[2] - gWork.spawns[i].z;
		long long d2 = dx * dx + dz * dz;

		if (bestI < 0 || d2 < best)
		{
			best = d2;
			bestI = i;
		}
	}

	if (bestI < 0)
		return;

	for (i = bestI; i < gWork.spawnCount - 1; i++)
		gWork.spawns[i] = gWork.spawns[i + 1];

	gWork.spawnCount--;

	if (gSelSlot >= gWork.spawnCount)
		gSelSlot = (gWork.spawnCount > 0) ? gWork.spawnCount - 1 : 0;

	printInfo("[cainescrossfire] arena editor: deleted spawn %d (%d left)\n",
		bestI, gWork.spawnCount);
}

static void cd2EditorCorner(void)
{
	CAR_DATA* cp = cd2EditorCar();
	int x, z;

	if (cp == NULL)
		return;

	x = cp->hd.where.t[0];
	z = cp->hd.where.t[2];

	if (gCornerState == 0)
	{
		gCornerAx = x;
		gCornerAz = z;
		gCornerState = 1;
		printInfo("[cainescrossfire] arena editor: corner A = (%d,%d)\n", x, z);
	}
	else
	{
		int x0 = (gCornerAx < x) ? gCornerAx : x;
		int x1 = (gCornerAx > x) ? gCornerAx : x;
		int z0 = (gCornerAz < z) ? gCornerAz : z;
		int z1 = (gCornerAz > z) ? gCornerAz : z;

		gWork.region.bounded = 1;
		gWork.region.x0 = x0;
		gWork.region.z0 = z0;
		gWork.region.x1 = x1;
		gWork.region.z1 = z1;
		gCornerState = 2;

		printInfo("[cainescrossfire] arena editor: region = %d,%d,%d,%d\n", x0, z0, x1, z1);
	}
}

static void cd2EditorSave(void)
{
	char path[512];

	if (!gEditorReady)
		return;

	if (!cd2ArenaFilePath(&gWork, path, sizeof(path)))
		return;

	if (cd2ArenaFileSave(path, &gWork))
	{
		/* update the LIVE arena so the barrier and the next spawn see the edit */
		cd2ArenaReplace(gWorkId, &gWork);

		printInfo("[cainescrossfire] arena editor: saved '%s' -> %s (%d spawns, %d pickups)\n",
			gWork.internalName, path, gWork.spawnCount, gWork.pickupCount);
	}
	else
	{
		printInfo("[cainescrossfire] arena editor: SAVE FAILED for %s (does CONFIG/arenas exist?)\n", path);
	}
}

static void cd2EditorReload(void)
{
	char path[512];

	if (!gEditorReady)
		return;

	if (!cd2ArenaFilePath(&gWork, path, sizeof(path)))
		return;

	if (cd2ArenaFileLoad(path, &gWork))
	{
		gSelSlot = 0;
		gCornerState = 0;
		printInfo("[cainescrossfire] arena editor: reloaded %s (%d spawns)\n",
			path, gWork.spawnCount);
	}
	else
	{
		printInfo("[cainescrossfire] arena editor: reload failed for %s\n", path);
	}
}

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------
static int cd2EditorOnCmdline(void* ud, void* args)
{
	JER_ARGS_CMDLINE* a = (JER_ARGS_CMDLINE*)args;
	int i;

	(void)ud;

	for (i = 1; i < a->argc; i++)
	{
		if (a->argv[i] != NULL && strcmp(a->argv[i], "-cceditor") == 0)
			gEditorOn = 1;
	}

	if (getenv("CC_EDITOR") != NULL)
		gEditorOn = 1;

	if (gEditorOn)
		printInfo("[cainescrossfire] arena editor: ON (-cceditor)\n");

	return JER_RESULT_CONTINUE;
}

static int cd2EditorOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	gEditorReady = 0;
	gLastPad = 0;

	if (gEditorOn)
		cd2EditorLoad();

	return JER_RESULT_CONTINUE;
}

static int cd2EditorOnFrame(void* ud, void* args)
{
	unsigned short pad, edge;

	(void)ud;
	(void)args;

	if (!gEditorOn)
		return JER_RESULT_CONTINUE;

	if (!gEditorReady)
	{
		if (!cd2EditorLoad())
			return JER_RESULT_CONTINUE;
	}

	/* the raw pad (shoulders included - the module strips them from the CAR_PAD
	 * stream, but the editor wants them) ORed with the debug driver's injected
	 * mask, so a scripted `pad:` step can drive the editor headlessly */
	pad = (unsigned short)(Pads[0].mapped | cd2DbgPadMask());
	edge = (unsigned short)(pad & ~gLastPad);
	gLastPad = pad;

	if (edge & MPAD_L1)	cd2EditorPlace();
	if (edge & MPAD_R1)	{ gSelSlot = (gSelSlot + 1) % ((gWork.spawnCount > 0) ? gWork.spawnCount : 1); }
	if (edge & MPAD_L2)	cd2EditorDeleteNearest();
	if (edge & MPAD_R2)	cd2EditorCorner();
	if (edge & MPAD_SELECT)	cd2EditorSave();
	if (edge & MPAD_START)	cd2EditorReload();

	cd2EditorDraw();

	{
		char line[128];

		snprintf(line, sizeof(line), "ARENA EDITOR: %s  spawn %d/%d  region %s  [L1 place R1 slot L2 del R2 corner SEL save START reload]",
			gWork.internalName, (gWork.spawnCount > 0) ? (gSelSlot + 1) : 0, gWork.spawnCount,
			gWork.region.bounded ? "set" : "none");
		jer_hud_panel(CD2_ED_PANEL, 0, line, 255, 230, 120);
	}

	return JER_RESULT_CONTINUE;
}

void cd2EditorRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CMDLINE, cd2EditorOnCmdline, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2EditorOnGameStart, NULL, 20);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2EditorOnFrame, NULL, -5);
}
