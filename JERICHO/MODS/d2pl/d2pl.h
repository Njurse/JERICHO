/*
 * d2pl.h — Driver 2 Parallel Lines (d2pl): the overridable weapon template
 * + the settings schema shared with the pause-menu shell (pause.c).
 *
 * The WeaponBase template: a custom weapon subclasses WeaponBase, sets its
 * properties in its constructor, and overrides any of the virtual hooks it
 * needs. The base provides sensible defaults for everything, so a minimal
 * weapon is just:
 *
 *     class MyGun : public WeaponBase {
 *     public:
 *         MyGun() {
 *             id = "mygun"; name = "My Gun";
 *             damage = 100; clipSize = 12; maxReserve = 60;
 *             fireRate = 6; reloadTime = 45; range = 30000;
 *             meshName = "BOMB";
 *         }
 *     };
 *
 * then register it from the module entry:
 *
 *     static MyGun gMyGun;
 *     gMyGun.init();
 *     WeaponSystem_Register(&gMyGun);
 *
 * The base's default primaryFire()/secondaryFire() play the frontend menu
 * accept (FESound 2) and backout (FESound 0) samples respectively.
 *
 * The skeleton structures below mirror motion_c.c's LIMBS/BONE exactly —
 * keep them in sync if the engine layout ever changes (the module casts
 * through void* via JER_ARGS_PED_SKELETON).
 */
#ifndef D2PL_H
#define D2PL_H

#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"

#include "driver2.h"
#include "camera.h"	/* camera_position/camera_angle for aimPoint() */
#include "draw.h"	/* RenderModel */
#include "models.h"	/* FindModelPtrWithName */
#include "pad.h"	/* MPAD_R1/MPAD_R2 fire buttons */
#include "jer_math.h"	/* game types (VECTOR) must be defined first */

 /* ------------------------------------------------------------------ */
 /* Shared tuning constants                                                */
 /* ------------------------------------------------------------------ */

#define LOOK_DEADZONE 24	/* stick counts below this are "idle" */
#define LOOK_YAW_SIGN 1	/* -1 = push right orbits the camera right */

/* aim mode: the look speed is halved while aiming for finer control */
#define AIM_LOOK_DIV 2

/* the L2/R2 look buttons: the camera eases to the car's left/right side
 * view (90 degrees, car-relative) — the stock TurnHead head-rot path
 * breaks the module's transforms, so the module owns the buttons */
#define CAM_LOOK_SIDE_ANGLE 1024
#define CAM_LOOK_SIDE_DIV 4		/* ease divisor: ~0.1 s to reach the side view */

 /* on-foot startup momentum: while the ped is just starting to move (speed
  * below this) the camera eases with a touch more lag, then tightens up */
#define CAM_FOOT_LAG_SPEED 6
#define CAM_FOOT_LAG_DIV 8
#define CAM_ORBIT_SPEED_MAX 256		/* max orbit speed at full stick, sensX = 64
				   (x4: the default was too slow) */
#define CAM_ORBIT_RAMP_FRAMES 10		/* frames to reach full orbit speed (~1/6 s —
				   the ramp speed was doubled) — the yaw "gains
				   momentum" instead of jumping to its max */
#define CAM_PITCH_STICK_SIGN -1	/* -1 = pushing up RAISES the camera on its
				   orbit around the player (see the header note) */
#define CAM_PITCH_MAX 1600		/* ~45 degrees of pitch up/down each way — the
				   vertical is deliberately tamer than the horizontal so it
				   doesn't feel janky at full deflection */
#define CAM_FOV_PITCH_SCALE 4	/* scr_z delta per 128 pitch units (GTA4-style:
				   looking down from above narrows the FOV, up widens) */
#define CAM_SETTLE_IDLE_FOOT 130	/* frames idle before settling back (on foot) */
#define CAM_SETTLE_IDLE_CAR 105	/* cars settle back after this long idle... */
#define CAM_SETTLE_IDLE_CAR_MOVING 30	/* ...but a car in motion resets almost
				   immediately: it interpolates back to its default
				   angle after a few frames of stick silence */
#define CAM_LOW_SPEED_GRIP 8	/* car wheel_speed at/below which the orbit stays
				   gripped (no camera swing at a standstill) */

