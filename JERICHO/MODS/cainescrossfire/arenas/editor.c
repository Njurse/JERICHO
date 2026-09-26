// arenas/editor.c — the IN-GAME ARENA EDITOR (a gated debug mode).
//
// Enabled by `-cceditor` on the command line or `CC_EDITOR=1` in the
// environment. The player's car IS the cursor: drive to a spot and press a
// button to drop a spawn point there (its position and heading are the car's),
// or to mark the region's corners. Everything is drawn as ghost markers with
// the engine's line primitives and a small on-screen readout, and the file is
// saved/reloaded on demand.
//
// The editor edits the match's LIVE arena (arenas/registry.c), so a change made
// in the other editor - a save from tools/arenaedit.py, writing the SAME .cca -
// is re-read by the runtime's file watcher and shows up here without a restart.
// That is the pseudo-realtime loop between the two editors. Edits made HERE are
// marked unsaved until SELECT, and the watcher leaves them alone meanwhile.
//
// Buttons (pad 0, EDGES - a press, not a hold):
//   L1      place / move the SELECTED spawn at the car
//   R1      cycle which spawn slot is selected
//   L2      delete the spawn nearest the car
//   R2      mark the region: first press = corner A, second = the rect (car = the other corner)
//   SELECT  save the arena to the mod's arenas/<name>.cca (and update the live arena)
//   START   reload the arena from its file (discards unsaved edits)
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
#include "weapons/core/crew.h"	/* cd2CrewRetractAll - crew sit in the car while editing */
#include "camera.h"		/* camera_position - where the freecam is */
#include "draw.h"		/* inv_camera_matrix - which way the freecam looks */
#include "dr2math.h"		/* ONE (4096) - the fixed-point scale of the camera basis */

/* The engine's F7 freecam toggle (utils/DebugOverlay.cpp). It is exported from
 * the game exe but has no public header; a JERICHO module is compiled as C++ and
 * links the exe's exported symbols, so a plain extern resolves it. */
extern int g_FreeCameraEnabled;
/* the engine's car placement (cars.c): copies hd.where into the rigid body's
 * fposition and rebuilds the orientation - the position the physics reads */
extern void TempBuildHandlingMatrix(CAR_DATA* cp, int init);
#include "weapons/core/weapon_internal.h"	/* cd2WpnLine - the ghost markers, cd2WpnPlayerCar */

#include <string.h>
#include <stdio.h>

int cd2ArenaFileLoad(const char* path, CD2_ARENA_PROFILE* out);
int cd2ArenaFileSave(const char* path, const CD2_ARENA_PROFILE* a);

#define CD2_ED_PANEL		3	/* the HUD slot the readout owns */
#define CD2_ED_MARK		90	/* ghost bar height (y-up) */
#define CD2_ED_CURSOR_RANGE	6000	/* how far in front of the freecam the cursor sits */

static int gEditorOn;			/* -cceditor / CC_EDITOR */
static int gEditorDirty;		/* an in-game edit is not saved yet */
static int gEditorHygiene;		/* one-shot: the weapons-off / crew-in note */
static int gSelSlot;
static int gCornerState;		/* 0 none, 1 A marked, 2 rect set */
static int gCornerAx, gCornerAz;
static unsigned short gLastPad;

int cd2EditorActive(void)
{
	return gEditorOn;
}

int cd2EditorHasUnsaved(void)
{
	return gEditorOn && gEditorDirty;
}

// Take a mutable copy of the match's live arena. Returns 0 when there is none.
static int cd2EditorBegin(CD2_ARENA_PROFILE* w)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();

	if (a == NULL)
		return 0;

	*w = *a;
	return 1;
}

// Write the working copy back into the live arena, unsaved.
static void cd2EditorCommit(const CD2_ARENA_PROFILE* w)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();

	if (a == NULL)
		return;

	cd2ArenaReplace(a->id, w);
	gEditorDirty = 1;
}

// The player's car, or NULL. The car IS the editor cursor.
static CAR_DATA* cd2EditorCar(void)
{
	CAR_DATA* cp = NULL;

	if (!cd2WpnPlayerCar(&cp))
		return NULL;

	return cp;
}

