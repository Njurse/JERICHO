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
#include "objcoll.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/fx/fx.h"

#include <string.h>

// How hard a homing shot turns per frame, as a fraction of 1024 blended
// toward the target. Deliberately not 1024: it should follow, not snap.
#define CD2_PROJ_HOME_TURN	340

#define CD2_MAX_PROJECTILES	16
#define CD2_PROJ_SUBSTEP	48	// world units per sub-step (tunneling guard)
#define CD2_PROJ_MAX_SUBSTEP	12

// Volley tracking (zoomy missiles): a burst of shots that all share one
// group. If every member lands on a car, the last one to land deals a bonus.
#define CD2_MAX_BURSTS		4	// concurrent bursts
#define CD2_MAX_VOLLEYS		4	// concurrent volleys (burst -> volley 1:1)
#define CD2_VOLLEY_TTL		600	// frames a volley waits before it expires

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
	int volley;	// index into gVolley (-1 = an ordinary shot)
	int vseq;	// this shot's position in the burst
	int vgen;	// gVolley slot generation (guards against slot reuse)
} CD2_PROJECTILE;

static CD2_PROJECTILE gProj[CD2_MAX_PROJECTILES];

// A burst in progress: spawns one projectile every `interval` frames.
typedef struct CD2_PROJ_BURST
{
	int active;
	int framesLeft;		// frames until the next spawn
	int remaining;		// shots still to spawn
	int interval;
	int seq;		// next shot's sequence number
	int volley;		// gVolley slot the members join (-1 = none)
	const CD2_WEAPON_DEF* def;
	const CAR_DATA* shooter;
} CD2_PROJ_BURST;

static CD2_PROJ_BURST gBurst[CD2_MAX_BURSTS];

// A volley group: how many shots it has, how many have landed on a car.
typedef struct CD2_VOLLEY
{
	int active;
	int count;		// shots in the volley
	int landed;		// shots that hit a car
	int ttl;
	int gen;		// bumped on (re)use so stale members are ignored
} CD2_VOLLEY;

static CD2_VOLLEY gVolley[CD2_MAX_VOLLEYS];
static int sVolleyGen;		// monotonic volley generation

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

int cd2ProjectileThreat(const CAR_DATA* car, VECTOR* pos, VECTOR* vel)
{
	int i;

	for (i = 0; i < CD2_MAX_PROJECTILES; i++)
	{
		CD2_PROJECTILE* p = &gProj[i];
		int dx, dy, dz, dist2, closing;

		if (!p->active || p->owner == car)
			continue;

		dx = car->hd.where.t[0] - p->pos.vx;
		dy = car->hd.where.t[1] - p->pos.vy;
		dz = car->hd.where.t[2] - p->pos.vz;
		dist2 = dx * dx + dy * dy + dz * dz;

		if (dist2 > CD2_THREAT_RANGE * CD2_THREAT_RANGE)
			continue;

		closing = p->vel.vx * dx + p->vel.vy * dy + p->vel.vz * dz;
		if (closing <= 0)
			continue;	// heading away

		if (pos != NULL) *pos = p->pos;
		if (vel != NULL) *vel = p->vel;
		return 1;
	}

	return 0;
}

void cd2ProjectileReset(void)
{
	int i;

	for (i = 0; i < CD2_MAX_PROJECTILES; i++)
		gProj[i].active = 0;

	for (i = 0; i < CD2_MAX_BURSTS; i++)
		gBurst[i].active = 0;

	for (i = 0; i < CD2_MAX_VOLLEYS; i++)
		gVolley[i].active = 0;

	sMissileModel = NULL;
	sMissileModelTried = 0;
}

// core spawn: volley / vseq tag the shot as a member of a burst (-1 = none)
static void cd2ProjectileSpawnEx(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
			const VECTOR* from, const VECTOR* vel, const VECTOR* dir,
			int volley, int vseq)
{
	int i;
	VECTOR carVel;

	// inertial launch: inherit the shooter's velocity so the missile pulls
	// ahead of the car instead of lagging behind it
	if (shooter != NULL)
		cd2WpnCarVelocity(shooter, &carVel);
	else
		carVel.vx = carVel.vy = carVel.vz = 0;

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
		p->vel.vx = vel->vx + carVel.vx;
		p->vel.vy = vel->vy + carVel.vy;
		p->vel.vz = vel->vz + carVel.vz;
		p->dir = *dir;
		p->travelled = 0;
		p->volley = volley;
		p->vseq = vseq;
		p->vgen = (volley >= 0) ? gVolley[volley].gen : 0;
		return;
	}
}

