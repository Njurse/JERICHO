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
	Start3DSoundVolPitch(-1, SOUND_BANK_SFX, gCd2Cfg.missileSound,
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

	d.damage = 900;		// direct hit
	d.speed = 485;		// world units/frame
	d.range = 10400;
	d.life = 0;

	d.splashRadius = 500;
	d.splashDamage = 800;
	d.explosionEffect = LITTLE_BANG;

	d.homing = 0;		// RESERVED (no logic this turn)
	d.collideScenery = 1;	// missiles detonate on walls/buildings

	d.colR = 255; d.colG = 130; d.colB = 40;

	d.fire = cd2MissileFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefMissile = cd2MakeMissileDef();
