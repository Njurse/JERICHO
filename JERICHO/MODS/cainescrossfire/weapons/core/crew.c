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
#include "ai/ai.h"		/* cd2AiIsOpponent, cd2AiTargetPos */
#include "hud/lockon.h"		/* cd2LockOnTarget — the player's crew aim target */
#include "profiles/profile.h"	/* per-vehicle crew offsets */
#include "crew.h"

// The engine's libgte angle helper (the inverse of RotMatrixYXZ needs it).
extern int ratan2(int y, int x);

// How long a side stays out after its last leaning shot, in frames (~0.8s at
// 30fps). The hold is RE-ARMED on every shot, so it only needs to bridge the
// gap between shots: shorter than any leaning weapon's refire (the shotgun is
// ~2.2s) yet long enough that a held trigger keeps the ped out continuously.
#define CD2_CREW_HOLD_FRAMES	10

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
#define CD2_CREW_DRIVER_HOLD	10
#define CD2_CREW_GUNNER_HOLD	CD2_CREW_DRIVER_HOLD

// The gunner is raised this far above the leaning driver, perching him ON the
// windowsill rather than hanging beside it. The render frame is Y-down, so
// raising means subtracting.
#define CD2_CREW_SILL_RAISE	6

// Weapon-arm reach, forced through JER_EVENT_PED_SKELETON phase 0 (the
// position channel). These are PED-LOCAL offsets: +x is out of the ped's own
// right side, -y is up (the render frame is Y-down), +z is the way the ped is
// facing. They are turned by the body rotation before use - see
// cd2CrewLocalToWorld - because the channel they land in is world-oriented.
// Keep them small: they move real bone joints, so the arm mesh follows.
//
// The arm AIMS: its horizontal direction is the car's current target, in the
// car's frame (see cd2CrewAim), turned into the ped's frame by the same body
// rotation. OUT_BIAS keeps it leaving the window rather than crossing the
// ped's chest when the target is dead ahead; a side with no target falls back
// to CD2_CREW_ARM_IDLE_YAW (the old fixed "out the window" reach).
#define CD2_CREW_ARM_OUT_BIAS	6	// outward lean on the window side (ped-local x)
#define CD2_CREW_ARM_ELBOW_LEN	9	// forearm extension along the aim
#define CD2_CREW_ARM_HAND_LEN	18	// hand extension past the elbow
#define CD2_CREW_ARM_UP		(-4)	// forearm raised
#define CD2_CREW_ARM_IDLE_YAW	900	// no target: reach out the window (~79 deg)
#define CD2_CREW_ARM_CONE	1000	// aim clamped to this far off the nose

// Aim mode also turns the upper body through the ROTATION channel
// (JER_EVENT_PED_POSE). The lean rolls the torso toward the crew member's own
// window; HEAD_AIM is the most the head turns toward the target (itself
// clamped to the arm's cone). Single knobs - if a lean goes the wrong way,
// flip the sign here.
#define CD2_CREW_HEAD_AIM	300	// head yaw toward the target (clamped)
#define CD2_CREW_DRIVER_LEAN	220	// driver: upper-body roll out of his window
#define CD2_CREW_PASS_LEAN	140	// passenger: leans out less (he is seated)

// The passenger sits ON the windowsill with his legs in the cabin: his body is
// turned 180 from the crew's forward facing (legs inward, back to the window)
// and his torso twists back so the head still looks where the car goes. These
// two are the pose's whole shape - turn them, not the code.
#define CD2_CREW_SILL_YAW	2048	// body yaw offset, 180 = legs face inward
#define CD2_CREW_SILL_TORSO	2048	// JOINT_1 twist back toward forward

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
#define CD2_CREW_BODY_LIFT	32

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

// Death cam: while the player's car is a wreck the view is HELD where the camera happened
// to be at the moment of the explosion, and rises slowly from there, so the death reads
// rather than just freezing.
//
// It used to pull back as well, and it measured that pull from the WRECK - so the camera
// slid away from the car it was watching. Jaret: "lets do a slower pan up and focus on the
// point the camera was at when it blew up". The anchor is now the camera's own position at
// the blast (sCamFocus) and there is no lateral drift: the wreckage the camera was looking
// at stays where it is in frame and only the height changes. To bring the pull-back back,
// anchor the focus on the car again and re-add the term.
#define CD2_DEATHCAM_RISE	260	// rise this far over the whole window (engine Y is
					// down). Was 420, which read as a lurch rather than a pan;
					// the eased curve below also starts it gently.
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
static VECTOR sCamFocus;	// where the CAMERA was when the car blew up - its anchor for
				// the whole death cam (see CD2_DEATHCAM_RISE)

