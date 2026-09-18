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
#include "cainescrossfire.h"
#include "cars.h"
#include "camera.h"
#include "players.h"		/* player[] */
#include "dr2roads.h"		/* MapHeight */
#include "debris.h"		/* Setup_Smoke / SMOKE_FIRE */
#include "pedest.h"		/* LPPEDESTRIAN, pUsedPeds, DestroyPedestrian */
#include "jericho.h"
#include "jer_events.h"
#include "jer_anim.h"		/* the all-bone pose API */
#include "jer_npc.h"		/* jer_npc_* ped scaffolding */
#include "teams/teams.h"	/* the team colours and the per-character suit rule */
#include "jer_ped_palette.h"	/* per-instance ped palettes (team colours) */
#include "factions/factions.h"	/* the five teams: their colours */
#include "ai/ai.h"		/* cd2AiIsOpponent */
#include "crew.h"

// The engine's libgte angle helper (the inverse of RotMatrixYXZ needs it).
extern int ratan2(int y, int x);

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

// Animation frames advanced per game tick. The engine's own get-out/get-in
// step ONE frame per tick (15 ticks ~ 0.5s), which reads as sluggish for a
// crew that pops out the moment a weapon is selected. Playing the same
// animation at this rate keeps the motion legible but much snappier; the
// frames are still visited in order (clamped to the end), so no pose is
// skipped.
#define CD2_CREW_ANIM_STEP	1

// The get-out frame each side settles on. Both sides use the SAME get-out
// motion (the pose the engine itself authors for a ped at a car door) at the
// SAME frame - the gunner differs only by his perch height, so he cannot land
// in a different part of the climb (a late frame stands him up, and raising
// THAT just floats him).
#define CD2_CREW_DRIVER_HOLD	9
#define CD2_CREW_GUNNER_HOLD	CD2_CREW_DRIVER_HOLD

// The gunner is raised this far above the leaning driver, perching him ON the
// windowsill rather than hanging beside it. The render frame is Y-down, so
// raising means subtracting.
#define CD2_CREW_SILL_RAISE	26

// Weapon-arm reach, forced through JER_EVENT_PED_SKELETON phase 0 (the
// position channel). These are PED-LOCAL offsets: +x is out of the ped's own
// right side, -y is up (the render frame is Y-down), +z is the way the ped is
// facing. They are turned by the body rotation before use - see
// cd2CrewLocalToWorld - because the channel they land in is world-oriented.
// Keep them small: they move real bone joints, so the arm mesh follows.
#define CD2_CREW_ARM_ELBOW_OUT	10	// forearm out of the door
#define CD2_CREW_ARM_ELBOW_UP	-2	// and raised
#define CD2_CREW_ARM_ELBOW_FWD	2
#define CD2_CREW_ARM_HAND_OUT	22	// hand out past the elbow (~arm length)
#define CD2_CREW_ARM_HAND_UP	-5
#define CD2_CREW_ARM_HAND_FWD	4

// Heading offset applied to the car's own heading to get the crew's body yaw.
//
// MEASURED, not assumed. Dumping a live crew ped's bones next to its car
// (cd2CrewDumpPose) shows the drawn body's forward sits at world angle
//      3072 - yaw
// while the car's own forward sits at 1024 - direction. The 2048 term is the
// MODEL's own offset: Tanner's mesh front is half a turn from the yaw vector,
// so a ped handed `yaw = direction` is drawn facing BACK down the car. That is
// why both crew read as backwards with the offset at 0.
//      yaw = direction + CD2_CREW_BODY_YAW
//   CD2_CREW_BODY_YAW = 2048 -> both face FORWARD along the car
//                     = 2048 -/+ 1024 -> face out of their own door
//                       (the engine's own get-out pairing is 2048 - 1024 for
//                        the left door and 2048 + 1024 for the right)
#define CD2_CREW_BODY_YAW	2048

// The crew ped's origin rides this far ABOVE the car's body centre. The old
// placement used the MAP ground (-MapHeight - 130), which is not tied to the
// car at all: a car in the air left its crew standing on the road below. This
// reproduces that on-ground offset (measured with the car level: car y +31 ->
// ped y -130, i.e. 99 above the body centre) while now following the car.
#define CD2_CREW_BODY_LIFT	49

