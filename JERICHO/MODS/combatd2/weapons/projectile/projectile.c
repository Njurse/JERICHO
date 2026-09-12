// weapons/projectile/projectile.c — PROJECTILE class pool.
//
// A PROJECTILE weapon is a moving object (drawn as a streak + nose) that
// flies until it hits a car, the ground, or its max range, then explodes
// (AOE blast FX + splash damage). Used by the missile.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "dr2roads.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

#define CD2_MAX_PROJECTILES	16

typedef struct CD2_PROJECTILE
{
	int active;
	const CD2_WEAPON_DEF* def;
	const CAR_DATA* owner;
	VECTOR pos;	// y-up world
	VECTOR prev;	// streak tail
	VECTOR vel;	// world units / frame
	int travelled;
} CD2_PROJECTILE;

static CD2_PROJECTILE gProj[CD2_MAX_PROJECTILES];

void cd2ProjectileReset(void)
{
	int i;

	for (i = 0; i < CD2_MAX_PROJECTILES; i++)
		gProj[i].active = 0;
}

void cd2ProjectileSpawn(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
			const VECTOR* from, const VECTOR* vel)
{
	int i;

	for (i = 0; i < CD2_MAX_PROJECTILES; i++)
	{
		CD2_PROJECTILE* p = &gProj[i];

		if (p->active)
			continue;

		p->active = 1;
		p->def = def;
		p->owner = shooter;
		p->pos = *from;
		p->prev = *from;
		p->vel = *vel;
		p->travelled = 0;
		return;
	}
}

static void cd2ProjectileExplode(CD2_PROJECTILE* p)
{
	cd2AoeBlast(&p->pos, p->def->splashRadius, p->def->splashDamage,
		p->def->explosionEffect, p->owner);
	p->active = 0;
}

void cd2ProjectileStep(void)
{
	int i;

	for (i = 0; i < CD2_MAX_PROJECTILES; i++)
	{
		CD2_PROJECTILE* p = &gProj[i];
		int j, gh;

		if (!p->active)
			continue;

		p->prev = p->pos;
		p->pos.vx += p->vel.vx;
		p->pos.vy += p->vel.vy;
		p->pos.vz += p->vel.vz;
		p->travelled += p->def->speed;

		// car hit (skip the shooter): full direct damage, then explode
		for (j = 0; j < MAX_CARS; j++)
		{
			CAR_DATA* cp = &car_data[j];

			if (cp == p->owner || cp->controlType == 0 ||
			    cp->ap.carCos == NULL)
				continue;

			if (cd2WpnPointInCar(cp, &p->pos))
			{
				cd2WpnDamageCar(cp, &p->pos, p->def->damage);
				cd2AoeBlast(&p->pos, p->def->splashRadius,
					p->def->splashDamage, p->def->explosionEffect, cp);
				p->active = 0;
				break;
			}
		}

		if (!p->active)
			continue;

		// ground hit
		gh = MapHeight(&p->pos);
		if (gh != 0 && p->pos.vy <= gh + 24)
		{
			cd2ProjectileExplode(p);
			continue;
		}

		// max range
		if (p->travelled >= p->def->range)
			cd2ProjectileExplode(p);
	}
}

void cd2ProjectileDraw(void)
{
	int i;

	for (i = 0; i < CD2_MAX_PROJECTILES; i++)
	{
		CD2_PROJECTILE* p = &gProj[i];
		VECTOR ahead;
		int speed;

		if (!p->active)
			continue;

		cd2WpnLine(&p->prev, &p->pos, p->def->colR, p->def->colG, p->def->colB);

		speed = (p->def->speed > 0) ? p->def->speed : 1;
		ahead.vx = p->pos.vx + (int)(((long long)p->vel.vx * 12) / speed);
		ahead.vy = p->pos.vy + (int)(((long long)p->vel.vy * 12) / speed);
		ahead.vz = p->pos.vz + (int)(((long long)p->vel.vz * 12) / speed);

		cd2WpnLine(&p->pos, &ahead, 255, 220, 140);
	}
}
