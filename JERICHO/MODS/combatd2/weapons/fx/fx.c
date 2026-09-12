// weapons/fx/fx.c — Combat D2 CUSTOM EXPLOSIONS library (profiles + sequencer).
//
// Two jobs:
//
//   1. PROFILES. A table of named explosion profiles (CD2_FX_DEF). The
//      JER_EVENT_EXPLOSION_SPAWN handler resolves a custom explosion type
//      (>= CD2_FX_BASE) to its profile and writes the profile's parameters
//      into the new EXOBJECT, then rewrites the type to the profile's stock
//      base bang (so the engine's sound + collision branches stay valid).
//      The engine's DRAW and COLLIDE hooks then read those EXOBJECT fields
//      back automatically (tint/spin/box), so those need no handler here:
//      whatever the SPAWN handler stores is what the explosion shows and does.
//
//   2. SEQUENCER. A small pool of delayed blasts, used by the cluster missile
//      to burst into several small explosions after the parent lands. A blast
//      can STICK to a car (its hit point is captured in the car's local frame
//      and re-projected every frame, so the burst rides the car), or stay at a
//      world position. Each due blast runs a normal radial blast with its
//      profile.
//
// See weapons/fx/fx.h for the types and the profile id space, and
// job_fx.c (AddExplosion / DrawExplosion, inspired by the original bomberman.c)
// for the engine side.

#include "driver2.h"
#include "combatd2.h"
#include "cars.h"
#include "job_fx.h"
#include "system.h"
#include "jericho.h"
#include "jer_events.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/fx/fx.h"

#include <stdlib.h>

// ---------------------------------------------------------------------------
// Profile table (index by CD2_FX_* - CD2_FX_BASE)
// ---------------------------------------------------------------------------
//
// Sizes are relative to the stock bangs: LITTLE_BANG is speed 192 /
// hscale-rscale 1024; BIG_BANG is speed 128 / 4096. `collide = 0` because the
// owning weapon already applied its own damage + knockback, so the bang must
// stay cosmetic (no double push). tint -1 = the stock colour.
static const CD2_FX_DEF gFxDefs[] =
{
	// id            name       base        speed hscale rscale colScale collide yawRate   tint
	{ CD2_FX_DEFAULT, "DEFAULT", BIG_BANG,	128,  4096,  4096,  4096,    1,      0,     -1, -1, -1 },
	{ CD2_FX_MISSILE, "MISSILE", BIG_BANG,	110,  4700,  4700,  4096,    0,      140,   255, 170, 110 },
	{ CD2_FX_MINE,    "MINE",    BIG_BANG,	130,  4200,  4200,  4096,    0,      90,    255, 140, 60  },
	{ CD2_FX_SEEKER,  "SEEKER",  BIG_BANG,	145,  2600,  2600,  3072,    0,      210,   175, 90,  255 },
	{ CD2_FX_CLUSTER, "CLUSTER", BIG_BANG,	128,  4096,  4096,  4096,    0,      120,   255, 110, 20  },
	{ CD2_FX_BOMBLET, "BOMBLET", LITTLE_BANG, 200, 1600,  1600,  2048,    0,      180,   255, 150, 40  },
	// a dying car: big and fiery (spectacular) but collide = 0 so it only
	// looks the part — no push, no damage (the user asked for exactly this)
	{ CD2_FX_WRECK,   "WRECK",   BIG_BANG,	90,   6000,  6000,  4096,    0,      40,    -1, -1, -1 },
};

#define CD2_FX_DEF_COUNT (int)(sizeof(gFxDefs) / sizeof(gFxDefs[0]))

const CD2_FX_DEF* cd2FxDef(int id)
{
	int i;

	for (i = 0; i < CD2_FX_DEF_COUNT; i++)
	{
		if (gFxDefs[i].id == id)
			return &gFxDefs[i];
	}

	return NULL;
}

const CD2_FX_DEF* cd2FxResolve(int type)
{
	if (type < CD2_FX_BASE)
		return NULL;		// a stock ExplosionType

	return cd2FxDef(type);
}

