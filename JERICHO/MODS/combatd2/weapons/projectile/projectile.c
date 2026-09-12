// weapons/projectile/projectile.c — PROJECTILE class pool.
//
// A PROJECTILE weapon is a moving object that flies until it hits a car, the
// ground, or its max range, then explodes (AOE blast FX + splash damage).
//
// Presentation: when a model name is configured (CD2_CONFIG.missileModel,
// default "BOMB") the projectile is drawn as a real game MODEL oriented along
// its travel direction and scaled by CD2_CONFIG.missileScale, with the
// prev->pos streak kept as an exhaust flame. If no model is available it
// falls back to an enlarged line "missile" (body + nose + fins).

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "camera.h"
#include "convert.h"
#include "draw.h"
#include "models.h"
#include "bomberman.h"
#include "drivinggames.h"
#include "dr2math.h"
#include "dr2roads.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

#include <string.h>

#define CD2_MAX_PROJECTILES	16

typedef struct CD2_PROJECTILE
{
	int active;
	const CD2_WEAPON_DEF* def;
	const CAR_DATA* owner;
	VECTOR pos;	// y-up world
	VECTOR prev;	// streak tail
	VECTOR vel;	// world units / frame
	VECTOR dir;	// unit*4096 travel direction (model orientation)
	int travelled;
} CD2_PROJECTILE;

static CD2_PROJECTILE gProj[CD2_MAX_PROJECTILES];

// model lookup cache (reset per level; resolved after the level's models are
// loaded, at first draw)
static MODEL* sMissileModel;
static int sMissileModelTried;

static int cd2Isqrt(int v)
{
	int r = 0;
	int bit = 1 << 30;

	if (v <= 0)
		return 0;

	while (bit > v)
		bit >>= 2;

	while (bit != 0)
	{
		if (v >= r + bit)
		{
			v -= r + bit;
			r = (r >> 1) + bit;
		}
		else
			r >>= 1;

		bit >>= 2;
	}

	return r;
}

static MODEL* cd2MissileModel(void)
{
	if (!sMissileModelTried)
	{
		const char* nm = gCd2Cfg.missileModel;

		sMissileModel = NULL;

		if (nm != NULL && nm[0] != 0)
		{
			// Prefer the models the engine already resolved at level load:
			// FindModelPtrWithName reads a level-name buffer that is junk by
			// gameplay time, so it usually fails there.
			if (strcmp(nm, "BOMB") == 0)
				sMissileModel = gBombModel;
			else if (strcmp(nm, "GREENCONE") == 0)
				sMissileModel = gTrailblazerConeModel;
			else
				sMissileModel = FindModelPtrWithName((char*)nm);
		}

		sMissileModelTried = 1;
	}

	return sMissileModel;
}

int cd2ProjectileModelValid(void)
{
	return (cd2MissileModel() != NULL) ? 1 : 0;
}

void cd2ProjectileReset(void)
{
	int i;

	for (i = 0; i < CD2_MAX_PROJECTILES; i++)
		gProj[i].active = 0;

	sMissileModel = NULL;
	sMissileModelTried = 0;
}

void cd2ProjectileSpawn(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
			const VECTOR* from, const VECTOR* vel, const VECTOR* dir)
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
		p->dir = *dir;
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

