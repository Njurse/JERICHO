// weapons/special/deadstar.c — DEADSTAR's special: DEATH DASH.
//
// Instant acceleration to turbo top speed for 2.5 seconds, with the horn
// blaring twice (once at the off, then again 0.75s in for effect). While the
// dash runs, the collision damage Deadstar DEALS is multiplied by four, and a
// near-90-degree hit into a car's side lands an extra 15%.
//
// The dash sets the car's forward velocity straight to a boosted top speed; the
// damage multiplier rides the car-vs-car hook, keyed on the OTHER car (the
// attacker) being mid-dash.

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

#include "jer_notify.h"	/* jer_notify - the player's T-bone callout */
#include "players.h"		/* player[0].playerCarId */

#include <string.h>

#define CD2_DASH_FRAMES		75	// 2.5s
#define CD2_DASH_SPEED_PCT	175	// ...as a multiplier on the car's own top (turbo uses 125)
#define CD2_DASH_ACCEL_PCT	260	// and how hard it gets there
#define CD2_DASH_THRUST		12000	// engine force while dashing (cp->thrust is ~4215 flat out)
#define CD2_DASH_HORN2_FRAME	22	// 0.75s in -> the second horn
#define CD2_DASH_DAMAGE_MULT	4
#define CD2_DASH_SIDE_BONUS	15	// % extra on a near-90 side hit

static int gDashFrames[MAX_CARS];
static int gHornTimer[MAX_CARS];	// frames until the second horn (0 = done)
static int gDashTold[MAX_CARS];	// the engine override has been reported once
static int gDashChannel = -1;

// The dash's real lever: the car's own top speed and acceleration, raised for
// the window. NOTHING ELSE can do this - the engine clamps a car at its own top
// speed no matter how much thrust it is given.
void cd2SpecialDashPct(int carId, int* speedPct, int* accelPct)
{
	*speedPct = 100;
	*accelPct = 100;

	if (carId < 0 || carId >= MAX_CARS || gDashFrames[carId] <= 0)
		return;

	*speedPct = CD2_DASH_SPEED_PCT;
	*accelPct = CD2_DASH_ACCEL_PCT;
}

// The car's OWN horn - the sample the engine plays for it in LeadHorn
// (SOUND_BANK_CARS, bank*3+2, bank = GetCarBankSample(model)). Two things were
// wrong with the first cut: it reached for SOUND_BANK_SFX 4, which is one of the
// engine's CRASH samples, so the "horns" were a thud at best; and it was played
// at a FIXED point taken when the dash fired, so a horn that should be coming
// from a car doing 600 units a frame was left standing at the start line. This
// tracks the car, exactly as LeadHorn does.
static void cd2DeadstarHorn(CAR_DATA* cp)
{
	if (gDashChannel < 0)
	{
		gDashChannel = GetFreeChannel(1);
		LockChannel(gDashChannel);
	}

	if (gDashChannel >= 0)
		Start3DTrackingSound(gDashChannel, SOUND_BANK_CARS,
			GetCarBankSample(cp->ap.model) * 3 + 2,
			(VECTOR*)cp->hd.where.t, (LONGVECTOR3*)cp->st.n.linearVelocity);
}

static void cd2DeadstarFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	gDashFrames[cp->id] = CD2_DASH_FRAMES;
	gHornTimer[cp->id] = CD2_DASH_HORN2_FRAME;
	gDashTold[cp->id] = 0;

	// horn #1
	cd2DeadstarHorn(cp);

	printInfo("[cainescrossfire] deadstar: dash on car=%d (%d frames, thrust %d)\n",
		cp->id, CD2_DASH_FRAMES, CD2_DASH_THRUST);
}

// The dash drives the car through the ENGINE. Writing linearVelocity/hd.speed
// straight in (which is what it used to do) reads as instant, and then the
// handling model integrates the wheel forces and overwrites the lot on the very
// next frame - the car simply STOPPED, and the dash went nowhere. `thrust` is
// the lever the engine actually obeys, so it is what the dash uses, every frame
// of the window, whether or not the driver is on the gas.
static int cd2DeadstarOnEngine(void* ud, void* args)
{
	JER_ARGS_CAR_ENGINE* a = (JER_ARGS_CAR_ENGINE*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;

	(void)ud;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS || gDashFrames[cp->id] <= 0)
		return JER_RESULT_CONTINUE;

	a->thrust = CD2_DASH_THRUST;

	if (!gDashTold[cp->id])
	{
		gDashTold[cp->id] = 1;
		printInfo("[cainescrossfire] deadstar: engine forced to %d on car=%d (speed was %d)\n",
			a->thrust, cp->id, a->speed);
	}

	return JER_RESULT_CONTINUE;
}

