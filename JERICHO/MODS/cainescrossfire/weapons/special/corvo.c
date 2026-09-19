// weapons/special/corvo.c — CORVO's special: SIREN'S WRATH.
//
// The siren runs for four seconds. While it does, a lightning bolt revolves
// around the car (drawn from the roof out to the attack radius) and, every
// CD2_CORVO_ZAP_INTERVAL frames, arcs onto the nearest other car inside the
// radius: moderate damage, a shaky twist (angular velocity) that reads as being
// electrocuted, and a small vertical jump on the hit.
//
// The bolt is a debug-line visual (first cut); the zap is real damage + knock.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"		/* AddCopCarLight - the forced siren light */
#include "sound.h"
#include "gamesnd.h"
#include "jericho.h"
#include "jer_events.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/special/special.h"

#include <string.h>

#define CD2_CORVO_FRAMES	120	// 4s of siren
#define CD2_CORVO_RANGE		900	// attack radius (world units)
#define CD2_CORVO_ZAP_INTERVAL	20	// frames between zaps
#define CD2_CORVO_REVOLVE	320	// bolt spin per frame (PSX angle units)
#define CD2_CORVO_HOP		300	// small vertical jump on a hit (linear vel)
#define CD2_CORVO_TWIST		90	// twist strength (angular velocity term)
#define CD2_CORVO_WAIL		24	// frames between siren wails (the loop)

// The police siren the engine plays for its siren cars (CarHasSiren ->
// M_SHRT_2(SOUND_BANK_VOICES, 0), played at pitch 4096 while the horn is held).
// "The Rio police car siren" is this sample, taken from the level's own bank.
#define CD2_CORVO_SIREN_BANK	SOUND_BANK_VOICES
#define CD2_CORVO_SIREN_SAMPLE	0

static int gCorvoFrames[MAX_CARS];
static int gCorvoAngle[MAX_CARS];
static int gCorvoAcc[MAX_CARS];
static int gCorvoWail[MAX_CARS];	// frames until the next siren wail
static int gCorvoChannel = -1;

static void cd2CorvoFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	gCorvoFrames[cp->id] = CD2_CORVO_FRAMES;
	gCorvoAngle[cp->id] = 0;
	gCorvoAcc[cp->id] = 0;
	gCorvoWail[cp->id] = CD2_CORVO_WAIL;

	if (gCorvoChannel < 0)
	{
		gCorvoChannel = GetFreeChannel(1);
		LockChannel(gCorvoChannel);
	}

	if (gCorvoChannel >= 0)
		Start3DSoundVolPitch(gCorvoChannel, CD2_CORVO_SIREN_BANK, CD2_CORVO_SIREN_SAMPLE,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -1200, 4096);
}

static int cd2CorvoOnFrame(void* ud, void* args)
{
	int i, j;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		VECTOR c;

		if (gCorvoFrames[i] <= 0)
			continue;

		if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
		{
			gCorvoFrames[i] = 0;
			continue;
		}

		c.vx = cp->hd.where.t[0];
		c.vy = cp->hd.where.t[1] + 60;
		c.vz = cp->hd.where.t[2];

		// FORCE THE SIREN onto the car for the whole window. Corvo is the
		// model-0 car, but the engine only draws a siren light for cop/pursuer
		// cars (cars.c, CarHasSiren + controlType), so a player-driven Corvo
		// would have none. We drive the light ourselves here, and wail the
		// siren on a loop.
		AddCopCarLight(cp);

		if (--gCorvoWail[i] <= 0)
		{
			gCorvoWail[i] = CD2_CORVO_WAIL;

			if (gCorvoChannel >= 0)
				Start3DSoundVolPitch(gCorvoChannel, CD2_CORVO_SIREN_BANK, CD2_CORVO_SIREN_SAMPLE,
					c.vx, c.vy, c.vz, -1200, 4096);
		}

		// the revolving bolt: an 8-eighth compass direction from the roof
		gCorvoAngle[i] = (gCorvoAngle[i] + CD2_CORVO_REVOLVE) & 4095;
		{
			VECTOR dir, edge;
			int eighth = (gCorvoAngle[i] >> 9) & 7;	// 4096/8 == 512 per eighth

			cd2SpecCompass(cp, eighth, &dir);
			edge.vx = c.vx + (dir.vx * CD2_CORVO_RANGE >> 12);
			edge.vy = c.vy - 30;
			edge.vz = c.vz + (dir.vz * CD2_CORVO_RANGE >> 12);

			cd2WpnLine(&c, &edge, 180, 220, 255);
		}

		// the zap
		if (++gCorvoAcc[i] >= CD2_CORVO_ZAP_INTERVAL)
		{
			int best = -1, bestD2 = CD2_CORVO_RANGE * CD2_CORVO_RANGE;

			gCorvoAcc[i] = 0;

			for (j = 0; j < MAX_CARS; j++)
			{
				CAR_DATA* oc = &car_data[j];
				int dx, dz, d2;

				if (j == i || oc->controlType == CONTROL_TYPE_NONE || oc->ap.carCos == NULL)
					continue;

				dx = oc->hd.where.t[0] - c.vx;
				dz = oc->hd.where.t[2] - c.vz;
				d2 = dx * dx + dz * dz;

				if (d2 < bestD2)
				{
					bestD2 = d2;
					best = j;
				}
			}

			if (best >= 0)
			{
				CAR_DATA* oc = &car_data[best];
				VECTOR dir;
				int dx = oc->hd.where.t[0] - c.vx;
				int dz = oc->hd.where.t[2] - c.vz;
				int ad = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

				if (ad < 1) ad = 1;

				cd2WpnDamageCar(oc, &c, cd2WdefSpecialCorvo.damage, cp);

				// twist + a small vertical hop
				dir.vx = (dx * 4096) / ad;
				dir.vz = (dz * 4096) / ad;
				dir.vy = 0;
				cd2WpnKnock(oc, &c, &dir, CD2_CORVO_TWIST);

				oc->st.n.linearVelocity[1] += CD2_CORVO_HOP;

				{
					VECTOR hit;

					hit.vx = oc->hd.where.t[0];
					hit.vy = oc->hd.where.t[1] + 40;
					hit.vz = oc->hd.where.t[2];

					cd2WpnLine(&c, &hit, 230, 240, 255);
				}
			}
		}

		if (--gCorvoFrames[i] <= 0)
			gCorvoFrames[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

static int cd2CorvoOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gCorvoFrames[i] = 0;
		gCorvoAngle[i] = 0;
		gCorvoAcc[i] = 0;
		gCorvoWail[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

void cd2SpecialCorvoRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2CorvoOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2CorvoOnGameStart, NULL, 0);
}

static CD2_WEAPON_DEF cd2MakeCorvoDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_CORVO;
	d.name = "special_corvo";
	d.displayName = "Siren's Wrath";
	d.cls = CD2_WCLS_AOE;

	d.isSpecial = 1;

	d.maxAmmo = 3;			// profile capacity
	d.fireInterval = 30;
	d.refireCooldown = 540;		// profile recharge: 18s

	d.damage = 300;			// moderate, per zap
	d.speed = 0;
	d.range = CD2_CORVO_RANGE;
	d.life = CD2_CORVO_FRAMES;

	d.explosionEffect = LITTLE_BANG;

	d.colR = 200; d.colG = 230; d.colB = 255;	// electric white-blue

	d.fire = cd2CorvoFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialCorvo = cd2MakeCorvoDef();
