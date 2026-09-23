// weapons/special/obelisk.c — OBELISK's special: MISSILE BARRAGE.
//
// A three-second salvo. Twice a volley - a pair at a time, fired from the
// MIDSECTION out of BOTH sides of the car at once - cheap missiles pour out,
// each one weaving: the pair leave crossed over, and the weave flips every
// volley, so the swarm snakes toward whatever it is chasing instead of flying
// in a straight line. Individually they barely scratch; there are a great many
// of them, and together they are a barrage.
//
// First cut: the weave is the launch angle (each missile is left to the pool,
// which curves it home with d.homing); there is no per-projectile steering hook
// to fold a real zig-zag into, so the zig-zag lives in the launch pattern.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "jericho.h"
#include "jer_events.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/special/special.h"

#include <string.h>

#define CD2_OBELISK_FRAMES		90	// 3s of barrage
#define CD2_OBELISK_VOLLEY_EVERY	7	// frames between volleys (2 missiles each, ~8/s)
#define CD2_OBELISK_WEAVE		760	// how hard a volley is thrown sideways (4096 scale)
#define CD2_OBELISK_SPREAD		540	// per-missile launch jitter (4096 scale, ~47 deg total)
#define CD2_OBELISK_WEAVE_VARY		300	// how far a volley's weave wanders off its line
#define CD2_OBELISK_LIFT_SPREAD		180	// vertical part of the jitter
#define CD2_OBELISK_SPEED		2600

static int gSalvoFrames[MAX_CARS];	// barrage frames left
static int gSalvoAcc[MAX_CARS];		// frames until the next volley
static int gSalvoVolley[MAX_CARS];	// which volley (the weave flips on it)
static int gSalvoChannel = -1;		// the launch report

// The MIDSECTION muzzle: the car's own origin, pushed out to one side (there is
// no forward offset - the missiles leave from the flanks, not the nose), raised
// slightly along the car's up axis.
static void cd2ObeliskMuzzle(const CAR_DATA* cp, int side, VECTOR* out)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int halfWidth = (cb->vx * 108) / 200;	// just outside the body side
	int h = cb->vy / 3;

	out->vx = w->t[0];
	out->vy = w->t[1];
	out->vz = w->t[2];

	out->vx += (int)(((long long)w->m[0][0] * halfWidth * side) >> 12);
	out->vy += (int)(((long long)w->m[1][0] * halfWidth * side) >> 12);
	out->vz += (int)(((long long)w->m[2][0] * halfWidth * side) >> 12);

	out->vx += (int)(((long long)w->m[0][1] * h) >> 12);
	out->vy += (int)(((long long)w->m[1][1] * h) >> 12);
	out->vz += (int)(((long long)w->m[2][1] * h) >> 12);
}

// Fire one missile out of `side` (-1 left, +1 right). `weave` is the sideways
// throw for this volley; the sides take OPPOSITE signs, so the pair crosses.
//
// Every missile then gets its own jitter on top. A salvo that left on a fixed
// bearing read as a machine putting things in a pattern; the point is that the
// swarm looks HUNGRY - each one thrown slightly wrong, some climbing, some
// dropping, the whole stream squirming toward whatever it is chasing.
static void cd2ObeliskLaunch(CAR_DATA* cp, int side, int weave)
{
	const MATRIX* w = &cp->hd.where;
	VECTOR muzzle, dir, vel, fwd;
	long long rx = w->m[0][0], ry = w->m[1][0], rz = w->m[2][0];	// the car's lateral axis
	int lat = weave * -side;
	int lift;

	// its own line, and its own height
	lat += cd2WpnRand(CD2_OBELISK_SPREAD * 2 + 1) - CD2_OBELISK_SPREAD;
	lift = cd2WpnRand(CD2_OBELISK_LIFT_SPREAD * 2 + 1) - CD2_OBELISK_LIFT_SPREAD;

	cd2WpnForward(cp, &fwd);
	cd2ObeliskMuzzle(cp, side, &muzzle);

	dir.vx = fwd.vx + (int)((rx * lat) >> 12);
	dir.vy = fwd.vy + (int)((ry * lat) >> 12) + lift;
	dir.vz = fwd.vz + (int)((rz * lat) >> 12);

	vel.vx = (int)(((long long)dir.vx * CD2_OBELISK_SPEED) >> 12);
	vel.vy = (int)(((long long)dir.vy * CD2_OBELISK_SPEED) >> 12);
	vel.vz = (int)(((long long)dir.vz * CD2_OBELISK_SPEED) >> 12);

	cd2ProjectileSpawn(&cd2WdefSpecialObelisk, cp, &muzzle, &vel, &dir);
}

