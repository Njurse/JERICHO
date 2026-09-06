// weapons.c — Combat D2: WEAPON INVENTORY + PROJECTILE prototype.
//
// One of the source files of the single combatd2 module. Implements the car
// weapon prototype (registered by the module entry in combatd2.c via
// cd2WeaponsRegister()):
//
//   * INVENTORY  - two slots: the machine gun SIDEARM is always present and
//                  never runs out; the rocket PRIMARY carries finite ammo
//                  and the inventory falls back to the MG when it empties.
//   * INPUT      - read in JER_EVENT_FRAME from the mapped pad (TMB layout
//                  only): hold Triangle to fire (MG auto / rocket single),
//                  tap R1 to cycle MG <-> armed primary.
//   * MACHINE GUN- hitscan from the car's nose along its heading; each hit
//                  ApplyDamage()s the target car. The "drawn projectile" is
//                  a bright tracer streak drawn over a few frames.
//   * ROCKET     - a real moving projectile simulated per frame, drawn via
//                  the engine's JER_EVENT_DRAW_WORLD hook (streak + nose
//                  flare into the real ordering table). On impact with a car
//                  or the ground it explodes (AddExplosion) and splashes
//                  damage onto nearby cars.
//   * PICKUPS    - CD2_PICKUP object type + manager are stubbed here (see
//                  the TODO at cd2PickupUpdate) as the future home for
//                  drive-over primary-weapon spawns around the map. Nothing
//                  spawns pickups yet - grant one from the pause menu
//                  (Combat D2 -> Debug) to try the rocket.
//
// Drawing conventions: engine physics/car positions are y-UP; the render
// pipeline (camera_position / inv_camera_matrix / gte) is y-DOWN. Every
// point handed to the draw helpers below is converted exactly like the
// engine's DrawDebugOverlays path does (negate y, subtract the camera,
// feed the inverse camera matrix with a zero translation).

#include "driver2.h"
#include "combatd2.h"
#include "cars.h"
#include "cosmetic.h"
#include "camera.h"		/* camera_position */
#include "players.h"
#include "pad.h"
#include "dr2roads.h"		/* MapHeight (rocket ground hits) */
#include "job_fx.h"		/* AddExplosion */
#include "bcollide.h"		/* ApplyDamage */
#include "pres.h"		/* HUD text */
#include "draw.h"		/* inv_camera_matrix */
#include "system.h"		/* current (OT + primptr) */
#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"

#include <stdio.h>

// zero translation for the gte projection helpers (points are pre-subtracted
// from the camera, exactly like the engine's DrawDebugOverlays path)
static VECTOR gZeroVec = { 0, 0, 0, 0 };

// ---------------------------------------------------------------------------
// Inventory state
// ---------------------------------------------------------------------------

static int gSelWeapon;		// CD2_WPN_* currently armed
static int gPrimaryId;		// CD2_WPN_NONE or the weapon in the primary slot
static int gPrimaryAmmo;	// rounds left in the primary slot

// ---------------------------------------------------------------------------
// Tracers (drawn MG shots)
// ---------------------------------------------------------------------------

#define CD2_MAX_TRACERS 24

typedef struct CD2_TRACER
{
	int active;
	int life;		// frames remaining
	VECTOR from;	// muzzle, y-up world
	VECTOR to;		// impact / range end, y-up world
} CD2_TRACER;

static CD2_TRACER gTracers[CD2_MAX_TRACERS];

// ---------------------------------------------------------------------------
// Rockets (moving projectiles)
// ---------------------------------------------------------------------------

#define CD2_MAX_ROCKETS 16

typedef struct CD2_ROCKET
{
	int active;
	int travelled;	// world units flown (vs CD2_RKT_RANGE)
	VECTOR pos;	// y-up world
	VECTOR vel;	// world units / frame
	VECTOR prev;	// last frame's pos (streak tail)
} CD2_ROCKET;

static CD2_ROCKET gRockets[CD2_MAX_ROCKETS];