// Spawn an explosion carrying profile `fxId`. Anything unknown (or < base)
// becomes the DEFAULT bang, so callers may pass a raw stock effect id safely.
void cd2FxSpawn(const VECTOR* at, int fxId)
{
	VECTOR p = *at;

	if (fxId < CD2_FX_BASE || cd2FxDef(fxId) == NULL)
		fxId = CD2_FX_DEFAULT;

	AddExplosion(p, fxId);
}

// ---------------------------------------------------------------------------
// JER_EVENT_EXPLOSION_SPAWN: turn a custom type into a parametric explosion
// ---------------------------------------------------------------------------
static int cd2FxOnSpawn(void* ud, void* args)
{
	JER_ARGS_EXPLOSION_SPAWN* a = (JER_ARGS_EXPLOSION_SPAWN*)args;
	const CD2_FX_DEF* fx;

	(void)ud;

	// a stock bang (mission explosions, un-themed weapons): leave it alone
	if (a->type < CD2_FX_BASE)
		return JER_RESULT_CONTINUE;

	fx = cd2FxResolve(a->type);

	if (fx == NULL)
		fx = cd2FxDef(CD2_FX_DEFAULT);

	if (fx == NULL)
		return JER_RESULT_CONTINUE;

	// rewrite to the stock base bang so the engine's sound + collision
	// branches (which switch on `type`) keep working
	a->type = fx->baseType;
	a->fxId = fx->id;

	a->speed = fx->speed;
	a->hscale = fx->hscale;
	a->rscale = fx->rscale;

	a->tintR = fx->tintR;
	a->tintG = fx->tintG;
	a->tintB = fx->tintB;

	a->yawRate = fx->yawRate;
	a->collide = fx->collide;
	a->colScale = fx->colScale;

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Delayed blast sequencer
// ---------------------------------------------------------------------------
#define CD2_FX_PENDING_MAX	(CD2_FX_BARRAGE_MAX * 4)

typedef struct CD2_FX_PENDING
{
	int active;
	int framesLeft;		// frames until this blast fires
	int fxId;
	int radius;
	int damage;
	const CAR_DATA* skip;	// car already damaged by the direct hit
	const CAR_DATA* stuck;	// car the burst rides (NULL = fixed in world)

	VECTOR world;		// world position (refreshed each frame when stuck)

	int localX, localY, localZ;	// hit offset in the stuck car's local frame
} CD2_FX_PENDING;

static CD2_FX_PENDING gFxPending[CD2_FX_PENDING_MAX];

static int cd2FxRand(int jitter)
{
	if (jitter <= 0)
		return 0;

	return (rand() % (2 * jitter + 1)) - jitter;
}

// world offset -> car local space (the inverse of cd2WpnPointInCar's frame)
static void cd2FxToLocal(const CAR_DATA* cp, const VECTOR* world, int* lx, int* ly, int* lz)
{
	const MATRIX* w = &cp->hd.where;
	int dx = world->vx - w->t[0];
	int dy = world->vy - w->t[1];
	int dz = world->vz - w->t[2];

	*lx = (int)(((long long)dx * w->m[0][0] + (long long)dy * w->m[1][0] + (long long)dz * w->m[2][0]) >> 12);
	*ly = (int)(((long long)dx * w->m[0][1] + (long long)dy * w->m[1][1] + (long long)dz * w->m[2][1]) >> 12);
	*lz = (int)(((long long)dx * w->m[0][2] + (long long)dy * w->m[1][2] + (long long)dz * w->m[2][2]) >> 12);
}

// car local space -> world
static void cd2FxToWorld(const CAR_DATA* cp, int lx, int ly, int lz, VECTOR* out)
{
	const MATRIX* w = &cp->hd.where;

	out->vx = w->t[0] + (int)(((long long)w->m[0][0] * lx + (long long)w->m[0][1] * ly + (long long)w->m[0][2] * lz) >> 12);
	out->vy = w->t[1] + (int)(((long long)w->m[1][0] * lx + (long long)w->m[1][1] * ly + (long long)w->m[1][2] * lz) >> 12);
	out->vz = w->t[2] + (int)(((long long)w->m[2][0] * lx + (long long)w->m[2][1] * ly + (long long)w->m[2][2] * lz) >> 12);
}

void cd2FxBarrage(const VECTOR* at, const CAR_DATA* car, int fxId,
		  int count, int interval, int jitter,
		  int radius, int damage, const CAR_DATA* skip)
{
	int i, k;

	if (count <= 0)
		return;

	if (count > CD2_FX_BARRAGE_MAX)
		count = CD2_FX_BARRAGE_MAX;

	if (interval < 1)
		interval = 1;

	for (i = 0; i < count; i++)
	{
		CD2_FX_PENDING* p = NULL;
		int jx, jy, jz;

		for (k = 0; k < CD2_FX_PENDING_MAX; k++)
		{
			if (!gFxPending[k].active)
			{
				p = &gFxPending[k];
				break;
			}
		}

		if (p == NULL)
			break;		// pool full: the rest of the burst is dropped

		jx = cd2FxRand(jitter);
		jy = cd2FxRand(jitter);
		jz = cd2FxRand(jitter);

		p->active = 1;
		p->framesLeft = i * interval;	// first blast lands immediately
		p->fxId = fxId;
		p->radius = radius;
		p->damage = damage;
		p->skip = skip;
		p->stuck = car;
		p->localX = jx;
		p->localY = jy;
		p->localZ = jz;

		if (car != NULL)
		{
			// capture the hit point as an offset in the car's frame, then add
			// the jitter there so the scatter is car-aligned
			int lx, ly, lz;

			cd2FxToLocal(car, at, &lx, &ly, &lz);
			p->localX = lx + jx;
			p->localY = ly + jy;
			p->localZ = lz + jz;

			cd2FxToWorld(car, p->localX, p->localY, p->localZ, &p->world);
		}
		else
		{
			p->world.vx = at->vx + jx;
			p->world.vy = at->vy + jy;
			p->world.vz = at->vz + jz;
		}
	}
}

void cd2FxStep(void)
{
	int i;

	for (i = 0; i < CD2_FX_PENDING_MAX; i++)
	{
		CD2_FX_PENDING* p = &gFxPending[i];

		if (!p->active)
			continue;

		// a stuck blast re-projects onto the car as it moves; if the car is
		// gone it freezes at its last world position
		if (p->stuck != NULL && p->stuck->controlType != CONTROL_TYPE_NONE &&
		    p->stuck->ap.carCos != NULL)
			cd2FxToWorld(p->stuck, p->localX, p->localY, p->localZ, &p->world);

		if (p->framesLeft > 0)
		{
			p->framesLeft--;
			continue;
		}

		cd2AoeBlast(&p->world, p->radius, p->damage, p->fxId, p->skip);

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] barrage blast fx=%d pos=(%d,%d,%d) stuck=%d\n",
				p->fxId, p->world.vx, p->world.vy, p->world.vz,
				(p->stuck != NULL) ? 1 : 0);

		p->active = 0;
	}
}

void cd2FxReset(void)
{
	int i;

	for (i = 0; i < CD2_FX_PENDING_MAX; i++)
		gFxPending[i].active = 0;
}

// ---------------------------------------------------------------------------
// Hook registration (called once from the combatd2 module entry)
// ---------------------------------------------------------------------------
static int cd2FxOnFrame(void* ud, void* args)
{
	(void)ud;
	(void)args;

	cd2FxStep();
	return JER_RESULT_CONTINUE;
}

static int cd2FxOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	cd2FxReset();
	return JER_RESULT_CONTINUE;
}

void cd2FxRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_EXPLOSION_SPAWN, cd2FxOnSpawn, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2FxOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2FxOnGameStart, NULL, 0);

	ctx->jer_log(ctx, "[combatd2] explosion fx registered (%d profiles)\n", CD2_FX_DEF_COUNT);
}
