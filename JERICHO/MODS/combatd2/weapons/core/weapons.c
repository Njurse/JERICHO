// weapons/core/weapons.c — Combat D2 weapon framework CORE.
//
// Owns the parts every weapon shares:
//   * the registry (CD2_WEAPON_DEF rows, grouped by functional class),
//   * the per-car inventory (ammo + the selected weapon),
//   * the shared world helpers (draw lines, hit tests, damage, muzzle),
//   * the JER_EVENT_FRAME input router + class step loop,
//   * the JER_EVENT_DRAW_WORLD / DRAW_OVERLAY presentation,
//   * the CAMERA_LOOK suppression that frees the side-view keys,
//   * JER_EVENT_GAME_START reset (+ optional "all weapons" test grant).
//
// The class pools live in weapons/raycast, weapons/projectile and
// weapons/drops; the explosion helper in weapons/aoe; the concrete weapon
// defs + fire functions in each class folder next to their pool.
//
// INPUT (TMB-style dual trigger; the stock side-view keys are disabled):
//   LT / L2  - hold to fire the BASE machine gun (auto)
//   RT / R2  - fire the SELECTED weapon (single shot per press)
//   RB / R1  - next weapon     LB / L1 - previous weapon
// The base weapon (machine gun) is never part of the cycle.

#include "driver2.h"
#include "combatd2.h"
#include "cars.h"
#include "cosmetic.h"
#include "camera.h"
#include "players.h"
#include "pad.h"
#include "dr2roads.h"
#include "job_fx.h"
#include "bcollide.h"
#include "sound.h"
#include "gamesnd.h"
#include "pres.h"
#include "draw.h"
#include "system.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"
#include "dr2math.h"
#include "weapon.h"
#include "weapon_internal.h"

#include <stdio.h>

// ---------------------------------------------------------------------------
// Input bindings (physical buttons are remapped by the engine's config.ini)
// ---------------------------------------------------------------------------
#define CD2_WPN_FIRE_BASE	MPAD_L2	// left trigger: base machine gun
#define CD2_WPN_FIRE_SEL	MPAD_R2	// right trigger: selected weapon
#define CD2_WPN_NEXT		MPAD_R1	// right bumper: next weapon
#define CD2_WPN_PREV		MPAD_L1	// left bumper: previous weapon

// Weapon impact knockback tuning (lever action): spin = leverArm * strength *
// CD2_KNOCK_SCALE, capped at CD2_KNOCK_MAX (world angular-velocity units).
#define CD2_KNOCK_SCALE		6
#define CD2_KNOCK_MAX		0x400000

// ---------------------------------------------------------------------------
// Registry (index == CD2_WID_*)
// ---------------------------------------------------------------------------
static const CD2_WEAPON_DEF* const gWdefs[CD2_WID_COUNT] =
{
	&cd2WdefMG,
	&cd2WdefMissile,
	&cd2WdefMine
};

const CD2_WEAPON_DEF* cd2WpnDef(int weaponId)
{
	if (weaponId < 0 || weaponId >= CD2_WID_COUNT)
		return NULL;

	return gWdefs[weaponId];
}

