// combatd2combat.c — Combat D2: COMBAT effects source file.
//
// One of the source files of the single combatd2 module (merged back from
// the old separate "combatd2combat" module; registered by the module entry
// in combatd2.c via cd2CombatRegister()). Owns the combat damage effects:
//
//   * JER_EVENT_CAR_STEP       - edge-detect a car reaching the game's damage
//                                cap ("totaled") and spawn a BIG_BANG explosion
//                                once, at the moment it crosses the threshold.
//   * JER_EVENT_CAR_DRAW_COLOR - render the totaled body flat solid black
//                                (gouraud shading off, "damping off").
//   * JER_EVENT_DRAW_WHEEL     - hide all four wheels once totaled (blown off).
//
// The damage cap is the same value the stock game uses to decide a car is
// totaled (cars.c DrawCar): MaxPlayerDamage[padid] for the player, otherwise
// MaxPlayerDamage[0].

#include "driver2.h"
#include "cars.h"
#include "job_fx.h"
#include "mission.h"
#include "players.h"
#include "convert.h"
#include "jericho.h"
#include "jer_events.h"

// per-car latch: 1 once the car has crossed the damage cap (edge detection)
static char gWasTotaled[MAX_CARS];

// wreck toss (applied once, on the explosion edge)
#define CD2C_TUMBLE_LAUNCH   0x18000  // upward velocity impulse (raw)
#define CD2C_TUMBLE_SPIN     0x100000 // roll/pitch angular impulse range (raw)

// The canonical "totaled" cap, mirroring cars.c DrawCar.
static int cd2cMaxDamage(CAR_DATA* cp)
{
	int maxDamage = MaxPlayerDamage[0];

	if (cp->controlType == CONTROL_TYPE_PLAYER && cp->ai.padid != NULL &&
		*cp->ai.padid >= 0 && *cp->ai.padid < 2)
		maxDamage = MaxPlayerDamage[*cp->ai.padid];

	return maxDamage;
}

static int cd2cIsTotaled(CAR_DATA* cp)
{
	return cp->totalDamage >= cd2cMaxDamage(cp);
}

// CAR_STEP: explode once when a car first reaches the damage cap.
static int cd2cOnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (cd2cIsTotaled(cp))
	{
		if (!gWasTotaled[cp->id])
		{
			VECTOR blastPos;

			gWasTotaled[cp->id] = 1;
			blastPos.vx = cp->hd.where.t[0];
			blastPos.vy = cp->hd.where.t[1];
			blastPos.vz = cp->hd.where.t[2];
			AddExplosion(blastPos, BIG_BANG);

			// toss the wreck so it tumbles and comes to rest: upward pop +
			// random roll/pitch spin (yaw is re-owned by the handling module,
			// but roll/pitch are free to tumble)
			cp->st.n.linearVelocity[1] += CD2C_TUMBLE_LAUNCH;
			cp->st.n.angularVelocity[0] += (Random2(CD2C_TUMBLE_SPIN) - (CD2C_TUMBLE_SPIN >> 1));
			cp->st.n.angularVelocity[2] += (Random2(CD2C_TUMBLE_SPIN) - (CD2C_TUMBLE_SPIN >> 1));
		}

		// dead car: kill any residual throttle so AI traffic stops driving it
		cp->thrust = 0;
	}
	else
	{
		gWasTotaled[cp->id] = 0;
	}

	return JER_RESULT_CONTINUE;
}

// CAR_DRAW_COLOR: totaled body renders flat solid black.
static int cd2cOnCarDrawColor(void* ud, void* args)
{
	JER_ARGS_CAR_DRAW_COLOR* a = (JER_ARGS_CAR_DRAW_COLOR*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (cd2cIsTotaled(cp))
		a->flatBlack = 1;

	return JER_RESULT_CONTINUE;
}

// DRAW_WHEEL: totaled car has its wheels blown off (hidden).
static int cd2cOnDrawWheel(void* ud, void* args)
{
	JER_ARGS_DRAW_WHEEL* a = (JER_ARGS_DRAW_WHEEL*)args;
	CAR_DATA* cp;
	(void)ud;

	if (a->carId < 0 || a->carId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	cp = &car_data[a->carId];

	if (cd2cIsTotaled(cp))
		a->hide = 1;

	return JER_RESULT_CONTINUE;
}

// CAR_ENGINE_SOUND: mute the totaled wreck's engine (idle hum).
static int cd2cOnCarEngineSound(void* ud, void* args)
{
	JER_ARGS_CAR_ENGINE_SOUND* a = (JER_ARGS_CAR_ENGINE_SOUND*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (cd2cIsTotaled(cp))
	{
		a->revVolume = -10000;  // silent
		a->idleVolume = -10000; // silent
	}

	return JER_RESULT_CONTINUE;
}

// CAR_DRAW: drop the totaled wreck's body so it drags on the ground.
static int cd2cOnCarDraw(void* ud, void* args)
{
	JER_ARGS_CAR_DRAW* a = (JER_ARGS_CAR_DRAW*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	MATRIX* m = (MATRIX*)a->matrix;
	(void)ud;

	if (cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (cd2cIsTotaled(cp))
		m->t[1] -= (int)cp->ap.carCos->wheelSize; // body rests on the ground, wheels gone

	return JER_RESULT_CONTINUE;
}

// RESET_CAR: clear the latch so a respawned car can explode again.
static int cd2cOnResetCar(void* ud, void* args)
{
	JER_ARGS_RESET_CAR* a = (JER_ARGS_RESET_CAR*)args;
	(void)ud;

	if (a->carId >= 0 && a->carId < MAX_CARS)
		gWasTotaled[a->carId] = 0;

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_combatd2_entry in combatd2.c)
// ---------------------------------------------------------------------------

void cd2CombatRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2cOnCarStep, NULL, -1); // runs before combatd2.c's CAR_STEP
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW_COLOR, cd2cOnCarDrawColor, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WHEEL, cd2cOnDrawWheel, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE_SOUND, cd2cOnCarEngineSound, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW, cd2cOnCarDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, cd2cOnResetCar, NULL, 0);

	ctx->jer_log(ctx, "[combatd2] combat effects registered (SDK v%d)\n", ctx->sdkVersion);
}