#define MOVE_DEADZONE 24	/* left-stick deadzone */
#define PED_MAX_SPEED 40	/* the engine's run speed (SetupRunner) — the
				   analog magnitude scales toward this */
#define PED_SPEED_LERP 8	/* lerp divisor — geometric: ~15 frames to reach
				   the target each way (the stand-start momentum) */
#define MOVE_TURN_SIGN 1	/* +1 = positive angle delta turns right */
#define MOVE_STICK_SIGN_X -1	/* the stick X was mirrored against the camera (flipped 2026-08-10) */
#define MOVE_STICK_SIGN_Y -1	/* flip if your pad's Y axis is inverted */
#define MOVE_LERP_DIV 16
#define AIM_MOVE_LERP_DIV 2	/* aiming: the heading lerps VERY fast (third-person —
				   he snaps to the camera-relative run direction) */		/* responsive exponential heading lerp: 1/8 of the
				   remaining gap toward the stick-derived heading each
				   frame (fast when far, eases in) */
#define RUN_TURN_LIMIT 2	/* max turn per frame while running (~11.25 deg,
				   ~675 deg/s): well beyond the original 32-unit
				   restriction so Tanner decisively turns toward
				   the stick heading (even a 180) instead of
				   creeping around; the exponential lerp keeps it
				   smooth, the cap keeps it from snapping */
#define PIVOT_TURN_LIMIT 3	/* standstill pivot speed (~22.5 deg/frame):
				   near-instant so the player starts running the new
				   way immediately, GTA-style */

#define CAR_ORBIT_UP -160	/* raise the vehicle orbit/focal point (Driver 2
				   uses INVERTED y — up = -y); the camera gravitates
				   toward looking AHEAD of the car near the forward
				   angle so view-panning in isn't jarring */

				   /* ground cap: Driver 2 uses INVERTED y (vy grows downward), so the road is
					* a heightfield the camera must not sink below. Mirror the engine's own
					* chase-cam clamp (camera.c: cammapht = carheight - MapHeight - 100) — the
					* camera stays at-or-above the terrain line and slides along the surface. */
#define TERRAIN_CLEAR_FOOT 15
#define TERRAIN_CLEAR_CAR 50
					/* ground-slide "forced look-up": when the terrain cap pins the camera (it
					 * would sink into the ground), the view tilts slightly upward — a fraction
					 * of the would-be intersection depth, capped — so the camera reads as
					 * being pushed up out of the ground instead of boring into it. */
#define GROUND_LOOKUP_SCALE 4	/* angle units per 256 units of penetration */
#define GROUND_LOOKUP_MAX 400	/* cap the forced up-tilt (~17.5 deg) */
					 // ON FOOT AIMING CAMERA HEIGHT
#define FOOT_AIM_HEIGHT 270	/* on-foot aim point: the torso ~90 units above
				   the waist anchor (camera frame: up = -y) */
#define CAR_SPEED_DIV 3		/* car pull-in fades with speed (GTA4 zoom-out) */

				   /* natural settle-back (GTA IV State A): the module eases the orbit toward
					* the blended natural heading instead of the engine's fixed-rate settle */
#define SETTLE_DIV 12		/* 1/12 of the gap per frame — lazy, cinematic */
#define VEL_NOISE 16		/* base-delta noise floor before velocity counts */
#define VEL_BLEND_SPEED_SCALE 24	/* velocity-blend weight per speed unit */
#define VEL_BLEND_MAX 2900	/* cap the base blend weight (~0.7 of 4096);
				   the drift boost may push past this */
#define VEL_BLEND_ABS_MAX 3500	/* absolute ceiling (~0.85) — never full weight,
				   so the heading vector never fully cancels */
#define VEL_BLEND_SLIP_MIN 384	/* slip angle where the drift boost starts */
#define VEL_BLEND_SLIP_DIV 2	/* slip boost per unit of slip */
#define FOV_SPEED_SCALE 512	/* subtle FOV widening with smoothed speed */
#define HEIGHT_SPEED_SCALE 256	/* subtle camera height rise with speed */
#define FOOT_WALK_SPEED 14	/* on-foot speed at which the camera sits at its
				   base distance (pull in when slower, extend when
				   running — relative-framing doc) */