const char* cd2WpnName(int weaponId)
{
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

	return d ? d->name : "NONE";
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------
static int gAmmo[CD2_WID_COUNT];	// rounds per weapon (base weapons: unused)
static int gSelected = CD2_WID_NONE;	// currently armed weapon (never a base weapon)

int cd2WpnOwns(int weaponId)
{
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

	if (d == NULL)
		return 0;

	return d->isBase ? 1 : (gAmmo[weaponId] > 0);
}

int cd2WpnAmmo(int weaponId)
{
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

	if (d == NULL)
		return 0;

	return d->isBase ? -1 : gAmmo[weaponId];	// -1 = infinite (base)
}

int cd2WpnSelected(void)
{
	return gSelected;
}

// The first carried non-base weapon, or CD2_WID_NONE.
static int cd2WpnFirstOwned(void)
{
	int i;

	for (i = 0; i < CD2_WID_COUNT; i++)
	{
		const CD2_WEAPON_DEF* d = gWdefs[i];

		if (d != NULL && !d->isBase && cd2WpnOwns(i))
			return i;
	}

	return CD2_WID_NONE;
}

int cd2WpnCycle(int dir)
{
	int n = CD2_WID_COUNT;
	int idx = (gSelected == CD2_WID_NONE) ? (dir > 0 ? -1 : 0) : gSelected;
	int k;

	for (k = 0; k < n; k++)
	{
		const CD2_WEAPON_DEF* d;

		idx = (idx + dir + n) % n;
		d = gWdefs[idx];

		if (d != NULL && !d->isBase && cd2WpnOwns(idx))
		{
			gSelected = idx;
			return idx;
		}
	}

	gSelected = CD2_WID_NONE;
	return gSelected;
}

void cd2WpnGrant(int weaponId, int ammo)
{
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);
	int cap;
	int was;

	if (d == NULL || d->isBase)
		return;

	was = (gAmmo[weaponId] > 0) ? 1 : 0;
	cap = (d->maxAmmo > 0) ? d->maxAmmo : 9999;
	gAmmo[weaponId] = jer_clamp_int(ammo, 0, cap);

	// pickups auto-equip the first weapon you ever hold (TMB behaviour)
	if (!was && gAmmo[weaponId] > 0 && gSelected == CD2_WID_NONE)
		gSelected = weaponId;
}

void cd2WpnClear(int weaponId)
{
	if (weaponId < 0 || weaponId >= CD2_WID_COUNT)
		return;

	gAmmo[weaponId] = 0;

	if (gSelected == weaponId)
		gSelected = cd2WpnFirstOwned();
}

// Debug / test: every non-base weapon to its max capacity.
void cd2WpnGrantAllMax(void)
{
	int i;

	for (i = 0; i < CD2_WID_COUNT; i++)
	{
		const CD2_WEAPON_DEF* d = gWdefs[i];

		if (d == NULL || d->isBase)
			continue;

		gAmmo[i] = (d->maxAmmo > 0) ? d->maxAmmo : 9999;
	}

	if (gSelected == CD2_WID_NONE)
		gSelected = cd2WpnFirstOwned();
}

void cd2WpnResetAll(void)
{
	int i;

	for (i = 0; i < CD2_WID_COUNT; i++)
		gAmmo[i] = 0;

	gSelected = CD2_WID_NONE;

	cd2RaycastReset();
	cd2ProjectileReset();
	cd2DropReset();
}

// ---------------------------------------------------------------------------
// Shared world helpers
// ---------------------------------------------------------------------------

#ifndef PSX
extern void Debug_AddLine(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
#endif

void cd2WpnLine(const VECTOR* a, const VECTOR* b, int r, int g, int bl)
{
#ifndef PSX
	CVECTOR col = { (unsigned char)r, (unsigned char)g, (unsigned char)bl };

	Debug_AddLineDepth(*((VECTOR*)a), *((VECTOR*)b), col);
#else
	(void)a; (void)b; (void)r; (void)g; (void)bl;
#endif
}

void cd2WpnMark(const VECTOR* p, int r, int g, int bl)
{
#ifndef PSX
	CVECTOR col = { (unsigned char)r, (unsigned char)g, (unsigned char)bl };
	int s = 40;
	VECTOR a, b;

	a.vx = p->vx - s; a.vy = p->vy; a.vz = p->vz;
	b.vx = p->vx + s; b.vy = p->vy; b.vz = p->vz;
	Debug_AddLine(a, b, col);

	a.vx = p->vx; a.vy = p->vy - s; a.vz = p->vz;
	b.vx = p->vx; b.vy = p->vy + s; b.vz = p->vz;
	Debug_AddLine(a, b, col);
#else
	(void)p; (void)r; (void)g; (void)bl;
#endif
}

int cd2WpnPointInCar(const CAR_DATA* cp, const VECTOR* p)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int dx = p->vx - w->t[0];
	int dy = p->vy - w->t[1];
	int dz = p->vz - w->t[2];
	int lx = (int)(((long long)dx * w->m[0][0] + (long long)dy * w->m[1][0] + (long long)dz * w->m[2][0]) >> 12);
	int ly = (int)(((long long)dx * w->m[0][1] + (long long)dy * w->m[1][1] + (long long)dz * w->m[2][1]) >> 12);
	int lz = (int)(((long long)dx * w->m[0][2] + (long long)dy * w->m[1][2] + (long long)dz * w->m[2][2]) >> 12);

	(void)ly;

	if (ABS(lx) > cb->vx / 2)
		return 0;

	if (ABS(lz) > cb->vz / 2)
		return 0;

	if (ABS(ly) > (cb->vy / 2) + 40)
		return 0;

	return 1;
}