void cd2ProjectileSpawn(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
			const VECTOR* from, const VECTOR* vel, const VECTOR* dir)
{
	cd2ProjectileSpawnEx(def, shooter, from, vel, dir, -1, 0);
}

// The impact: the parent blast always plays, then a barrage weapon (the
// cluster) also schedules its delayed burst. `carHit` is the car struck (NULL
// for a ground/scenery/range impact) and decides whether the burst sticks;
// `skip` is the car the blast should not damage (the shooter, or the car
// already hit directly).
static void cd2ProjectileImpact(CD2_PROJECTILE* p, const CAR_DATA* carHit, const CAR_DATA* skip)
{
	cd2AoeBlast(&p->pos, p->def->splashRadius, p->def->splashDamage,
		CD2_WPN_FX(p->def), skip);

	if (p->def->barrageCount > 0)
		cd2FxBarrage(&p->pos, (p->def->barrageStick ? carHit : NULL),
			p->def->barrageFx, p->def->barrageCount, p->def->barrageInterval,
			p->def->barrageJitter, p->def->barrageRadius,
			p->def->barrageDamage, p->owner);

	p->active = 0;
}

static void cd2ProjectileExplode(CD2_PROJECTILE* p)
{
	int v = p->volley;
	int vg = p->vgen;

	cd2ProjectileImpact(p, NULL, p->owner);

	// a volley member that did NOT hit a car means the "all landed" bonus can
	// never trigger, so retire the group
	if (v >= 0 && gVolley[v].active && gVolley[v].gen == vg)
		gVolley[v].active = 0;
}

// Integer square root (Newton on the bit pattern); the engine's own helpers
// live in files the weapon core does not otherwise pull in.
static int cd2ProjIsqrt(int v)
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
		{
			r >>= 1;
		}

		bit >>= 2;
	}

	return r;
}

// ---------------------------------------------------------------------------
// Homing: bend the shot toward the nearest car that is not its launcher.
// The turn is RATE LIMITED (CD2_PROJ_HOME_TURN), so a homing missile follows
// you rather than being unavoidable - you can still out-drive it. Direction
// changes, speed does not.
// ---------------------------------------------------------------------------
static void cd2ProjectileSeek(CD2_PROJECTILE* p)
{
	int i, best = -1;
	long long bestD = 0;
	int dx, dy, dz, dm, vm;
	int tx, ty, tz;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* o = &car_data[i];
		long long ox, oz, d2;

		if (o == p->owner || o->controlType == CONTROL_TYPE_NONE || o->ap.carCos == NULL)
			continue;

		ox = o->hd.where.t[0] - p->pos.vx;
		oz = o->hd.where.t[2] - p->pos.vz;
		d2 = ox * ox + oz * oz;

		if (best < 0 || d2 < bestD)
		{
			bestD = d2;
			best = i;
		}
	}

	if (best < 0)
		return;

	tx = car_data[best].hd.where.t[0];
	ty = car_data[best].hd.where.t[1];
	tz = car_data[best].hd.where.t[2];

	dx = tx - p->pos.vx;
	dy = ty - p->pos.vy;
	dz = tz - p->pos.vz;

	vm = cd2ProjIsqrt((int)(p->vel.vx * p->vel.vx + p->vel.vy * p->vel.vy + p->vel.vz * p->vel.vz));
	dm = cd2ProjIsqrt(dx * dx + dy * dy + dz * dz);

	if (vm <= 0 || dm <= 0)
		return;

	{
		// per-weapon homing strength (def->homingRate; 0 = the default). The
		// zoomy missiles pass a very small rate for a weak, lazy track.
		int t = (p->def->homingRate > 0) ? p->def->homingRate : CD2_PROJ_HOME_TURN;

		if (t > 1024) t = 1024;
		if (t < 1) t = 1;

		long long nx = ((long long)p->vel.vx * (1024 - t)) / vm + ((long long)dx * t) / dm;
		long long ny = ((long long)p->vel.vy * (1024 - t)) / vm + ((long long)dy * t) / dm;
		long long nz = ((long long)p->vel.vz * (1024 - t)) / vm + ((long long)dz * t) / dm;
		int nm = cd2ProjIsqrt((int)(nx * nx + ny * ny + nz * nz));

		if (nm <= 0)
			return;

		// same speed, new heading
		p->vel.vx = (int)((nx * vm) / nm);
		p->vel.vy = (int)((ny * vm) / nm);
		p->vel.vz = (int)((nz * vm) / nm);

		// keep the model pointing along the travel direction (unit * 4096)
		p->dir.vx = (int)(((long long)p->vel.vx << 12) / vm);
		p->dir.vy = (int)(((long long)p->vel.vy << 12) / vm);
		p->dir.vz = (int)(((long long)p->vel.vz << 12) / vm);

		// observability: how much of the shot is now aimed at the target.
		// 4096 = dead on, lower = still turning. It should climb.
		if (gCd2Cfg.debugLog)
		{
			static unsigned int t2;

			if ((t2++ % 40) == 0)
			{
				long long dot = (long long)p->vel.vx * dx + (long long)p->vel.vy * dy + (long long)p->vel.vz * dz;
				int cosE = (int)(dot / (vm / 64) / dm);

				printInfo("[combatd2] homing car=%d dist=%d aim=%d\n", best, (int)dm, cosE * 64);
			}
		}
	}
}

