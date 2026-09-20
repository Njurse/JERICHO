// weapons/special/invocada.c — INVOCADA's special: the CYCLONE.
//
// Six seconds of storm. The car goes SOLID BLACK - body and wheels, so it reads
// as a silhouette rather than a car - and big black smoke pours around it in a
// circle. Anything inside the storm is dragged toward the eye and pushed across
// it, so instead of being flung away the victims are pulled in and left orbiting
// while the tick damage grinds them down.
//
// The pull is a per-frame addition to the victim's velocity (radial + tangential,
// which is what makes it an orbit rather than a vacuum), scaled by how deep in
// the storm they are. Damage is a slow tick, not a burst: the storm is a place
// you do not want to be, not an instant.

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
#include "weapons/fx/fx.h"
#include "weapons/special/special.h"

#include <string.h>

#define CD2_INVOCADA_FRAMES	180	// 6s
#define CD2_INVOCADA_RADIUS	1500	// the storm's reach (world units)
#define CD2_INVOCADA_TICK	15	// frames between damage ticks (0.5s)
#define CD2_INVOCADA_DAMAGE	220	// moderate/low, per tick
#define CD2_INVOCADA_PULL	16	// velocity added toward the eye, per frame, at the rim
#define CD2_INVOCADA_SPIN	11	// ...and across it, so they orbit instead of piling in
#define CD2_INVOCADA_DEADZONE	260	// inside this the pull backs off (no jitter at the eye)
#define CD2_INVOCADA_MAXV	700	// per-axis cap, so nothing is flung out of the level
#define CD2_INVOCADA_SMOKE_EVERY 3	// frames between smoke puffs
#define CD2_INVOCADA_SMOKE_R	760	// radius of the circle the smoke runs around
#define CD2_INVOCADA_PUFFS	4	// puffs per spawn (evenly spaced around the circle)

static int gStormFrames[MAX_CARS];
static int gStormTick[MAX_CARS];
static int gStormSmoke[MAX_CARS];
static int gStormAngle[MAX_CARS];

int cd2InvocadaStormActive(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	return gStormFrames[carId] > 0;
}

static void cd2InvocadaFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	gStormFrames[cp->id] = CD2_INVOCADA_FRAMES;
	gStormTick[cp->id] = 0;
	gStormSmoke[cp->id] = 0;
	gStormAngle[cp->id] = 0;

	printInfo("[cainescrossfire] invocada: storm on car=%d (%d frames, r=%d)\n",
		cp->id, CD2_INVOCADA_FRAMES, CD2_INVOCADA_RADIUS);
}

// Drag one car into the storm: a radial pull toward the eye plus a tangential
// one across it, so it ends up circling rather than stopping dead in the middle.
static void cd2InvocadaPull(const CAR_DATA* cp, CAR_DATA* oc, int dx, int dz, int ad)
{
	int inx, inz, tx, tz, fall, strength;

	if (ad < 1)
		ad = 1;

	// toward the eye, and the perpendicular of that
	inx = (dx * 4096) / ad;
	inz = (dz * 4096) / ad;
	tx = -inz;
	tz = inx;

	// weakest at the rim, full strength a little way in, and no pull at all
	// right on top of the car (that is the dead zone, or it would judder there)
	fall = 4096 - (ad * 4096) / CD2_INVOCADA_RADIUS;
	if (fall < 0)
		fall = 0;
	if (ad < CD2_INVOCADA_DEADZONE)
		fall = (fall * ad) / CD2_INVOCADA_DEADZONE;

	strength = (CD2_INVOCADA_PULL * fall) >> 12;

	oc->st.n.linearVelocity[0] += (int)(((long long)inx * strength) >> 12);
	oc->st.n.linearVelocity[2] += (int)(((long long)inz * strength) >> 12);

	strength = (CD2_INVOCADA_SPIN * fall) >> 12;

	oc->st.n.linearVelocity[0] += (int)(((long long)tx * strength) >> 12);
	oc->st.n.linearVelocity[2] += (int)(((long long)tz * strength) >> 12);

	// and a cap, so a long storm cannot accelerate a light car out of the world
	{
		int ax = 0;

		for (ax = 0; ax < 3; ax++)
		{
			if (oc->st.n.linearVelocity[ax] > CD2_INVOCADA_MAXV)
				oc->st.n.linearVelocity[ax] = CD2_INVOCADA_MAXV;
			else if (oc->st.n.linearVelocity[ax] < -CD2_INVOCADA_MAXV)
				oc->st.n.linearVelocity[ax] = -CD2_INVOCADA_MAXV;
		}
	}

	(void)cp;
}