#define FOOT_SPEED_DIST 4	/* distance units added per unit of run speed */
#define MAX_POS_STEP 600	/* the camera position NEVER snaps during play: a
				   big correction travels at a bounded rate — the
				   writeup's 'never snap, always blend' applies to the
				   position rig as well as the rotation */
#define CONTEXT_JUMP 30000	/* a target jump bigger than this is a context
				   switch (level load, intro sweep -> play, spawn):
				   re-place instantly instead of flying across the
				   map at MAX_POS_STEP */

				   /* Instability Governor (spin-out recovery, GTA IV §4): when the car's
					* angular rate spikes (spin, crash), the camera stops trying to be clever
					* and holds its orientation until the car settles. Hysteresis: trigger
					* above the upper threshold, release only after the rate has stayed below
					* the lower threshold for a confirmation window. Priority below Manual
					* Look, above Natural. */
#define INSTAB_TRIGGER 256	/* smoothed deg-ish units/frame that claims focus */
#define INSTAB_RELEASE 32	/* must drop below this to release */
#define INSTAB_CONFIRM 32	/* frames of calm before releasing */

					/* aim camera (on foot) */
#define AIM_PULL 2000		/* /4096: pull toward Tanner while aiming */
#define AIM_HEIGHT -100
#define AIM_ZOOM 160		/* scr_z increase -> FOV decrease (focus) */
#define AIM_LATERAL 55		/* shoulder bias while aiming (ped scale ~130) */

/* impact marker: a big black X at the aim point, 1 second (60 frames) */
#define MARKER_LIFETIME 600
#define MARKER_MIN_SIZE 150
#define MARKER_MAX_SIZE 800

/* --- limb ids (must match motion_c.c's enum LIMBS) ------------------- */
enum WeaponLimbs
{
	WL_ROOT = 0,
	WL_LOWERBACK = 1,
	WL_JOINT_1 = 2,
	WL_NECK = 3,
	WL_HEAD = 4,
	WL_LSHOULDER = 5,
	WL_LELBOW = 6,
	WL_LHAND = 7,
	WL_LFINGERS = 8,
	WL_RSHOULDER = 9,
	WL_RELBOW = 10,
	WL_RHAND = 11,
	WL_RFINGERS = 12,
	WL_HIPS = 13,
	WL_LHIP = 14,
	WL_LKNEE = 15,
	WL_LFOOT = 16,
	WL_LTOE = 17,
	WL_RHIP = 18,
	WL_RKNEE = 19,
	WL_RFOOT = 20,
	WL_RTOE = 21,
	WL_JOINT = 22,
};

/* --- bone layout (must match motion_c.c's struct BONE) --------------- */
struct WeaponBone
{
	int id;
	WeaponBone* pParent;
	char numChildren;
	WeaponBone* pChildren[3];
	SVECTOR_NOPAD* pvOrigPos;
	SVECTOR* pvRotation;
	VECTOR vOffset;
	VECTOR vCurrPos;
	MODEL** pModel;
};

/* --- arm-pose tuning (ped local frame, parent-relative offsets) --- */
/* The ped's skeleton offsets are single-digit units, so the pose values
 * are too: the arm reaches forward ~7 units from the shoulder at full
 * extension. The shoulder bone is also fixated at its rest offset
 * (vOffset) so the walk cycle can't swing the arm — the whole chain is
 * steadied. Each pose is ONE row below — to add a pose (e.g. a lowered
 * "ready" carry), add a D2PL_POSE row here and a method that calls
 * poseArmPose() with it. */