void cd2WpnDamageCar(CAR_DATA* cp, const VECTOR* at, int value)
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

	// region: 0 front, 1 back, 2 right, 3 left, 4 top, 5 bottom
	if (dom == lz) region = (lz > 0) ? 0 : 1;
	else if (dom == lx) region = (lx > 0) ? 3 : 2;
	else region = (ly > 0) ? 4 : 5;

	ApplyDamage(cp, (char)region, value, 0);
}

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

// Weapon impact "lever action": the impact point's offset from the car centre
// (the lever) crossed with the shot direction gives the spin axis, so a shot on
// a corner pitches/yaws the car while a dead-centre hit barely moves it. `dir`
// is the impulse direction (the shot's velocity; any magnitude — normalised
// here). Added straight to the world-space angular velocity, which the engine
// integrates as q += (omega (*) q).
void cd2WpnKnock(CAR_DATA* cp, const VECTOR* at, const VECTOR* dir, int strength)
{
	const MATRIX* w = &cp->hd.where;
	int lx = at->vx - w->t[0];
	int ly = at->vy - w->t[1];
	int lz = at->vz - w->t[2];
	int dmag = cd2Isqrt(dir->vx * dir->vx + dir->vy * dir->vy + dir->vz * dir->vz);
	int* av = cp->st.n.angularVelocity;
	int dnx, dny, dnz, cxi, cyi, czi, mag, rate, i;

	if (dmag < 1 || strength <= 0)
		return;

	// unit*4096 shot direction
	dnx = (int)(((long long)dir->vx * 4096) / dmag);
	dny = (int)(((long long)dir->vy * 4096) / dmag);
	dnz = (int)(((long long)dir->vz * 4096) / dmag);

	// cross(lever, dir) >> 12 ~= lever arm (world units) per axis
	cxi = (int)(((long long)ly * dnz - (long long)lz * dny) >> 12);
	cyi = (int)(((long long)lz * dnx - (long long)lx * dnz) >> 12);
	czi = (int)(((long long)lx * dny - (long long)ly * dnx) >> 12);

	mag = cd2Isqrt(cxi * cxi + cyi * cyi + czi * czi);
	if (mag < 1)
		return;

	// spin (world) = unit axis * (arm * hit strength * scale)
	cxi = (int)(((long long)cxi * 4096) / mag);
	cyi = (int)(((long long)cyi * 4096) / mag);
	czi = (int)(((long long)czi * 4096) / mag);

	rate = mag * strength * CD2_KNOCK_SCALE;
	if (rate > CD2_KNOCK_MAX)
		rate = CD2_KNOCK_MAX;

	av[0] += (int)(((long long)cxi * rate) >> 12);
	av[1] += (int)(((long long)cyi * rate) >> 12);
	av[2] += (int)(((long long)czi * rate) >> 12);

	for (i = 0; i < 3; i++)
		av[i] = jer_clamp_int(av[i], -0x800000, 0x800000);
}

// Opponent-AI helper: any live hostile shot closing on `car`?
int cd2WpnIncomingThreat(const CAR_DATA* car, VECTOR* pos, VECTOR* vel)
{
	if (cd2ProjectileThreat(car, pos, vel))
		return 1;

	if (cd2RaycastThreat(car, pos, vel))
		return 1;

	return 0;
}

int cd2WpnPlayerCar(CAR_DATA** out)
{
	int carId = player[0].playerCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	*out = &car_data[carId];

	if ((*out)->controlType != CONTROL_TYPE_PLAYER)
		return 0;

	return 1;
}

