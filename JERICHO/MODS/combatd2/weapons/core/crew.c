// weapons/core/crew.c — Combat D2 MOUNTED CREW: lean request state + ped lifecycle.
//
// Two halves, one system:
//
//   1. The REQUEST (which window a crew member should be leaning out of). Every
//      weapon fire — the player's and the AI's — comes through cd2WpnTryFire,
//      which calls cd2CrewNotifyFire with that weapon's lean flags. Flags OR
//      together and each set side gets its hold refreshed, so a side that any
//      leaning weapon wants out stays out even while another firing weapon (the
//      base MG, leanOut 0) wants nobody.
//
//   2. The PED lifecycle, driven off that request. While a side's hold is live
//      a TANNER_MODEL ped (model 0) is spawned beside the car's door, plays the
//      engine's PED_ACTION_GETOUTCAR (get out) animation to its final frame and
//      HOLDS there, re-placed from the car's transform every frame so it rides
//      the car. When the hold lapses it plays PED_ACTION_GETINCAR (get back in)
//      and is destroyed as it disappears. We drive the frames ourselves rather
//      than through the engine's PedGetInCar/PedGetOutCar (those hand the car
//      and the peds back to the PLAYER and would hijack the player's vehicle).
//
// Both crew members use Tanner's model (model 0) — the only ped model that is
// always loaded and that renders through the skeleton path (customers use the
// sprite path, which cannot play the get-out pose and gets recycled by the
// ambient system).

#include "driver2.h"
#include "combatd2.h"
#include "cars.h"
#include "camera.h"
#include "players.h"		/* player[] */
#include "dr2roads.h"		/* MapHeight */
#include "debris.h"		/* Setup_Smoke / SMOKE_FIRE */
#include "pedest.h"		/* LPPEDESTRIAN, pUsedPeds, DestroyPedestrian */
#include "jericho.h"
#include "jer_events.h"
#include "jer_npc.h"		/* jer_npc_* ped scaffolding */
#include "ai/ai.h"		/* cd2AiIsOpponent */
#include "crew.h"

// How long a side stays out after its last leaning shot, in frames (~0.8s at
// 30fps). The hold is RE-ARMED on every shot, so it only needs to bridge the
// gap between shots: shorter than any leaning weapon's refire (the shotgun is
// ~2.2s) yet long enough that a held trigger keeps the ped out continuously.
#define CD2_CREW_HOLD_FRAMES	24

// The get-out pose is held at the last "climbing out" frame; the engine's own
// transition would fire at 15. The get-in animation plays 0..14 and the ped is
// destroyed as it reaches 15 (matching PedGetInCar's `frame1 < 0xf`).
#define CD2_CREW_GETOUT_LAST	14
#define CD2_CREW_GETIN_LAST	15

// The get-out frame each side settles on. The DRIVER stops earlier, mid-climb,
// so he reads as leaning out of the window with the arm reach below; the
// GUNNER plays the full climb and then sits up on the sill.
#define CD2_CREW_DRIVER_HOLD	9

// The gunner's resting pose once out of the car: a SIT raised this far above
// the ped's normal standing height, so he perches ON the windowsill rather
// than standing beside the car. The render frame is Y-down, so raising means
// subtracting.
#define CD2_CREW_SIT_FRAME	0
#define CD2_CREW_SILL_RAISE	26

// Driver arm reach, forced through JER_EVENT_PED_SKELETON phase 0 (the
// position channel). Ped-local units, parent-relative, mirroring d2pl's proven
// POSE_AIM: the forearm is raised and pushed forward and the hand extends past
// it - handZ must EXCEED elbowZ or the arm folds back on itself.
#define CD2_CREW_ARM_ELBOW_Y	-1	// raise the forearm
#define CD2_CREW_ARM_ELBOW_Z	1	// reach it forward
#define CD2_CREW_ARM_HAND_Y	-1	// raise the hand
#define CD2_CREW_ARM_HAND_Z	1	// and extend it past the elbow

// Both crew peds share ONE body yaw (see cd2CrewPlace), so the driver's reach
// is rotated half a turn to point out of HIS window instead of the gunner's.
#define CD2_CREW_ARM_FLIP	2048

// No crew is spawned for a car further than this from the camera (world units):
// it would never be drawn, and the ped pool is shared with the ambient civs.
// Generous, because the chase camera sits ~1000 units behind the player's car.
#define CD2_CREW_MAX_DIST	2000

