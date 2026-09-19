// weapons/special/highwayman.c — HIGHWAYMAN's special: BREATH OF FIRE.
//
// First press starts a four-second flame: a narrow cone of fire pours from the
// car's nose (short-range raycast flame particles, spawned a couple a frame,
// drawn as lines). While the flame is running, tapping fire again launches a
// HOMING FIREBALL - a projectile that seeks a target and explodes for extra
// damage on impact.
//
// First cut: the flame is raycast particles in a tight cone (they damage what
// they reach within a short range); the fireball is the projectile pool.

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

#define CD2_FLAME_FRAMES	120	// 4s
#define CD2_FLAME_SPAWN_EVERY	2	// frames between a flame particle burst
#define CD2_FLAME_CONE		260	// narrow cone half-width (car-relative fixed point)
#define CD2_FIREBALL_SPEED	1600

static int gFlameFrames[MAX_CARS];
static int gFlameAcc[MAX_CARS];
static int gFlameChannel = -1;

static void cd2FireballLaunch(CAR_DATA* cp)
{
	VECTOR muzzle, dir, vel;

	cd2WpnMuzzle(cp, 0, &muzzle);
	cd2WpnForward(cp, &dir);

	vel.vx = (int)(((long long)dir.vx * CD2_FIREBALL_SPEED) >> 12);
	vel.vy = (int)(((long long)dir.vy * CD2_FIREBALL_SPEED) >> 12);
	vel.vz = (int)(((long long)dir.vz * CD2_FIREBALL_SPEED) >> 12);

	cd2ProjectileSpawn(&cd2WdefSpecialHighwayman, cp, &muzzle, &vel, &dir);
}

static void cd2HighwaymanFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	if (gFlameFrames[cp->id] > 0)
	{
		// the flame is already burning: this press lobs a homing fireball
		cd2FireballLaunch(cp);
		return;
	}

	gFlameFrames[cp->id] = CD2_FLAME_FRAMES;
	gFlameAcc[cp->id] = 0;

	if (gFlameChannel < 0)
	{
		gFlameChannel = GetFreeChannel(1);
		LockChannel(gFlameChannel);
	}

	if (gFlameChannel >= 0)
		Start3DSoundVolPitch(gFlameChannel, SOUND_BANK_SFX, 5,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -2000, 4096 - 1200);
}

static int cd2HighwaymanOnFrame(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (gFlameFrames[i] <= 0)
			continue;

		if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
		{
			gFlameFrames[i] = 0;
			continue;
		}

		// a couple of flame particles a frame, jittered inside a narrow cone
		if (++gFlameAcc[i] >= CD2_FLAME_SPAWN_EVERY)
		{
			int k;

			gFlameAcc[i] = 0;

			for (k = 0; k < 2; k++)
			{
				const MATRIX* w = &cp->hd.where;
				VECTOR muzzle, dir, fwd;
				int jh = cd2WpnRand(CD2_FLAME_CONE * 2 + 1) - CD2_FLAME_CONE;
				int jv = cd2WpnRand(CD2_FLAME_CONE + 1) - CD2_FLAME_CONE / 2;
				long long rx = w->m[0][0], ry = w->m[1][0], rz = w->m[2][0];
				long long ux = w->m[0][1], uy = w->m[1][1], uz = w->m[2][1];

				cd2WpnForward(cp, &fwd);
				cd2WpnMuzzle(cp, 0, &muzzle);

				dir.vx = fwd.vx + (int)((rx * jh + ux * jv) >> 12);
				dir.vy = fwd.vy + (int)((ry * jh + uy * jv) >> 12);
				dir.vz = fwd.vz + (int)((rz * jh + uz * jv) >> 12);

				cd2RaycastSpawn(&cd2WdefSpecialHighwayman, cp, &muzzle, &dir);
			}
		}

		// the flame's read: a couple of orange lines out the nose
		{
			VECTOR a, b, fwd;

			cd2WpnMuzzle(cp, 0, &a);
			cd2WpnForward(cp, &fwd);
			b.vx = a.vx + (fwd.vx * 1200 >> 12);
			b.vy = a.vy + (fwd.vy * 1200 >> 12) + 30;
			b.vz = a.vz + (fwd.vz * 1200 >> 12);

			cd2WpnLine(&a, &b, 255, 140 + cd2WpnRand(80), 40);
		}

		if (--gFlameFrames[i] <= 0)
			gFlameFrames[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

static int cd2HighwaymanOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gFlameFrames[i] = 0;
		gFlameAcc[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

void cd2SpecialHighwaymanRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2HighwaymanOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2HighwaymanOnGameStart, NULL, 0);
	// the same reset when the game returns to the frontend menus
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2HighwaymanOnGameStart, NULL, 0);
}

static CD2_WEAPON_DEF cd2MakeHighwaymanDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_HIGHWAYMAN;
	d.name = "special_highwayman";
	d.displayName = "Breath of Fire";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isSpecial = 1;

	d.maxAmmo = 3;			// profile capacity
	d.fireInterval = 30;
	d.refireCooldown = 660;		// profile recharge: 22s

	d.damage = 180;			// per flame tick / fireball direct hit
	d.speed = 3000;			// the flame is fast and short
	d.range = 1800;
	d.life = 0;

	d.splashRadius = 320;
	d.splashDamage = 260;		// the fireball explodes
	d.explosionEffect = BIG_BANG;

	d.homing = 1;
	d.homingRate = 120;		// the fireball seeks
	d.collideScenery = 1;

	d.fireCone = 600;

	d.colR = 255; d.colG = 150; d.colB = 40;	// fire

	d.fire = cd2HighwaymanFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialHighwayman = cd2MakeHighwaymanDef();