static int cd2InvocadaOnFrame(void* ud, void* args)
{
	int i, j;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		VECTOR c;

		if (gStormFrames[i] <= 0)
			continue;

		if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
		{
			gStormFrames[i] = 0;
			continue;
		}

		c.vx = cp->hd.where.t[0];
		c.vy = cp->hd.where.t[1] + 40;
		c.vz = cp->hd.where.t[2];

		// the smoke ring: puffs set around a circle that turns as the storm runs
		if (++gStormSmoke[i] >= CD2_INVOCADA_SMOKE_EVERY)
		{
			int k;

			gStormSmoke[i] = 0;
			gStormAngle[i] = (gStormAngle[i] + 300) & 4095;

			for (k = 0; k < CD2_INVOCADA_PUFFS; k++)
			{
				int a = (gStormAngle[i] + (k * 4096) / CD2_INVOCADA_PUFFS) & 4095;
				VECTOR dir, at;

				cd2SpecCompass(cp, (a >> 9) & 7, &dir);

				at.vx = c.vx + ((dir.vx * CD2_INVOCADA_SMOKE_R) >> 12);
				at.vy = c.vy;
				at.vz = c.vz + ((dir.vz * CD2_INVOCADA_SMOKE_R) >> 12);

				cd2FxSpawn(&at, CD2_FX_SMOKE);
			}
		}

		// the storm itself
		for (j = 0; j < MAX_CARS; j++)
		{
			CAR_DATA* oc = &car_data[j];
			long long dx, dz, d2;
			long long reach2;

			if (j == i || oc->controlType == CONTROL_TYPE_NONE || oc->ap.carCos == NULL)
				continue;

			dx = (long long)oc->hd.where.t[0] - c.vx;
			dz = (long long)oc->hd.where.t[2] - c.vz;
			d2 = dx * dx + dz * dz;
			reach2 = (long long)CD2_INVOCADA_RADIUS * CD2_INVOCADA_RADIUS;

			if (d2 > reach2)
				continue;

			cd2InvocadaPull(cp, oc, (int)dx, (int)dz,
				(int)((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz)));

			// the tick damage, on its own slow clock
			if (gStormTick[i] == 0)
				cd2WpnDamageCar(oc, &c, CD2_INVOCADA_DAMAGE, cp);
		}

		if (++gStormTick[i] >= CD2_INVOCADA_TICK)
			gStormTick[i] = 0;

		if (--gStormFrames[i] <= 0)
		{
			gStormFrames[i] = 0;

			printInfo("[cainescrossfire] invocada: storm done (car=%d)\n", i);
		}
	}

	return JER_RESULT_CONTINUE;
}

// The black-out: body flat black (the engine draws it as a solid silhouette)...
static int cd2InvocadaOnDrawColor(void* ud, void* args)
{
	JER_ARGS_CAR_DRAW_COLOR* a = (JER_ARGS_CAR_DRAW_COLOR*)args;

	(void)ud;

	if (a->car != NULL && cd2InvocadaStormActive(((CAR_DATA*)a->car)->id))
		a->flatBlack = 1;

	return JER_RESULT_CONTINUE;
}

// ...and the wheels hidden with it, so what is left is one black shape.
static int cd2InvocadaOnDrawWheel(void* ud, void* args)
{
	JER_ARGS_DRAW_WHEEL* a = (JER_ARGS_DRAW_WHEEL*)args;

	(void)ud;

	if (cd2InvocadaStormActive(a->carId))
		a->hide = 1;

	return JER_RESULT_CONTINUE;
}

static int cd2InvocadaOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gStormFrames[i] = 0;
		gStormTick[i] = 0;
		gStormSmoke[i] = 0;
		gStormAngle[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

void cd2SpecialInvocadaRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2InvocadaOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW_COLOR, cd2InvocadaOnDrawColor, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WHEEL, cd2InvocadaOnDrawWheel, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2InvocadaOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2InvocadaOnGameStart, NULL, 0);
}

static CD2_WEAPON_DEF cd2MakeInvocadaDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_INVOCADA;
	d.name = "special_invocada";
	d.displayName = "Cyclone";
	d.cls = CD2_WCLS_AOE;

	d.isSpecial = 1;

	d.leanOut = 0;			// the storm is the car: nobody leans out of it

	d.maxAmmo = 2;
	d.fireInterval = 30;
	d.refireCooldown = 780;		// 26s

	d.damage = CD2_INVOCADA_DAMAGE;
	d.speed = 0;
	d.range = CD2_INVOCADA_RADIUS;
	d.life = CD2_INVOCADA_FRAMES;

	d.explosionEffect = CD2_FX_SMOKE;

	d.colR = 20; d.colG = 20; d.colB = 26;	// near-black

	d.fire = cd2InvocadaFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialInvocada = cd2MakeInvocadaDef();
