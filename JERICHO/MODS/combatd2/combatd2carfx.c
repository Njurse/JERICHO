// combatd2carfx.c — Combat D2: how a totaled car PRESENTS.
//
// (Split out of the old combatd2combat.c; see combatd2_internal.h for the file
// map.)
//
// Pure per-car drawing/sound overrides, all keyed off cd2CarTotaled: the body
// renders flat solid black, the wheels are blown off (hidden), the engine is
// silenced, and the body drops to the ground (the wheels are gone).

#include "driver2.h"
#include "combatd2.h"
#include "cars.h"
#include "jericho.h"
#include "jer_events.h"

// CAR_DRAW_COLOR: totaled body renders flat solid black.
static int cd2cOnCarDrawColor(void* ud, void* args)
{
	JER_ARGS_CAR_DRAW_COLOR* a = (JER_ARGS_CAR_DRAW_COLOR*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (cd2CarTotaled(cp))
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

	if (cd2CarTotaled(cp))
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

	if (cd2CarTotaled(cp))
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

	if (cd2CarTotaled(cp))
		m->t[1] -= (int)cp->ap.carCos->wheelSize; // body rests on the ground, wheels gone

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_combatd2_entry in combatd2.c)
// ---------------------------------------------------------------------------
void cd2CarFxRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW_COLOR, cd2cOnCarDrawColor, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WHEEL, cd2cOnDrawWheel, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE_SOUND, cd2cOnCarEngineSound, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW, cd2cOnCarDraw, NULL, 0);

	ctx->jer_log(ctx, "[combatd2] car effects registered (SDK v%d)\n", ctx->sdkVersion);
}