void cd2ProjectileBurst(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
			int count, int interval)
{
	int i, bi = -1, vi = -1;

	if (def == NULL || count <= 0)
		return;

	if (count > CD2_MAX_PROJECTILES)
		count = CD2_MAX_PROJECTILES;

	if (interval < 1)
		interval = 1;

	for (i = 0; i < CD2_MAX_BURSTS; i++)
	{
		if (!gBurst[i].active)
		{
			bi = i;
			break;
		}
	}

	if (bi < 0)
		return;		// all burst slots busy

	for (i = 0; i < CD2_MAX_VOLLEYS; i++)
	{
		if (!gVolley[i].active)
		{
			vi = i;
			break;
		}
	}

	if (vi >= 0)
	{
		gVolley[vi].active = 1;
		gVolley[vi].count = count;
		gVolley[vi].landed = 0;
		gVolley[vi].ttl = CD2_VOLLEY_TTL;
		gVolley[vi].gen = ++sVolleyGen;
	}

	gBurst[bi].active = 1;
	gBurst[bi].framesLeft = 0;	// first shot leaves immediately
	gBurst[bi].remaining = count;
	gBurst[bi].interval = interval;
	gBurst[bi].seq = 0;
	gBurst[bi].volley = vi;
	gBurst[bi].def = def;
	gBurst[bi].shooter = shooter;

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] volley start: %s x%d every %d frames (volley=%d)\n",
			def->name, count, interval, vi);
}

// Spawn the next shot of every in-flight burst: the pool staggers a volley's
// launch (re-aimed from the shooter) instead of firing all at once.
static void cd2ProjBurstStep(void)
{
	int i;

	for (i = 0; i < CD2_MAX_BURSTS; i++)
	{
		CD2_PROJ_BURST* b = &gBurst[i];
		VECTOR muzzle, dir, vel;

		if (!b->active)
			continue;

		if (b->framesLeft > 0)
		{
			b->framesLeft--;
			continue;
		}

		// the shooter must still exist; the burst trails it frame by frame
		if (b->shooter == NULL || b->shooter->controlType == CONTROL_TYPE_NONE ||
		    b->shooter->ap.carCos == NULL)
		{
			b->active = 0;
			continue;
		}

		cd2WpnMuzzle(b->shooter, 0, &muzzle);
		cd2WpnForward(b->shooter, &dir);

		vel.vx = (int)(((long long)dir.vx * b->def->speed) >> 12);
		vel.vy = (int)(((long long)dir.vy * b->def->speed) >> 12);
		vel.vz = (int)(((long long)dir.vz * b->def->speed) >> 12);

		cd2ProjectileSpawnEx(b->def, b->shooter, &muzzle, &vel, &dir, b->volley, b->seq);

		b->seq++;
		b->remaining--;

		if (b->remaining <= 0)
			b->active = 0;
		else
			b->framesLeft = b->interval;
	}
}