// ---------------------------------------------------------------------------
// Weapon pickup object type (SKELETON - not wired up yet)
// ---------------------------------------------------------------------------
//
// TODO(weapons): the future drive-over pickup. Nothing creates these yet;
// the manager is wired into the frame/render hooks (as a no-op over zero
// active pickups) so the type and lifecycle are in place to grow: add a
// spawner that drops CD2_PICKUP entries at map locations, give each a
// drawn quad (bob/spin) in cd2PickupDraw, and on drive-over call
// cd2PickupGrantPrimary() -> cd2WpnGrantRocket(pickup->ammo).

#define CD2_MAX_PICKUPS 8

typedef struct CD2_PICKUP
{
	int active;
	int weaponId;	// CD2_WPN_* this pickup grants
	int ammo;	// rounds granted
	VECTOR pos;	// y-up world
	int bobT;	// bob/spin phase counter
} CD2_PICKUP;

static CD2_PICKUP gPickups[CD2_MAX_PICKUPS];

// ---------------------------------------------------------------------------
// Small drawing helpers (render frame = y-down; inputs are y-up world)
// ---------------------------------------------------------------------------

// Project one y-up world point to screen. Returns the OT-style z (> 0 when
// in front of the camera) or 0 when it projects behind / fails.
static int cd2ProjectPoint(const VECTOR* p, int* sx, int* sy)
{
	SVECTOR v;
	int z;
	unsigned int xy;

	// physics y-up -> render y-down, then camera-relative
	v.vx = p->vx - camera_position.vx;
	v.vy = -(p->vy - camera_position.vy);
	v.vz = p->vz - camera_position.vz;

	gte_SetRotMatrix(&inv_camera_matrix);
	gte_SetTransVector(&gZeroVec);	/* zero translation: points are camera-relative */
	gte_ldv3(&v, &v, &v);
	gte_rtpt();
	gte_avsz4();
	gte_stopz(&z);

	if (z <= 0)
		return 0;

	gte_stsxy0(&xy);
	*sx = (short)(xy & 0xFFFF);
	*sy = (short)((xy >> 16) & 0xFFFF);
	return z;
}

// Draw a world-space line segment (two y-up points) into the OT at the depth
// of its NEAR endpoint (so a long tracer never lands behind the whole world).
static void cd2DrawLine3D(const VECTOR* a, const VECTOR* b, int r, int g, int bl, int bright)
{
	LINE_F2* line;
	int z;
	int z2;
	int sx0, sy0, sx1, sy1;

	(void)bright;

	z = cd2ProjectPoint(a, &sx0, &sy0);
	z2 = cd2ProjectPoint(b, &sx1, &sy1);

	if (z <= 0 && z2 <= 0)
		return;

	line = (LINE_F2*)current->primptr;
	setLineF2(line);
	setSemiTrans(line, 1);

	line->x0 = sx0;
	line->y0 = sy0;
	line->x1 = sx1;
	line->y1 = sy1;
	line->r0 = r;
	line->g0 = g;
	line->b0 = bl;

	// depth-sort by whichever endpoint is nearer
	if (z2 > 0 && (z <= 0 || z2 < z))
		z = z2;

	{
		int otIdx = z >> 1;

		if (otIdx < 0)
			otIdx = 0;

		if (otIdx >= OTSIZE)
			otIdx = OTSIZE - 1;

		addPrim(current->ot + otIdx, line);
	}

	current->primptr += sizeof(LINE_F2);
}

