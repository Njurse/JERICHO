// weapons/special/bootlegger.c — BOOTLEGGER's special: the GUNNER'S WINDOW GUN.
//
// PLACEHOLDER. Five seconds of machine gun out of the gunner's side, wound up
// from a slow chatter to a rate nothing can sit in front of for long. Each
// bullet is nearly nothing on its own - the point is how many arrive, and how
// fast it gets there: the interval ramps linearly from CD2_BOOTLEGGER_SLOW to
// CD2_BOOTLEGGER_FAST over the first second, so the gun reads as spinning up.
//
// The bullets are the ordinary RAYCAST class (the same pool the base MG uses),
// so they travel, hit, stop on scenery and follow the tracer draw for free; only
// the def differs - a lower damage and an amber-white colour of its own.

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
#include "weapons/core/crew.h"		/* CD2_CREW_GUNNER - the side this gun is on */
#include "weapons/special/special.h"

#include <string.h>

#define CD2_BOOTLEGGER_FRAMES	150	// 5s
#define CD2_BOOTLEGGER_SLOW	9	// frames between bullets as it opens
#define CD2_BOOTLEGGER_FAST	2	// ...and once it is wound up
#define CD2_BOOTLEGGER_RAMP	30	// frames to wind all the way up (1s)
#define CD2_BOOTLEGGER_SIDE	1	// the gunner's muzzle (right)

static int gBootFrames[MAX_CARS];	// burst frames left
static int gBootAcc[MAX_CARS];		// frames until the next bullet
static int gBootChannel = -1;

// One bullet, out of the gunner's side.
static void cd2BootleggerShot(CAR_DATA* cp)
{
	VECTOR muzzle, dir;

	cd2WpnMuzzle(cp, CD2_BOOTLEGGER_SIDE, &muzzle);
	cd2WpnForward(cp, &dir);

	cd2RaycastSpawn(&cd2WdefSpecialBootlegger, cp, &muzzle, &dir);
}

static void cd2BootleggerFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	gBootFrames[cp->id] = CD2_BOOTLEGGER_FRAMES;
	gBootAcc[cp->id] = 0;

	printInfo("[cainescrossfire] bootlegger: gunner burst on car=%d (%d frames, %d->%d frame interval)\n",
		cp->id, CD2_BOOTLEGGER_FRAMES, CD2_BOOTLEGGER_SLOW, CD2_BOOTLEGGER_FAST);

	// the gun firing: the gunner is out of his window for the whole burst
	cd2CrewNotifyFire(cp, CD2_CREW_GUNNER);

	// and the first round leaves immediately
	cd2BootleggerShot(cp);
}

static int cd2BootleggerOnFrame(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		int elapsed, interval;

		if (gBootFrames[i] <= 0)
			continue;

		if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
		{
			gBootFrames[i] = 0;
			continue;
		}

		// the wind-up: a straight line from the slow interval to the fast one
		// over the first CD2_BOOTLEGGER_RAMP frames, then flat out
		elapsed = CD2_BOOTLEGGER_FRAMES - gBootFrames[i];
		if (elapsed > CD2_BOOTLEGGER_RAMP)
			elapsed = CD2_BOOTLEGGER_RAMP;

		interval = CD2_BOOTLEGGER_SLOW -
			((CD2_BOOTLEGGER_SLOW - CD2_BOOTLEGGER_FAST) * elapsed) / CD2_BOOTLEGGER_RAMP;

		if (interval < CD2_BOOTLEGGER_FAST)
			interval = CD2_BOOTLEGGER_FAST;

		if (++gBootAcc[i] >= interval)
		{
			gBootAcc[i] = 0;

			cd2BootleggerShot(cp);

			// the gunner stays out while it is firing
			cd2CrewNotifyFire(cp, CD2_CREW_GUNNER);

			if (gBootChannel < 0)
			{
				gBootChannel = cd2TakeVoice();
			}

			if (gBootChannel >= 0)
				Start3DSoundVolPitch(gBootChannel, SOUND_BANK_SFX, 5,
					cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -2600, 4096 + 2600);
		}

		if (--gBootFrames[i] <= 0)
		{
			gBootFrames[i] = 0;

			printInfo("[cainescrossfire] bootlegger: burst done (car=%d)\n", i);
		}
	}

	return JER_RESULT_CONTINUE;
}

static int cd2BootleggerOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gBootFrames[i] = 0;
		gBootAcc[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

void cd2SpecialBootleggerRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2BootleggerOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2BootleggerOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2BootleggerOnGameStart, NULL, 0);
}

static CD2_WEAPON_DEF cd2MakeBootleggerDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_BOOTLEGGER;
	d.name = "special_bootlegger";
	d.displayName = "Lead Hail";
	d.cls = CD2_WCLS_RAYCAST;

	d.isSpecial = 1;

	d.leanOut = CD2_CREW_GUNNER;	// the gunner's window, not the driver's

	d.maxAmmo = 2;			// placeholder
	d.fireInterval = 30;
	d.refireCooldown = 600;		// placeholder: 20s

	d.damage = 55;			// per bullet: almost nothing...
	d.speed = 1200;			// ...but they arrive in a stream
	d.range = 14000;
	d.life = 4;

	d.explosionEffect = LITTLE_BANG;
	d.collideScenery = 1;
	d.fireCone = 500;

	d.colR = 255; d.colG = 240; d.colB = 170;

	d.fire = cd2BootleggerFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialBootlegger = cd2MakeBootleggerDef();