// ped lifecycle (defined below; the wreck bail-out uses them)
static void cd2CrewSpawnSide(CD2_CREW_CAR* c, int i, const CAR_DATA* cp);
static void cd2CrewDespawnSide(CD2_CREW_CAR* c, int i);
static int cd2CrewClimbFrame(int i);

// The per-vehicle crew offsets for a car (its profile's CD2_VEH_CREW block), or
// an all-inherit block for a car with no profile. Every field is a DELTA on the
// module default, so a body with unusual doors can move the mount point, turn
// the body, raise the seated passenger and scale the arm.
static const CD2_VEH_CREW sCrewNoOffsets;

static const CD2_VEH_CREW* cd2CrewOffsets(const CAR_DATA* cp)
{
	const CD2_VEH_PROFILE* p = cd2VehDef(cd2VehOfCar((void*)cp));

	return (p != NULL) ? &p->crew : &sCrewNoOffsets;
}

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

// The car's current target, as a world position. AI opponents report whatever
// their own brain is chasing (cd2AiTargetPos); the local player reports the
// radar lock-on (cd2LockOnTarget). Returns 0 when there is nothing to aim at.
static int cd2CrewFindTarget(const CAR_DATA* cp, VECTOR* out)
{
	if (cd2AiTargetPos(cp, out))
		return 1;

	if (cp->id == MainPlayer.playerCarId)
	{
		int t = cd2LockOnTarget();

		if (t >= 0 && t < MAX_CARS && t != cp->id && car_data[t].ap.carCos != NULL)
		{
			out->vx = car_data[t].hd.where.t[0];
			out->vy = car_data[t].hd.where.t[1];
			out->vz = car_data[t].hd.where.t[2];
			return 1;
		}
	}

	return 0;
}

// The crew's aim as a horizontal direction in the CAR's frame: (axr, azr) is a
// 4096-fixed-point unit vector (RSIN/RCOS of the aim yaw), so +azr is straight
// ahead and +axr is toward the gunner's door. With no target it falls back to
// the side's "out the window" reach. The yaw is clamped to CD2_CREW_ARM_CONE so
// the arm never swings round to point behind the car.
static void cd2CrewAim(const CAR_DATA* cp, int side, int* axr, int* azr)
{
	const int outSign = (side == CD2_CREW_SIDE_GUNNER) ? 1 : -1;
	VECTOR tp;
	int aimYaw;

	if (!cd2CrewFindTarget(cp, &tp))
	{
		aimYaw = outSign * CD2_CREW_ARM_IDLE_YAW;
	}
	else
	{
		const MATRIX* w = &cp->hd.where;
		int tx = tp.vx - cp->hd.where.t[0];
		int tz = tp.vz - cp->hd.where.t[2];

		// world -> car-local is the transpose of the car's matrix (which maps
		// local -> world): x,z only, the arm aims horizontally.
		int lx = (int)(((long long)w->m[0][0] * tx + (long long)w->m[2][0] * tz) >> 12);
		int lz = (int)(((long long)w->m[0][2] * tx + (long long)w->m[2][2] * tz) >> 12);

		aimYaw = ratan2(lx, lz) & 0xfff;
	}

	// wrap to -2048..2048 and clamp to the forward cone, so a target behind
	// does not fold the arm back through the ped
	if (aimYaw > 2048)
		aimYaw -= 4096;

	if (aimYaw > CD2_CREW_ARM_CONE)
		aimYaw = CD2_CREW_ARM_CONE;
	else if (aimYaw < -CD2_CREW_ARM_CONE)
		aimYaw = -CD2_CREW_ARM_CONE;

	*axr = RSIN(aimYaw);
	*azr = RCOS(aimYaw);
}