/* ------------------------------------------------------------------ */
/* AXIS CONVENTIONS — read before touching any pose/camera offset.    */
/*                                                                     */
/*   * The RAW world frame has +Y UP: raw basePos[1], MapHeight() and  */
/*     FindSurfaceD2 normals are in it.                                */
/*   * The RENDER frame (the ped skeleton AND this module's camera     */
/*     positions) has Y INVERTED: +Y is DOWN (the GTE screen          */
/*     convention). The skeleton's vOffset/vCurrPos/vJPos and the      */
/*     module's camPos->vy all use this inverted frame — the module    */
/*     converts with `camPos->vy = -base[1] + offset`.                 */
/*   * X/Z in the pose offsets are parent-relative; X mirrors with the */
/*     shoulder side (left flips it), and D2plRotatePose() rotates the */
/*     pair by the ped's facing (pPed->dir.vy) so the pose follows     */
/*     his rotation. +Z in the pose reaches toward the facing.         */
/*     Therefore: elbowY +2 LOWERS the forearm (render y-down),        */
/*     handY -1 raises it, and the hand Z must EXCEED the elbow Z for  */
/*     the arm to extend (a hand Z <= elbow Z folds the arm back — the */
/*     "stuck folded" look).                                           */
/* ------------------------------------------------------------------ */
typedef struct D2PL_POSE
{
	int elbowX, elbowY, elbowZ;	/* forearm, relative to the shoulder */
	int handX, handY, handZ;	/* hand, relative to the elbow */
} D2PL_POSE;

/* hip carry: forearm low, hand at the side, muzzle forward */
static const D2PL_POSE POSE_CARRY = { 0, 2, 3, 0, -1, 4 };

/* presenting (aiming): arm raised to eye level, fully extended forward */
static const D2PL_POSE POSE_AIM = { 0, -2, 4, 0, -4, 6 };

/* ------------------------------------------------------------------ */
/* Settings (persisted via the JERICHO config API into                 */
/* JERICHO/CONFIG/d2pl.ini)                                               */
/*                                                                     */
/* The D2PL_ITEM_* item ids and D2PL_LASER_* colors live in            */
/* jer_events.h (they are the pause-menu <-> module contract shared    */
/* with pause.c).                                                      */
/* ------------------------------------------------------------------ */

#define D2PL_LASER_DEFAULT D2PL_LASER_RED

/* ------------------------------------------------------------------ */
/* The overridable weapon base                                         */
/* ------------------------------------------------------------------ */
class WeaponBase
{
public:
	/* identity */
	const char* id;
	const char* name;

	/* --- properties (set in the constructor) --- */
	int damage;		/* damage dealt per hit */
	int fireMode;		/* 0 = semi-auto, 1 = full-auto */
	int clipSize;		/* rounds per magazine */
	int maxReserve;		/* spare rounds carried */
	int fireRate;		/* frames between shots */
	int reloadTime;		/* frames a reload takes */
	int range;		/* world units the aim ray reaches */
	int spread;		/* 0..4096 angle units of random spread */
	const char* meshName;	/* mesh drawn at the hand (game model name) */
	int shoulderSide;	/* -1 or +1: which shoulder the camera sits on */
	int aimZoom;		/* camera scr_z while aiming (0 = no change) */

	/* --- live state --- */
	int clip;		/* rounds left in the magazine */
	int reserve;		/* spare rounds left */
	int reloading;		/* frames left on the current reload, 0 = none */
	int cooldown;		/* frames until the next shot is allowed */
	int fired;		/* set when a round was fired this frame (module clears) */
	MODEL* meshModel;	/* resolved from meshName (lazy) */
	int muzzle[3];		/* last RHAND world position (x, y, z) */
	int aiming;		/* 1 while L1 aim is held (set by the module) */

	WeaponBase()
	{
		id = "weapon";
		name = "Weapon";
		damage = 100;
		fireMode = 0;
		clipSize = 12;
		maxReserve = 60;
		fireRate = 6;
		reloadTime = 45;
		range = 30000;
		spread = 0;
		meshName = NULL;
		shoulderSide = -1;
		aimZoom = 0;

		clip = 0;
		reserve = 0;
		reloading = 0;
		cooldown = 0;
		fired = 0;
		meshModel = NULL;
		muzzle[0] = muzzle[1] = muzzle[2] = 0;
		aiming = 0;
	}

	/* resolve the mesh lazily (models load after boot) */
	void init()
	{
		if (meshName != NULL && meshModel == NULL)
			meshModel = FindModelPtrWithName((char*)meshName);
	}

	/* --- overridable behavior --- */
	virtual void onEquip(void) { /* nothing by default */ }
	virtual void onHolster(void) { reloading = 0; }