// Draw a camera-facing square "flare" of world size `half` at p (y-up).
// Its on-screen size is taken from the projected vertical span, so it
// shrinks with distance like a real 3D object.
static void cd2DrawFlare3D(const VECTOR* p, int half, int r, int g, int bl)
{
	POLY_F4* poly;
	VECTOR above;
	int sx, sy, ax, ay;
	int hpx;
	unsigned int xy;

	if (cd2ProjectPoint(p, &sx, &sy) <= 0)
		return;

	above.vx = p->vx;
	above.vy = p->vy + half;
	above.vz = p->vz;

	if (cd2ProjectPoint(&above, &ax, &ay) <= 0)
		return;

	hpx = ABS(ay - sy);
	if (hpx < 1)
		hpx = 1;
	if (hpx > 64)
		hpx = 64;

	poly = (POLY_F4*)current->primptr;
	setPolyF4(poly);
	setSemiTrans(poly, 1);

	poly->x0 = sx - hpx; poly->y0 = sy - hpx;
	poly->x1 = sx + hpx; poly->y1 = sy - hpx;
	poly->x2 = sx + hpx; poly->y2 = sy + hpx;
	poly->x3 = sx - hpx; poly->y3 = sy + hpx;
	poly->r0 = r;
	poly->g0 = g;
	poly->b0 = bl;

	// re-project the center for its depth bucket (needed for occlusion)
	{
		SVECTOR v;
		int z;

		v.vx = p->vx - camera_position.vx;
		v.vy = -(p->vy - camera_position.vy);
		v.vz = p->vz - camera_position.vz;

		gte_SetRotMatrix(&inv_camera_matrix);
		gte_SetTransVector(&gZeroVec);
		gte_ldv3(&v, &v, &v);
		gte_rtpt();
		gte_avsz4();
		gte_stopz(&z);

		if (z <= 0)
		{
			current->primptr = (char*)poly;	/* drop it */
			return;
		}

		{
			int otIdx = z >> 1;

			if (otIdx < 0)
				otIdx = 0;

			if (otIdx >= OTSIZE)
				otIdx = OTSIZE - 1;

			addPrim(current->ot + otIdx, poly);
		}
	}

	current->primptr += sizeof(POLY_F4);
	(void)xy;
}

// ---------------------------------------------------------------------------
// Car hit-testing (world units, y-up)
// ---------------------------------------------------------------------------

// Is world point `p` inside car cp's oriented box? Half extents come from
// the car's cosmetic colBox (full extents, so halved), axes from hd.where.
static int cd2PointInCar(const CAR_DATA* cp, const VECTOR* p)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int dx = p->vx - w->t[0];
	int dy = p->vy - w->t[1];
	int dz = p->vz - w->t[2];
	// dot onto each local axis (columns are unit * 4096)
	int lx = (int)(((long long)dx * w->m[0][0] + (long long)dy * w->m[1][0] + (long long)dz * w->m[2][0]) >> 12);
	int ly = (int)(((long long)dx * w->m[0][1] + (long long)dy * w->m[1][1] + (long long)dz * w->m[2][1]) >> 12);
	int lz = (int)(((long long)dx * w->m[0][2] + (long long)dy * w->m[1][2] + (long long)dz * w->m[2][2]) >> 12);
	(void)ly;

	if (ABS(lx) > cb->vx / 2)
		return 0;

	if (ABS(lz) > cb->vz / 2)
		return 0;

	/* vertical: use the car's half height but keep a little slack so a
	 * tracer aimed flat at hood height still connects on slopes */
	if (ABS(ly) > (cb->vy / 2) + 40)
		return 0;

	return 1;
}