static void cd2ObeliskFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	if (gSalvoFrames[cp->id] > 0)
		return;				// the barrage is already running

	gSalvoFrames[cp->id] = CD2_OBELISK_FRAMES;
	gSalvoAcc[cp->id] = 0;
	gSalvoVolley[cp->id] = 0;

	printInfo("[cainescrossfire] obelisk: salvo fired (car=%d, %d frames, a pair every %d)\n",
		cp->id, CD2_OBELISK_FRAMES, CD2_OBELISK_VOLLEY_EVERY);

	if (gSalvoChannel < 0)
	{
		gSalvoChannel = cd2TakeVoice();
	}

	if (gSalvoChannel >= 0)
		Start3DSoundVolPitch(gSalvoChannel, SOUND_BANK_SFX, 6,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -1800, 4096 - 900);
}

static int cd2ObeliskOnFrame(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (gSalvoFrames[i] <= 0)
			continue;

		if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
		{
			gSalvoFrames[i] = 0;
			continue;
		}

		if (++gSalvoAcc[i] >= CD2_OBELISK_VOLLEY_EVERY)
		{
			// one volley: both flanks at once, the weave flipping each time AND
			// wandering, so the stream snakes instead of holding a clean line
			int weave = (gSalvoVolley[i] & 1) ? CD2_OBELISK_WEAVE : -CD2_OBELISK_WEAVE;

			weave += cd2WpnRand(CD2_OBELISK_WEAVE_VARY * 2 + 1) - CD2_OBELISK_WEAVE_VARY;

			gSalvoAcc[i] = 0;
			gSalvoVolley[i]++;

			cd2ObeliskLaunch(cp, -1, weave);
			cd2ObeliskLaunch(cp, 1, weave);
		}

		// the muzzle flash, out of each flank
		{
			VECTOR a, b;

			cd2ObeliskMuzzle(cp, -1, &a);
			b.vx = a.vx; b.vy = a.vy; b.vz = a.vz;
			cd2WpnMark(&b, 255, 240, 170);

			cd2ObeliskMuzzle(cp, 1, &a);
			b = a;
			cd2WpnMark(&b, 255, 240, 170);
		}

		if (--gSalvoFrames[i] <= 0)
		{
			gSalvoFrames[i] = 0;

			printInfo("[cainescrossfire] obelisk: salvo done (car=%d, %d missiles)\n",
				i, gSalvoVolley[i] * 2);
		}
	}

	return JER_RESULT_CONTINUE;
}

static int cd2ObeliskOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gSalvoFrames[i] = 0;
		gSalvoAcc[i] = 0;
		gSalvoVolley[i] = 0;
	}

	if (gSalvoChannel >= 0)
		StopChannel(gSalvoChannel);

	return JER_RESULT_CONTINUE;
}

void cd2SpecialObeliskRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2ObeliskOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2ObeliskOnGameStart, NULL, 0);
	// the same reset when the game returns to the frontend menus
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2ObeliskOnGameStart, NULL, 0);
}

static CD2_WEAPON_DEF cd2MakeObeliskDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_OBELISK;
	d.name = "special_obelisk";
	d.displayName = "Missile Barrage";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isSpecial = 1;

	d.leanOut = 0;			// the missiles leave the flanks, not a window

	d.maxAmmo = 1;			// one salvo per charge
	d.fireInterval = 30;
	d.refireCooldown = 900;		// profile recharge: 30s

	d.damage = 55;			// individually weak...
	d.speed = CD2_OBELISK_SPEED;
	d.range = 6000;
	d.life = 0;

	d.splashRadius = 200;
	d.splashDamage = 60;
	d.explosionEffect = LITTLE_BANG;

	d.homing = 1;
	d.homingRate = 200;		// ...but they curve hard onto what they chase
	d.collideScenery = 1;

	d.fireCone = 900;

	d.colR = 226; d.colG = 206; d.colB = 130;	// pale gold

	d.fire = cd2ObeliskFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialObelisk = cd2MakeObeliskDef();