// The car matrix is Y-UP while the ped's body is rendered Y-DOWN, so a physical
// pitch/roll maps to the NEGATED angles here. If a tilted car's crew leans the
// wrong way, this is the single knob to flip.
#define CD2_CREW_TILT_SIGN	(-1)

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
	int sitLogged[2];	// one-shot: the "perched on the sill" note has been logged
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
static int cd2CrewClimbFrame(int i);

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
		printInfo("[cainescrossfire] crew: car=%d fired lean=0x%X -> driver=%d gunner=%d\n",
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
		printInfo("[cainescrossfire] crew: car=%d side=%d bails out (wreck)\n", cp->id, i);
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
// POSITION channel). All-bone access comes from jer_anim - no private mirror of
// the engine's bone layout, and jer_anim_rotate_offset() handles the
// world-oriented frame the channel actually works in (see
// JERICHO/docs/ped-animation.md).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Ped-local offset -> the frame the POSITION channel actually works in.
//
// By the time PED_SKELETON phase 0 runs, motion_c.c has already rotated the
// bone chain by the body matrix, so vCurrPos is WORLD-oriented: it is not a
// local offset any more. Writing a local offset straight into it (or rotating
// it by the yaw alone) pins the pose to one world direction and it stops
// following the car - which is exactly how the weapon arm ended up "locked".
// Turn it with the body's own rotation instead; for a crew ped that rotation is
// the car's, which is also what makes the pose follow the car exactly.
//   local +x = out of the ped's right, -y = up, +z = the ped's facing
// ---------------------------------------------------------------------------
static void cd2CrewLocalToWorld(const CAR_DATA* cp, int lx, int ly, int lz, JER_BONE_POS* out)
{
	const MATRIX* w = &cp->hd.where;

	out->vx = (int)((((long long)w->m[0][0] * lx) + ((long long)w->m[0][1] * ly) + ((long long)w->m[0][2] * lz)) >> 12);
	out->vy = -(int)((((long long)w->m[1][0] * lx) + ((long long)w->m[1][1] * ly) + ((long long)w->m[1][2] * lz)) >> 12);
	out->vz = (int)((((long long)w->m[2][0] * lx) + ((long long)w->m[2][1] * ly) + ((long long)w->m[2][2] * lz)) >> 12);
}

// Force the right arm into a weapon-holding reach: the shoulder stays at its
// rest offset, the forearm is raised and pushed forward, and the hand extends
// beyond it. `facing` is the heading the pose's local +Z points along.
static void cd2CrewPoseArm(void* skel, const CAR_DATA* cp, int side)
{
	// the gunner is the driver mirrored, so his reach goes out of the OTHER
	// side of his body and uses the mirrored limbs. The car's local +x runs
	// toward the gunner's door (the crew are placed at -/+ lateral), so the
	// driver's "out" is local -x and the gunner's is +x.
	const int mirrored = (side == CD2_CREW_SIDE_GUNNER);
	const int outSign = mirrored ? 1 : -1;
	JER_BONE_POS* elbow = jer_anim_bone_pos(skel, mirrored ? JER_LIMB_LELBOW : JER_LIMB_RELBOW);
	JER_BONE_POS* hand = jer_anim_bone_pos(skel, mirrored ? JER_LIMB_LHAND : JER_LIMB_RHAND);
	const JER_BONE_POS* shoulder = jer_anim_bone_pos(skel, mirrored ? JER_LIMB_LSHOULDER : JER_LIMB_RSHOULDER);
	JER_BONE_POS off;

	// the shoulder is left exactly where the motion put it: it is already in
	// the world frame, and writing a local rest value into it was what yanked
	// the whole arm to a fixed spot
	if (elbow == NULL || hand == NULL || shoulder == NULL)
		return;

	cd2CrewLocalToWorld(cp, outSign * CD2_CREW_ARM_ELBOW_OUT, CD2_CREW_ARM_ELBOW_UP, CD2_CREW_ARM_ELBOW_FWD, &off);
	elbow->vx = shoulder->vx + off.vx;
	elbow->vy = shoulder->vy + off.vy;
	elbow->vz = shoulder->vz + off.vz;

	cd2CrewLocalToWorld(cp, outSign * CD2_CREW_ARM_HAND_OUT, CD2_CREW_ARM_HAND_UP, CD2_CREW_ARM_HAND_FWD, &off);
	hand->vx = elbow->vx + off.vx;
	hand->vy = elbow->vy + off.vy;
	hand->vz = elbow->vz + off.vz;
}

// ---------------------------------------------------------------------------
// Debug: dump one crew ped's drawn pose next to its parent car's frame.
//
// The crew's orientation has to satisfy two things at once - the CAR's heading
// (so the body turns with it) and the get-out MOTION's own twist (which is
// authored per door) - and reasoning about which term does what from the source
// alone has been error-prone. So print the actual numbers: the car's frame, the
// `dir` the engine will use, and the bone positions, plus the axes derived from
// them (right = R shoulder - L shoulder, up = hips - upper body, forward =
// right x up). The angle deltas at the end say exactly how far the drawn body
// is from the car's own forward.
// ---------------------------------------------------------------------------
static void cd2CrewDumpPose(const CAR_DATA* cp, int side, LPPEDESTRIAN pPed, void* skel)
{
	const MATRIX* w = &cp->hd.where;
	const int carFwdAng = ratan2(w->m[2][2], w->m[0][2]) & 0xfff;
	const JER_BONE_POS* hips = jer_anim_bone_pos(skel, JER_LIMB_HIPS);
	const JER_BONE_POS* j1 = jer_anim_bone_pos(skel, JER_LIMB_JOINT_1);
	const JER_BONE_POS* ls = jer_anim_bone_pos(skel, JER_LIMB_LSHOULDER);
	const JER_BONE_POS* rs = jer_anim_bone_pos(skel, JER_LIMB_RSHOULDER);
	const JER_BONE_POS* lf = jer_anim_bone_pos(skel, JER_LIMB_LFOOT);
	const JER_BONE_POS* lt = jer_anim_bone_pos(skel, JER_LIMB_LTOE);
	int rx = 0, ry = 0, rz = 0, ux = 0, uy = 0, uz = 0;
	int fx, fy, fz, fAng, footAng = -1;

	if (ls && rs) { rx = rs->vx - ls->vx; ry = rs->vy - ls->vy; rz = rs->vz - ls->vz; }
	if (hips && j1) { ux = hips->vx - j1->vx; uy = hips->vy - j1->vy; uz = hips->vz - j1->vz; }

	// forward = right x up (Y-down frame)
	fx = ry * uz - rz * uy;
	fy = rz * ux - rx * uz;
	fz = rx * uy - ry * ux;
	fAng = ratan2(fz, fx) & 0xfff;

	if (lf && lt)
		footAng = ratan2(lt->vz - lf->vz, lt->vx - lf->vx) & 0xfff;

	printInfo("[crew] ---- car=%d side=%d ----\n", cp->id, side);
	printInfo("[crew] car  dir=%d  fwdAng=%d  m00=%d m02=%d m20=%d m22=%d m11=%d\n",
	          cp->hd.direction, carFwdAng, w->m[0][0], w->m[0][2], w->m[2][0], w->m[2][2], w->m[1][1]);
	printInfo("[crew] ped  dir=(%d,%d,%d)  pos=(%d,%d,%d)\n",
	          pPed->dir.vx, pPed->dir.vy, pPed->dir.vz,
	          pPed->position.vx, pPed->position.vy, pPed->position.vz);
	printInfo("[crew] bone hips=(%d,%d,%d) j1=(%d,%d,%d) Lsh=(%d,%d,%d) Rsh=(%d,%d,%d)\n",
	          hips?hips->vx:0, hips?hips->vy:0, hips?hips->vz:0,
	          j1?j1->vx:0, j1?j1->vy:0, j1?j1->vz:0,
	          ls?ls->vx:0, ls?ls->vy:0, ls?ls->vz:0,
	          rs?rs->vx:0, rs?rs->vy:0, rs?rs->vz:0);
	printInfo("[crew] axis right=(%d,%d,%d) up=(%d,%d,%d) fwd=(%d,%d,%d)\n", rx, ry, rz, ux, uy, uz, fx, fy, fz);
	printInfo("[crew] ang  carFwd=%d  pedFwd=%d  dPed=%d  lToe=%d  dToe=%d\n",
	          carFwdAng, fAng, (fAng - carFwdAng) & 0xfff, footAng,
	          footAng < 0 ? -1 : ((footAng - carFwdAng) & 0xfff));

	{
		const JER_BONE_POS* sh = jer_anim_bone_pos(skel, side ? JER_LIMB_LSHOULDER : JER_LIMB_RSHOULDER);
		const JER_BONE_POS* hd = jer_anim_bone_pos(skel, side ? JER_LIMB_LHAND : JER_LIMB_RHAND);
		int ax = 0, az = 0;

		if (sh && hd) { ax = hd->vx - sh->vx; az = hd->vz - sh->vz; }

		printInfo("[crew] arm  reach=(%d,%d,%d) ang=%d  dCar=%d\n",
		          ax, (sh && hd) ? (hd->vy - sh->vy) : 0, az,
		          (ratan2(az, ax) & 0xfff), ((ratan2(az, ax) & 0xfff) - carFwdAng) & 0xfff);
	}
}

// JER_EVENT_PED_POSE — the ONLY effective rotation channel (it fires between
// SetupTannerSkeleton and newRotateBones). The engine MIRROR-FLIPS a ped's
// root rotation for a get-out on one side of the car, via the shared
// bReverseYRotation global (SetupGetOutCar: bReverseYRotation = !entrySide;
// consumed in newRotateBones). Our crew plays that same get-out motion at both
// doors, so without it the second door reads as a stranger pose - visibly "a
// bit off". Set it per side here (the knob sits immediately before our ped's
// bones rotate), and put it back afterwards for the rest of the game in the
// skeleton phase-1 pass.
// ---------------------------------------------------------------------------
static int sCrewRevSaved;
static int sCrewRevValid;

static int cd2CrewOnPedPose(void* ud, void* args)
{
	JER_ARGS_PED_POSE* a = (JER_ARGS_PED_POSE*)args;
	int i, side;

	(void)ud;

	if (a == NULL || a->ped == NULL)
		return JER_RESULT_CONTINUE;

	// belt and braces: if a previous frame's hand-back was somehow missed, put
	// the game's value back before touching it again
	if (sCrewRevValid)
	{
		bReverseYRotation = sCrewRevSaved;
		sCrewRevValid = 0;
	}

	for (i = 0; i < MAX_CARS; i++)
	{
		for (side = 0; side < 2; side++)
		{
			if ((void*)gCrew[i].ped[side] == a->ped)
			{
				sCrewRevSaved = bReverseYRotation;
				sCrewRevValid = 1;

				// the driver's door is the unmirrored case (the engine's own
				// left-side pairing), the gunner's the mirrored one
				bReverseYRotation = (side == CD2_CREW_SIDE_GUNNER) ? 1 : 0;

				return JER_RESULT_CONTINUE;
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_PED_SKELETON: fires for the player ped and for every module-owned
// ped (jer_npc_owned), so filter to OURS. Both crew get the weapon arm, each on
// his own side (the gunner mirrored).
static int cd2CrewOnPedSkeleton(void* ud, void* args)
{
	JER_ARGS_PED_SKELETON* a = (JER_ARGS_PED_SKELETON*)args;
	int i, side;

	(void)ud;

	if (a == NULL || a->ped == NULL || a->skel == NULL)
		return JER_RESULT_CONTINUE;

	// phase 1 runs after this ped's bones are built and drawn, so this is where
	// the get-out mirror flag cd2CrewOnPedPose set gets handed back to the game.
	if (a->phase == 1 && sCrewRevValid)
	{
		bReverseYRotation = sCrewRevSaved;
		sCrewRevValid = 0;
	}

	if (a->phase == 1 && gCd2Cfg.debugLog && !a->shadow)
	{
		static int sDumps;
		int ci, si;

		for (ci = 0; ci < MAX_CARS && sDumps < 24; ci++)
			for (si = 0; si < 2 && sDumps < 24; si++)
				if ((void*)gCrew[ci].ped[si] == a->ped && gCrew[ci].state[si] == CD2_CREW_OUT)
				{
					sDumps++;
					cd2CrewDumpPose(&car_data[ci], si, (LPPEDESTRIAN)a->ped, a->skel);
				}
	}

	// the position channel only, and never during the shadow pass
	if (a->phase != 0 || a->shadow)
		return JER_RESULT_CONTINUE;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MAX_CARS; i++)
	{
		for (side = 0; side < 2; side++)
		{
			if ((void*)gCrew[i].ped[side] == a->ped &&
			    gCrew[i].state[side] != CD2_CREW_FLEE)
			{
				LPPEDESTRIAN pPed = (LPPEDESTRIAN)a->ped;

				cd2CrewPoseArm(a->skel, &car_data[i], side);
				break;
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

// Inverse of RotMatrixYXZ (PsyCross LIBGTE.C): pull the YXZ Euler triple back
// out of a rotation matrix. The engine builds a ped's whole body with
// RotMatrixYXZ(pPed->dir) (newRotateBones), so feeding it the CAR's tilt is
// what makes a mounted crew bank and pitch with the car rather than staying
// upright while the car is thrown around.
//
//   m[1][2] = -s0            m[1][0] = s2*c0     m[1][1] = c2*c0
//   m[0][2] = s1*c0          m[2][2] = c1*c0
static void cd2CrewMatrixEuler(const MATRIX* m, SVECTOR* out)
{
	int c2, s2, c0;

	out->vz = ratan2(m->m[1][0], m->m[1][1]);	// roll
	c2 = RCOS(out->vz);
	s2 = RSIN(out->vz);

	// c0 recovered without a square root: project the (m10, m11) pair back
	// through the roll just found - c2*(c2*c0) + s2*(s2*c0) == c0.
	c0 = FIXEDH(m->m[1][1] * c2) + FIXEDH(m->m[1][0] * s2);

	out->vy = ratan2(m->m[0][2], m->m[2][2]);	// yaw
	out->vx = ratan2(-m->m[1][2], c0);		// pitch
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
	int x, z, y, yaw;
	SVECTOR rot;

	x = w->t[0] + (int)(((long long)w->m[0][0] * lat * s) >> 12)
	           + (int)(((long long)w->m[0][2] * fwd) >> 12);
	z = w->t[2] + (int)(((long long)w->m[2][0] * lat * s) >> 12)
	           + (int)(((long long)w->m[2][2] * fwd) >> 12);

	// Ride the CAR, not the map: the old -MapHeight placement pinned the crew
	// to the ground, so a car in the air (or being tossed by a wreck) left them
	// standing on the road below. The car matrix is Y-UP while the ped's
	// position is Y-DOWN, hence the negation.
	y = -(cp->hd.where.t[1]) - CD2_CREW_BODY_LIFT - raiseY;

	// Body rotation straight off the car's matrix, so the crew banks and
	// pitches with the car (and the weapon arm follows, being posed in the
	// ped's own local frame). The extracted yaw is identical to
	// hd.direction - verified - so the facing the crew was tuned with is
	// unchanged; only the tilt is new.
	cd2CrewMatrixEuler(w, &rot);
	yaw = (rot.vy + CD2_CREW_BODY_YAW) & 0xfff;

	jer_npc_set_world((JerNpc*)pPed, x, y, z, yaw);
	jer_npc_set_orient((JerNpc*)pPed, (CD2_CREW_TILT_SIGN * rot.vx) & 0xfff, yaw, (CD2_CREW_TILT_SIGN * rot.vz) & 0xfff);
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
			printInfo("[cainescrossfire] crew: car=%d side=%d skip: ped pool full\n", cp->id, i);
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
	c->sitLogged[i] = 0;

	jer_npc_set_action(c->ped[i], PED_ACTION_GETOUTCAR, 0);
	cd2CrewPlace(pPed, cp, i, c->raiseY[i]);

	if (gCd2Cfg.debugLog)
		printInfo("[cainescrossfire] crew: car=%d side=%d out (get out)\n", cp->id, i);
}

static void cd2CrewDespawnSide(CD2_CREW_CAR* c, int i)
{
	if (c->ped[i] != NULL)
		jer_npc_despawn(c->ped[i]);

	c->ped[i] = NULL;
	c->state[i] = CD2_CREW_IN;
	c->frame[i] = 0;
	c->raiseY[i] = 0;
	c->sitLogged[i] = 0;
}

// The frame the side's CLIMB stops at: the driver leans out there, the gunner
// climbs further so he sits up on the sill.
static int cd2CrewClimbFrame(int i)
{
	return (i == CD2_CREW_SIDE_DRIVER) ? CD2_CREW_DRIVER_HOLD : CD2_CREW_GUNNER_HOLD;
}

// Drive the side's "out" pose: climb out of the car and hold at the side's
// frame. BOTH sides use the same GETOUTCAR motion - the one the engine itself
// uses for a ped at a car door - so there is no snap and nothing to clip; only
// the held frame and the gunner's perch height differ. (The gunner's SIT pose
// was tried and reverted: its dangling legs hung through the door panel.)
static void cd2CrewApplyOutPose(CD2_CREW_CAR* c, int i)
{
	int hold = cd2CrewClimbFrame(i);

	if (c->frame[i] < hold)
	{
		c->frame[i] += CD2_CREW_ANIM_STEP;

		if (c->frame[i] > hold)
			c->frame[i] = hold;
	}

	jer_npc_set_action(c->ped[i], PED_ACTION_GETOUTCAR, c->frame[i]);

	// the gunner rises onto the sill as he climbs; the driver just leans
	c->raiseY[i] = (i == CD2_CREW_SIDE_GUNNER)
		? (CD2_CREW_SILL_RAISE * c->frame[i]) / CD2_CREW_GUNNER_HOLD
		: 0;

	if (c->frame[i] >= hold && c->sitLogged[i] == 0)
	{
		c->sitLogged[i] = 1;

		if (gCd2Cfg.debugLog)
			printInfo("[cainescrossfire] crew: side=%d settled out (frame=%d raise=%d)\n", i, c->frame[i], c->raiseY[i]);
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
				printInfo("[cainescrossfire] crew: car=%d side=%d in (get in)\n", cp->id, i);
		}

		c->frame[i] += CD2_CREW_ANIM_STEP;

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
				printInfo("[cainescrossfire] crew: car=%d skip: not owned (controlType=%d)\n",
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
					printInfo("[cainescrossfire] crew: player wrecked - camera held until respawn\n");
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
					printInfo("[cainescrossfire] crew: deathcam tick=%d/%d zoom=%d rise=%d\n",
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
				printInfo("[cainescrossfire] crew: camera released\n");

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
// JER_EVENT_PED_DRAW: give a Tanner its faction's outfit colour.
//
// The engine recolours the CLUT rows for THIS ped only (see the SDK's
// ped-palette.md), so a team reads at a glance without touching anyone else. The
// player takes the configured player faction; a crew ped is matched back to its
// car and takes that car's faction. A ped that is neither - a mission Tanner, a
// plain civilian - is explicitly cleared back to stock colours, because the
// engine's selection persists until it is changed.
// ---------------------------------------------------------------------------
static int cd2CrewOnPedPalette(void* ud, void* args)
{
	JER_ARGS_PED_DRAW* a = (JER_ARGS_PED_DRAW*)args;
	LPPEDESTRIAN pPed;
	unsigned char r, g, b;
	int faction = CD2_FAC_NONE;
	int suitTint;
	int i, side;

	(void)ud;

	if (a == NULL)
		return JER_RESULT_CONTINUE;

	if (a->ped == NULL)
	{
		jer_ped_palette_select(-1);
		return JER_RESULT_CONTINUE;
	}

	if (!gCd2Cfg.teamPalette || !gCd2Cfg.factions)
	{
		jer_ped_palette_select(-1);
		return JER_RESULT_CONTINUE;
	}

	pPed = (LPPEDESTRIAN)a->ped;

	if (pPed->padId >= 0)
	{
		faction = cd2FacPlayerFaction();
	}
	else
	{
		for (i = 0; i < MAX_CARS; i++)
		{
			for (side = 0; side < 2; side++)
			{
				if ((void*)gCrew[i].ped[side] == a->ped)
					faction = cd2FacOfCarId(i);
			}
		}
	}

	/* cd2FacColourOf refuses an unknown faction (and CD2_FAC_NONE), which is how a
	 * ped with no team ends up on stock colours */
	if (!cd2FacColourOf(faction, &r, &g, &b))
	{
		jer_ped_palette_select(-1);
		return JER_RESULT_CONTINUE;
	}

	/* the per-character rule: Tanner and McKenzie keep a lightly washed version of
	 * their own look, Jericho and Vasquez ARE their colour (teams/teams.h). A
	 * faction with no team row falls back to the config's global strength. */
	suitTint = cd2TeamSuitTint(faction);

	if (suitTint <= 0)
		suitTint = gCd2Cfg.teamPaletteStrength;

	/* the engine caches a row set per colour and rebuilds it after a level reload,
	 * so asking on every draw is cheap, stays correct across levels, and picks up a
	 * colour changed at runtime (cd2TeamSet) on the next draw */
	jer_ped_palette_set_floor(gCd2Cfg.teamPaletteFloor);
	jer_ped_palette_select(jer_ped_palette_team(r, g, b, suitTint));

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_cainescrossfire_entry in cainescrossfire.c)
// ---------------------------------------------------------------------------
void cd2CrewRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2CrewOnFrame, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, cd2CrewOnCamera, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_DRAW, cd2CrewOnPedDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_DRAW, cd2CrewOnPedPalette, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_POSE, cd2CrewOnPedPose, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_SKELETON, cd2CrewOnPedSkeleton, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2CrewOnGameStart, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] mounted crew registered (SDK v%d)\n", ctx->sdkVersion);
}