// Crew ped states.
enum { CD2_CREW_IN = 0, CD2_CREW_OUT, CD2_CREW_INANIM, CD2_CREW_FLEE };

// Side indices (0 = driver / left, 1 = gunner / right). The matching lean flag
// is (1 << index) == CD2_CREW_DRIVER/GUNNER.
enum { CD2_CREW_SIDE_DRIVER = 0, CD2_CREW_SIDE_GUNNER = 1 };

// Wreck bail-out: when a car the module drives is destroyed, its crew (both
// sides) bails out and runs from the wreck, on fire, for this many frames
// (~3s at 30fps) before being cleaned up.
#define CD2_CREW_FLEE_FRAMES	90
#define CD2_CREW_FLEE_SPEED	8	// world units / frame
#define CD2_CREW_FLEE_SWAY	40	// heading swing per step (PSX angle units):
					// 4096/90 ~= 45-frame sine period (~1.5s) - a slow weave
#define CD2_CREW_FLEE_AMP	700	// sway amplitude (fixed point, 4096 = 1.0)

// Death cam: while the player's car is a wreck the held view slowly PULLS BACK
// and RISES over the respawn delay, so the death reads rather than just
// freezing. Values are world units at full progress (t = 1).
#define CD2_DEATHCAM_ZOOM	700	// pull the camera back this far
#define CD2_DEATHCAM_RISE	220	// and raise it this far (engine Y is down)
#define CD2_DEATHCAM_DELAY	300	// fallback respawn window when the cfg has 0

typedef struct CD2_CREW_CAR
{
	int hold[2];		// frames the side stays out (0 = in)
	JerNpc* ped[2];		// the crew ped (NULL = in the car)
	int state[2];		// CD2_CREW_IN / OUT / INANIM / FLEE
	int frame[2];		// animation frame within the current phase
	int fled;		// 1 once the wreck bail-out has been started
	int fleeTicks[2];	// frames the side has been fleeing
	int fleeHead[2];	// base heading to flee along (away from the wreck)
	int raiseY[2];		// extra height for the resting pose (sill perch), 0 = none
} CD2_CREW_CAR;

static CD2_CREW_CAR gCrew[MAX_CARS];

// The camera pose held while the local player's car is a wreck (last live frame).
static VECTOR sCamPos;
static SVECTOR sCamAng;
static int sCamHeld;
static int sCamTicks;		// frames since the death began (drives the drift)
static VECTOR sCamFocus;	// the wreck's position at the moment of death

// ped lifecycle (defined below; the wreck bail-out uses them)
static void cd2CrewSpawnSide(CD2_CREW_CAR* c, int i, const CAR_DATA* cp);
static void cd2CrewDespawnSide(CD2_CREW_CAR* c, int i);

// ---------------------------------------------------------------------------
// request
// ---------------------------------------------------------------------------

// Refresh (arm) the requested sides on a car's crew hold.
static void cd2CrewRequest(const CAR_DATA* c, int leanMask)
{
	if (c == NULL || c->id < 0 || c->id >= MAX_CARS)
		return;

	if (leanMask & CD2_CREW_DRIVER)
		gCrew[c->id].hold[CD2_CREW_SIDE_DRIVER] = CD2_CREW_HOLD_FRAMES;

	if (leanMask & CD2_CREW_GUNNER)
		gCrew[c->id].hold[CD2_CREW_SIDE_GUNNER] = CD2_CREW_HOLD_FRAMES;
}

void cd2CrewNotifyFire(const CAR_DATA* cp, int leanMask)
{
	cd2CrewRequest(cp, leanMask);

	// Observability: a leaning shot fired (leanMask 0 = the MG / a non-leaning
	// weapon, deliberately silent). This is what a headless run greps to prove
	// the flags reach the crew state for the player AND for AI cars.
	if (leanMask != 0 && gCd2Cfg.debugLog && cp != NULL && cp->id >= 0 && cp->id < MAX_CARS)
		printInfo("[combatd2] crew: car=%d fired lean=0x%X -> driver=%d gunner=%d\n",
			cp->id, leanMask,
			gCrew[cp->id].hold[CD2_CREW_SIDE_DRIVER],
			gCrew[cp->id].hold[CD2_CREW_SIDE_GUNNER]);
}

void cd2CrewArmed(const CAR_DATA* cp, int leanMask)
{
	// Runs every frame while a leaning weapon is selected, so no log here.
	cd2CrewRequest(cp, leanMask);
}

