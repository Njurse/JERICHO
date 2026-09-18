// weapons/shotgun/special_jericho.c — SPECIAL JERICHO (CD2_WCLS_SHOTGUN).
//
// The demo weapon: the SHOTGUN, doubled - twice the pellets, twice the damage
// per pellet - fired from BOTH WINDOWS AT ONCE. It exists to show the mounted
// crew doing simultaneous work: both crew members lean out (leanOut 3), and the
// blast alternates its pellets between the driver's and the gunner's window, so
// the two of them are visibly firing together.
//
// Same mechanics as the shotgun (weapons/shotgun/shotgun.c): pellets are
// RAYCAST-class particles (weapons/raycast/raycast.c, cd2RaycastScatter), fired
// simultaneously, each jittered across the cone and biased outward per fender.
// Only the numbers and the muzzle rule differ, so both windows really do fire -
// see cd2WpnShotMuzzle, which honours the pellet's own side whenever a weapon
// leans BOTH ways.

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

static int gJerichoChannel = -1;	// voice reserved for the blast

static void cd2SpecialJerichoFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR muzzle;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	// the doubled blast, still alternating the two windows
	cd2RaycastScatter(&cd2WdefSpecialJericho, cp,
		cd2WdefSpecialJericho.pelletCount,
		cd2WdefSpecialJericho.pelletSpread,
		cd2WdefSpecialJericho.pelletFanout);

	if (gJerichoChannel < 0)
	{
		gJerichoChannel = GetFreeChannel(1);
		LockChannel(gJerichoChannel);

		if (gCd2Cfg.debugLog)
			printInfo("[cainescrossfire] special_jericho sound: channel=%d locked\n", gJerichoChannel);
	}

	cd2WpnShotMuzzle(&cd2WdefSpecialJericho, cp, 0, &muzzle);

	// the same low boom, pitched down for the heavier gun
	Start3DSoundVolPitch(gJerichoChannel, SOUND_BANK_SFX, 6,
		muzzle.vx, muzzle.vy, muzzle.vz, -3000, 4096 - 1792);
}

static CD2_WEAPON_DEF cd2MakeSpecialJerichoDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_JERICHO;
	d.name = "SPECIAL";
	d.cls = CD2_WCLS_SHOTGUN;

	d.isBase = 0;
	d.hidden = 0;
	d.pickupEnabled = 1;
	d.pickupAmmo = 4;

	d.maxAmmo = 8;
	d.fireInterval = 55;
	d.refireCooldown = 130;	// ~2.2s between blasts, like the shotgun

	d.damage = 190;		// DOUBLE the shotgun's 95, and it is per pellet:
				// 20 pellets landed up close is ~3800 - it deletes a car
	d.speed = 2200;
	d.range = 2600;		// still a shotgun: short
	d.life = 0;

	d.splashRadius = 0;
	d.splashDamage = 0;
	d.explosionEffect = BIG_BANG;	// a heavier bang than the shotgun's

	d.homing = 0;
	d.collideScenery = 1;

	d.fireCone = 500;

	// -------------------------------------------------------------------
	// BOTH crew lean out: this is the weapon that demos the two of them
	// working at once. Bit 0 = driver (left window), bit 1 = gunner (right).
	// -------------------------------------------------------------------
	d.leanOut = 3;

	d.colR = 255; d.colG = 140; d.colB = 255;	// violet-white, to read as
							// something special

	// -------------------------------------------------------------------
	// 20 pellets (2x the shotgun's 10), same cone and the same outward fan,
	// so it patterns like the shotgun only twice as dense.
	// -------------------------------------------------------------------
	d.pelletCount = 20;
	d.pelletSpread = 800;
	d.pelletFanout = 260;

	d.fire = cd2SpecialJerichoFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialJericho = cd2MakeSpecialJerichoDef();