	/* per-frame update; `pad` = held bits, `padNew` = pressed this frame.
	 * The default ticks cooldown/reload and fires on the fire buttons
	 * (R1 = primary, R2 = secondary) when ammo allows. */
	virtual void update(int pad, int padNew)
	{
		if (cooldown > 0)
			cooldown--;

		if (reloading > 0)
		{
			reloading--;

			if (reloading == 0)
			{
				int needed = clipSize - clip;

				if (reserve >= needed)
				{
					reserve -= needed;
					clip = clipSize;
				}
				else
				{
					clip += reserve;
					reserve = 0;
				}

				onAmmoChanged();
			}
		}

		if (reloading > 0)
			return;

		/* firing is gated on the L1 aim state (GTA-style: R1/R2 shoot while
		 * aiming). Override update() to allow hip-firing. */
		if (!aiming)
			return;

		if ((pad & MPAD_R1) != 0 && (fireMode == 1 || (padNew & MPAD_R1) != 0))
			primaryFire();

		if ((pad & MPAD_R2) != 0 && (padNew & MPAD_R2) != 0)
			secondaryFire();
	}

	virtual bool canFire(void)
	{
		return cooldown <= 0 && clip > 0 && reloading <= 0;
	}

	/* default primary fire: frontend menu ACCEPT sound + consume a round */
	virtual void primaryFire(void)
	{
		if (!canFire())
			return;

		playSound(2);		/* frontend menu accept */
		consumeAmmo();
	}

	/* default secondary fire: frontend menu BACKOUT sound */
	virtual void secondaryFire(void)
	{
		playSound(0);		/* frontend menu backout */
	}

	virtual void reload(void)
	{
		if (reloading == 0 && clip < clipSize && reserve > 0)
		{
			reloading = reloadTime;
			onAmmoChanged();
		}
	}

	/* aim point: where the camera-centre ray reaches at `range`. The default
	 * derives it from the current camera position + angle (yaw = the free-look
	 * heading, pitch from camera_angle.vx). Override for custom ballistic
	 * behaviour. out3 receives world (x, y, z). */
	virtual void aimPoint(int* out3)
	{
		int yawH = -camera_angle.vy & 0xfff;	/* camera forward heading */
		int pitch = camera_angle.vx & 0xfff;	/* 0 = level, + = down */
		int cosP = RCOS(pitch);
		int dirX = FIXEDH(RSIN(yawH) * cosP);
		int dirY = FIXEDH(RSIN(pitch));
		int dirZ = FIXEDH(RCOS(yawH) * cosP);

		out3[0] = camera_position.vx + (dirX * range) / 4096;
		out3[1] = camera_position.vy + (dirY * range) / 4096;
		out3[2] = camera_position.vz + (dirZ * range) / 4096;
	}

	virtual void onAmmoChanged(void) { /* HUD refresh hook */ }
	/* rotate a local (x, z) offset by the ped's facing yaw. newRotateBones
 * builds the skeleton's vCurrPos frame as the model rotated by dir.vy, so
 * the arm pose must be rotated the same way to point where Tanner faces.
 * Convention matches the engine: facing h -> forward = (RSIN h, RCOS h). */
	static inline void D2plRotatePose(int* px, int* pz, int h)
	{
		int x = *px;
		int z = *pz;
		int c = RCOS(h);
		int s = RSIN(h);

		/* fixed‑point multiply: both cos/sin are scaled by 4096 (2^12) */
		*px = (x * c + z * s) >> 12;
		*pz = (-x * s + z * c) >> 12;
	}

