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
#include "sound.h"	/* FESound */
#include "gamesnd.h"	/* LoadBankFromLump, SBK_ID_MENU/SFX */
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

/* --- arm-hold pose tuning (ped local frame, parent-relative offsets) - */
/* The ped unit scale is small (~200 units tall), so the arm needs big
 * offsets to visibly reach forward. */
#define POSE_ELBOW_X 0
#define POSE_ELBOW_Y 80		/* lower the forearm a little */
#define POSE_ELBOW_Z 220	/* reach forward */
#define POSE_HAND_X 0
#define POSE_HAND_Y -60		/* hand slightly higher than the elbow */
#define POSE_HAND_Z 260		/* extend to arm's length */

/* --- presenting (aiming) pose: arm raised to eye level, fully forward - */
#define AIM_ELBOW_X 0
#define AIM_ELBOW_Y -60
#define AIM_ELBOW_Z 260
#define AIM_HAND_X 0
#define AIM_HAND_Y -200		/* hand at eye/shoulder height */
#define AIM_HAND_Z 520		/* arm fully extended */

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

	/* force the arm into a holding pose (phase-0 skeleton hook). Override
	 * to pose different bones or add recoil; the base reaches the right
	 * arm forward and up to hold the weapon at shoulder height. */
	virtual void poseArmSide(void* skelVoid, int left)
	{
		WeaponBone* skel = (WeaponBone*)skelVoid;
		int elbow = left ? WL_LELBOW : WL_RELBOW;
		int hand = left ? WL_LHAND : WL_RHAND;
		int side = left ? -1 : 1;

		/* parent-relative offsets: elbow in front of the shoulder, hand in
		 * front of the elbow — tuning constants at the top of this file */
		skel[elbow].vCurrPos.vx = POSE_ELBOW_X * side;
		skel[elbow].vCurrPos.vy = POSE_ELBOW_Y;
		skel[elbow].vCurrPos.vz = POSE_ELBOW_Z;

		skel[hand].vCurrPos.vx = POSE_HAND_X * side;
		skel[hand].vCurrPos.vy = POSE_HAND_Y;
		skel[hand].vCurrPos.vz = POSE_HAND_Z;
	}

	virtual void poseArm(void* skelVoid)
	{
		/* hip carry: whichever arm matches the camera shoulder */
		poseArmSide(skelVoid, shoulderSide > 0);
	}

	virtual void poseArmAim(void* skelVoid)
	{
		/* presenting: arm raised to eye level and fully extended forward */
		WeaponBone* skel = (WeaponBone*)skelVoid;
		int elbow = (shoulderSide > 0) ? WL_LELBOW : WL_RELBOW;
		int hand = (shoulderSide > 0) ? WL_LHAND : WL_RHAND;
		int side = (shoulderSide > 0) ? -1 : 1;

		skel[elbow].vCurrPos.vx = AIM_ELBOW_X * side;
		skel[elbow].vCurrPos.vy = AIM_ELBOW_Y;
		skel[elbow].vCurrPos.vz = AIM_ELBOW_Z;

		skel[hand].vCurrPos.vx = AIM_HAND_X * side;
		skel[hand].vCurrPos.vy = AIM_HAND_Y;
		skel[hand].vCurrPos.vz = AIM_HAND_Z;
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

	/* The frontend menu samples (accept = 2, backout = 0) live in the menu
	 * SFX bank, but in-game the SFX bank slot holds the game's own sounds
	 * (loaded at level start). Swap the menu bank in for the play and put
	 * the game's SFX bank back immediately so gameplay sounds keep working. */
	void playSound(int sample)
	{
		LoadBankFromLump(SOUND_BANK_SFX, SBK_ID_MENU);
		FESound(sample);
		LoadBankFromLump(SOUND_BANK_SFX, SBK_ID_SFX);
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
