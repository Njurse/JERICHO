// hud/lockon.c — the lock-on readout, and the corner the map sits in.
//
// Two things, both about the top-right of the screen:
//
//   1. The NAME of whoever is on the crosshair. The lock is the module's own
//      forward cone plus a clear line - the same shape Fixer's laser uses - so
//      it reads as "what the guns are pointing at" rather than "what is
//      nearby", and it disappears the moment the target leaves the cone, the
//      range, or the line of sight.
//   2. The MAP moves up under it. The engine parks the minimap at y=181, i.e.
//      along the bottom; the corner holds both now, with the map tucked in
//      under the name instead of fighting it.
//
// The name is drawn with the JERICHO HUD's anchored panels (jer_hud_panel):
// persistent, corner-anchored lines, which is what a readout needs and what the
// message queue deliberately is not.

#include "driver2.h"
#include "cars.h"
#include "players.h"		/* MainPlayer */
#include "objcoll.h"		/* lineClear - the lock's line-of-sight test */
#include "overmap.h"		/* gMapYOffset - where the minimap sits */
#include "jericho.h"
#include "jer_events.h"
#include "jer_hud.h"
#include "cainescrossfire.h"
#include "ai/ai.h"		/* cd2AiIsOpponent */
#include "factions/factions.h"	/* the contestant's name and its colour */
#include "hud/lockon.h"

extern int ratan2(int y, int x);	/* the module's angle helper */

#define CD2_LOCK_RANGE	5200	// how far the readout reaches
#define CD2_LOCK_CONE	480	// ...and how far off the nose (heading units, ~42 deg)
#define CD2_LOCK_PANEL	0	// the HUD panel slot it owns
#define CD2_HUD_MAP_Y	40	// where the map sits, under the name

static int gLockCar = -1;

int cd2LockOnTarget(void)
{
	return gLockCar;
}

static void cd2LockClear(void)
{
	if (gLockCar >= 0)
		printInfo("[cainescrossfire] lock: released (was car=%d)\n", gLockCar);

	gLockCar = -1;
	jer_hud_panel_clear(CD2_LOCK_PANEL);
}

// The best opponent in the player's forward cone with a clear line to it.
static int cd2LockFind(const CAR_DATA* cp)
{
	VECTOR at;
	long long range2 = (long long)CD2_LOCK_RANGE * CD2_LOCK_RANGE;
	int j, best = -1;
	long long bestD = 0;

	at.vx = cp->hd.where.t[0];
	at.vy = cp->hd.where.t[1] + 40;
	at.vz = cp->hd.where.t[2];

	for (j = 0; j < MAX_CARS; j++)
	{
		CAR_DATA* oc = &car_data[j];
		VECTOR him;
		long long dx, dz, d2;
		int toTarget, diff;

		if (j == cp->id || oc->ap.carCos == NULL || !cd2AiIsOpponent(oc))
			continue;

		dx = (long long)oc->hd.where.t[0] - at.vx;
		dz = (long long)oc->hd.where.t[2] - at.vz;
		d2 = dx * dx + dz * dz;

		if (d2 > range2)
			continue;

		// forward only: the readout follows the nose, like the guns do
		toTarget = ratan2((int)dx, (int)dz);
		diff = (toTarget - cp->hd.direction) & 0xfff;

		if (diff > 2048)
			diff -= 4096;
		if (diff < 0)
			diff = -diff;

		if (diff > CD2_LOCK_CONE)
			continue;

		// and there has to be a LINE to it: scenery breaks the lock
		him.vx = oc->hd.where.t[0];
		him.vy = oc->hd.where.t[1] + 40;
		him.vz = oc->hd.where.t[2];

		if (lineClear((VECTOR*)&at, &him) == 0)
			continue;

		if (best < 0 || d2 < bestD)
		{
			best = j;
			bestD = d2;
		}
	}

	return best;
}

static int cd2LockOnFrame(void* ud, void* args)
{
	CAR_DATA* cp;
	int t;

	(void)ud;
	(void)args;

	// The corner, every frame: the engine parks the map along the bottom and
	// only ever re-derives its X, so the Y set here is what the map keeps.
	gMapYOffset = CD2_HUD_MAP_Y;

	if (MainPlayer.playerCarId < 0 || MainPlayer.playerCarId >= MAX_CARS)
	{
		cd2LockClear();
		return JER_RESULT_CONTINUE;
	}

	cp = &car_data[MainPlayer.playerCarId];

	t = (cp->controlType == CONTROL_TYPE_NONE) ? -1 : cd2LockFind(cp);

	if (t < 0)
	{
		cd2LockClear();
		return JER_RESULT_CONTINUE;
	}

	{
		const char* name = cd2FacTagOfCar(&car_data[t]);

		if (name == NULL || name[0] == '\0')
		{
			cd2LockClear();
			return JER_RESULT_CONTINUE;
		}

		if (t != gLockCar)
		{
			printInfo("[cainescrossfire] lock: holding car=%d (%s)\n", t, name);
			gLockCar = t;
		}

		{
			unsigned char r = 255, g = 255, b = 255;

			cd2FacColourOfCar(&car_data[t], &r, &g, &b);
			jer_hud_panel(CD2_LOCK_PANEL, JER_HUD_ANCHOR_TOP_RIGHT, name, r, g, b);
		}
	}

	return JER_RESULT_CONTINUE;
}

static int cd2LockOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	cd2LockClear();

	return JER_RESULT_CONTINUE;
}

void cd2LockOnRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2LockOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2LockOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2LockOnGameStart, NULL, 0);
}