int cd2CrewSideOut(const CAR_DATA* cp, int side)
{
	const CAR_DATA* c = cp;
	int i;

	if (c == NULL || c->id < 0 || c->id >= MAX_CARS)
		return 0;

	i = (side == CD2_CREW_GUNNER) ? CD2_CREW_SIDE_GUNNER : CD2_CREW_SIDE_DRIVER;

	return gCrew[c->id].hold[i] > 0;
}

int cd2CrewPedCount(void)
{
	int i, side;
	int n = 0;

	for (i = 0; i < MAX_CARS; i++)
		for (side = 0; side < 2; side++)
			if (gCrew[i].ped[side] != NULL)
				n++;

	return n;
}

int cd2CrewPedPos(const CAR_DATA* cp, int side, int out[3])
{
	const CAR_DATA* c = cp;
	LPPEDESTRIAN pPed;
	int i;

	if (c == NULL || c->id < 0 || c->id >= MAX_CARS)
		return 0;

	i = (side == CD2_CREW_GUNNER) ? CD2_CREW_SIDE_GUNNER : CD2_CREW_SIDE_DRIVER;

	pPed = (LPPEDESTRIAN)gCrew[c->id].ped[i];

	if (pPed == NULL)
		return 0;

	out[0] = pPed->position.vx;
	out[1] = pPed->position.vy;
	out[2] = pPed->position.vz;

	return 1;
}

// ---------------------------------------------------------------------------
// ped lifecycle
// ---------------------------------------------------------------------------

// A car the module drives: the player's car, or an AI opponent. Traffic and
// empty slots never carry crew.
static int cd2CrewOwned(const CAR_DATA* cp)
{
	return cp->controlType == CONTROL_TYPE_PLAYER || cd2AiIsOpponent(cp);
}

// Is the car close enough to the camera to bother with a crew ped?
static int cd2CrewNear(const CAR_DATA* cp)
{
	long long dx = (long long)cp->hd.where.t[0] - camera_position.vx;
	long long dz = (long long)cp->hd.where.t[2] - camera_position.vz;

	return (dx * dx + dz * dz) <= (long long)CD2_CREW_MAX_DIST * CD2_CREW_MAX_DIST;
}

// A crew ped can be destroyed from under us (level reset, a cutscene). pUsedPeds
// is the authority on what exists, so trust it rather than a stale pointer.
static int cd2CrewPedAlive(LPPEDESTRIAN p)
{
	LPPEDESTRIAN q = pUsedPeds;

	while (q != NULL)
	{
		if (q == p)
			return 1;

		q = q->pNext;
	}

	return 0;
}

// The local player's CAR_DATA, or NULL when on foot.
static CAR_DATA* cd2CrewPlayerCar(void)
{
	int id = player[0].playerCarId;

	if (id < 0 || id >= MAX_CARS)
		return NULL;

	return &car_data[id];
}

// ---------------------------------------------------------------------------
// Wreck bail-out: the crew runs from the burning wreck.
// ---------------------------------------------------------------------------

// Begin the flee for one side: make sure a ped exists, then point it away from
// the wreck and mark it fleeing (it burns - the PED_DRAW handler paints it).
static void cd2CrewFleeSide(CD2_CREW_CAR* c, int i, const CAR_DATA* cp, int wx, int wz)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)c->ped[i];

	if (pPed == NULL)
	{
		// wasn't leaning: put them out at the door first so they can run
		cd2CrewSpawnSide(c, i, cp);
		pPed = (LPPEDESTRIAN)c->ped[i];
	}

	if (pPed == NULL)
		return;		// too far / no ped available - nobody bails

	c->state[i] = CD2_CREW_FLEE;
	c->fleeTicks[i] = 0;
	c->hold[i] = 0;

	{
		int dx = pPed->position.vx - wx;
		int dz = pPed->position.vz - wz;

		if (dx == 0 && dz == 0)
		{
			dx = RSIN(cp->hd.direction);
			dz = RCOS(cp->hd.direction);
		}

		c->fleeHead[i] = ratan2(dx, dz);	// heading AWAY from the wreck
	}

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] crew: car=%d side=%d bails out (wreck)\n", cp->id, i);
}

