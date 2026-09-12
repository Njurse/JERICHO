// weapons/projectile/homing.c — the HOMING MISSILE (purple).
//
// Same PROJECTILE class as the missile (the shared pool does the travelling,
// the hit tests and the blast - see projectile.c), but this one steers itself.
// It trades damage for tracking: 420 direct against the missile's 900.
//
// It is also the reason CD2_WEAPON_DEF gained a per-weapon fireCone. A weapon
// that only travels where it is pointed has to be launched close to on-axis,
// but one that homes can be fired through a heading error the other could
// never hit - so the AI's firing tolerance belongs to the weapon, not to the
// AI. Wide cone here, tight one on the plain missile.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

#include <string.h>

static int gHomingChannel = -1;	// voice reserved for the launch sound

static void cd2HomingFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR muzzle;
	VECTOR dir;
	VECTOR vel;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	cd2WpnMuzzle(cp, 0, &muzzle);	// centred (side 0)
	cd2WpnForward(cp, &dir);

	vel.vx = (int)(((long long)dir.vx * cd2WdefHoming.speed) >> 12);
	vel.vy = (int)(((long long)dir.vy * cd2WdefHoming.speed) >> 12);
	vel.vz = (int)(((long long)dir.vz * cd2WdefHoming.speed) >> 12);

	cd2ProjectileSpawn(&cd2WdefHoming, cp, &muzzle, &vel, &dir);

	if (gHomingChannel < 0)
	{
		// GetFreeChannel(1), NOT GetFreeChannel(): sound.h declares the
		// parameter as 'int force = 1', a C++ default argument, and our module
		// is C - so the default never applies and must be passed explicitly.
		gHomingChannel = GetFreeChannel(1);
		LockChannel(gHomingChannel);

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] homing sound: channel=%d locked\n", gHomingChannel);
	}

	// a rising whine, distinct from the missile's launch
	Start3DSoundVolPitch(gHomingChannel, SOUND_BANK_SFX, 5,
		muzzle.vx, muzzle.vy, muzzle.vz, -1800, 4096 + 3072);
}

static CD2_WEAPON_DEF cd2MakeHomingDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_HOMING;
	d.name = "SEEKER";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isBase = 0;
	d.hidden = 0;
	d.pickupEnabled = 1;
	d.pickupAmmo = 8;

	d.maxAmmo = 16;
	d.fireInterval = 40;
	d.refireCooldown = 110;	// 1.8s minimum between seekers

	d.damage = 420;		// deliberately well under the missile's 900
	d.speed = 1150;		// slower than the missile: it turns, so it can afford to
	d.range = 14000;
	d.life = 0;

	d.radius = 0;
	d.splashRadius = 380;
	d.splashDamage = 320;
	d.explosionEffect = BIG_BANG;

	d.homing = 1;		// the whole point of it
	d.collideScenery = 1;

	// Wide: it steers into the target, so a big heading error still connects.
	d.fireCone = 1000;

	d.colR = 200; d.colG = 60; d.colB = 255;	// purple

	d.fire = cd2HomingFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefHoming = cd2MakeHomingDef();