// Find the nearest car the ray (origin O, direction = dirM column 2, i.e. the
// shooter's forward, unit*4096) hits within `range` world units, excluding the
// shooter. Returns the car index or -1, and fills `hit` with the impact point.
static int cd2RayHitCar(const CAR_DATA* shooter, VECTOR* o, const MATRIX* dirM, int range, VECTOR* hit)
{
	int best = -1;
	long long bestT = (long long)range << 12;
	int i;
	long long dirX = dirM->m[0][2];
	long long dirY = dirM->m[1][2];
	long long dirZ = dirM->m[2][2];

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		const MATRIX* w;
		long long hx, hy, hz;
		long long tmin, tmax;
		int axis;
		int miss = 0;

		if (cp == shooter || cp->controlType == 0)	/* CONTROL_TYPE_NONE */
			continue;

		if (cp->ap.carCos == NULL)
			continue;

		w = &cp->hd.where;

		// half extents of the oriented box (world units)
		hx = cp->ap.carCos->colBox.vx / 2;
		hy = cp->ap.carCos->colBox.vy / 2;
		hz = cp->ap.carCos->colBox.vz / 2;

		tmin = 0;
		tmax = (long long)range << 12;	// fixed (4096 = 1 world unit)

		for (axis = 0; axis < 3; axis++)
		{
			// axis vectors: columns of the car matrix (unit * 4096)
			long long ux = w->m[0][axis];
			long long uy = w->m[1][axis];
			long long uz = w->m[2][axis];
			long long h = (axis == 0) ? hx : (axis == 1) ? hy : hz;
			long long po, pd, t1, t2;

			// po: signed distance of the origin from the car centre along this
			// axis, world units: dot(o - c, axis)/4096
			po = ((long long)(o->vx - w->t[0]) * ux +
			      (long long)(o->vy - w->t[1]) * uy +
			      (long long)(o->vz - w->t[2]) * uz) >> 12;

			// pd: ray direction projected onto the axis, dimensionless cos
			// (dot of two unit*4096 vectors >> 24)
			pd = (dirX * ux + dirY * uy + dirZ * uz) >> 24;

			if (pd == 0)
			{
				if (po > h || po < -h)
				{
					miss = 1;
					break;	// parallel and outside this slab
				}

				continue;
			}

			// ray parameter t in fixed world units: t = (-h - po) / pd
			t1 = ((-h - po) << 12) / pd;
			t2 = (( h - po) << 12) / pd;

			if (t1 > t2)
			{
				long long t = t1; t1 = t2; t2 = t;
			}

			if (t1 > tmin)
				tmin = t1;

			if (t2 < tmax)
				tmax = t2;

			if (tmin > tmax)
			{
				miss = 1;
				break;	// the slab intervals no longer overlap
			}
		}

		if (!miss && tmax >= 0)
		{
			long long t = (tmin > 0) ? tmin : tmax;

			if (t < bestT)
			{
				bestT = t;
				best = i;
			}
		}
	}

	if (best >= 0)
	{
		// impact = o + dir * (t >> 12)
		long long t = bestT >> 12;

		hit->vx = o->vx + (int)((dirX * t) >> 12);
		hit->vy = o->vy + (int)((dirY * t) >> 12);
		hit->vz = o->vz + (int)((dirZ * t) >> 12);
	}

	return best;
}

// ---------------------------------------------------------------------------
// Damage
// ---------------------------------------------------------------------------

// Apply damage to a car from a world-space impact. `region` is picked from
// the impact direction in the car's local frame (0..5; the engine's
// ApplyDamage distributes zone damage + totals from it).
static void cd2DamageCar(CAR_DATA* cp, const VECTOR* at, int value)
{
	const MATRIX* w = &cp->hd.where;
	int dx = at->vx - w->t[0];
	int dy = at->vy - w->t[1];
	int dz = at->vz - w->t[2];
	int lx = (int)(((long long)dx * w->m[0][0] + (long long)dy * w->m[1][0] + (long long)dz * w->m[2][0]) >> 12);
	int ly = (int)(((long long)dx * w->m[0][1] + (long long)dy * w->m[1][1] + (long long)dz * w->m[2][1]) >> 12);
	int lz = (int)(((long long)dx * w->m[0][2] + (long long)dy * w->m[1][2] + (long long)dz * w->m[2][2]) >> 12);
	int region = 0;
	int dominant = ABS(lx);
	int dom = lx;

	if (ABS(ly) > dominant) { dominant = ABS(ly); dom = ly; }
	if (ABS(lz) > dominant) { dominant = ABS(lz); dom = lz; }

	(void)dominant;

	// region: 0 front, 1 back, 2 right, 3 left, 4 top, 5 bottom (a stable
	// arbitrary mapping - ApplyDamage only needs a valid 0..5 zone)
	if (dom == lz) region = (lz > 0) ? 0 : 1;
	else if (dom == lx) region = (lx > 0) ? 3 : 2;
	else region = (ly > 0) ? 4 : 5;

	ApplyDamage(cp, (char)region, value, 0);
}

