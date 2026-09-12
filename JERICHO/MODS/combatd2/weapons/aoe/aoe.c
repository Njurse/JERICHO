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
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

void cd2AoeBlast(const VECTOR* at, int radius, int damage, int effect,
		 const CAR_DATA* skip)
{
	VECTOR blast;
	int i;

	blast.vx = at->vx;
	blast.vy = at->vy;
	blast.vz = at->vz;
	AddExplosion(blast, effect);

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

		cd2WpnDamageCar(cp, at, dmg);
	}
}
