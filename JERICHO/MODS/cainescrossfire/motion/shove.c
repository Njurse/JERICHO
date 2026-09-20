// motion/shove.c — the push request queue (see motion/shove.h).
//
// Requests ACCUMULATE within a frame rather than replacing one another: two
// things can shove the same car on the same frame (a shotgun blast and a
// collision), and the honest answer is the sum, not whichever ran last.

#include "driver2.h"
#include "cars.h"
#include "jericho.h"
#include "jer_events.h"
#include "cainescrossfire.h"
#include "motion/shove.h"

typedef struct CD2_SHOVE
{
	int dirX;
	int dirZ;
	int pct;
} CD2_SHOVE;

static CD2_SHOVE sShove[MAX_CARS];

void cd2ShovePush(int carId, int dirX, int dirZ, int pct)
{
	if (carId < 0 || carId >= MAX_CARS || pct == 0)
		return;

	sShove[carId].dirX += dirX;
	sShove[carId].dirZ += dirZ;
	sShove[carId].pct += pct;
}

int cd2ShoveTake(int carId, int* dirX, int* dirZ, int* pct)
{
	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	if (sShove[carId].pct == 0)
		return 0;

	*dirX = sShove[carId].dirX;
	*dirZ = sShove[carId].dirZ;
	*pct = sShove[carId].pct;

	sShove[carId].dirX = 0;
	sShove[carId].dirZ = 0;
	sShove[carId].pct = 0;

	return 1;
}

void cd2ShoveReset(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		sShove[i].dirX = 0;
		sShove[i].dirZ = 0;
		sShove[i].pct = 0;
	}
}

static int cd2ShoveOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	cd2ShoveReset();

	return JER_RESULT_CONTINUE;
}

void cd2ShoveRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2ShoveOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2ShoveOnGameStart, NULL, 0);
}
