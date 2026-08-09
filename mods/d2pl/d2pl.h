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

#include "driver2.h"
#include "camera.h"	/* camera_position/camera_angle for aimPoint() */
#include "draw.h"	/* RenderModel */
#include "models.h"	/* FindModelPtrWithName */
#include "pad.h"	/* MPAD_R1/MPAD_R2 fire buttons */
#include "jer_math.h"	/* game types (VECTOR) must be defined first */

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

/* --- arm-hold pose tuning (ped local frame, parent-relative offsets). --- */
/* The ped's skeleton offsets are single-digit units, so the pose values are
 * too: the arm reaches forward ~7 units from the shoulder at full extension.
 * The shoulder bone is also fixated at its rest offset (vOffset) so the walk
 * cycle can't swing the arm — the whole chain is steadied. */
#define POSE_ELBOW_X 0
#define POSE_ELBOW_Y 2		/* lower the forearm a little */
#define POSE_ELBOW_Z 3		/* reach forward */
#define POSE_HAND_X 0
#define POSE_HAND_Y -1		/* hand slightly higher than the elbow */
#define POSE_HAND_Z 4		/* extend to arm's length */

/* --- presenting (aiming) pose: arm raised to eye level, fully forward - */
#define AIM_ELBOW_X 0
#define AIM_ELBOW_Y -2
#define AIM_ELBOW_Z 4
#define AIM_HAND_X 0
#define AIM_HAND_Y -4		/* hand at eye/shoulder height */
#define AIM_HAND_Z 6		/* arm fully extended */

/* ------------------------------------------------------------------ */
/* Settings (persisted via the JERICHO config API into                 */
/* mods/config/d2pl.ini)                                               */
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

	*px = (x * c + z * s) >> 12;
	*pz = (-x * s + z * c) >> 12;
}

	/* force the arm into a holding pose (phase-0 skeleton hook). Override
	 * to pose different bones or add recoil; the base fixates the shoulder
	 * at its (facing-rotated) rest offset and reaches the arm forward and
	 * up to hold the weapon — a local transform that follows Tanner's
	 * rotation. `facing` is the ped's yaw (pPed->dir.vy, 0..4095). */
	virtual void poseArmSide(void* skelVoid, int left, int facing)
	{
		WeaponBone* skel = (WeaponBone*)skelVoid;
		int shoulder = left ? WL_LSHOULDER : WL_RSHOULDER;
		int elbow = left ? WL_LELBOW : WL_RELBOW;
		int hand = left ? WL_LHAND : WL_RHAND;
		int side = left ? -1 : 1;
		int sx, sz, ex, ez, hx, hz;

		/* fix the shoulder at its REST offset, rotated into the facing
		 * frame, so the walk/idle keyframes can't swing the arm — the
		 * whole chain stays steadied */
		sx = skel[shoulder].vOffset.vx;
		sz = skel[shoulder].vOffset.vz;
		D2plRotatePose(&sx, &sz, facing);
		skel[shoulder].vCurrPos.vx = sx;
		skel[shoulder].vCurrPos.vy = skel[shoulder].vOffset.vy;
		skel[shoulder].vCurrPos.vz = sz;

		/* parent-relative pose offsets, rotated into the facing frame:
		 * elbow in front of the shoulder, hand in front of the elbow —
		 * tuning constants at the top of this file */
		ex = POSE_ELBOW_X * side;
		ez = POSE_ELBOW_Z;
		D2plRotatePose(&ex, &ez, facing);
		skel[elbow].vCurrPos.vx = ex;
		skel[elbow].vCurrPos.vy = POSE_ELBOW_Y;
		skel[elbow].vCurrPos.vz = ez;

		hx = POSE_HAND_X * side;
		hz = POSE_HAND_Z;
		D2plRotatePose(&hx, &hz, facing);
		skel[hand].vCurrPos.vx = hx;
		skel[hand].vCurrPos.vy = POSE_HAND_Y;
		skel[hand].vCurrPos.vz = hz;
	}

	virtual void poseArm(void* skelVoid, int facing)
	{
		/* hip carry: whichever arm matches the camera shoulder */
		poseArmSide(skelVoid, shoulderSide > 0, facing);
	}

	virtual void poseArmAim(void* skelVoid, int facing)
	{
		/* presenting: arm raised to eye level and fully extended forward */
		WeaponBone* skel = (WeaponBone*)skelVoid;
		int shoulder = (shoulderSide > 0) ? WL_LSHOULDER : WL_RSHOULDER;
		int elbow = (shoulderSide > 0) ? WL_LELBOW : WL_RELBOW;
		int hand = (shoulderSide > 0) ? WL_LHAND : WL_RHAND;
		int side = (shoulderSide > 0) ? -1 : 1;
		int sx, sz, ex, ez, hx, hz;

		sx = skel[shoulder].vOffset.vx;
		sz = skel[shoulder].vOffset.vz;
		D2plRotatePose(&sx, &sz, facing);
		skel[shoulder].vCurrPos.vx = sx;
		skel[shoulder].vCurrPos.vy = skel[shoulder].vOffset.vy;
		skel[shoulder].vCurrPos.vz = sz;

		ex = AIM_ELBOW_X * side;
		ez = AIM_ELBOW_Z;
		D2plRotatePose(&ex, &ez, facing);
		skel[elbow].vCurrPos.vx = ex;
		skel[elbow].vCurrPos.vy = AIM_ELBOW_Y;
		skel[elbow].vCurrPos.vz = ez;

		hx = AIM_HAND_X * side;
		hz = AIM_HAND_Z;
		D2plRotatePose(&hx, &hz, facing);
		skel[hand].vCurrPos.vx = hx;
		skel[hand].vCurrPos.vy = AIM_HAND_Y;
		skel[hand].vCurrPos.vz = hz;
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

void WeaponSystem_Register(WeaponBase* weapon);
void WeaponSystem_Equip(WeaponBase* weapon);
void WeaponSystem_Cycle(int direction);
WeaponBase* WeaponSystem_Current(void);
int* WeaponSystem_HandPos(void);

#endif /* D2PL_H */
