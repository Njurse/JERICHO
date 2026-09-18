// weapons/aoe/aoe.c — AOE class helper: explosion FX + radial damage.
//
// Shared by the PROJECTILE (missile impact) and DROP (mine trigger) classes.
// Spawns the engine explosion effect at the blast point and applies falling
// damage to every car in radius (the directly-hit car is normally passed as
// `skip` because it already took the full direct damage).

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "job_fx.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/fx/fx.h"

void cd2AoeBlast(const VECTOR* at, int radius, int damage, int effect,
		 const CAR_DATA* skip, const CAR_DATA* owner)
{
	VECTOR blast;
	int i;

	blast.vx = at->vx;
	blast.vy = at->vy;
	blast.vz = at->vz;

	// AddExplosion fills the next free slot in job_fx's explosion[] list, which
	// HandleExplosion ages (main.c) and DrawAllExplosions renders. explosion[]
	// is exported, so verify from this side that a valid, in-bounds slot was
	// actually armed rather than assuming it.
	if (effect >= CD2_FX_BASE)
		cd2FxSpawn(&blast, effect);	// a themed CD2_FX_* profile
	else
		AddExplosion(blast, effect);	// a stock ExplosionType

	if (gCd2Cfg.debugLog)
	{
		int i, live = 0, slot = -1, chk;

		for (i = 0; i < MAX_EXPLOSION_OBJECTS; i++)
		{
			if (explosion[i].time != -1)
				live++;

			if (slot < 0 && explosion[i].time == 0 &&
			    explosion[i].pos.vx == blast.vx && explosion[i].pos.vz == blast.vz)
				slot = i;
		}

		chk = (slot >= 0) ? slot : 0;

		printInfo("[cainescrossfire] aoe blast (%d,%d,%d) r=%d dmg=%d fx=%d -> slot=%d live=%d/%d armed(type=%d speed=%d hscale=%d)\n",
			blast.vx, blast.vy, blast.vz, radius, damage, effect,
			slot, live, MAX_EXPLOSION_OBJECTS,
			explosion[chk].type, explosion[chk].speed, explosion[chk].hscale);
	}

	if (radius <= 0 || damage <= 0)
		return;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		int dx, dz, dist, dmg;

		if (cp == skip || cp->controlType == 0 || cp->ap.carCos == NULL)
			continue;

		dx = at->vx - cp->hd.where.t[0];
		dz = at->vz - cp->hd.where.t[2];
		dist = (ABS(dx) + ABS(dz)) / 2;

		if (dist > radius)
			continue;

		if (dist < 1)
			dist = 1;

		dmg = damage * (radius - dist) / radius;
		if (dmg < 1)
			continue;

		// `owner` (not `skip`) carries the attacker for kill attribution.
		cd2WpnDamageCar(cp, at, dmg, owner);
	}
}