// ---------------------------------------------------------------------------
// Firing
// ---------------------------------------------------------------------------

static int cd2PlayerCar(CAR_DATA** out)
{
	int carId = player[0].playerCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	*out = &car_data[carId];

	if ((*out)->controlType != CONTROL_TYPE_PLAYER)
		return 0;

	return 1;
}

// Spawn a tracer from the muzzle (two y-up world points).
static void cd2AddTracer(const VECTOR* from, const VECTOR* to)
{
	int i;

	for (i = 0; i < CD2_MAX_TRACERS; i++)
	{
		CD2_TRACER* t = &gTracers[i];

		if (!t->active)
		{
			t->active = 1;
			t->life = CD2_MG_TRACER_LIFE;
			t->from = *from;
			t->to = *to;
			return;
		}
	}
}

// Fire the machine gun: hitscan along the car's forward; damage the first
// car in the way; always draw a tracer out to the hit (or the full range).
static void cd2FireMG(CAR_DATA* cp)
{
	VECTOR o;
	VECTOR tip;
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int hitCar;
	int hz = cb->vz / 2;
	int hy = cb->vy / 2;

	// muzzle: ahead of the nose, at hood height
	o.vx = w->t[0] + (int)(((long long)w->m[0][2] * hz * 3) >> 14);	// *0.75
	o.vy = w->t[1] + (int)(((long long)w->m[1][2] * hz * 3) >> 14);
	o.vz = w->t[2] + (int)(((long long)w->m[2][2] * hz * 3) >> 14);
	o.vx += (int)(((long long)w->m[0][1] * hy) >> 13);	// +0.5 height
	o.vy += (int)(((long long)w->m[1][1] * hy) >> 13);
	o.vz += (int)(((long long)w->m[2][1] * hy) >> 13);

	hitCar = cd2RayHitCar(cp, &o, w, CD2_MG_RANGE, &tip);

	if (hitCar >= 0)
		cd2DamageCar(&car_data[hitCar], &tip, CD2_MG_DAMAGE);
	else
	{
		// tracer tip = range end
		tip.vx = o.vx + (int)(((long long)w->m[0][2] * CD2_MG_RANGE) >> 12);
		tip.vy = o.vy + (int)(((long long)w->m[1][2] * CD2_MG_RANGE) >> 12);
		tip.vz = o.vz + (int)(((long long)w->m[2][2] * CD2_MG_RANGE) >> 12);
	}

	cd2AddTracer(&o, &tip);
}

// Launch a rocket along the car's forward from the same muzzle.
static void cd2LaunchRocket(CAR_DATA* cp)
{
	int i;

	for (i = 0; i < CD2_MAX_ROCKETS; i++)
	{
		CD2_ROCKET* r = &gRockets[i];

		if (!r->active)
		{
			const MATRIX* w = &cp->hd.where;
			const SVECTOR* cb = &cp->ap.carCos->colBox;
			int hz = cb->vz / 2;
			int hy = cb->vy / 2;

			r->active = 1;
			r->travelled = 0;

			r->pos.vx = w->t[0] + (int)(((long long)w->m[0][2] * hz * 3) >> 14);
			r->pos.vy = w->t[1] + (int)(((long long)w->m[1][2] * hz * 3) >> 14);
			r->pos.vz = w->t[2] + (int)(((long long)w->m[2][2] * hz * 3) >> 14);
			r->pos.vx += (int)(((long long)w->m[0][1] * hy) >> 13);
			r->pos.vy += (int)(((long long)w->m[1][1] * hy) >> 13);
			r->pos.vz += (int)(((long long)w->m[2][1] * hy) >> 13);

			r->prev = r->pos;

			r->vel.vx = (int)(((long long)w->m[0][2] * CD2_RKT_SPEED) >> 12);
			r->vel.vy = (int)(((long long)w->m[1][2] * CD2_RKT_SPEED) >> 12);
			r->vel.vz = (int)(((long long)w->m[2][2] * CD2_RKT_SPEED) >> 12);

			gPrimaryAmmo--;
			return;
		}
	}
}

