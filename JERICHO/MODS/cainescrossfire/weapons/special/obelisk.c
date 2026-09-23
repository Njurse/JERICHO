// weapons/special/obelisk.c — OBELISK's special: MISSILE BARRAGE.
//
// A three-second salvo. Twice a volley - a pair at a time, fired from the
// MIDSECTION out of BOTH flanks at once - cheap missiles pour out. Every missile
// leaves OUTWARD off its OWN flank (away from the car body, on a throw that
// wanders from volley to volley) and each one picks its own spawn point around
// the launcher and its own line inside that wedge, so the swarm scatters instead
// of hosing out of two fixed points. The pool then HOLDS that outward bearing for
// the weapon's arming window (CD2_WEAPON_DEF.homingDelay) before the seeker
// engages, so a salvo splays out of the car and only then comes round onto
// whatever it is chasing - buzzing in like hornets, not turning on a rail.
// Individually they barely scratch; there are a great many of them, and together
// they are a barrage.
//
// The arming window is also what keeps the salvo off its OWN car: the launcher is
// no longer inside the turn radius of a shot that is already seeking, and the
// blast of a missile that connects beside the car spares its owner (see
// cd2ProjectileImpact in weapons/projectile/projectile.c).
//
// First cut: the launch bearing is the WHOLE of it (each missile is left to the
// pool, which curves it home with d.homing); there is no per-projectile steering
// hook to fold a real zig-zag into, so the zig-zag lives in the launch pattern.

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

#define CD2_OBELISK_FRAMES		180	// 3s of barrage
#define CD2_OBELISK_VOLLEY_EVERY	7	// frames between volleys (2 missiles each, ~8/s)
#define CD2_OBELISK_OUT		1000	// OUTWARD launch bearing per flank (4096 scale,
					// ~14 deg off the nose) - each missile leaves
					// away from the car body, not across it
#define CD2_OBELISK_SPREAD		540	// per-missile launch jitter (4096 scale, ~47 deg total)
#define CD2_OBELISK_OUT_VARY		400	// how far a volley's outward throw wanders
#define CD2_OBELISK_LIFT_SPREAD		180	// vertical part of the jitter
#define CD2_OBELISK_SIDE_JITTER		200	// extra OUTWARD scatter at the spawn point
#define CD2_OBELISK_ALONG_JITTER	420	// fore/aft scatter along the car
#define CD2_OBELISK_SPAWN_LIFT		110	// vertical scatter at the spawn point
#define CD2_OBELISK_SPEED		2600

static int gSalvoFrames[MAX_CARS];	// barrage frames left
static int gSalvoAcc[MAX_CARS];		// frames until the next volley
static int gSalvoVolley[MAX_CARS];	// volleys fired so far (2 missiles each)
static int gSalvoChannel = -1;		// the launch report

// The MIDSECTION muzzle: the car's own origin, pushed out to one side (there is
// no forward offset - the missiles leave from the flanks, not the nose), raised
// slightly along the car's up axis.
//
// `outExtra`, `alongExtra` and `liftExtra` scatter the point in the car's own
// frame: further OUT beyond the body, fore/aft along the car, and up/down.
// (0,0,0) is the plain flank muzzle - which is what the muzzle FLASH is drawn
// at, because the flashes are the car's launchers rather than the shots.
static void cd2ObeliskMuzzleEx(const CAR_DATA* cp, int side, int outExtra,
				int alongExtra, int liftExtra, VECTOR* out)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int halfWidth = (cb->vx * 108) / 200 + outExtra;	// just outside the body side
	int h = cb->vy / 3 + liftExtra;

	out->vx = w->t[0];
	out->vy = w->t[1];
	out->vz = w->t[2];

	out->vx += (int)(((long long)w->m[0][0] * halfWidth * side) >> 12);
	out->vy += (int)(((long long)w->m[1][0] * halfWidth * side) >> 12);
	out->vz += (int)(((long long)w->m[2][0] * halfWidth * side) >> 12);

	// fore/aft along the car, so a volley's spawn points are spread down the
	// length of the flank instead of all leaving the same spot
	out->vx += (int)(((long long)w->m[0][2] * alongExtra) >> 12);
	out->vy += (int)(((long long)w->m[1][2] * alongExtra) >> 12);
	out->vz += (int)(((long long)w->m[2][2] * alongExtra) >> 12);

	out->vx += (int)(((long long)w->m[0][1] * h) >> 12);
	out->vy += (int)(((long long)w->m[1][1] * h) >> 12);
	out->vz += (int)(((long long)w->m[2][1] * h) >> 12);
}

// The plain flank muzzle, used by the flash.
static void cd2ObeliskMuzzle(const CAR_DATA* cp, int side, VECTOR* out)
{
	cd2ObeliskMuzzleEx(cp, side, 0, 0, 0, out);
}

// Where ONE missile is born: the flank muzzle, scattered. Each shot gets its own
// extra offset OUTWARD beyond the body, its own fore/aft offset along the car and
// its own height, so a volley does not pour out of two fixed points - the swarm's
// origins are a cloud around the launcher, which is what stops the barrage
// reading as a hose.
static void cd2ObeliskLaunchFrom(const CAR_DATA* cp, int side, VECTOR* out)
{
	int outExtra = cd2WpnRand(CD2_OBELISK_SIDE_JITTER + 1);
	int alongExtra = cd2WpnRand(CD2_OBELISK_ALONG_JITTER * 2 + 1) - CD2_OBELISK_ALONG_JITTER;
	int liftExtra = cd2WpnRand(CD2_OBELISK_SPAWN_LIFT * 2 + 1) - CD2_OBELISK_SPAWN_LIFT;

	cd2ObeliskMuzzleEx(cp, side, outExtra, alongExtra, liftExtra, out);
}

