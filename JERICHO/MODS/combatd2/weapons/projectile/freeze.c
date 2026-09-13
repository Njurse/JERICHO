// weapons/projectile/freeze.c — the FREEZE MISSILE (projectile weapon) and the
// "frozen car" status it applies.
//
// A primary weapon: a moderate seek (def->homingRate sits between the zoomy's
// lazy 40 and the seeker's 340), no damage of its own. When it hits a car it
// ENCASES it in ice for CD2_FREEZE_FRAMES (5s):
//
//   * body renders a bright flat cyan (JER_EVENT_CAR_DRAW_COLOR tint) so it
//     reads as frozen solid;
//   * grip is cut (JER_EVENT_CAR_FRICTION) so it slides like it's on ice;
//   * controls lock (JER_EVENT_CAR_STEP, which runs AFTER the player's pad pass
//     AND after the AI's own input write, so it wins for both): no gas, no
//     brake, no handbrake, and the steering is pinned to whatever it was the
//     instant it froze.
//
// Status lives here next to the weapon because the weapon's only job is to
// call cd2FreezeApply(); the projectile pool does that on a car hit when
// def->freezeFrames > 0.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "system.h"
#include "jericho.h"
#include "jer_events.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/fx/fx.h"

#include <string.h>

// 5 seconds at the sim's 60 frames/sec (the same unit the weapon defs use for
// refireCooldown: 120 = 2s).
#define CD2_FREEZE_FRAMES	300

// grip = stock / this while frozen (lower = icier). 5 leaves ~20% bite.
#define CD2_FREEZE_GRIP_DIV	5

// Priorities: lower runs first, so the freeze hooks run AFTER combatd2's own
// CAR_* tuners and after the opponent AI's input write, and thus take the last
// word for the frame.
#define CD2_FREEZE_PRIO		20

// The ice body colour (flat, full brightness).
static const int gFreezeTintR = 170;
static const int gFreezeTintG = 238;
static const int gFreezeTintB = 255;

static int gFreezeFrames[MAX_CARS];	// frames of freeze left (0 = not frozen)
static short gFreezeAngle[MAX_CARS];	// steering pinned at the moment of freezing

void cd2FreezeApply(int carId, int frames)
{
	if (carId < 0 || carId >= MAX_CARS || frames <= 0)
		return;

	// pin the CURRENT steering so it visibly stays put ("turning stays the
	// value it was at the time of freezing")
	gFreezeAngle[carId] = car_data[carId].wheel_angle;
	gFreezeFrames[carId] = frames;

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] FREEZE car=%d frames=%d angle=%d\n",
			carId, frames, (int)gFreezeAngle[carId]);
}

int cd2FreezeActive(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	return gFreezeFrames[carId] > 0;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static int cd2FreezeOnStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;

	(void)ud;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS || gFreezeFrames[cp->id] <= 0)
		return JER_RESULT_CONTINUE;

	// last word before the physics step: no gas, no brake, no handbrake, no
	// burnout, and the steering stays where it froze
	cp->thrust = 0;
	cp->handbrake = 0;
	cp->wheelspin = 0;
	cp->wheel_angle = gFreezeAngle[cp->id];

	return JER_RESULT_CONTINUE;
}

static int cd2FreezeOnFriction(void* ud, void* args)
{
	JER_ARGS_CAR_FRICTION* a = (JER_ARGS_CAR_FRICTION*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;

	(void)ud;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS || gFreezeFrames[cp->id] <= 0)
		return JER_RESULT_CONTINUE;

	a->frontFS = a->frontFS / CD2_FREEZE_GRIP_DIV;
	a->rearFS = a->rearFS / CD2_FREEZE_GRIP_DIV;

	return JER_RESULT_CONTINUE;
}

static int cd2FreezeOnDrawColor(void* ud, void* args)
{
	JER_ARGS_CAR_DRAW_COLOR* a = (JER_ARGS_CAR_DRAW_COLOR*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;

	(void)ud;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS || gFreezeFrames[cp->id] <= 0)
		return JER_RESULT_CONTINUE;

	// encased in ice: a bright flat cyan body. A totaled wreck (flatBlack) is
	// left black - a burnt-out car doesn't look icy.
	if (!a->flatBlack)
	{
		a->tintR = gFreezeTintR;
		a->tintG = gFreezeTintG;
		a->tintB = gFreezeTintB;
	}

	return JER_RESULT_CONTINUE;
}