// Build the model matrix: +Z = travel, X/Y = an orthonormal right/up basis,
// scaled by `scale` (fixed point, 4096 = 1x).
static void cd2ProjBuildMatrix(const VECTOR* dir, int scale, MATRIX* m)
{
	int fx = dir->vx, fy = dir->vy, fz = dir->vz;
	int rx = fz, rz = -fx;	// up x fwd with up = (0,1,0)
	int rlen = cd2Isqrt(rx * rx + rz * rz);
	int ux, uy, uz;

	if (rlen < 1)
	{
		rx = ONE;	// fwd ~ vertical: pick any stable right
		rz = 0;
	}
	else
	{
		rx = (int)(((long long)rx * ONE) / rlen);
		rz = (int)(((long long)rz * ONE) / rlen);
	}

	// up = fwd x right (unit*4096; the 4096^2 product is shifted back)
	ux = (int)(((long long)fy * rz) >> 12);
	uy = (int)(((long long)(fz * rx - fx * rz)) >> 12);
	uz = (int)(((long long)(-fy * rx)) >> 12);

	// columns: X = right, Y = up, Z = fwd, all scaled
	m->m[0][0] = (short)(((long long)rx * scale) >> 12);
	m->m[1][0] = 0;
	m->m[2][0] = (short)(((long long)rz * scale) >> 12);

	m->m[0][1] = (short)(((long long)ux * scale) >> 12);
	m->m[1][1] = (short)(((long long)uy * scale) >> 12);
	m->m[2][1] = (short)(((long long)uz * scale) >> 12);

	m->m[0][2] = (short)(((long long)fx * scale) >> 12);
	m->m[1][2] = (short)(((long long)fy * scale) >> 12);
	m->m[2][2] = (short)(((long long)fz * scale) >> 12);
}

static void cd2ProjDrawModel(CD2_PROJECTILE* p)
{
	MATRIX mat;
	VECTOR pos;
	MODEL* model = cd2MissileModel();
	int dx, dz;

	if (model == NULL)
		return;

	// cheap distance cull (the missile is short lived; keeps far ones out)
	dx = camera_position.vx - p->pos.vx;
	dz = camera_position.vz - p->pos.vz;
	if (dx * dx + dz * dz > (9000 * 9000))
		return;

	cd2ProjBuildMatrix(&p->dir, gCd2Cfg.missileScale, &mat);

	pos.vx = p->pos.vx - camera_position.vx;
	pos.vy = p->pos.vy - camera_position.vy;
	pos.vz = p->pos.vz - camera_position.vz;

	gte_SetRotMatrix(&inv_camera_matrix);
	_MatrixRotate(&pos);

	RenderModel(model, &mat, &pos, 0, 0, 0, 0);
}

// Line fallback when no model is available: a larger body + fins + nose.
static void cd2ProjDrawLines(CD2_PROJECTILE* p)
{
	VECTOR tip, back, finA, finB;
	int len = 70;
	const VECTOR* d = &p->dir;

	tip.vx = p->pos.vx + (int)(((long long)d->vx * len * 2) >> 12);
	tip.vy = p->pos.vy + (int)(((long long)d->vy * len * 2) >> 12);
	tip.vz = p->pos.vz + (int)(((long long)d->vz * len * 2) >> 12);

	back.vx = p->pos.vx - (int)(((long long)d->vx * len) >> 12);
	back.vy = p->pos.vy - (int)(((long long)d->vy * len) >> 12);
	back.vz = p->pos.vz - (int)(((long long)d->vz * len) >> 12);

	cd2WpnLine(&back, &tip, p->def->colR, p->def->colG, p->def->colB);

	// tail fins (a small cross at the back)
	finA.vx = back.vx + (int)(((long long)d->vz * len) >> 12);
	finA.vy = back.vy;
	finA.vz = back.vz - (int)(((long long)d->vx * len) >> 12);
	finB.vx = back.vx - (int)(((long long)d->vz * len) >> 12);
	finB.vy = back.vy;
	finB.vz = back.vz + (int)(((long long)d->vx * len) >> 12);
	cd2WpnLine(&finA, &finB, 255, 220, 140);
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

		// exhaust streak (kept regardless of the model)
		cd2WpnLine(&p->prev, &p->pos, p->def->colR, p->def->colG, p->def->colB);

		if (cd2MissileModel() != NULL)
		{
			cd2ProjDrawModel(p);
		}
		else
		{
			cd2ProjDrawLines(p);
		}

		// bright nose dot
		speed = (p->def->speed > 0) ? p->def->speed : 1;
		ahead.vx = p->pos.vx + (int)(((long long)p->vel.vx * 12) / speed);
		ahead.vy = p->pos.vy + (int)(((long long)p->vel.vy * 12) / speed);
		ahead.vz = p->pos.vz + (int)(((long long)p->vel.vz * 12) / speed);

		cd2WpnLine(&p->pos, &ahead, 255, 230, 170);
	}
}
