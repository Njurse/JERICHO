// weapons/shotgun/shotgun.c — the SHOTGUN (CD2_WCLS_SHOTGUN).
//
// One trigger = one blast of pellets fired SIMULTANEOUSLY from BOTH fenders:
// the LEFT and RIGHT fender muzzles alternate, each pellet jittered across a
// cone (pelletSpread) and biased outward per fender (pelletFanout) so the two
// barrels visibly fan apart. Up close every pellet lands (damage is PER PELLET
// and the range is short); at range they scatter and most miss.
//
// The pellets are RAYCAST-class particles - the same pool the machine gun uses
// (weapons/raycast/raycast.c, cd2RaycastScatter) - so they travel, self-test a
// hit each frame and draw a tracer, and the class already handles car/ground/
// scenery impacts. This def only declares the numbers and the fire entry.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "system.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

#include <string.h>

static int gShotgunChannel = -1;	// voice reserved for the blast

static void cd2ShotgunFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR muzzle;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	// the whole blast: pellets from both fenders, spread across the cone
	cd2RaycastScatter(&cd2WdefShotgun, cp,
		cd2WdefShotgun.pelletCount,
		cd2WdefShotgun.pelletSpread,
		cd2WdefShotgun.pelletFanout);

	if (gShotgunChannel < 0)
	{
		// ONE voice for this sound, held only while the engine keeps its own
		// reserve (cd2TakeVoice / jer_sound_lock); a busy field still yields one.
		gShotgunChannel = cd2TakeVoice();

		if (gCd2Cfg.debugLog)
			printInfo("[cainescrossfire] shotgun sound: channel=%d locked\n", gShotgunChannel);
	}

	cd2WpnShotMuzzle(&cd2WdefShotgun, cp, 0, &muzzle);	// driver's window, for the sound position

	// a low boom
	Start3DSoundVolPitch(gShotgunChannel, SOUND_BANK_SFX, 6,
		muzzle.vx, muzzle.vy, muzzle.vz, -3000, 4096 - 1280);
}

static CD2_WEAPON_DEF cd2MakeShotgunDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SHOTGUN;
	d.name = "SHOTGUN";
	d.cls = CD2_WCLS_SHOTGUN;

	d.isBase = 0;
	d.hidden = 0;
	d.pickupEnabled = 1;
	d.pickupAmmo = 4;

	d.maxAmmo = 8;
	d.fireInterval = 27;
	d.refireCooldown = 65;	// ~2.2s between blasts

	d.damage = 190;		// PER PELLET (10 pellets landed up close = ~1900)
	d.speed = 2200;		// fast pellets
	d.range = 2600;		// SHORT: a shotgun, not a rifle
	d.life = 0;

	d.splashRadius = 0;
	d.splashDamage = 0;
	d.explosionEffect = LITTLE_BANG;

	d.homing = 0;
	d.collideScenery = 1;

	d.fireCone = 500;

	d.leanOut = 1;		// the driver leans out of their window to fire

	d.colR = 255; d.colG = 200; d.colB = 90;	// hot buckshot orange

	// -------------------------------------------------------------------
	// 10 pellets, alternating fenders, +/- 800 fixed-point spread (~11 deg)
	// plus a 260 outward fan per fender (~3.6 deg) so the two barrels split.
	// -------------------------------------------------------------------
	d.pelletCount = 10;
	d.pelletSpread = 800;
	d.pelletFanout = 260;

	d.fire = cd2ShotgunFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefShotgun = cd2MakeShotgunDef();