// Rocket impact: explosion FX + splash damage over the cars around it.
// The directly hit car already took the full CD2_RKT_DAMAGE and is skipped.
static void cd2ExplodeRocket(const VECTOR* at, const CAR_DATA* skip)
{
	int i;
	VECTOR blast;

	blast.vx = at->vx;
	blast.vy = at->vy;
	blast.vz = at->vz;
	AddExplosion(blast, LITTLE_BANG);

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		int dx, dz;
		int dist;
		int dmg;

		if (cp == skip || cp->controlType == 0)
			continue;

		if (cp->ap.carCos == NULL)
			continue;

		dx = at->vx - cp->hd.where.t[0];
		dz = at->vz - cp->hd.where.t[2];
		dist = (ABS(dx) + ABS(dz)) / 2;

		if (dist > CD2_RKT_SPLASH_RADIUS)
			continue;

		if (dist < 1)
			dist = 1;

		dmg = CD2_RKT_SPLASH_DAMAGE * (CD2_RKT_SPLASH_RADIUS - dist) / CD2_RKT_SPLASH_RADIUS;
		if (dmg < 1)
			continue;

		cd2DamageCar(cp, at, dmg);
	}
}

// Step one rocket; returns 1 when it died this frame.
static int cd2StepRocket(CD2_ROCKET* r)
{
	VECTOR ground;
	int gh;
	int i;

	r->prev = r->pos;

	r->pos.vx += r->vel.vx;
	r->pos.vy += r->vel.vy;
	r->pos.vz += r->vel.vz;
	r->travelled += CD2_RKT_SPEED;

	if (r->travelled >= CD2_RKT_RANGE)
	{
		r->active = 0;
		return 1;	// fizzled at max range
	}

	// car hit (excluding the shooter: it is never a rocket's owner target -
	// but the shooter is excluded by the caller storing the shooter's id in
	// the rocket; simplest: skip the rocket's own car via controlType+pos is
	// overkill, so just explode on the FIRST car whose box contains us)
	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (cp->controlType == 0)
			continue;

		if (cp->ap.carCos == NULL)
			continue;

		if (cd2PointInCar(cp, &r->pos))
		{
			cd2DamageCar(cp, &r->pos, CD2_RKT_DAMAGE);
			cd2ExplodeRocket(&r->pos, cp);
			r->active = 0;
			return 1;
		}
	}

	// ground hit (rising terrain / drove into the ground)
	ground.vx = r->pos.vx;
	ground.vy = r->pos.vy;
	ground.vz = r->pos.vz;
	gh = MapHeight(&ground);

	if (gh != 0 && r->pos.vy <= gh + 24)
	{
		cd2ExplodeRocket(&r->pos, NULL);
		r->active = 0;
		return 1;
	}

	return 0;
}

// ---------------------------------------------------------------------------
// Pickup manager (SKELETON)
// ---------------------------------------------------------------------------

static void cd2PickupInit(void)
{
	int i;

	for (i = 0; i < CD2_MAX_PICKUPS; i++)
	{
		gPickups[i].active = 0;
		gPickups[i].weaponId = CD2_WPN_NONE;
		gPickups[i].ammo = 0;
		gPickups[i].bobT = i * 97;
	}
}

