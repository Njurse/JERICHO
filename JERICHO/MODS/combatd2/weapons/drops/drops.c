// weapons/drops/drops.c — DROP class pool.
//
// A DROP weapon is an object that is released into the world, falls to the
// ground, and then ARMS where it lands. Once armed it watches for any car
// that is NOT its caster coming within def->radius, and detonates (direct
// damage on the triggering car + an AOE blast). Drawn as a small cone/pyramid
// placeholder marker (bright = armed, with a blink).

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "dr2roads.h"
#include "objcoll.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

#define CD2_MAX_DROPS		24
#define CD2_DROP_GRAVITY	48	// vertical accel (world units/frame^2)

typedef struct CD2_DROP
{
	int active;
	const CD2_WEAPON_DEF* def;
	const CAR_DATA* owner;	// caster: never triggers its own drop
	VECTOR pos;	// y-up world
	VECTOR vel;	// world units / frame
	int startY;	// spawn height (fallback floor)
	int armed;	// 0 = still falling, 1 = armed on the ground
	int bobT;	// blink phase
} CD2_DROP;

static CD2_DROP gDrops[CD2_MAX_DROPS];

void cd2DropReset(void)
{
	int i;

	for (i = 0; i < CD2_MAX_DROPS; i++)
		gDrops[i].active = 0;
}

void cd2DropSpawn(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
		  const VECTOR* from, const VECTOR* vel)
{
	int i;

	for (i = 0; i < CD2_MAX_DROPS; i++)
	{
		CD2_DROP* d = &gDrops[i];

		if (d->active)
			continue;

		d->active = 1;
		d->def = def;
		d->owner = shooter;
		d->pos = *from;
		d->vel = *vel;
		d->startY = from->vy;
		d->armed = 0;
		d->bobT = 0;
		return;
	}
}

// small cone/pyramid placeholder (base triangle + apex)
static void cd2DropCone(const VECTOR* p, int r, int g, int b)
{
	int h = 60, w = 34;
	VECTOR a, bb, c, apex;

	a.vx = p->vx - w; a.vy = p->vy; a.vz = p->vz - w;
	bb.vx = p->vx + w; bb.vy = p->vy; bb.vz = p->vz - w;
	c.vx = p->vx; c.vy = p->vy; c.vz = p->vz + w;
	apex.vx = p->vx; apex.vy = p->vy + h; apex.vz = p->vz;

	cd2WpnLine(&a, &bb, r, g, b);
	cd2WpnLine(&bb, &c, r, g, b);
	cd2WpnLine(&c, &a, r, g, b);
	cd2WpnLine(&a, &apex, r, g, b);
	cd2WpnLine(&bb, &apex, r, g, b);
	cd2WpnLine(&c, &apex, r, g, b);
}

void cd2DropStep(void)
{
	int i;

	for (i = 0; i < CD2_MAX_DROPS; i++)
	{
		CD2_DROP* d = &gDrops[i];

		if (!d->active)
			continue;

		if (!d->armed)
		{
			VECTOR prev;
			int gh, floorY;

			prev = d->pos;

			d->vel.vy -= CD2_DROP_GRAVITY;
			d->pos.vx += d->vel.vx;
			d->pos.vy += d->vel.vy;
			d->pos.vz += d->vel.vz;

			// scenery collision: land (arm) on a wall/roof instead of
			// falling through it
			if (d->def->collideScenery && lineClear(&prev, &d->pos) == 0)
			{
				d->vel.vx = d->vel.vy = d->vel.vz = 0;
				d->armed = 1;
				continue;
			}

			gh = MapHeight(&d->pos);
			floorY = (gh != 0) ? (gh + 8) : (d->startY - 400);

			if (d->pos.vy <= floorY)
			{
				d->pos.vy = floorY;
				d->vel.vx = d->vel.vy = d->vel.vz = 0;
				d->armed = 1;
			}
			continue;
		}

		d->bobT++;

		// proximity trigger: a NON-caster car inside def->radius
		{
			int j;
			long long r2 = (long long)d->def->radius * d->def->radius;

			for (j = 0; j < MAX_CARS; j++)
			{
				CAR_DATA* cp = &car_data[j];
				long long dx, dz;

				if (cp == d->owner || cp->controlType == 0 ||
				    cp->ap.carCos == NULL)
					continue;

				dx = d->pos.vx - cp->hd.where.t[0];
				dz = d->pos.vz - cp->hd.where.t[2];

				if (dx * dx + dz * dz > r2)
					continue;

				cd2WpnDamageCar(cp, &d->pos, d->def->damage, d->owner);
				{
					// blow the car outward from the mine (impulse direction)
					VECTOR kdir;

					kdir.vx = cp->hd.where.t[0] - d->pos.vx;
					kdir.vy = cp->hd.where.t[1] - d->pos.vy;
					kdir.vz = cp->hd.where.t[2] - d->pos.vz;
					cd2WpnKnock(cp, &d->pos, &kdir, d->def->damage);
				}
				cd2AoeBlast(&d->pos, d->def->splashRadius,
					d->def->splashDamage, CD2_WPN_FX(d->def), cp, d->owner);
				d->active = 0;
				break;
			}
		}
	}
}

void cd2DropDraw(void)
{
	int i;

	for (i = 0; i < CD2_MAX_DROPS; i++)
	{
		CD2_DROP* d = &gDrops[i];

		if (!d->active)
			continue;

		if (!d->armed)
			cd2DropCone(&d->pos, 160, 160, 160);
		else
		{
			// blink while armed so it reads as "live"
			int on = (d->bobT >> 3) & 1;
			cd2DropCone(&d->pos, on ? d->def->colR : (d->def->colR / 3),
				on ? d->def->colG : (d->def->colG / 3),
				on ? d->def->colB : (d->def->colB / 3));
		}
	}
}