// Where the editor's CURSOR is, which is what every action places/moves/deletes
// relative to:
//   - freecam (engine F7) ON  -> the point the camera is looking at, so you can
//     lay an arena out from above without driving there;
//   - freecam off             -> back on the player's car (drive-and-place).
// Returns 0 when there is neither. `heading` is the car's when the car is the
// cursor, and 0 from the freecam (a camera has no heading); `fromFreecam` says
// which it was, for the readout.
static int cd2EditorCursor(int* x, int* y, int* z, int* heading, int* fromFreecam)
{
	CAR_DATA* cp;

	if (g_FreeCameraEnabled != 0)
	{
		/* the freecam's look direction is row 2 of the engine's inverse camera
		 * matrix, scaled by ONE (same expression DoFreeCamera uses to fly) */
		*x = camera_position.vx + (inv_camera_matrix.m[2][0] * CD2_ED_CURSOR_RANGE) / ONE;
		*y = camera_position.vy + (inv_camera_matrix.m[2][1] * CD2_ED_CURSOR_RANGE) / ONE;
		*z = camera_position.vz + (inv_camera_matrix.m[2][2] * CD2_ED_CURSOR_RANGE) / ONE;

		if (heading) *heading = 0;
		if (fromFreecam) *fromFreecam = 1;

		return 1;
	}

	cp = cd2EditorCar();

	if (cp == NULL)
		return 0;

	*x = cp->hd.where.t[0];
	*y = cp->hd.where.t[1];
	*z = cp->hd.where.t[2];

	if (heading) *heading = cp->hd.direction & 0xfff;
	if (fromFreecam) *fromFreecam = 0;

	return 1;
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
	const CD2_ARENA_PROFILE* w = cd2ArenaCurrent();
	CAR_DATA* cp = cd2EditorCar();
	int y = (cp != NULL) ? cp->hd.where.t[1] : 0;
	int i;

	if (w == NULL)
		return;

	/* the region: a plain rectangle */
	if (w->region.bounded)
		cd2EditorRect(w->region.x0, w->region.z0, w->region.x1, w->region.z1,
			y, 255, 200, 60);

	/* spawn 0 is the player: green; the rest red; the selected one big/white */
	for (i = 0; i < w->spawnCount && i < CD2_ARENA_MAX_SPAWNS; i++)
	{
		int sel = (i == gSelSlot);

		if (i == 0)
			cd2EditorBar(w->spawns[i].x, w->spawns[i].z, y,
				sel ? 255 : 80, 255, sel ? 255 : 120, sel);
		else
			cd2EditorBar(w->spawns[i].x, w->spawns[i].z, y,
				255, sel ? 255 : 120, sel ? 255 : 120, sel);
	}

	/* a single corner mark while A is pending */
	if (gCornerState == 1)
		cd2EditorBar(gCornerAx, gCornerAz, y, 255, 255, 120, 1);

	/* the CURSOR: cyan while it is the freecam's aim point (so it is obvious the
	 * car is not what you are about to place), hidden when it is the car itself
	 * (the car is its own marker) */
	{
		int cx, cy, cz, ch, fromFreecam;

		if (cd2EditorCursor(&cx, &cy, &cz, &ch, &fromFreecam) && fromFreecam)
			cd2EditorBar(cx, cz, cy, 120, 210, 255, 1);
	}
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
static void cd2EditorPlace(void)
{
	CD2_ARENA_PROFILE w;
	int x, y, z, heading, fromFreecam;

	if (!cd2EditorCursor(&x, &y, &z, &heading, &fromFreecam) || !cd2EditorBegin(&w))
		return;

	if (gSelSlot < 0)
		gSelSlot = 0;
	if (gSelSlot >= CD2_ARENA_MAX_SPAWNS)
		gSelSlot = CD2_ARENA_MAX_SPAWNS - 1;

	w.spawns[gSelSlot].x = x;
	w.spawns[gSelSlot].z = z;
	w.spawns[gSelSlot].y = y;		/* the cursor's height, so cars do not sink */
	w.spawns[gSelSlot].heading = heading;	/* 0 from the freecam */

	if (gSelSlot + 1 > w.spawnCount)
		w.spawnCount = gSelSlot + 1;

	cd2EditorCommit(&w);

	printInfo("[cainescrossfire] arena editor: spawn %d = (%d,%d,%d) heading %d (%s)\n",
		gSelSlot, x, y, z, heading, fromFreecam ? "freecam" : "car");
}

static void cd2EditorDeleteNearest(void)
{
	CD2_ARENA_PROFILE w;
	long long best = 0;
	int i, bestI = -1;
	int cx, cy, cz, ch, fromFreecam;

	if (!cd2EditorCursor(&cx, &cy, &cz, &ch, &fromFreecam) ||
	    !cd2EditorBegin(&w) || w.spawnCount == 0)
		return;

	for (i = 0; i < w.spawnCount; i++)
	{
		long long dx = (long long)cx - w.spawns[i].x;
		long long dz = (long long)cz - w.spawns[i].z;
		long long d2 = dx * dx + dz * dz;

		if (bestI < 0 || d2 < best)
		{
			best = d2;
			bestI = i;
		}
	}

	if (bestI < 0)
		return;

	for (i = bestI; i < w.spawnCount - 1; i++)
		w.spawns[i] = w.spawns[i + 1];

	w.spawnCount--;

	if (gSelSlot >= w.spawnCount)
		gSelSlot = (w.spawnCount > 0) ? w.spawnCount - 1 : 0;

	cd2EditorCommit(&w);

	printInfo("[cainescrossfire] arena editor: deleted spawn %d (%d left)\n",
		bestI, w.spawnCount);
}

static void cd2EditorCorner(void)
{
	CD2_ARENA_PROFILE w;
	int x, z, y, heading, fromFreecam;

	if (!cd2EditorCursor(&x, &y, &z, &heading, &fromFreecam) || !cd2EditorBegin(&w))
		return;

	if (gCornerState == 0)
	{
		gCornerAx = x;
		gCornerAz = z;
		gCornerState = 1;
		printInfo("[cainescrossfire] arena editor: corner A = (%d,%d)\n", x, z);
		return;
	}

	w.region.bounded = 1;
	w.region.x0 = (gCornerAx < x) ? gCornerAx : x;
	w.region.x1 = (gCornerAx > x) ? gCornerAx : x;
	w.region.z0 = (gCornerAz < z) ? gCornerAz : z;
	w.region.z1 = (gCornerAz > z) ? gCornerAz : z;
	gCornerState = 2;

	cd2EditorCommit(&w);

	printInfo("[cainescrossfire] arena editor: region = %d,%d,%d,%d\n",
		w.region.x0, w.region.z0, w.region.x1, w.region.z1);
}

// The quick way to BE somewhere: fly the freecam over a spot and press TRIANGLE
// to drop the car there, then drive it to feel the arena out - no restart, no
// walking the whole map. A no-op unless the freecam is the cursor (otherwise the
// car is already where the cursor is).
static void cd2EditorWarpCar(void)
{
	CAR_DATA* cp = cd2EditorCar();
	int x, y, z, heading, fromFreecam;

	if (cp == NULL || !cd2EditorCursor(&x, &y, &z, &heading, &fromFreecam) || !fromFreecam)
		return;

	cp->hd.where.t[0] = x;
	cp->hd.where.t[1] = y;
	cp->hd.where.t[2] = z;

	cp->st.n.linearVelocity[0] = 0;
	cp->st.n.linearVelocity[1] = 0;
	cp->st.n.linearVelocity[2] = 0;
	cp->st.n.angularVelocity[0] = 0;
	cp->st.n.angularVelocity[1] = 0;
	cp->st.n.angularVelocity[2] = 0;

	/* the engine reads the rigid body, not hd.where: this copies where into
	 * fposition and rebuilds the orientation from hd.direction */
	TempBuildHandlingMatrix(cp, 1);

	printInfo("[cainescrossfire] arena editor: warped car %d to (%d,%d,%d)\n", cp->id, x, y, z);
}

static void cd2EditorSave(void)
{
	const CD2_ARENA_PROFILE* w = cd2ArenaCurrent();
	char path[512];

	if (w == NULL || !cd2ArenaFilePath(w, path, sizeof(path)))
		return;

	if (cd2ArenaFileSave(path, w))
	{
		gEditorDirty = 0;
		cd2ArenaWatchReset();	/* our write is not "an external change" */

		printInfo("[cainescrossfire] arena editor: saved '%s' -> %s (%d spawns, %d pickups)\n",
			w->internalName, path, w->spawnCount, w->pickupCount);
	}
	else
	{
		printInfo("[cainescrossfire] arena editor: SAVE FAILED for %s (does the arenas folder exist?)\n", path);
	}
}

static void cd2EditorReload(void)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	CD2_ARENA_PROFILE w;
	char path[512];

	if (a == NULL || !cd2ArenaFilePath(a, path, sizeof(path)))
		return;

	if (cd2ArenaFileLoad(path, &w))
	{
		cd2ArenaReplace(a->id, &w);
		gEditorDirty = 0;
		gSelSlot = 0;
		gCornerState = 0;
		cd2ArenaWatchReset();

		printInfo("[cainescrossfire] arena editor: reloaded %s (%d spawns)\n",
			path, w.spawnCount);
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
	const CD2_ARENA_PROFILE* a;

	(void)ud;
	(void)args;

	gEditorDirty = 0;
	gSelSlot = 0;
	gCornerState = 0;
	gLastPad = 0;

	if (!gEditorOn)
		return JER_RESULT_CONTINUE;

	a = cd2ArenaCurrent();

	cd2CrewRetractAll();

	if (a != NULL)
		printInfo("[cainescrossfire] arena editor: editing '%s' (%d spawns, %d pickups)\n",
			a->internalName, a->spawnCount, a->pickupCount);

	return JER_RESULT_CONTINUE;
}

static int cd2EditorOnFrame(void* ud, void* args)
{
	const CD2_ARENA_PROFILE* w;
	unsigned short pad, edge;
	char line[220];

	(void)ud;
	(void)args;

	if (!gEditorOn)
		return JER_RESULT_CONTINUE;

	/* the player's own car is the cursor: weapons are off while editing (see
	 * cd2WpnOnFrame), so send any crew that was leaning out back inside */
	cd2CrewRetractAll();

	if (!gEditorHygiene)
	{
		gEditorHygiene = 1;
		printInfo("[cainescrossfire] arena editor: weapons disabled and crew retracted - "
			"the shoulder buttons are the editor's while it is on\n");
	}

	w = cd2ArenaCurrent();

	if (w == NULL)
		return JER_RESULT_CONTINUE;

	/* the raw pad (shoulders included - the module strips them from the CAR_PAD
	 * stream, but the editor wants them) ORed with the debug driver's injected
	 * mask, so a scripted `pad:` step can drive the editor headlessly */
	pad = (unsigned short)(Pads[0].mapped | cd2DbgPadMask());
	edge = (unsigned short)(pad & ~gLastPad);
	gLastPad = pad;

	if (edge & MPAD_L1)	cd2EditorPlace();
	if (edge & MPAD_R1)	{ gSelSlot = (gSelSlot + 1) % ((w->spawnCount > 0) ? w->spawnCount : 1); }
	if (edge & MPAD_L2)	cd2EditorDeleteNearest();
	if (edge & MPAD_R2)	cd2EditorCorner();
	if (edge & MPAD_TRIANGLE)	cd2EditorWarpCar();
	if (edge & MPAD_SELECT)	cd2EditorSave();
	if (edge & MPAD_START)	cd2EditorReload();

	/* the arena may have been replaced (by an action above, or by the runtime's
	 * file watcher picking up a Python-editor save) - re-read for the draw */
	w = cd2ArenaCurrent();

	if (w == NULL)
		return JER_RESULT_CONTINUE;

	cd2EditorDraw();

	snprintf(line, sizeof(line),
		"ARENA EDITOR: %s%s  spawn %d/%d  region %s  cursor %s  [L1 place  R1 slot  L2 del  R2 corner  TRI warp  SEL save  START reload]",
		w->internalName, gEditorDirty ? " *unsaved*" : "",
		(w->spawnCount > 0) ? (gSelSlot + 1) : 0, w->spawnCount,
		w->region.bounded ? "set" : "none",
		(g_FreeCameraEnabled != 0) ? "FREECAM (F7)" : "car");
	jer_hud_panel(CD2_ED_PANEL, 0, line, gEditorDirty ? 255 : 220, 230, 120);

	return JER_RESULT_CONTINUE;
}

void cd2EditorRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CMDLINE, cd2EditorOnCmdline, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2EditorOnGameStart, NULL, 20);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2EditorOnFrame, NULL, -5);
}