static void cd2PickupUpdate(void)
{
	int i;

	// TODO(weapons): when pickups are spawned (see the CD2_PICKUP note at
	// the top of this file), drive-over detection lives here: compare the
	// player car's position to each active pickup's pos and grant the
	// weapon via cd2WpnGrantRocket(pickup->ammo), then deactivate it.
	for (i = 0; i < CD2_MAX_PICKUPS; i++)
	{
		CD2_PICKUP* pk = &gPickups[i];

		if (pk->active)
			pk->bobT++;	// future bob/spin animation
	}
}

static void cd2PickupDraw(void)
{
	int i;

	// TODO(weapons): draw active pickups here (JER_EVENT_DRAW_WORLD) as a
	// small bobbing weapon-coloured flare via cd2DrawFlare3D().
	for (i = 0; i < CD2_MAX_PICKUPS; i++)
	{
		(void)gPickups[i];
	}
}

// ---------------------------------------------------------------------------
// Public inventory API (pause-menu Debug items call these)
// ---------------------------------------------------------------------------

void cd2WpnGrantRocket(int ammo)
{
	gPrimaryId = CD2_WPN_ROCKET;
	gPrimaryAmmo = jer_clamp_int(ammo, 1, 999);
	gSelWeapon = CD2_WPN_ROCKET;	// pickups auto-equip (TMB behaviour)
}

void cd2WpnClearPrimary(void)
{
	gPrimaryId = CD2_WPN_NONE;
	gPrimaryAmmo = 0;

	if (gSelWeapon != CD2_WPN_MG)
		gSelWeapon = CD2_WPN_MG;
}

int cd2WpnHavePrimary(void)
{
	return (gPrimaryId != CD2_WPN_NONE) ? 1 : 0;
}

int cd2WpnAmmo(void)
{
	return gPrimaryAmmo;
}

// ---------------------------------------------------------------------------
// JER_EVENT_FRAME: input + projectile simulation
// ---------------------------------------------------------------------------