	/* force the arm into a holding pose (phase-0 skeleton hook). Override
	 * to pose different bones or add recoil; the base fixates the shoulder
	 * at its (facing-rotated) rest offset and reaches the arm forward and
	 * up to hold the weapon — a local transform that follows Tanner's
	 * rotation. `facing` is the ped's yaw (pPed->dir.vy, 0..4095).
	 * poseArmPose() is the ONE shared implementation — the pose rows at
	 * the top of this header are the only tuning needed for a new pose. */
	virtual void poseArmPose(void* skelVoid, int left, int facing, const D2PL_POSE& pose)
	{
		WeaponBone* skel = (WeaponBone*)skelVoid;
		int shoulder = left ? WL_LSHOULDER : WL_RSHOULDER;
		int elbow = left ? WL_LELBOW : WL_RELBOW;
		int hand = left ? WL_LHAND : WL_RHAND;
		int side = left ? -1 : 1;
		int sx, sz, ex, ez, hx, hz;
		int shoulderX, shoulderY, shoulderZ;
		int elbowX, elbowY, elbowZ;

		// --- Shoulder: Fixed at rest position ---
		sx = skel[shoulder].vOffset.vx;
		sz = skel[shoulder].vOffset.vz;
		D2plRotatePose(&sx, &sz, facing);
		skel[shoulder].vCurrPos.vx = sx;
		skel[shoulder].vCurrPos.vy = skel[shoulder].vOffset.vy;
		skel[shoulder].vCurrPos.vz = sz;

		shoulderX = skel[shoulder].vCurrPos.vx;
		shoulderY = skel[shoulder].vCurrPos.vy;
		shoulderZ = skel[shoulder].vCurrPos.vz;

		// --- Elbow: Offset from shoulder (small forward and slightly outward) ---
		// Scale the pose values to prevent folding; adjust divisor as needed.
		ex = (pose.elbowX * side) >> 2;   // lateral offset (outward)
		ez = pose.elbowZ >> 2;            // forward offset
		D2plRotatePose(&ex, &ez, facing);
		elbowX = shoulderX + ex;
		elbowY = shoulderY + (pose.elbowY >> 2); // vertical offset (up)
		elbowZ = shoulderZ + 32 + ez;
		skel[elbow].vCurrPos.vx = elbowX;
		skel[elbow].vCurrPos.vy = elbowY;
		skel[elbow].vCurrPos.vz = elbowZ;

		// --- Hand: Offset from elbow (longer forward extension) ---
		hx = (pose.handX * side) >> 2;   // lateral offset (outward)
		hz = pose.handZ >> 2;            // forward offset (larger for extension)
		D2plRotatePose(&hx, &hz, facing);
		skel[hand].vCurrPos.vx = elbowX + hx;
		skel[hand].vCurrPos.vy = elbowY - (pose.handY >> 2); // raise hand upward (negate Y)
		skel[hand].vCurrPos.vz = elbowZ + hz;
	}

	virtual void poseArm(void* skelVoid, int facing)
	{
		/* hip carry: whichever arm matches the camera shoulder */
		poseArmPose(skelVoid, shoulderSide > 0, facing, POSE_CARRY);
	}

	virtual void poseArmAim(void* skelVoid, int facing)
	{
		/* presenting: arm raised to eye level and fully extended forward */
		poseArmPose(skelVoid, shoulderSide > 0, facing, POSE_AIM);
	}

	/* --- helpers --- */
	void consumeAmmo(void)
	{
		if (clip > 0)
		{
			clip--;
			cooldown = fireRate;
			fired = 1;	/* the module draws the impact marker on this */
			onAmmoChanged();
		}
	}

	/* Weapon audio is intentionally DISABLED for now: swapping the SFX
	 * soundbank (LoadBankFromLump(SOUND_BANK_SFX, SBK_ID_MENU)) around
	 * every shot caused problems in-game, so we play nothing rather than
	 * risk corrupting the bank state. The call sites stay wired so audio
	 * can be re-added cleanly later. */
	void playSound(int sample)
	{
		(void)sample;
	}
};

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

void D2plWeaponInit(void);
void D2plAimAtPlayer(SVECTOR* camAngle, VECTOR* camPos, int* base,
	int inCar, int baseDir, int cameraAngle, int freeLook);
void WeaponSystem_Register(WeaponBase* weapon);
void WeaponSystem_Equip(WeaponBase* weapon);
void WeaponSystem_Cycle(int direction);
WeaponBase* WeaponSystem_Current(void);
int* WeaponSystem_HandPos(void);



/* ------------------------------------------------------------------ */
/* Settings (persisted via the JERICHO config API into                 */
/* JERICHO/CONFIG/d2pl.ini)                                               */
/* ------------------------------------------------------------------ */

