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
#define CD2_DASH_SPEED		560	// world units/frame (a boosted top)
#define CD2_DASH_HORN2_FRAME	22	// 0.75s in -> the second horn
#define CD2_DASH_DAMAGE_MULT	4
#define CD2_DASH_SIDE_BONUS	15	// % extra on a near-90 side hit

static int gDashFrames[MAX_CARS];
static int gHornTimer[MAX_CARS];	// frames until the second horn (0 = done)
static int gDashChannel = -1;

static void cd2DeadstarFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR fwd;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	// instant acceleration: set the forward velocity to the dash top
	cd2WpnForward(cp, &fwd);

	cp->hd.speed = CD2_DASH_SPEED;
	cp->st.n.linearVelocity[0] = (int)(((long long)fwd.vx * CD2_DASH_SPEED) >> 12);
	cp->st.n.linearVelocity[1] = (int)(((long long)fwd.vy * CD2_DASH_SPEED) >> 12);
	cp->st.n.linearVelocity[2] = (int)(((long long)fwd.vz * CD2_DASH_SPEED) >> 12);

	gDashFrames[cp->id] = CD2_DASH_FRAMES;
	gHornTimer[cp->id] = CD2_DASH_HORN2_FRAME;

	if (gDashChannel < 0)
	{
		gDashChannel = GetFreeChannel(1);
		LockChannel(gDashChannel);
	}

	// horn #1
	if (gDashChannel >= 0)
		Start3DSoundVolPitch(gDashChannel, SOUND_BANK_SFX, 4,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -1500, 4096);
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
		if (gHornTimer[i] > 0 && --gHornTimer[i] == 0 && gDashChannel >= 0)
			Start3DSoundVolPitch(gDashChannel, SOUND_BANK_SFX, 4,
				cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -1500, 4096);

		if (--gDashFrames[i] <= 0)
			gDashFrames[i] = 0;
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