static void cd2CrewFleeUpdate(CD2_CREW_CAR* c, int i, const CAR_DATA* cp)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)c->ped[i];
	int dir, head;
	VECTOR g;

	(void)cp;

	if (pPed == NULL || !cd2CrewPedAlive(pPed))
	{
		c->ped[i] = NULL;
		c->state[i] = CD2_CREW_IN;
		return;
	}

	if (c->fleeTicks[i]++ >= CD2_CREW_FLEE_FRAMES)
	{
		cd2CrewDespawnSide(c, i);
		return;
	}

	// a wavy sine path: the heading away from the wreck, swung by a sine so the
	// ped weaves as it runs
	head = c->fleeHead[i] +
	       ((RSIN(c->fleeTicks[i] * CD2_CREW_FLEE_SWAY) * CD2_CREW_FLEE_AMP) >> 12);

	// step along it, exactly like the engine's AnimatePed forward branch
	pPed->dir.vy = (head + 2048) & 0xfff;
	pPed->speed = CD2_CREW_FLEE_SPEED;

	dir = pPed->dir.vy - 2048;
	pPed->position.vx += FIXED(CD2_CREW_FLEE_SPEED * RSIN(dir));
	pPed->position.vz += FIXED(CD2_CREW_FLEE_SPEED * RCOS(dir));

	// running animation (the same 16-frame cycle the engine uses), then ground
	jer_npc_set_action(c->ped[i], PED_ACTION_RUN, c->fleeTicks[i] & 15);

	g.vx = pPed->position.vx;
	g.vz = pPed->position.vz;
	g.vy = 0;
	pPed->position.vy = -MapHeight(&g) - 130;

	// fire at the ped's origin so they read as running away in flames
	if ((c->fleeTicks[i] & 7) == 0)
	{
		VECTOR sp, drift;

		sp.vx = pPed->position.vx;
		sp.vz = pPed->position.vz;
		sp.vy = pPed->position.vy + 130;	// undo the root offset (smoke is y-up)

		drift.vx = 0;
		drift.vy = 0;
		drift.vz = 0;

		Setup_Smoke(&sp, 20, 60, SMOKE_FIRE, 0, &drift, 0);
	}
}

// ---------------------------------------------------------------------------
// The driver's arm pose, forced through JER_EVENT_PED_SKELETON phase 0 (the
// POSITION channel — see JERICHO/docs/ped-animation.md). The hook hands the
// engine's BONE array over as a void*, so mirror the layout (exactly as d2pl
// does) to index it. vOffset is the rest parent->child delta; vCurrPos is what
// the draw accumulates and it is reset every frame, so the pose is re-applied
// on every draw.
// ---------------------------------------------------------------------------
typedef struct CD2_BONE
{
	int id;
	struct CD2_BONE* pParent;
	char numChildren;
	struct CD2_BONE* pChildren[3];
	void* pvOrigPos;
	void* pvRotation;
	VECTOR vOffset;
	VECTOR vCurrPos;
	void** pModel;
} CD2_BONE;

enum { CD2_LIMB_RSHOULDER = 9, CD2_LIMB_RELBOW = 10, CD2_LIMB_RHAND = 11 };

// Rotate a local (x, z) pose offset by the ped's facing heading (RSIN/RCOS are
// 4096-scaled). This mirrors how newRotateBones builds the skeleton frame: the
// model rotated by the ped's yaw.
static void cd2CrewRotatePose(int* px, int* pz, int h)
{
	int x = *px;
	int z = *pz;
	int c = RCOS(h);
	int s = RSIN(h);

	*px = (x * c + z * s) >> 12;
	*pz = (-x * s + z * c) >> 12;
}