typedef struct D2PL_SETTINGS
{
	int sensX;		/* view-change sensitivity, horizontal (64 = default) */
	int sensY;		/* view-change sensitivity, vertical (64 = default) */
	int joyCamera;		/* 1 = right-stick orbit/look control (default on) */
	int footDist;		/* on-foot camera pull-in distance */
	int footLat;		/* on-foot shoulder offset */
	int footHeight;		/* on-foot camera height (inverted y: below the body) */
	int carDist;		/* in-car pull-in (fraction of the view axis) */
	int carLatPct;		/* in-car lateral, % of the car's bbox width */
	int carHeight;		/* in-car camera height offset (taller cars need
				   a higher cam) */
	int shoulder;		/* -1 = left shoulder, +1 = right */
	int invertH;		/* 1 = invert horizontal look */
	int invertV;		/* 1 = invert vertical look */
	int fovEnabled;		/* 1 = override the engine FOV */
	int fovDeg;		/* target horizontal FOV (degrees) */
	int laserColor;		/* D2PL_LASER_* index (0 = off) */
	int cameraEnabled;	/* 1 = the mod's camera/movement run (kill-switch) */
} D2PL_SETTINGS;

#define D2PL_CFG_MOD "d2pl"
#define D2PL_CFG_VERSION 6	/* bump when defaults change — a stale profile is
				   deleted and regenerated from scratch */

extern D2PL_SETTINGS gS;



/* ================================================================== */
/* Module state                                                        */
/* ================================================================== */

#define MAX_WEAPONS 8

/* ------------------------------------------------------------------ */
/* Shared module state (defined in d2pl.c / d2pl_car.c / d2pl_ped.c /       */
/* d2pl_weapon.c — extern here so the theme files stay independent)          */
/* ------------------------------------------------------------------ */

extern int gLookIdle;		/* frames since the stick was last used */
extern int gLookPitch;		/* smoothed pitch */
extern int gLookPitchTarget;
extern int gYawSpeed;		/* current orbit speed (momentum: ramps toward
				   the stick's max over ~1/3 s, decays on release) */
extern int gGripDist;		/* orbit radius frozen for the whole pan (the
				   spherical-pan grip) — hoisted so level resets can
				   clear it */
extern int gMoveRefHeading;	/* camera heading frozen at view-manipulation start
				   (the on-foot movement reference frame, GTA IV §8) */
extern int gMoveRefActive;	/* 1 while that freeze is in effect */
extern int gVelAngle;		/* smoothed ground-velocity heading (0..4095) */
extern int gSpeedSmooth;	/* smoothed wheel speed (FIXEDH units, 0 on foot) */
extern int gBobLastX;	/* last on-foot base position (view bob motion) */
extern int gBobLastZ;
extern int gLastBaseX, gLastBaseZ;
extern int gCamInitialized;	/* one-shot: snap the spawn orbit angle behind */
extern int gSettleInfluence;	/* natural-settle influence fade (0..4096): ramps
				   in on release so the ease eases IN — no correction
				   snap at the handoff, live target throughout */
extern int gSettlePitch0;	/* the pitch held when the settle began — it
				   returns to level IN SYNC with the angle fade
				   (the old fast 1/6 pitch snap-back was what made
				   follow <-> orbit switching feel disjointed) */
extern int gAngRate;		/* smoothed car-body angular rate (units/frame) */
extern int gLastCarDir;		/* previous car heading for the rate delta */
extern int gInstability;	/* 1 = the Instability Governor holds the camera */
extern int gInstabConfirm;	/* frames the rate has stayed calm (hysteresis) */
extern int gFootSpeedSmooth;	/* smoothed on-foot speed (pPed->speed) */
extern int gLastTannerPad;	/* previous frame's on-foot pad — the hook carries
				   no padNew, so the X-reload edge is tracked here */
extern int gTrackedCarId;	/* the car the governor is sampling (seeds the
				   angular-rate delta on car entry, so the first
				   in-car frame can't false-trigger a spin) */
extern VECTOR gCamSmooth;	/* smoothed camera position (live target + bounded
				   rate; hoisted so level starts re-place it) */
extern int gCamSmoothValid;

/* ped was moving last frame (run-vs-pivot turn limit) */
extern int gPedWasMoving;
extern int gPedRunSpeed;	/* smoothed run speed (0..40): the analog
				   deflection magnitude scaled to the engine's
				   run speed, interpolated so Tanner presses
				   off from a stand-start (written to pPed->speed by D2plOnPedMove) */
