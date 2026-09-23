// weapons/projectile/smg.c — SMG (projectile weapon, burst sidearm).
//
// One trigger = one BURST of 6 shots, a couple of frames apart, then a pause of
// about 1.1s before the next volley. Each shot homes only very weakly, so the
// burst is a fast, close-range spray you aim by driving rather than a guided
// weapon - the SMG's job is to put a lot of small hits on a car quickly and to
// look busy doing it.
//
// The staggered launch lives in the projectile pool
// (weapons/projectile/projectile.c: cd2ProjectileBurst), the same machinery the
// ZOOMY volley uses; this def declares the numbers and the fire entry. Because
// it is a PROJECTILE-class weapon it inherits the pool's homing, so no new
// steering code is needed for the "weak homing" the SMG wants.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/fx/fx.h"

#include <string.h>

static int gSmgChannel = -1;		// voice reserved for the burst

static void cd2SmgFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	// one trigger = one burst; the pool staggers the 6 shots and re-aims each
	// from the car, so the burst trails the launcher
	cd2ProjectileBurst(&cd2WdefSmg, cp, cd2WdefSmg.burstCount,
		cd2WdefSmg.burstInterval);

	if (gSmgChannel < 0)
	{
		// ONE voice for this sound, held only while the engine keeps its own
		// reserve (cd2TakeVoice / jer_sound_lock); a busy field still yields one.
		gSmgChannel = cd2TakeVoice();

		if (gCd2Cfg.debugLog)
			printInfo("[cainescrossfire] smg sound: channel=%d locked\n", gSmgChannel);
	}

	// a short, dry chatter (MG sample, pitched up hard)
	Start3DSoundVolPitch(gSmgChannel, SOUND_BANK_SFX, 5,
		cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2],
		-2600, 4096 + 3072);
}

static CD2_WEAPON_DEF cd2MakeSmgDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SMG;
	d.name = "SMG";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isBase = 0;
	d.hidden = 0;
	d.pickupEnabled = 1;
	d.pickupAmmo = 4;

	d.maxAmmo = 8;
	d.fireInterval = 3;	// fast while the burst is running
	d.refireCooldown = 16;	// ~0.5s at 30fps: one volley at a time, then a
				// beat to aim - this is the SMG's whole rhythm

	d.damage = 36;		// small per shot; six of them is the point
	d.speed = 1500;		// fast enough to read as a bullet
	d.range = 12000;	// must reach the AI's max launch range
	d.life = 0;

	d.splashRadius = 90;	// a light pop, not a blast
	d.splashDamage = 24;
	d.explosionEffect = LITTLE_BANG;
	d.impactFx = CD2_FX_ZOOMY;	// small, cool-blue: a bullet strike

	// weak homing - the burst drifts onto a car rather than chasing it down
	d.homing = 1;
	d.homingRate = 60;
	d.collideScenery = 1;

	d.fireCone = 420;

	// the driver leans out of his window for the burst; the gunner stays in
	d.leanOut = 1;

	d.colR = 255; d.colG = 235; d.colB = 160;	// hot tracer yellow

	// -------------------------------------------------------------------
	// The burst: 6 shots, 2 frames apart (a ~0.2s chatter), then the
	// refireCooldown above holds the next volley off.
	// -------------------------------------------------------------------
	d.burstCount = 6;
	d.burstInterval = 2;
	d.volleyBonusDamage = 0;	// no all-landed bonus: this is a spray
	d.volleyBonusKnock = 0;

	d.fire = cd2SmgFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSmg = cd2MakeSmgDef();