void cd2ProjectileStep(void)
{
	int i;

	// launch any due burst shots, and expire volleys that never completed
	cd2ProjBurstStep();

	for (i = 0; i < CD2_MAX_VOLLEYS; i++)
	{
		if (gVolley[i].active && --gVolley[i].ttl <= 0)
			gVolley[i].active = 0;
	}

	for (i = 0; i < CD2_MAX_PROJECTILES; i++)
	{
		CD2_PROJECTILE* p = &gProj[i];
		int gh;

		if (!p->active)
			continue;

		p->prev = p->pos;

		if (p->def->homing)
			cd2ProjectileSeek(p);

		// advance in sub-steps sized by the ACTUAL per-frame distance (which
		// grows once the shooter's velocity is inherited) so a fast missile
		// can't tunnel through a car
		{
			int ax = ABS(p->vel.vx), ay = ABS(p->vel.vy), az = ABS(p->vel.vz);
			int mag = ((ax < az) ? az : ax) + ((ax < az) ? ax : az) / 2 + ay / 2;
			int steps = mag / CD2_PROJ_SUBSTEP;
			int s;

			if (steps < 1) steps = 1;
			if (steps > CD2_PROJ_MAX_SUBSTEP) steps = CD2_PROJ_MAX_SUBSTEP;

			p->travelled += mag;

			for (s = 0; s < steps && p->active; s++)
			{
				int j;
				VECTOR stepPrev = p->pos;

				p->pos.vx += p->vel.vx / steps;
				p->pos.vy += p->vel.vy / steps;
				p->pos.vz += p->vel.vz / steps;

				// vehicle hit (skip the shooter): direct damage + blast NOW
				for (j = 0; j < MAX_CARS; j++)
				{
					CAR_DATA* cp = &car_data[j];

					if (cp == p->owner || cp->controlType == 0 ||
					    cp->ap.carCos == NULL)
						continue;

					if (cd2WpnPointInCar(cp, &p->pos))
					{
						int v = p->volley;
						int vg = p->vgen;
						VECTOR at = p->pos;
						VECTOR kdir = p->vel;

						cd2WpnDamageCar(cp, &p->pos, p->def->damage);
						cd2WpnKnock(cp, &p->pos, &p->vel, p->def->damage);
						cd2ProjectileImpact(p, cp, cp);

						// volley bookkeeping: every member that lands on a car counts;
						// the shot that completes the set (all landed) deals the bonus
						if (v >= 0 && gVolley[v].active && gVolley[v].gen == vg)
						{
							gVolley[v].landed++;

							if (gCd2Cfg.debugLog)
								printInfo("[combatd2] volley land %d/%d car=%d\n",
									gVolley[v].landed, gVolley[v].count, cp->id);

							if (gVolley[v].landed >= gVolley[v].count)
							{
								cd2WpnDamageCar(cp, &at, p->def->volleyBonusDamage);
								cd2WpnKnock(cp, &at, &kdir, p->def->volleyBonusKnock);

								if (gCd2Cfg.debugLog)
									printInfo("[combatd2] volley COMPLETE %d/%d bonus dmg=%d car=%d\n",
										gVolley[v].landed, gVolley[v].count,
										p->def->volleyBonusDamage, cp->id);

								gVolley[v].active = 0;
							}
						}
						break;
					}
				}

				if (!p->active)
					break;

				// scenery hit on this sub-step -> detonate on the wall
				if (p->def->collideScenery && lineClear(&stepPrev, &p->pos) == 0)
				{
					cd2ProjectileExplode(p);
					break;
				}

				// ground hit on this sub-step
				gh = MapHeight(&p->pos);
				if (gh != 0 && p->pos.vy <= gh + 24)
				{
					cd2ProjectileExplode(p);
					break;
				}
			}
		}

		if (!p->active)
			continue;

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