extern int gPedStickMove;	/* 1 while the left stick is the movement source
				   (D2plOnPedMove only writes the speed then, so the
				   stock D-pad/keyboard movement keeps the engine speed) */

/* weapon state */
extern WeaponBase* gWeapons[MAX_WEAPONS];
extern int gWeaponCount;
extern WeaponBase* gCurrentWeapon;
extern int gAiming;
extern int gHandPos[3];		/* world RHAND position (muzzle origin) */
extern int gAimPoint[3];	/* world aim point at weapon range */

/* impact marker state */
extern int gMarkerTimer;	/* frames left until the marker fades */
extern int gMarkerPos[3];
extern int gPedScaleLogged;
extern int gAimLookBlend;	/* 0 = settled (forward), 4096 = free-looking */

/* mouse integration: relative motion consumed by the look handler (yaw/pitch
 * nudges on top of the stick); buttons map to aim (right), primary fire
 * (left), secondary fire (middle). Per-pixel nudge at sens 64:
 * (dx * sens * MOUSE_YAW) / (64*128). */
#include <SDL.h>

#define MOUSE_YAW	1152
#define MOUSE_PITCH	1152

/* input-device authority: the mouse only drives the view while it is the
 * recent input device (motion/click grants ~0.75 s; live controller
 * analog revokes it immediately) — a connected controller + an idle mouse
 * must never fight over the camera */
#define MOUSE_AUTHORITY_FRAMES 45

extern int gMouseDX;	/* relative mouse motion this frame (look) */
extern int gMouseDY;
extern Uint32 gLastMouseButtons;	/* for synthesized press edges */
extern int gMouseActive;	/* frames of remaining mouse authority */
extern int gLookForceSettle;
extern int gLookBackActive;	/* 1 while the R3/L2+R2 look-behind is held: the
				   camera extends to the full chase distance and the
				   collision push is skipped, so the rear view never zooms
				   onto the car (no pursuit "zoom focus") */	/* a L2/R2/L3 look-button release returns
				   the camera to the normal view immediately —
				   no hold-window delay, and past the standstill
				   reverse-grace (armed while the button is held) */


/* ------------------------------------------------------------------ */
/* Cross-file prototypes                                                 */
/* ------------------------------------------------------------------ */

/* main (d2pl.c) */
int D2plFovScrZ(int degrees);
int D2plOnLook(void* userdata, void* args);
int D2plOnCamera(void* userdata, void* args);
int D2plOnFrame(void* userdata, void* args);
int D2plOnPauseMenu(void* userdata, void* args);
int D2plOnGameStart(void* userdata, void* args);
int D2plSmoothCamera(VECTOR* camPos, int* base, int inCar, int* penOut);
void D2plLogCamera(PLAYER* lp, int baseDirIn, VECTOR* camPos,
	SVECTOR* camAngle, int clipped);
void D2plReset(void);
void D2plAdjustItem(int item, int direction);
const char* D2plItemLabel(int item);

/* car (d2pl_car.c) */
int D2plCarSpeed(PLAYER* lp);
int D2plNaturalHeading(PLAYER* lp, int inCar);
void D2plTrackInstability(PLAYER* lp, int inCar);
void D2plCarCamera(void* args, int heading);

/* ped (d2pl_ped.c) */
int D2plOnPedInput(void* userdata, void* args);
int D2plOnPedMove(void* userdata, void* args);
int D2plOnPedPose(void* userdata, void* args);	/* JER_EVENT_PED_POSE: the
				   torso-aim + head-lock (the only effective rotation channel) */	/* JER_EVENT_PED_MOVE: the
				   last word on the run speed before the position advances */
void D2plPedCamera(void* args, int heading);

/* weapon (d2pl_weapon.c) */
int D2plOnSkeleton(void* userdata, void* args);
int D2plOnOverlay(void* userdata, void* args);
void D2plWeaponFrame(PLAYER* lp, int pad, int padNew);
int D2plAimCamera(void* args, int heading);
void D2plLaserColor(CVECTOR* out);
void WeaponSystem_Register(WeaponBase* weapon);
void WeaponSystem_Equip(WeaponBase* weapon);
void WeaponSystem_Cycle(int direction);
WeaponBase* WeaponSystem_Current(void);
int* WeaponSystem_HandPos(void);

#endif /* D2PL_H */