static int cd2FreezeOnFrame(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (gFreezeFrames[i] <= 0)
			continue;

		// a car that went away (or was reset) stops being frozen
		if (car_data[i].controlType == CONTROL_TYPE_NONE || car_data[i].ap.carCos == NULL)
		{
			gFreezeFrames[i] = 0;
			continue;
		}

		if (--gFreezeFrames[i] <= 0)
		{
			gFreezeFrames[i] = 0;

			if (gCd2Cfg.debugLog)
				printInfo("[combatd2] FREEZE car=%d thawed\n", i);
		}
	}

	return JER_RESULT_CONTINUE;
}

static int cd2FreezeOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gFreezeFrames[i] = 0;
		gFreezeAngle[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

void cd2FreezeRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2FreezeOnStep, NULL, CD2_FREEZE_PRIO);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_FRICTION, cd2FreezeOnFriction, NULL, CD2_FREEZE_PRIO);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW_COLOR, cd2FreezeOnDrawColor, NULL, CD2_FREEZE_PRIO);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2FreezeOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2FreezeOnGameStart, NULL, 0);

	ctx->jer_log(ctx, "[combatd2] freeze status registered\n");
}

// ---------------------------------------------------------------------------
// The weapon
// ---------------------------------------------------------------------------
static int gFreezeChannel = -1;	// voice reserved for the launch sound

static void cd2FreezeFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR muzzle;
	VECTOR dir;
	VECTOR vel;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	cd2WpnMuzzle(cp, 0, &muzzle);	// centred (side 0)
	cd2WpnForward(cp, &dir);

	vel.vx = (int)(((long long)dir.vx * cd2WdefFreeze.speed) >> 12);
	vel.vy = (int)(((long long)dir.vy * cd2WdefFreeze.speed) >> 12);
	vel.vz = (int)(((long long)dir.vz * cd2WdefFreeze.speed) >> 12);

	cd2ProjectileSpawn(&cd2WdefFreeze, cp, &muzzle, &vel, &dir);

	if (gFreezeChannel < 0)
	{
		// GetFreeChannel(1), not GetFreeChannel(): sound.h declares the
		// parameter as 'int force = 1' (a C++ default argument) and this
		// module is C, so the default never applies and must be passed.
		gFreezeChannel = GetFreeChannel(1);
		LockChannel(gFreezeChannel);

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] freeze sound: channel=%d locked\n", gFreezeChannel);
	}

	// a cold, descending launch
	Start3DSoundVolPitch(gFreezeChannel, SOUND_BANK_SFX, 5,
		muzzle.vx, muzzle.vy, muzzle.vz, -2000, 4096 - 768);
}

static CD2_WEAPON_DEF cd2MakeFreezeDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_FREEZE;
	d.name = "FREEZE";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isBase = 0;
	d.hidden = 0;
	d.pickupEnabled = 1;
	d.pickupAmmo = 3;

	d.maxAmmo = 6;
	d.fireInterval = 70;
	d.refireCooldown = 180;	// 3s minimum between freezes

	d.damage = 0;		// no damage on its own
	d.speed = 1250;		// slower: it seeks, so it can afford to
	d.range = 14000;
	d.life = 0;

	d.splashRadius = 0;	// no blast: the freeze is the payload
	d.splashDamage = 0;
	d.explosionEffect = LITTLE_BANG;
	d.impactFx = CD2_FX_FREEZE;	// cold pale-blue puff

	d.homing = 1;
	d.homingRate = 170;	// MODERATE seek: between zoomy (40) and seeker (340)
	d.collideScenery = 1;

	d.fireCone = 700;	// it steers, so a bigger launch error still connects

	d.colR = 170; d.colG = 230; d.colB = 255;	// icy cyan body

	// the payload: encase the car in ice for 5 seconds
	d.freezeFrames = CD2_FREEZE_FRAMES;

	d.fire = cd2FreezeFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefFreeze = cd2MakeFreezeDef();