// Force the right arm into a weapon-holding reach: the shoulder stays at its
// rest offset, the forearm is raised and pushed forward, and the hand extends
// beyond it. `facing` is the heading the pose's local +Z points along.
static void cd2CrewPoseArm(void* skelVoid, int facing)
{
	CD2_BONE* skel = (CD2_BONE*)skelVoid;
	int ex, ez, hx, hz;
	int shoulderX, shoulderY, shoulderZ;

	skel[CD2_LIMB_RSHOULDER].vCurrPos.vx = skel[CD2_LIMB_RSHOULDER].vOffset.vx;
	skel[CD2_LIMB_RSHOULDER].vCurrPos.vy = skel[CD2_LIMB_RSHOULDER].vOffset.vy;
	skel[CD2_LIMB_RSHOULDER].vCurrPos.vz = skel[CD2_LIMB_RSHOULDER].vOffset.vz;

	shoulderX = skel[CD2_LIMB_RSHOULDER].vCurrPos.vx;
	shoulderY = skel[CD2_LIMB_RSHOULDER].vCurrPos.vy;
	shoulderZ = skel[CD2_LIMB_RSHOULDER].vCurrPos.vz;

	ex = 0;
	ez = CD2_CREW_ARM_ELBOW_Z;
	cd2CrewRotatePose(&ex, &ez, facing);
	skel[CD2_LIMB_RELBOW].vCurrPos.vx = shoulderX + ex;
	skel[CD2_LIMB_RELBOW].vCurrPos.vy = shoulderY + CD2_CREW_ARM_ELBOW_Y;
	skel[CD2_LIMB_RELBOW].vCurrPos.vz = shoulderZ + ez;

	hx = 0;
	hz = CD2_CREW_ARM_HAND_Z;
	cd2CrewRotatePose(&hx, &hz, facing);
	skel[CD2_LIMB_RHAND].vCurrPos.vx = skel[CD2_LIMB_RELBOW].vCurrPos.vx + hx;
	skel[CD2_LIMB_RHAND].vCurrPos.vy = skel[CD2_LIMB_RELBOW].vCurrPos.vy + CD2_CREW_ARM_HAND_Y;
	skel[CD2_LIMB_RHAND].vCurrPos.vz = skel[CD2_LIMB_RELBOW].vCurrPos.vz + hz;
}

// JER_EVENT_PED_SKELETON: fires for the player ped and for every module-owned
// ped (jer_npc_owned), so filter to OURS - and pose only the driver, who is
// the one leaning out with a hand pointed.
static int cd2CrewOnPedSkeleton(void* ud, void* args)
{
	JER_ARGS_PED_SKELETON* a = (JER_ARGS_PED_SKELETON*)args;
	int i;

	(void)ud;

	if (a == NULL || a->ped == NULL || a->skel == NULL)
		return JER_RESULT_CONTINUE;

	// the position channel only, and never during the shadow pass
	if (a->phase != 0 || a->shadow)
		return JER_RESULT_CONTINUE;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MAX_CARS; i++)
	{
		if ((void*)gCrew[i].ped[CD2_CREW_SIDE_DRIVER] == a->ped &&
		    gCrew[i].state[CD2_CREW_SIDE_DRIVER] != CD2_CREW_FLEE)
		{
			LPPEDESTRIAN pPed = (LPPEDESTRIAN)a->ped;

			cd2CrewPoseArm(a->skel, (pPed->dir.vy + CD2_CREW_ARM_FLIP) & 0xfff);
			break;
		}
	}

	return JER_RESULT_CONTINUE;
}

// Hang the ped on the car's door: the side of the body just outside the panel,
// a touch ahead of the cabin centre, facing outward, grounded against the map at
// its own x/z. Driven by the same box/matrix math the weapon muzzles use.
static void cd2CrewPlace(LPPEDESTRIAN pPed, const CAR_DATA* cp, int i, int raiseY)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int s = (i == CD2_CREW_SIDE_DRIVER) ? -1 : 1;	// -1 = driver (left door), +1 = gunner (right door)
	int lat = (cb->vx * 108) / 100;			// just outside the body side
	int fwd = cb->vz / 4;				// a touch ahead of centre, not the rear
	int x, z, yaw;
	VECTOR g;

	x = w->t[0] + (int)(((long long)w->m[0][0] * lat * s) >> 12)
	           + (int)(((long long)w->m[0][2] * fwd) >> 12);
	z = w->t[2] + (int)(((long long)w->m[2][0] * lat * s) >> 12)
	           + (int)(((long long)w->m[2][2] * fwd) >> 12);

	g.vx = x;
	g.vz = z;
	g.vy = 0;

	// Facing: the SAME yaw at both doors. The ped model's front is not aligned
	// with the yaw vector, so mirroring the offsets per side (direction -/+ 1024,
	// which is how the engine orients a ped climbing out of a door in
	// SetupGetOutCar) leaves one door right and the other 180 out - verified
	// in-game both ways round. dir - 1024 reads correctly on BOTH sides.
	// Only the lateral offset above is per-side.
	yaw = (cp->hd.direction - 1024) & 0xfff;

	jer_npc_set_world((JerNpc*)pPed, x, -MapHeight(&g) - 130 - raiseY, z, yaw);
}

