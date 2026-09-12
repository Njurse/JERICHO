// weapons/projectile/missile.c — the MISSILE (projectile weapon).
//
// A primary weapon: fired straight ahead from the car's nose, it flies as a
// real projectile and detonates with an explosion where it lands (car hit,
// ground, or max range). The PROJECTILE class pool simulates + draws it; the
// AOE helper does the blast (see weapons/aoe/aoe.c).

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

#include <string.h>

static int gMissileChannel = -1;	// voice reserved for the missile launch

static void cd2MissileFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR muzzle;
	VECTOR dir;
	VECTOR vel;
	int speed;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	speed = cd2WdefMissile.speed;

	cd2WpnMuzzle(cp, 0, &muzzle);	// centred (side 0)
	cd2WpnForward(cp, &dir);

	vel.vx = (int)(((long long)dir.vx * speed) >> 12);
	vel.vy = (int)(((long long)dir.vy * speed) >> 12);
	vel.vz = (int)(((long long)dir.vz * speed) >> 12);

	cd2ProjectileSpawn(&cd2WdefMissile, cp, &muzzle, &vel, &dir);

	// distinct launch sound (configurable sample; default 12 = a punchier
	// SFX than the machine gun's 5)
	if (gMissileChannel < 0)
	{
		// GetFreeChannel(1), NOT GetFreeChannel(): sound.h declares it as
		// 'int force = 1', a C++ default argument. Our module is C, so the
		// default never applies and force arrives as garbage - which is why
		// this returned -1 (no sound) whenever no voice happened to be idle.
		gMissileChannel = GetFreeChannel(1);
		LockChannel(gMissileChannel);

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] missile sound: channel=%d locked sample=%d\n",
				gMissileChannel, gCd2Cfg.missileSound);
	}

	// SOUND_BANK_SFX, not MISSION: sample 29 in the mission bank is not
	// resident, so the launch was silent. The engine's SFX bank is always
	// loaded; 6 is its heavy-impact sample, distinct from the MG's 5.
	Start3DSoundVolPitch(gMissileChannel, SOUND_BANK_SFX, gCd2Cfg.missileSound,
		muzzle.vx, muzzle.vy, muzzle.vz, -1600, 4096 + 1024);
}

static CD2_WEAPON_DEF cd2MakeMissileDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_MISSILE;
	d.name = "MISSILE";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isBase = 0;
	d.hidden = 0;
	d.pickupEnabled = 1;
	d.pickupAmmo = 10;

	d.maxAmmo = 20;
	d.fireInterval = 30;
	d.refireCooldown = 65;	// 1.05s minimum between missiles

	d.damage = 900;		// direct hit
	d.speed = 1485;		// world units/frame
	d.range = 10400;
	d.life = 0;

	d.splashRadius = 500;
	d.splashDamage = 800;
	// BIG_BANG, not LITTLE_BANG: a little bang is hscale 1024 for ~21 frames,
	// which at combat speed reads as nothing at all. This is the visible one.
	d.explosionEffect = BIG_BANG;

	d.homing = 0;		// RESERVED (no logic this turn)
	d.collideScenery = 1;	// missiles detonate on walls/buildings

	d.colR = 255; d.colG = 130; d.colB = 40;

	d.fire = cd2MissileFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefMissile = cd2MakeMissileDef();
