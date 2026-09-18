// cainescrossfiregearbox.c — Combat D2: the engine gearbox tuner.
//
// (Split out of the old cainescrossfiremedia.c; see cainescrossfire_internal.h for the file
// map.)
//
// JER_EVENT_CAR_GEARBOX (gamesnd.c GetEngineRevs): while a player drives,
// retune the rev model so gears feel SHORT and punchy but the tall top gear
// levels the pitch at the car's cainescrossfire top speed - revs never wind past
// CD2_REV_CEILING even as the point-mass speed keeps climbing. Tuners are the
// CD2_GEAR_* macros in cainescrossfire.h.

#include "driver2.h"
#include "cainescrossfire.h"
#include "cars.h"
#include "jericho.h"
#include "jer_events.h"

// GEARBOX (JER_EVENT_CAR_GEARBOX, gamesnd.c GetEngineRevs): while a player
// drives, retune the rev model so gears feel SHORT and punchy but the tall
// top gear levels the pitch at the car's cainescrossfire top speed — revs never
// wind past CD2_REV_CEILING even as the point-mass speed keeps climbing.
static int cd2mOnCarGearbox(void* ud, void* args)
{
	JER_ARGS_CAR_GEARBOX* g = (JER_ARGS_CAR_GEARBOX*)args;
	CAR_DATA* cp = (CAR_DATA*)g->car;
	int top, i, prevHi, wsTop;
	(void)ud;

	if (!gCd2Cfg.enabled || !CD2_GEAR_AUTO)
		return JER_RESULT_CONTINUE;

	if (cp->controlType != CONTROL_TYPE_PLAYER)
		return JER_RESULT_CONTINUE;

	top = cd2CarTopSpeed(cp); // effective top (config x scale x chassis)
	if (top < 40)
		return JER_RESULT_CONTINUE;

	wsTop = (int)(((long long)top * CD2_WS_PER_SPEED) >> 12); // ws at top speed
	if (wsTop < 30)
		return JER_RESULT_CONTINUE;

	{
		int up[4];
		int fracs[3] = { CD2_GEAR_1_FRAC, CD2_GEAR_2_FRAC, CD2_GEAR_3_FRAC };

		prevHi = 0;
		for (i = 0; i < 3; i++)
		{
			up[i] = (int)(((long long)top * fracs[i]) >> 12);
			up[i] = (int)(((long long)up[i] * CD2_WS_PER_SPEED) >> 12);
			if (up[i] <= prevHi)
				up[i] = prevHi + 1;
			prevHi = up[i];
		}
		up[3] = wsTop;
		if (up[3] <= prevHi)
			up[3] = prevHi + 1;

		for (i = 0; i < 4; i++)
		{
			g->hiWs[i] = up[i];

			if (i == 0)
			{
				g->lowWs[i] = 0;
				g->lowIdleWs[i] = 0;
			}
			else
			{
				int lo = (int)(((long long)up[i - 1] * CD2_GEAR_DOWN_FRAC) >> 12);
				if (lo >= up[i])
					lo = up[i] - 1;
				g->lowWs[i] = lo;
				g->lowIdleWs[i] = (int)(((long long)lo * 3) >> 2);
			}

			if (i < 3)
			{
				int ratio = CD2_GEAR_SHIFT_REVS / up[i];
				if (ratio < 8) ratio = 8;
				g->ratioAc[i] = ratio;
				g->ratioIdle[i] = ratio;
			}
			else
			{
				int ratio = CD2_REV_CEILING / up[i];
				if (ratio < 4) ratio = 4;
				g->ratioAc[i] = ratio;
				g->ratioIdle[i] = ratio;
			}
		}

		g->revCeiling = CD2_REV_CEILING;
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_cainescrossfire_entry in cainescrossfire.c)
// ---------------------------------------------------------------------------
void cd2GearboxRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_GEARBOX, cd2mOnCarGearbox, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] gearbox registered (SDK v%d)\n", ctx->sdkVersion);
}
