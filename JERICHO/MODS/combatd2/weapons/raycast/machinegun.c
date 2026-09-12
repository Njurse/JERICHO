// weapons/raycast/machinegun.c — the MACHINE GUN (base raycast weapon).
//
// The always-available SIDEARM. It is not part of the weapon cycle: it is
// fired straight off the LEFT TRIGGER and never runs out. Each shot spawns a
// fast raycast particle from an alternating fender muzzle (the RAYCAST class
// pool does the travelling + hit test).

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

#include <string.h>

// alternate left/right fender each shot
static int gMgSide = 1;

static void cd2MgFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR muzzle;
	VECTOR dir;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	cd2WpnMuzzle(cp, gMgSide, &muzzle);
	cd2WpnForward(cp, &dir);

	cd2RaycastSpawn(&cd2WdefMG, cp, &muzzle, &dir);

	Start3DSoundVolPitch(-1, SOUND_BANK_SFX, 5,
		muzzle.vx, muzzle.vy, muzzle.vz, -2000, 4096 + 2048);

	gMgSide = -gMgSide;
}

static CD2_WEAPON_DEF cd2MakeMGDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_MG;
	d.name = "MG";
	d.cls = CD2_WCLS_RAYCAST;

	d.isBase = 1;		// always carried, never in the cycle
	d.hidden = 0;
	d.pickupEnabled = 1;
	d.pickupAmmo = 0;

	d.maxAmmo = 0;		// infinite
	d.fireInterval = 5;	// frames between shots (auto)
	d.refireCooldown = 5;	// minimum gap between refires

	d.damage = 390;
	d.speed = 150;		// world units/frame (fast particle)
	d.range = 6400;
	d.life = 2;

	d.explosionEffect = LITTLE_BANG;
	d.collideScenery = 1;	// bullets stop on walls/buildings
	d.colR = 255; d.colG = 235; d.colB = 110;

	d.fire = cd2MgFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefMG = cd2MakeMGDef();
