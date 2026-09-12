// weapons/projectile/zoomy.c — ZOOMY MISSILES (projectile weapon).
//
// A primary weapon that fires a fast BURST of 10 small missiles a few frames
// apart. Each one homes only very weakly (def->homingRate is tiny), so the
// volley is really a spray you aim by driving — the shots drift toward the
// target but never chase it down. On their own each hit barely scratches a
// car; the payoff is the VOLLEY: if all ten land on cars, the shot that lands
// last deals a big bonus hit with a hard shove.
//
// The staggered launch + the "all ten landed" bookkeeping live in the
// projectile pool (weapons/projectile/projectile.c: cd2ProjectileBurst and the
// volley group); this def just declares the numbers and the fire entry.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/fx/fx.h"

#include <string.h>

static int gZoomyChannel = -1;	// voice reserved for the launch sound

static void cd2ZoomyFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	// one trigger = one volley; the pool staggers the 10 shots and re-aims
	// each from the car, so the burst trails the launcher
	cd2ProjectileBurst(&cd2WdefZoomy, cp, cd2WdefZoomy.burstCount,
		cd2WdefZoomy.burstInterval);

	if (gZoomyChannel < 0)
	{
		// GetFreeChannel(1), not GetFreeChannel(): sound.h declares the
		// parameter as 'int force = 1' (a C++ default argument) and this
		// module is C, so the default never applies and must be passed.
		gZoomyChannel = GetFreeChannel(1);
		LockChannel(gZoomyChannel);

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] zoomy sound: channel=%d locked\n", gZoomyChannel);
	}

	// a high, rattly launch (samples 5 is the MG/SFX default; pitch it up)
	Start3DSoundVolPitch(gZoomyChannel, SOUND_BANK_SFX, 5,
		cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2],
		-2400, 4096 + 2048);
}

static CD2_WEAPON_DEF cd2MakeZoomyDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_ZOOMY;
	d.name = "ZOOMY";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isBase = 0;
	d.hidden = 0;
	d.pickupEnabled = 1;
	d.pickupAmmo = 4;

	d.maxAmmo = 8;
	d.fireInterval = 60;
	d.refireCooldown = 160;	// 2.7s — the volley is the weapon

	d.damage = 40;		// very small on its own
	d.speed = 1600;		// fast / zoomy
	d.range = 14000;	// must reach the AI's max launch range
	d.life = 0;

	d.splashRadius = 130;	// small blast
	d.splashDamage = 25;
	d.explosionEffect = LITTLE_BANG;
	d.impactFx = CD2_FX_ZOOMY;	// small, cool-blue

	d.homing = 1;		// it DOES steer... barely
	d.homingRate = 40;	// extremely weak (the seeker uses 340)
	d.collideScenery = 1;

	d.fireCone = 350;	// it barely homes, so the AI must aim it (the plain
				// missile uses the default cone)

	d.colR = 140; d.colG = 230; d.colB = 255;	// pale cyan streaks

	// -------------------------------------------------------------------
	// Volley: 10 shots, 3 frames apart. Finish the set and the last one to
	// land delivers a big bonus hit + shove.
	// -------------------------------------------------------------------
	d.burstCount = 10;
	d.burstInterval = 6;	// ~0.6s from first shot to last
	d.volleyBonusDamage = 900;
	d.volleyBonusKnock = 1200;

	d.fire = cd2ZoomyFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefZoomy = cd2MakeZoomyDef();
