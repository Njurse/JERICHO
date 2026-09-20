// weapons/fx/fx.h — Combat D2 CUSTOM EXPLOSIONS library: parametric FX profiles.
//
// The engine's explosion (job_fx.c, inspired by bomberman.c) is a single
// GTE-drawn hemisphere with a fixed size per type. The JERICHO explosion
// events (JER_EVENT_EXPLOSION_SPAWN/DRAW/COLLIDE) expose that effect's
// parameters, and this library is the cainescrossfire side of them: a table of named
// explosion *profiles* a weapon can point its impact at, so every weapon's hit
// reads differently (its own size, colour, spin and whether it collides) and
// the player can tell what just hit them.
//
// A profile is deliberately PARAMETRIC (no new geometry): it resizes, tints and
// spins the stock bang, and decides whether the bang pushes/damages cars. The
// parent weapon still owns the real damage; a themed impact is usually
// collide = 0 so the two never double-dip.
//
// A profile id is just a custom ExplosionType (>= CD2_FX_BASE) passed to
// AddExplosion; the SPAWN handler resolves it to a profile and rewrites the
// type to the profile's stock base bang (so the engine's sound and collision
// branches stay valid).

#ifndef CD2_FX_H
#define CD2_FX_H

#include "driver2.h"
#include "jericho.h"

// Custom explosion-type id space. Stock ExplosionType values are 0/1/666/999,
// so anything from here up is free for modules.
#define CD2_FX_BASE		1000

enum
{
	CD2_FX_NONE = 0,		// no profile (use the stock explosionEffect)
	CD2_FX_DEFAULT = CD2_FX_BASE,	// shared fallback bang for un-themed impacts
	CD2_FX_MISSILE,			// missile warhead: big, hot orange
	CD2_FX_MINE,			// mine: red-orange, wide
	CD2_FX_SEEKER,			// seeker: purple, smaller than the missile
	CD2_FX_CLUSTER,			// cluster parent: deep orange
	CD2_FX_BOMBLET,			// cluster sub-blast: small orange
	CD2_FX_WRECK,			// dying car: spectacular but NO damage/collision
	CD2_FX_ZOOMY,			// zoomy missile: small, cool blue
	CD2_FX_FREEZE,			// freeze missile: pale ice blue
	CD2_FX_SMOKE,			// pure black and slow: a puff of smoke, not a bang
	CD2_FX_COUNT
};

// One parametric explosion profile.
typedef struct CD2_FX_DEF
{
	int id;			// CD2_FX_*
	const char* name;	// short label (debug)

	int baseType;		// stock ExplosionType the bang rewrites to
				// (BIG_BANG / LITTLE_BANG / HEY_MOMMA) — picks
				// the stock sound + engine branch

	int speed;		// EXOBJECT.speed  (time units/frame; bigger = shorter)
	int hscale;		// vertical mesh scale
	int rscale;		// radial mesh scale
	int colScale;		// collision-box scale, 4096 = stock
	int collide;		// 1 = stock car push/damage, 0 = visual only
	int yawRate;		// extra spin, PSX angle units per frame

	int tintR, tintG, tintB;	// -1 = stock colour, else 0..255
} CD2_FX_DEF;

// The profile for `id`, or NULL when `id` is not a CD2_FX_* id.
const CD2_FX_DEF* cd2FxDef(int id);

// Resolve a raw explosion `type` to its profile (NULL for a stock type).
const CD2_FX_DEF* cd2FxResolve(int type);

// Spawn an explosion carrying profile `fxId`. Unknown/stock ids fall back to
// the default bang, so callers may pass anything.
void cd2FxSpawn(const VECTOR* at, int fxId);

// ---------------------------------------------------------------------------
// Delayed blast sequencer — the cluster-missile "burst".
//
// Schedules `count` blasts `interval` frames apart, each jittered by up to
// +/-`jitter` world units so the burst scatters. When `car` is non-NULL the
// blasts STICK to it: the world hit point `at` is captured as an offset in the
// car's local frame and re-projected each frame, so the burst rides the car.
// When `car` is NULL the blasts stay at the world position. Each due blast runs
// a real radial blast (radius/damage) with profile `fxId`.
// ---------------------------------------------------------------------------
#define CD2_FX_BARRAGE_MAX	5	// blasts per barrage (cluster = 5)

void cd2FxBarrage(const VECTOR* at, const CAR_DATA* car, int fxId,
		  int count, int interval, int jitter,
		  int radius, int damage, const CAR_DATA* skip,
		  const CAR_DATA* owner);

// Advance the sequencer (once per frame) and clear it on a fresh level.
void cd2FxStep(void);
void cd2FxReset(void);

// Register the fx hooks (called once from the cainescrossfire module entry).
void cd2FxRegister(JERICHO_CONTEXT* ctx);

#endif /* CD2_FX_H */