// Force the weapon arm into an aiming reach: the shoulder stays where the
// motion (and any torso aim) put it, the forearm extends along the aim and is
// raised, and the hand extends beyond it. `(axr, azr)` is the aim direction in
// the car's frame (see cd2CrewAim); `outSign` keeps the arm on its own side.
static void cd2CrewPoseArm(void* skel, const CAR_DATA* cp, int side, int axr, int azr)
{
	// the gunner is the driver mirrored, so his reach goes out of the OTHER
	// side of his body and uses the mirrored limbs. The car's local +x runs
	// toward the gunner's door (the crew are placed at -/+ lateral), so the
	// driver's "out" is local -x and the gunner's is +x.
	const int mirrored = (side == CD2_CREW_SIDE_GUNNER);
	const int outSign = mirrored ? 1 : -1;
	const CD2_VEH_CREW* cr = cd2CrewOffsets(cp);
	int elbowLen = CD2_CREW_ARM_ELBOW_LEN;
	int handLen = CD2_CREW_ARM_HAND_LEN;
	JER_BONE_POS* elbow = jer_anim_bone_pos(skel, mirrored ? JER_LIMB_LELBOW : JER_LIMB_RELBOW);
	JER_BONE_POS* hand = jer_anim_bone_pos(skel, mirrored ? JER_LIMB_LHAND : JER_LIMB_RHAND);
	const JER_BONE_POS* shoulder = jer_anim_bone_pos(skel, mirrored ? JER_LIMB_LSHOULDER : JER_LIMB_RSHOULDER);
	JER_BONE_POS off;

	// the shoulder is left exactly where the motion put it: it is already in
	// the world frame, and writing a local rest value into it was what yanked
	// the whole arm to a fixed spot
	if (elbow == NULL || hand == NULL || shoulder == NULL)
		return;

	// a profile may scale the reach for a body whose doors sit far from the
	// ped (a wide car, a truck); 0 = the default 100%
	if (cr->armScale != 0)
	{
		elbowLen = (elbowLen * cr->armScale) / 100;
		handLen = (handLen * cr->armScale) / 100;
	}

	cd2CrewLocalToWorld(cp,
		outSign * CD2_CREW_ARM_OUT_BIAS + (int)(((long long)axr * elbowLen) >> 12),
		CD2_CREW_ARM_UP,
		(int)(((long long)azr * elbowLen) >> 12), &off);
	elbow->vx = shoulder->vx + off.vx;
	elbow->vy = shoulder->vy + off.vy;
	elbow->vz = shoulder->vz + off.vz;

	cd2CrewLocalToWorld(cp,
		outSign * CD2_CREW_ARM_OUT_BIAS + (int)(((long long)axr * handLen) >> 12),
		CD2_CREW_ARM_UP,
		(int)(((long long)azr * handLen) >> 12), &off);
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

// The bone names, in JER_LIMB_* order - purely so a dumped table reads as the
// limb it belongs to (the author's rig must use this order).
static const char* const sCrewLimbNames[JER_LIMB_COUNT] =
{
	"ROOT", "LOWERBACK", "JOINT_1", "NECK", "HEAD",
	"LSHOULDER", "LELBOW", "LHAND", "LFINGERS",
	"RSHOULDER", "RELBOW", "RHAND", "RFINGERS",
	"HIPS", "LHIP", "LKNEE", "LFOOT", "LTOE",
	"RHIP", "RKNEE", "RFOOT", "RTOE", "JOINT"
};

// Dump the whole skeleton's ROTATIONS as a paste-ready C table, so a pose can
// be authored from a real, on-model baseline (see CREW_POSES.md). This is the
// rotation channel, so it must run from JER_EVENT_PED_POSE - that is the only
// point where the values are BOTH readable and our own aim applied (the
// skeleton pass has already restored the shared buffer by then).
static void cd2CrewDumpBones(void* skel, int car, int side)
{
	int limb;

	printInfo("[crew] ---- bones car=%d side=%d (JER_BONE_ROT, parent-relative ZYX, 4096/turn) ----\n", car, side);
	printInfo("[crew] static const JER_BONE_ROT side%d[JER_LIMB_COUNT] = {\n", side);

	for (limb = 0; limb < JER_LIMB_COUNT; limb++)
	{
		JER_BONE_ROT* r = jer_anim_bone_rotation(skel, limb);

		if (r != NULL)
			printInfo("[crew] \t{ %5d, %5d, %5d },\t/* %2d %s */\n",
				r->vx, r->vy, r->vz, limb, sCrewLimbNames[limb]);
		else
			printInfo("[crew] \t{     0,     0,     0 },\t/* %2d %s (no data) */\n",
				limb, sCrewLimbNames[limb]);
	}

	printInfo("[crew] };\n");
}

// How many bone tables to dump per side (a run only needs one good baseline).
#define CD2_CREW_BONE_DUMPS	3
static int sCrewBoneDumps[2];

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

// The rotation-channel snapshot. A bone's rotation points into the SHARED
// per-type motion buffer, so a write leaks into every ped drawing that motion
// frame; snapshot before writing and put it back once this ped's bones are
// built (the skeleton phase-1 pass), exactly as the mirror flag is handled.
static JER_BONE_ROT sCrewRotSave[JER_LIMB_COUNT];
static int sCrewRotValid;

// The torso/head aim, on the ROTATION channel: the driver rolls his upper body
// out of his own window and turns his head toward the target; the passenger's
// torso twists back to forward (he is turned away from it) and his head follows
// the target too. Called only for our own ped, in JER_EVENT_PED_POSE.
static void cd2CrewPoseTorso(void* skel, int side, int axr, int azr)
{
	JER_BONE_ROT* j1 = jer_anim_bone_rotation(skel, JER_LIMB_JOINT_1);
	JER_BONE_ROT* head = jer_anim_bone_rotation(skel, JER_LIMB_HEAD);
	int aimYaw = ratan2(axr, azr) & 0xfff;

	if (aimYaw > 2048)
		aimYaw -= 4096;

	if (aimYaw > CD2_CREW_HEAD_AIM)
		aimYaw = CD2_CREW_HEAD_AIM;
	else if (aimYaw < -CD2_CREW_HEAD_AIM)
		aimYaw = -CD2_CREW_HEAD_AIM;

	if (j1 != NULL)
	{
		// the passenger sits facing away from the car's forward, so twist the
		// torso back toward it; the lean then rolls the upper body toward the
		// crew member's own window (the driver's is local -x).
		if (side == CD2_CREW_SIDE_GUNNER)
			j1->vy = (short)((j1->vy + CD2_CREW_SILL_TORSO) & 0xfff);

		j1->vz = (short)((j1->vz + ((side == CD2_CREW_SIDE_GUNNER)
			? CD2_CREW_PASS_LEAN : -CD2_CREW_DRIVER_LEAN)) & 0xfff);
	}

	if (head != NULL)
		head->vy = (short)((head->vy + aimYaw) & 0xfff);
}

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

	if (sCrewRotValid)
	{
		jer_anim_restore_rotations(a->skel, sCrewRotSave);
		sCrewRotValid = 0;
	}

	for (i = 0; i < MAX_CARS; i++)
	{
		for (side = 0; side < 2; side++)
		{
			if ((void*)gCrew[i].ped[side] == a->ped)
			{
				int axr, azr;

				sCrewRevSaved = bReverseYRotation;
				sCrewRevValid = 1;

				// the driver's door is the unmirrored case (the engine's own
				// left-side pairing), the gunner's the mirrored one
				// to do: fix the driver's awkward posing, the gunner is fine
				bReverseYRotation = (side == CD2_CREW_SIDE_GUNNER) ? 1 : 1;

				// the torso/head aim goes through the ROTATION channel, so
				// snapshot the shared buffer first (restored in phase 1)
				jer_anim_save_rotations(a->skel, sCrewRotSave);
				sCrewRotValid = 1;

				cd2CrewAim(&car_data[i], side, &axr, &azr);
				cd2CrewPoseTorso(a->skel, side, axr, azr);

				// a paste-ready baseline of the posed rotations, once or
				// twice per side (this is the only point they are both
				// readable AND ours; the skeleton pass restores them after)
				if (gCd2Cfg.debugLog && sCrewBoneDumps[side] < CD2_CREW_BONE_DUMPS)
				{
					sCrewBoneDumps[side]++;
					cd2CrewDumpBones(a->skel, i, side);
				}

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

	// and the shared motion buffer is put back, so the torso/head aim written
	// in PED_POSE does not leak into every ped playing this motion frame
	if (a->phase == 1 && sCrewRotValid)
	{
		jer_anim_restore_rotations(a->skel, sCrewRotSave);
		sCrewRotValid = 0;
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
				int axr, azr;

				cd2CrewAim(&car_data[i], side, &axr, &azr);
				cd2CrewPoseArm(a->skel, &car_data[i], side, axr, azr);
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
	const CD2_VEH_CREW* cr = cd2CrewOffsets(cp);
	int s = (i == CD2_CREW_SIDE_DRIVER) ? -1 : 1;	// -1 = driver (left door), +1 = gunner (right door)
	int lat = (cb->vx * 104) / 100;			// just outside the body side
	int fwd = cb->vz / 5;				// a touch ahead of centre, not the rear
	int x, z, y, yaw;
	int upExtra;
	SVECTOR rot;

	// the body's own mount deltas (a limo/truck door that sits elsewhere)
	lat += cr->lat[i];
	fwd += cr->fwd[i];
	upExtra = cr->up[i] + ((i == CD2_CREW_SIDE_GUNNER) ? cr->sillRaise : 0);

	x = w->t[0] + (int)(((long long)w->m[0][0] * lat * s) >> 12)
	           + (int)(((long long)w->m[0][2] * fwd) >> 12);
	z = w->t[2] + (int)(((long long)w->m[2][0] * lat * s) >> 12)
	           + (int)(((long long)w->m[2][2] * fwd) >> 12);

	// Ride the CAR, not the map: the old -MapHeight placement pinned the crew
	// to the ground, so a car in the air (or being tossed by a wreck) left them
	// standing on the road below. The car matrix is Y-UP while the ped's
	// position is Y-DOWN, hence the negation.
	y = -(cp->hd.where.t[1]) - CD2_CREW_BODY_LIFT - raiseY - upExtra;

	// Body rotation straight off the car's matrix, so the crew banks and
	// pitches with the car (and the weapon arm follows, being posed in the
	// ped's own local frame). The extracted yaw is identical to
	// hd.direction - verified - so the facing the crew was tuned with is
	// unchanged; only the tilt is new.
	cd2CrewMatrixEuler(w, &rot);

	// the passenger sits ON the sill facing INTO the cabin (legs in, back to
	// the window) - a 180 yaw - while the driver faces forward, leaning out.
	// His torso then twists back toward the car's forward (the rotation
	// channel, cd2CrewPoseTorso) so his head still looks where the car goes.
	yaw = (rot.vy + CD2_CREW_BODY_YAW
		+ (i == CD2_CREW_SIDE_GUNNER ? CD2_CREW_SILL_YAW : 0)
		+ cr->yaw[i]) & 0xfff;

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

	sCrewBoneDumps[0] = 0;
	sCrewBoneDumps[1] = 0;

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

				// the anchor: where the CAMERA is at this instant, not where the car
				// is - so the camera holds that spot instead of sliding off after the
				// wreckage
				if (a != NULL && a->cameraPosition != NULL)
					sCamFocus = *(VECTOR*)a->cameraPosition;
				else
					sCamFocus = sCamPos;

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
				/* The camera holds the spot it was at when the car blew: no lateral
				 * drift, so the wreckage it was looking at stays where it is in frame
				 * and only the height changes. */
				VECTOR pos = sCamFocus;
				long long ease;			// 0..4096, slow at first

				/* RISE: engine camera Y is down, so up means a smaller vy. EASED IN
				 * (t^2), so the pan starts almost still and gathers - a "slower pan up"
				 * instead of the linear lurch the old 420-unit ramp gave. */
				ease = ((long long)t * t) >> 12;
				pos.vy -= (int)((CD2_DEATHCAM_RISE * ease) >> 12);

				// Observability: the rise at each step, so a run can show the pan
				// interpolating over the respawn window.
				if (gCd2Cfg.debugLog && (sCamTicks % 60) == 0)
					printInfo("[cainescrossfire] crew: deathcam tick=%d/%d rise=%d (held at the blast point)\n",
						sCamTicks, delay, (int)(((long long)CD2_DEATHCAM_RISE * ease) >> 12));

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
	// the same reset when the game returns to the frontend menus
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2CrewOnGameStart, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] mounted crew registered (SDK v%d)\n", ctx->sdkVersion);
}