// Fire one missile out of `side` (-1 left, +1 right). `outward` is this volley's
// outward throw: the missile leaves along its OWN side, AWAY from the car body
// (the two flanks take opposite signs, so a volley splays out of both sides and
// both missiles have to come back across to reach anything).
//
// Every missile then gets its own jitter on top. A salvo that left on a fixed
// bearing read as a machine putting things in a pattern; the point is that the
// swarm looks HUNGRY - each one thrown slightly wrong, some climbing, some
// dropping, the whole stream squirming toward whatever it is chasing.
//
// The pool then holds this bearing for the weapon's arming window
// (CD2_WEAPON_DEF.homingDelay) before the seeker takes over, so the outward
// launch is actually SEEN - and, more to the point, each missile is pointed
// away from the car that fired it while it is still close enough to matter.
static void cd2ObeliskLaunch(CAR_DATA* cp, int side, int outward)
{
	const MATRIX* w = &cp->hd.where;
	VECTOR muzzle, dir, vel, fwd;
	long long rx = w->m[0][0], ry = w->m[1][0], rz = w->m[2][0];	// the car's lateral axis
	int lat = outward * side;
	int lift;

	// its own line, and its own height
	lat += cd2WpnRand(CD2_OBELISK_SPREAD * 2 + 1) - CD2_OBELISK_SPREAD;
	lift = cd2WpnRand(CD2_OBELISK_LIFT_SPREAD * 2 + 1) - CD2_OBELISK_LIFT_SPREAD;

	cd2WpnForward(cp, &fwd);
	cd2ObeliskLaunchFrom(cp, side, &muzzle);

	dir.vx = fwd.vx + (int)((rx * lat) >> 12);
	dir.vy = fwd.vy + (int)((ry * lat) >> 12) + lift;
	dir.vz = fwd.vz + (int)((rz * lat) >> 12);

	vel.vx = (int)(((long long)dir.vx * CD2_OBELISK_SPEED) >> 12);
	vel.vy = (int)(((long long)dir.vy * CD2_OBELISK_SPEED) >> 12);
	vel.vz = (int)(((long long)dir.vz * CD2_OBELISK_SPEED) >> 12);

	// Observability, because the whole point of the launch is the SHAPE of it:
	// `lat` is the sideways throw (negative on the left flank = outward) and the
	// spawn offsets are in the car's own frame (lateral negative = left).
	if (gCd2Cfg.debugLog)
	{
		long long ox = muzzle.vx - w->t[0];
		long long oy = muzzle.vy - w->t[1];
		long long oz = muzzle.vz - w->t[2];

		printInfo("[cainescrossfire] obelisk launch: side=%d lat=%d lift=%d spawn lat=%d along=%d up=%d\n",
			side, lat, lift,
			(int)((ox * w->m[0][0] + oy * w->m[1][0] + oz * w->m[2][0]) >> 12),
			(int)((ox * w->m[0][2] + oy * w->m[1][2] + oz * w->m[2][2]) >> 12),
			(int)((ox * w->m[0][1] + oy * w->m[1][1] + oz * w->m[2][1]) >> 12));
	}

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
			// one volley: both flanks at once, each missile thrown OUTWARD off
			// its own side, and the outward throw wandering volley to volley,
			// so the stream fans and snakes instead of holding one clean line
			int out = CD2_OBELISK_OUT;

			out += cd2WpnRand(CD2_OBELISK_OUT_VARY * 2 + 1) - CD2_OBELISK_OUT_VARY;

			gSalvoAcc[i] = 0;
			gSalvoVolley[i]++;

			cd2ObeliskLaunch(cp, -1, out);
			cd2ObeliskLaunch(cp, 1, out);
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
	d.displayName = "Syndicate Hellstorm";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isSpecial = 1;

	d.leanOut = 0;			// the missiles leave the flanks, not a window

	d.maxAmmo = 2;			// one salvo per charge
	d.fireInterval = 30;
	d.refireCooldown = 900;		// profile recharge: 30s

	d.damage = 55;			// individually weak...
	d.speed = CD2_OBELISK_SPEED;
	d.range = 9000;			// ~3.5 frames at that speed: enough flight for the
					// outward launch to read AND for the seeker still
					// to bring the shot round
	d.life = 0;

	d.splashRadius = 200;
	d.splashDamage = 60;
	d.explosionEffect = LITTLE_BANG;

	d.homing = 1;
	d.homingRate = 160;		// ...but they curve hard onto what they chase
	d.homingDelay = 1;		// ...after a beat on the launch bearing: 1-2
	d.homingDelayVary = 1;		// frames (the jitter staggers a volley), so the
					// pair leaves the flanks OUTWARD and clears the
					// car before anything turns it back across
	d.collideScenery = 1;

	d.fireCone = 900;

	d.colR = 226; d.colG = 206; d.colB = 130;	// pale gold

	d.fire = cd2ObeliskFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialObelisk = cd2MakeObeliskDef();