static void cd2CrewSpawnSide(CD2_CREW_CAR* c, int i, const CAR_DATA* cp)
{
	LPPEDESTRIAN pPed;

	if (c->ped[i] != NULL)
		return;

	if (!cd2CrewNear(cp))
		return;

	pPed = (LPPEDESTRIAN)jer_npc_spawn_model(TANNER_MODEL, cp->hd.where.t[0], cp->hd.where.t[2]);

	if (pPed == NULL)
	{
		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] crew: car=%d side=%d skip: ped pool full\n", cp->id, i);
		return;
	}

	// A crew ped is nobody's player: -1 keeps the player-only pose/draw hooks
	// (which gate on padId >= 0) and the shadow off.
	pPed->padId = -1;
	pPed->index = -1;
	pPed->head_rot = 0;
	pPed->flags = 0;

	c->ped[i] = (JerNpc*)pPed;
	c->state[i] = CD2_CREW_OUT;
	c->frame[i] = 0;
	c->raiseY[i] = 0;

	jer_npc_set_action(c->ped[i], PED_ACTION_GETOUTCAR, 0);
	cd2CrewPlace(pPed, cp, i, c->raiseY[i]);

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] crew: car=%d side=%d out (get out)\n", cp->id, i);
}

static void cd2CrewDespawnSide(CD2_CREW_CAR* c, int i)
{
	if (c->ped[i] != NULL)
		jer_npc_despawn(c->ped[i]);

	c->ped[i] = NULL;
	c->state[i] = CD2_CREW_IN;
	c->frame[i] = 0;
	c->raiseY[i] = 0;
}

// Drive the side's "out" pose for its current frame: climb out, then settle
// into the resting pose. The DRIVER stops mid-climb, leaning out of the window
// (the arm reach is forced separately, in the skeleton hook); the GUNNER
// completes the climb and then perches ON the windowsill - a SIT, raised.
static void cd2CrewApplyOutPose(CD2_CREW_CAR* c, int i)
{
	int last = (i == CD2_CREW_SIDE_DRIVER) ? CD2_CREW_DRIVER_HOLD : CD2_CREW_GETOUT_LAST;

	if (c->frame[i] < last)
	{
		c->frame[i]++;
		jer_npc_set_action(c->ped[i], PED_ACTION_GETOUTCAR, c->frame[i]);
		c->raiseY[i] = 0;
	}
	else if (i == CD2_CREW_SIDE_GUNNER)
	{
		if (c->raiseY[i] == 0 && gCd2Cfg.debugLog)
			printInfo("[combatd2] crew: side=%d perched on the sill (SIT +%d)\n", i, CD2_CREW_SILL_RAISE);

		jer_npc_set_action(c->ped[i], PED_ACTION_SIT, CD2_CREW_SIT_FRAME);
		c->raiseY[i] = CD2_CREW_SILL_RAISE;
	}
	else
	{
		// the driver holds the mid-climb lean
		jer_npc_set_action(c->ped[i], PED_ACTION_GETOUTCAR, CD2_CREW_DRIVER_HOLD);
		c->raiseY[i] = 0;
	}
}

static void cd2CrewUpdateSide(CD2_CREW_CAR* c, int i, const CAR_DATA* cp)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)c->ped[i];
	int wantOut = c->hold[i] > 0;

	// the ped may have been destroyed under us - the list is authoritative
	if (pPed != NULL && !cd2CrewPedAlive(pPed))
	{
		c->ped[i] = NULL;
		c->state[i] = CD2_CREW_IN;
		c->frame[i] = 0;
		pPed = NULL;
	}

	if (wantOut)
	{
		if (pPed == NULL)
		{
			cd2CrewSpawnSide(c, i, cp);
			pPed = (LPPEDESTRIAN)c->ped[i];
		}
		else if (c->state[i] == CD2_CREW_INANIM)
		{
			// fire resumed mid get-in: lean straight back out
			c->state[i] = CD2_CREW_OUT;
			c->frame[i] = 0;
		}

		if (pPed != NULL)
		{
			if (c->state[i] == CD2_CREW_OUT)
				cd2CrewApplyOutPose(c, i);

			cd2CrewPlace(pPed, cp, i, c->raiseY[i]);
		}
	}
	else if (pPed != NULL)
	{
		if (c->state[i] != CD2_CREW_INANIM)
		{
			c->state[i] = CD2_CREW_INANIM;
			c->frame[i] = 0;

			if (gCd2Cfg.debugLog)
				printInfo("[combatd2] crew: car=%d side=%d in (get in)\n", cp->id, i);
		}

		c->frame[i]++;

		if (c->frame[i] >= CD2_CREW_GETIN_LAST)
		{
			cd2CrewDespawnSide(c, i);
		}
		else
		{
			jer_npc_set_action(c->ped[i], PED_ACTION_GETINCAR, c->frame[i]);
			cd2CrewPlace(pPed, cp, i, c->raiseY[i]);
		}
	}
}