void cd2WpnForward(const CAR_DATA* cp, VECTOR* out)
{
	const MATRIX* w = &cp->hd.where;

	out->vx = w->m[0][2];
	out->vy = w->m[1][2];
	out->vz = w->m[2][2];
}

void cd2WpnCarVelocity(const CAR_DATA* cp, VECTOR* out)
{
	// linearVelocity is fixed point (4096 = 1 world unit/frame)
	out->vx = FIXEDH(cp->st.n.linearVelocity[0]);
	out->vy = FIXEDH(cp->st.n.linearVelocity[1]);
	out->vz = FIXEDH(cp->st.n.linearVelocity[2]);
}

void cd2WpnMuzzle(const CAR_DATA* cp, int side, VECTOR* out)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int halfLength = cb->vz;
	int halfWidth = (cb->vx * 102) / 200;	// ~1.02 * vx/2
	int muzzleHeight = cb->vy / 4;

	out->vx = w->t[0] + (int)(((long long)w->m[0][2] * halfLength * 3) >> 14);
	out->vy = w->t[1] + (int)(((long long)w->m[1][2] * halfLength * 3) >> 14);
	out->vz = w->t[2] + (int)(((long long)w->m[2][2] * halfLength * 3) >> 14);

	out->vx += (int)(((long long)w->m[0][0] * halfWidth * side) >> 12);
	out->vy += (int)(((long long)w->m[1][0] * halfWidth * side) >> 12);
	out->vz += (int)(((long long)w->m[2][0] * halfWidth * side) >> 12);

	out->vx += (int)(((long long)w->m[0][1] * muzzleHeight) >> 12);
	out->vy += (int)(((long long)w->m[1][1] * muzzleHeight) >> 12);
	out->vz += (int)(((long long)w->m[2][1] * muzzleHeight) >> 12);
}

// ---------------------------------------------------------------------------
// JER_EVENT_FRAME: class step loop + weapon input
// ---------------------------------------------------------------------------