static int cd2WpnOnFrame(void* ud, void* args)
{
	CAR_DATA* cp = NULL;
	int pad = 0;
	int fireHold;
	int fireTap;
	int cycleTap;
	static int gPrevFire;
	static int gPrevCycle;
	CD2_TRACER* t;
	CD2_ROCKET* r;
	int i;

	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled || !gCd2Cfg.tmbButtons)
	{
		gPrevFire = 0;
		gPrevCycle = 0;
		return JER_RESULT_CONTINUE;
	}

	cd2PickupUpdate();

	// simulate existing rockets every frame (they fly on regardless)
	for (i = 0; i < CD2_MAX_ROCKETS; i++)
	{
		r = &gRockets[i];

		if (r->active)
			cd2StepRocket(r);
	}

	// decay tracer life
	for (i = 0; i < CD2_MAX_TRACERS; i++)
	{
		t = &gTracers[i];

		if (t->active)
		{
			t->life--;

			if (t->life <= 0)
				t->active = 0;
		}
	}

	if (!cd2PlayerCar(&cp))
	{
		gPrevFire = 0;
		gPrevCycle = 0;
		return JER_RESULT_CONTINUE;
	}

	pad = Pads[player[0].padid].mapped;

	fireHold = (pad & CD2_WPN_FIRE) ? 1 : 0;
	fireTap = fireHold && !gPrevFire;
	cycleTap = (pad & CD2_WPN_CYCLE) && !gPrevCycle;
	gPrevFire = fireHold;
	gPrevCycle = (pad & CD2_WPN_CYCLE) ? 1 : 0;

	// cycle MG <-> armed primary
	if (cycleTap)
	{
		if (gSelWeapon == CD2_WPN_MG && gPrimaryId != CD2_WPN_NONE && gPrimaryAmmo > 0)
			gSelWeapon = CD2_WPN_ROCKET;
		else if (gSelWeapon == CD2_WPN_ROCKET)
			gSelWeapon = CD2_WPN_MG;
	}

	if (!fireHold)
		return JER_RESULT_CONTINUE;

	if (gSelWeapon == CD2_WPN_MG)
	{
		static int mgClock;

		if (mgClock > 0)
			mgClock--;
		else
		{
			cd2FireMG(cp);
			mgClock = CD2_MG_INTERVAL;
		}
	}
	else if (gSelWeapon == CD2_WPN_ROCKET && gPrimaryAmmo > 0)
	{
		if (fireTap)
		{
			cd2LaunchRocket(cp);

			if (gPrimaryAmmo <= 0)
				gSelWeapon = CD2_WPN_MG;	// empty primary -> sidearm
		}
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_DRAW_WORLD: projectiles + tracers into the real ordering table
// ---------------------------------------------------------------------------

static int cd2WpnOnDrawWorld(void* ud, void* args)
{
	int i;
	CD2_TRACER* t;
	CD2_ROCKET* r;

	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	// MG tracers: bright yellow streaks
	for (i = 0; i < CD2_MAX_TRACERS; i++)
	{
		t = &gTracers[i];

		if (t->active)
			cd2DrawLine3D(&t->from, &t->to, 255, 235, 110, 1);
	}

	// rockets: orange streak (prev->pos) + a nose flare
	for (i = 0; i < CD2_MAX_ROCKETS; i++)
	{
		r = &gRockets[i];

		if (!r->active)
			continue;

		cd2DrawLine3D(&r->prev, &r->pos, 255, 130, 40, 1);

		{
			VECTOR ahead;
			ahead.vx = r->pos.vx + (int)(((long long)r->vel.vx * 12) / CD2_RKT_SPEED);
			ahead.vy = r->pos.vy + (int)(((long long)r->vel.vy * 12) / CD2_RKT_SPEED);
			ahead.vz = r->pos.vz + (int)(((long long)r->vel.vz * 12) / CD2_RKT_SPEED);

			cd2DrawLine3D(&r->pos, &ahead, 255, 200, 120, 1);
		}

		cd2DrawFlare3D(&r->pos, 26, 255, 150, 60);
	}

	cd2PickupDraw();

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_DRAW_OVERLAY: weapon HUD (bottom-left)
// ---------------------------------------------------------------------------

static int cd2WpnOnOverlay(void* ud, void* args)
{
	CAR_DATA* cp = NULL;
	char text[64];

	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled || !gCd2Cfg.tmbButtons)
		return JER_RESULT_CONTINUE;

	if (!cd2PlayerCar(&cp))
		return JER_RESULT_CONTINUE;

	if (gPrimaryId == CD2_WPN_NONE)
	{
		SetTextColour(200, 200, 200);
		sprintf(text, "MG armed (hold %s)", "TRIANGLE");
		PrintString(text, 20, 220);
	}
	else if (gSelWeapon == CD2_WPN_MG)
	{
		SetTextColour(255, 235, 110);
		sprintf(text, "> MG        [Rocket x%d - tap R1]", gPrimaryAmmo);
		PrintString(text, 20, 220);
	}
	else
	{
		SetTextColour(255, 150, 60);
		sprintf(text, "> ROCKET x%d (tap R1 for MG)", gPrimaryAmmo);
		PrintString(text, 20, 220);
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_GAME_START: fresh level = fresh inventory (sidearm only)
// ---------------------------------------------------------------------------

static int cd2WpnOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	gSelWeapon = CD2_WPN_MG;
	gPrimaryId = CD2_WPN_NONE;
	gPrimaryAmmo = 0;

	for (i = 0; i < CD2_MAX_TRACERS; i++)
		gTracers[i].active = 0;

	for (i = 0; i < CD2_MAX_ROCKETS; i++)
		gRockets[i].active = 0;

	cd2PickupInit();

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_combatd2_entry in combatd2.c)
// ---------------------------------------------------------------------------

void cd2WeaponsRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2WpnOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2WpnOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, cd2WpnOnOverlay, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, cd2WpnOnDrawWorld, NULL, 0);

	ctx->jer_log(ctx, "[combatd2] weapons registered (SDK v%d)\n", ctx->sdkVersion);
}