// ---------------------------------------------------------------------------
// JER_EVENT_FRAME: age the holds, then drive every car's crew.
//
// Registered at priority 1 so it runs AFTER the weapon FRAME hook (priority 0)
// that fires the weapons: a shot fired this frame is still fully "out" for the
// ped render this frame, and only ages from the next frame on.
// ---------------------------------------------------------------------------
static int cd2CrewOnFrame(void* ud, void* args)
{
	int i, side;
	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (gCrew[i].hold[CD2_CREW_SIDE_DRIVER] > 0)
			gCrew[i].hold[CD2_CREW_SIDE_DRIVER]--;

		if (gCrew[i].hold[CD2_CREW_SIDE_GUNNER] > 0)
			gCrew[i].hold[CD2_CREW_SIDE_GUNNER]--;

		if (!cd2CrewOwned(cp))
		{
			// not ours: make sure nothing is left hanging out
			if (gCd2Cfg.debugLog && (gCrew[i].hold[0] > 0 || gCrew[i].hold[1] > 0))
				printInfo("[combatd2] crew: car=%d skip: not owned (controlType=%d)\n",
					i, cp->controlType);

			for (side = 0; side < 2; side++)
				if (gCrew[i].ped[side] != NULL)
					cd2CrewDespawnSide(&gCrew[i], side);

			continue;
		}

		// a wrecked car's crew bails out and runs from the fire
		if (cd2CarTotaled(cp))
		{
			if (!gCrew[i].fled)
			{
				gCrew[i].fled = 1;
				cd2CrewFleeSide(&gCrew[i], CD2_CREW_SIDE_DRIVER, cp, cp->hd.where.t[0], cp->hd.where.t[2]);
				cd2CrewFleeSide(&gCrew[i], CD2_CREW_SIDE_GUNNER, cp, cp->hd.where.t[0], cp->hd.where.t[2]);
			}

			for (side = 0; side < 2; side++)
				if (gCrew[i].state[side] == CD2_CREW_FLEE)
					cd2CrewFleeUpdate(&gCrew[i], side, cp);
				else if (gCrew[i].ped[side] != NULL)
					cd2CrewDespawnSide(&gCrew[i], side);

			continue;
		}

		gCrew[i].fled = 0;

		for (side = 0; side < 2; side++)
			cd2CrewUpdateSide(&gCrew[i], side, cp);
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_GAME_START: a fresh level. InitPedestrians has ALREADY reset the
// ped pool by the time this fires, so every stored pointer is dangling - forget
// them without touching the (rebuilt) pool.
// ---------------------------------------------------------------------------
static int cd2CrewOnGameStart(void* ud, void* args)
{
	int i, side;
	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		for (side = 0; side < 2; side++)
		{
			gCrew[i].hold[side] = 0;
			gCrew[i].ped[side] = NULL;
			gCrew[i].state[side] = CD2_CREW_IN;
			gCrew[i].frame[side] = 0;
			gCrew[i].fleeTicks[side] = 0;
			gCrew[i].fleeHead[side] = 0;
		}

		gCrew[i].fled = 0;
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_CAMERA: re-place every crew ped from the cars' CURRENT transforms.
//
// The peds are drawn at the top of the render pass, which runs AFTER StepCars()
// for this frame. The FRAME handler runs BEFORE StepCars(), so a ped placed
// there is a whole step behind the car it rides (very visible at speed). This
// hook fires from InitCamera, just before DrawAllPedestrians, so the placement
// uses the cars' final positions for the frame and the ped tracks exactly.
// ---------------------------------------------------------------------------
static int cd2CrewOnCamera(void* ud, void* args)
{
	int i, side;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MAX_CARS; i++)
	{
		for (side = 0; side < 2; side++)
		{
			LPPEDESTRIAN pPed = (LPPEDESTRIAN)gCrew[i].ped[side];

			// a fleeing ped moved on its own in the FRAME pass; a leaning one
			// is parked and follows the car
			if (pPed != NULL && cd2CrewPedAlive(pPed) && gCrew[i].state[side] != CD2_CREW_FLEE)
				cd2CrewPlace(pPed, &car_data[i], side, gCrew[i].raiseY[side]);
		}
	}

	// Player death: hold the camera still while the local player's car is a
	// wreck, so the death plays without the view sliding around. The held pose
	// is the last live frame's; it resumes when the car respawns.
	{
		CAR_DATA* pc = cd2CrewPlayerCar();
		JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;

		if (pc != NULL && cd2CarTotaled(pc))
		{
			int delay;
			int t;

			if (!sCamHeld)
			{
				sCamHeld = 1;
				sCamTicks = 0;

				// remember where the car died so the pull-back is measured from
				// there, not from the (possibly tumbling) wreck
				sCamFocus.vx = pc->hd.where.t[0];
				sCamFocus.vy = pc->hd.where.t[1];
				sCamFocus.vz = pc->hd.where.t[2];

				if (gCd2Cfg.debugLog)
					printInfo("[combatd2] crew: player wrecked - camera held until respawn\n");
			}

			if (sCamTicks < CD2_DEATHCAM_DELAY)
				sCamTicks++;

			// progress over the respawn window, 0..4096 (fixed point 1.0)
			delay = (gCd2Cfg.respawnDelay > 0) ? gCd2Cfg.respawnDelay : CD2_DEATHCAM_DELAY;
			t = (sCamTicks << 12) / delay;
			if (t > 4096)
				t = 4096;

			if (a != NULL)
			{
				VECTOR pos = sCamPos;
				int bx = sCamPos.vx - sCamFocus.vx;
				int by = sCamPos.vy - sCamFocus.vy;
				int bz = sCamPos.vz - sCamFocus.vz;
				int d = ABS(bx) + ABS(bz);	// horizontal distance (a cheap length)

				if (d < 1)
					d = 1;

				// PULL BACK: advance along (camera - wreck), i.e. away from the
				// car, so the wreck slowly shrinks (a zoom out at fixed FOV).
				pos.vx += (int)(((long long)bx * CD2_DEATHCAM_ZOOM * t) / ((long long)d * 4096));
				pos.vy += (int)(((long long)by * CD2_DEATHCAM_ZOOM * t) / ((long long)d * 4096));
				pos.vz += (int)(((long long)bz * CD2_DEATHCAM_ZOOM * t) / ((long long)d * 4096));

				// RISE: engine camera Y is down, so up means a smaller vy.
				pos.vy -= (CD2_DEATHCAM_RISE * t) >> 12;

				// Observability: the requested pull-back/rise at each step, so a
				// run can show the drift interpolating over the respawn window.
				if (gCd2Cfg.debugLog && (sCamTicks % 60) == 0)
					printInfo("[combatd2] crew: deathcam tick=%d/%d zoom=%d rise=%d\n",
						sCamTicks, delay, (CD2_DEATHCAM_ZOOM * t) >> 12, (CD2_DEATHCAM_RISE * t) >> 12);

				if (a->cameraPosition != NULL)
					*(VECTOR*)a->cameraPosition = pos;

				if (a->cameraAngle != NULL)
					*(SVECTOR*)a->cameraAngle = sCamAng;

				a->override = 1;
			}
		}
		else
		{
			if (sCamHeld && gCd2Cfg.debugLog)
				printInfo("[combatd2] crew: camera released\n");

			sCamHeld = 0;

			if (a != NULL)
			{
				if (a->cameraPosition != NULL)
					sCamPos = *(VECTOR*)a->cameraPosition;

				if (a->cameraAngle != NULL)
					sCamAng = *(SVECTOR*)a->cameraAngle;
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_PED_DRAW: paint a fleeing crew ped flat black (burning). Matched
// back to the crew by the ped pointer; with no match the ped draws stock.
// ---------------------------------------------------------------------------
static int cd2CrewOnPedDraw(void* ud, void* args)
{
	JER_ARGS_PED_DRAW* a = (JER_ARGS_PED_DRAW*)args;
	int i, side;
	(void)ud;

	if (a == NULL || a->ped == NULL)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MAX_CARS; i++)
	{
		for (side = 0; side < 2; side++)
		{
			if ((void*)gCrew[i].ped[side] == a->ped && gCrew[i].state[side] == CD2_CREW_FLEE)
				a->flatBlack = 1;
		}
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_combatd2_entry in combatd2.c)
// ---------------------------------------------------------------------------
void cd2CrewRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2CrewOnFrame, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, cd2CrewOnCamera, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_DRAW, cd2CrewOnPedDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_SKELETON, cd2CrewOnPedSkeleton, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2CrewOnGameStart, NULL, 0);

	ctx->jer_log(ctx, "[combatd2] mounted crew registered (SDK v%d)\n", ctx->sdkVersion);
}