static int cd2WpnOnFrame(void* ud, void* args)
{
	static int prevRT, prevLB, prevRB;
	static int mgClock;
	static unsigned int dbg;

	CAR_DATA* cp = NULL;
	int pad, lt, rt, rb, lb;

	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled)
	{
		prevRT = prevLB = prevRB = 0;
		return JER_RESULT_CONTINUE;
	}

	// RB (R1) is the weapon-cycle button now, but R1 is ALSO the stock horn:
	// the car loop set horn.on from it this frame, so clear it before the
	// sound pass consumes it (FRAME fires after the car loop, before sound).
	{
		int p;
		for (p = 0; p < 2; p++)
			if (player[p].playerCarId >= 0)
				player[p].horn.on = 0;
	}

	if (!cd2WpnPlayerCar(&cp))
	{
		prevRT = prevLB = prevRB = 0;

		// no player car: nothing to fire, but live shots keep flying
		cd2RaycastStep();
		cd2ProjectileStep();
		cd2DropStep();
		return JER_RESULT_CONTINUE;
	}

	pad = Pads[(unsigned char)*cp->ai.padid].mapped;

	if (gCd2Cfg.debugLog)
	{
		static int probed = 0;

		if (!probed)
		{
			probed = 1;
			printInfo("[combatd2] missile model '%s' resolved=%d scale=%d sound=%d\n",
				gCd2Cfg.missileModel, cd2ProjectileModelValid(),
				gCd2Cfg.missileScale, gCd2Cfg.missileSound);
		}
	}

	lt = (pad & CD2_WPN_FIRE_BASE) ? 1 : 0;
	rt = (pad & CD2_WPN_FIRE_SEL) ? 1 : 0;
	rb = (pad & CD2_WPN_NEXT) ? 1 : 0;
	lb = (pad & CD2_WPN_PREV) ? 1 : 0;

	if (gCd2Cfg.debugLog && (dbg++ & 31) == 0)
		printInfo("[combatd2] wpn frame: pad=0x%04X LT=%d RT=%d LB=%d RB=%d sel=%d ammo=%d\n",
			pad, lt, rt, lb, rb, gSelected, (gSelected != CD2_WID_NONE) ? gAmmo[gSelected] : 0);

	// weapon select: bumpers, edge triggered
	if (rb && !prevRB)
		cd2WpnCycle(+1);
	if (lb && !prevLB)
		cd2WpnCycle(-1);

	// base machine gun: LT held, auto fire
	if (lt)
	{
		const CD2_WEAPON_DEF* d = cd2WpnDef(CD2_WID_MG);

		if (mgClock > 0)
			mgClock--;
		else if (d != NULL && d->fire != NULL)
		{
			d->fire(cp);
			mgClock = (d->fireInterval > 0) ? d->fireInterval : 4;
		}
	}
	else
		mgClock = 0;

	// selected weapon: RT, single shot per press
	if (rt && !prevRT)
	{
		const CD2_WEAPON_DEF* d = cd2WpnDef(gSelected);

		if (d != NULL && cd2WpnOwns(gSelected) && gAmmo[gSelected] > 0)
		{
			if (d->fire != NULL)
				d->fire(cp);

			gAmmo[gSelected]--;

			if (gAmmo[gSelected] <= 0)
				cd2WpnClear(gSelected);
		}
		else
		{
			// no primary armed: fall back to the base machine gun so the
			// right trigger is never dead
			const CD2_WEAPON_DEF* mg = cd2WpnDef(CD2_WID_MG);

			if (mg != NULL && mg->fire != NULL)
				mg->fire(cp);
		}
	}

	prevRT = rt;
	prevLB = lb;
	prevRB = rb;

	// advance the live shots AFTER the fire pass so a shot fired this frame
	// registers its first move + impact immediately (not one frame late)
	cd2RaycastStep();
	cd2ProjectileStep();
	cd2DropStep();

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_CAMERA_LOOK: free the side-view keys for weapon fire
// ---------------------------------------------------------------------------
static int cd2WpnOnLook(void* ud, void* args)
{
	JER_ARGS_CAMERA_LOOK* a = (JER_ARGS_CAMERA_LOOK*)args;
	PLAYER* lp = (PLAYER*)a->player;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	// While driving, the L2/R2 side-view look is replaced by weapon fire, so
	// suppress the stock look branches (leaves the engine's normal
	// settle-back lerp untouched: gripOrbit stays 0).
	if (lp != NULL && lp->playerCarId >= 0)
		a->suppress = 1;

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_DRAW_WORLD: projectiles + drops into the real ordering table
// ---------------------------------------------------------------------------
static int cd2WpnOnDrawWorld(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	cd2RaycastDraw();
	cd2ProjectileDraw();
	cd2DropDraw();

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

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	if (!cd2WpnPlayerCar(&cp))
		return JER_RESULT_CONTINUE;

	SetTextColour(200, 200, 200);
	sprintf(text, "MG (LT)");
	PrintString(text, 20, 210);

	if (gSelected == CD2_WID_NONE)
	{
		SetTextColour(160, 160, 160);
		sprintf(text, "> (no primary weapons)");
		PrintString(text, 20, 222);
	}
	else
	{
		SetTextColour(255, 200, 90);
		sprintf(text, "> %s x%d (RT)", cd2WpnName(gSelected), gAmmo[gSelected]);
		PrintString(text, 20, 222);
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_GAME_START: fresh level = fresh inventory (+ test grant)
// ---------------------------------------------------------------------------
static int cd2WpnOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	cd2WpnResetAll();

	// test/dev: spawn with the machine gun plus EVERY weapon at max capacity
	if (gCd2Cfg.allWeapons)
		cd2WpnGrantAllMax();

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_combatd2_entry in combatd2.c)
// ---------------------------------------------------------------------------
void cd2WeaponsRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2WpnOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2WpnOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA_LOOK, cd2WpnOnLook, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, cd2WpnOnOverlay, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, cd2WpnOnDrawWorld, NULL, 0);

	ctx->jer_log(ctx, "[combatd2] weapons registered (SDK v%d)\n", ctx->sdkVersion);
}
