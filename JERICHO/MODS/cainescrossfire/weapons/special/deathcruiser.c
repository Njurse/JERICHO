// weapons/special/deathcruiser.c — DEATH CRUISER's special (PLACEHOLDER).
//
// The car is in the roster; its special is not designed yet. Firing it plays
// the weapon-pickup cue at the car and nothing else - no projectile, no damage,
// no status. That is deliberate: it keeps the profile, the HUD and the
// per-car ammo path exercised while the real weapon is still being made.
//
// Replace cd2DeathcruiserFire and the def's class/ammo/recharge when the real
// special lands; the profile only names "special_deathcruiser".

#include "driver2.h"
#include "cars.h"
#include "jericho.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/special/special.h"

#include <string.h>

static void cd2DeathcruiserFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	cd2WpnSndPlay(CD2_SND_PICKUP_WEAPON,
		cp->hd.where.t[0], cp->hd.where.t[1] + 40, cp->hd.where.t[2],
		-1500, 4096);

	printInfo("[cainescrossfire] deathcruiser: placeholder special fired\n");
}

void cd2SpecialDeathcruiserRegister(JERICHO_CONTEXT* ctx)
{
	(void)ctx;
}

static CD2_WEAPON_DEF cd2MakeDeathcruiserDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_DEATHCRUISER;
	d.name = "special_deathcruiser";
	d.displayName = "Death Cruiser";
	d.cls = CD2_WCLS_AOE;

	d.isSpecial = 1;

	d.leanOut = 0;			// the placeholder has no crew pose

	d.maxAmmo = 5;			// placeholder: enough to hear the cue a few times
	d.fireInterval = 30;
	d.refireCooldown = 120;		// placeholder: 4 s

	d.damage = 0;
	d.speed = 0;
	d.range = 0;
	d.life = 0;

	d.colR = 240; d.colG = 120; d.colB = 80;

	d.fire = cd2DeathcruiserFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialDeathcruiser = cd2MakeDeathcruiserDef();