static int cd2DeadstarOnFrame(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (gDashFrames[i] <= 0)
			continue;

		if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
		{
			gDashFrames[i] = 0;
			continue;
		}

		// the second horn, 0.75s in
		if (gHornTimer[i] > 0 && --gHornTimer[i] == 0)
			cd2DeadstarHorn(cp);

		// the dash profile, every 15 frames: what the thrust is actually doing
		// to the car, so the dials are set from a run rather than by feel
		if (gDashFrames[i] % 15 == 0)
			printInfo("[cainescrossfire] deadstar: dash car=%d left=%d speed=%d pos=(%d,%d)\n",
				i, gDashFrames[i], cp->hd.speed, cp->hd.where.t[0], cp->hd.where.t[2]);

		if (--gDashFrames[i] <= 0)
		{
			gDashFrames[i] = 0;

			printInfo("[cainescrossfire] deadstar: dash done car=%d speed=%d\n", i, cp->hd.speed);
		}
	}

	return JER_RESULT_CONTINUE;
}

// The dash multiplies the damage the dashing car DEALS: this car (a->car) is
// the victim; if the other car is mid-dash, the hit lands x4 (and +15% on a
// near-90-degree side hit).
static int cd2DeadstarOnCarVsCar(void* ud, void* args)
{
	JER_ARGS_CAR_VS_CAR* a = (JER_ARGS_CAR_VS_CAR*)args;
	CAR_DATA* victim = (CAR_DATA*)a->car;
	CAR_DATA* other = (CAR_DATA*)a->other;

	(void)ud;

	if (victim == NULL || other == NULL)
		return JER_RESULT_CONTINUE;

	if (other->id < 0 || other->id >= MAX_CARS || gDashFrames[other->id] <= 0)
		return JER_RESULT_CONTINUE;

	{
		int v = a->value * CD2_DASH_DAMAGE_MULT;

		// near-90 side hit: the attacker sits almost perpendicular to the
		// victim's heading (dot of the offset with the victim's forward ~ 0)
		{
			VECTOR fwd;
			int dx = other->hd.where.t[0] - victim->hd.where.t[0];
			int dz = other->hd.where.t[2] - victim->hd.where.t[2];
			int adx = (dx < 0 ? -dx : dx), adz = (dz < 0 ? -dz : dz);
			int mag = (adx > adz) ? (adx + adz / 2) : (adz + adx / 2);

			cd2WpnForward(victim, &fwd);

			if (mag > 1)
			{
				int dot = (int)(((long long)fwd.vx * dx + (long long)fwd.vz * dz) >> 12);

				if (dot < 0) dot = -dot;

				// |fwd| ~ 4096, so |cos| < 1/6 is within ~10 deg of square
				if (dot < mag / 6)
				{
					v = v + (v * CD2_DASH_SIDE_BONUS) / 100;

					// the T-bone critical is the player's moment - name it, for 3s
					if (other->id == player[0].playerCarId)
						jer_notify("T-Bone Damage Bonus.", 2, 3);
				}
			}
		}

		a->value = v;
	}

	return JER_RESULT_CONTINUE;
}

static int cd2DeadstarOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gDashFrames[i] = 0;
		gHornTimer[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

void cd2SpecialDeadstarRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2DeadstarOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE, cd2DeadstarOnEngine, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_VS_CAR, cd2DeadstarOnCarVsCar, NULL, 5);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2DeadstarOnGameStart, NULL, 0);
	// the same reset when the game returns to the frontend menus
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2DeadstarOnGameStart, NULL, 0);
}

static CD2_WEAPON_DEF cd2MakeDeadstarDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_DEADSTAR;
	d.name = "special_deadstar";
	d.displayName = "Death Dash";
	d.cls = CD2_WCLS_AOE;

	d.isSpecial = 1;

	d.maxAmmo = 2;			// profile capacity
	d.fireInterval = 30;
	d.refireCooldown = 750;		// profile recharge: 25s

	d.damage = 0;			// the dash is the payload (collision damage x4)
	d.speed = CD2_DASH_SPEED;
	d.range = 0;
	d.life = CD2_DASH_FRAMES;

	d.explosionEffect = LITTLE_BANG;

	d.colR = 255; d.colG = 60; d.colB = 60;	// hot red

	d.fire = cd2DeadstarFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialDeadstar = cd2MakeDeadstarDef();
