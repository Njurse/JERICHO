/* spawn.c - the "see the imported cars" lever.
 *
 * An import only fills a RESIDENT MODEL SLOT. Nothing in the engine ever creates a
 * vehicle from slots 5 or 6 -- traffic picks its model from modelRandomList
 * (`civ_ai.c:47`), which names 0/1/2/4 only, and the player's car comes from
 * wantedCar -- so a foreign car is INVISIBLE unless you drive it, or it happened to
 * land in a civilian slot. That is why a level can load three cities and show none
 * of them.
 *
 * With `spawn_imports = 1` in carhacks.ini this puts one car per imported city on
 * the ground in a line ahead of the player, ONCE, as soon as the level is live, so
 * the geometry and the placement can be looked at.
 *
 * It is a MEASUREMENT lever, not a feature:
 *   - the cars are CONTROL_TYPE_CUTSCENE and nothing drives them: they sit where
 *     they are put, which is exactly what makes them easy to inspect;
 *   - their COLOURS ARE NOT RIGHT yet -- three imports overflow the CLUT column
 *     (CROSS_CITY.md, "The budget"), which the band-placement unit fixes. Geometry
 *     and placement are what this proves.
 */
#include "driver2.h"
#include "system.h"		/* LevelNames[] */
#include "mission.h"		/* GameLevel, residentCarModels[] */
#include "cars.h"		/* car_data, MAX_CARS, gCarCleanModelPtr */
#include "players.h"		/* MainPlayer == player[0] */
#include "civ_ai.h"		/* InitCar() */
#include "jericho.h"
#include "jer_config.h"
#include "jer_events.h"

#include "carhacks.h"
#include "carid.h"
#include "carimport.h"

/* How far AHEAD of the player each car is put, and the gap between them. They go
 * in a LINE down the road, not fanned out to the side: at a spawn point the side is
 * usually a wall or the pavement, which is exactly where these kept landing. */
#define CHK_SPAWN_OFFSET	900

/* Set once this level's imports have been placed; cleared when the level changes
 * (chkSpawnReset, from the CAR_DATA_SOURCE handler). */
static int sChkSpawned;

void chkSpawnReset(void)
{
	sChkSpawned = 0;
}

static int chkSpawnEnabled(void)
{
	return jer_config_get_int("carhacks", "spawn_imports", 0) != 0;
}

/* The first CAR_DATA slot the world is not using, or NULL. Same search
 * cainescrossfire's cd2AiSpawnOne does. */
static CAR_DATA* chkSpawnFreeCar(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (car_data[i].controlType == CONTROL_TYPE_NONE)
			return &car_data[i];
	}

	return NULL;
}

/* FRAME: nothing until the level is live AND the player is in a car, then one car
 * per imported city, and latch. Cheap when it is not doing anything (a compare). */
static int chkSpawnOnFrame(void* ud, void* args)
{
	CAR_DATA* pcp;
	int i, placed = 0;

	(void)ud;
	(void)args;

	if (sChkSpawned || !chkSpawnEnabled())
		return JER_RESULT_CONTINUE;

	if (!carhacks_enabled(CHK_HACK_CROSS_CITY))
		return JER_RESULT_CONTINUE;

	/* Is the player actually driving? An id out of range, or a car that is not in
	 * the world, means the level is still loading -- the same test the rest of the
	 * codebase uses for "in a game". */
	if (player[0].playerCarId < 0 || player[0].playerCarId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	pcp = &car_data[player[0].playerCarId];

	if (pcp->controlType != CONTROL_TYPE_PLAYER)
		return JER_RESULT_CONTINUE;

	sChkSpawned = 1;		/* one attempt per level, whatever it manages */

	for (i = 0; i < CHK_IMPORT_MAX_SLOTS; i++)
	{
		CHK_CAR_ID id = chkImportSlotId(i);
		CAR_DATA* car;
		LONGVECTOR4 pos;
		static char padId = 0;		/* CUTSCENE car pad id (no replay stream) */
		int d, palette;

		if (id.city == CHK_CITY_NATIVE)
			continue;

		/* The slot has to really hold a BUILT model: InitCar with a slot whose
		 * model pointers are NULL faults the moment the car is drawn or dented. */
		if (gCarCleanModelPtr[i] == NULL || gCarDamModelPtr[i] == NULL)
		{
			printInfo("[carhacks] spawn: slot %d (%s model %d) has no built model - not placed\n",
				i, chkCityName(id.city), id.model);
			continue;
		}

		car = chkSpawnFreeCar();

		if (car == NULL)
		{
			printInfo("[carhacks] spawn: no free CAR_DATA slot for %s model %d\n",
				chkCityName(id.city), id.model);
			continue;
		}

		/* In a LINE ahead of the player, into the road where there is room. The
		 * forward axis is the matrix's third column -- the engine reads the very
		 * same pair for a point ahead of a car (cop_ai.c:426-428 uses m[0][2]/
		 * m[2][2] for 400 units ahead; handling.c:788 derives hd.direction from
		 * those two as well). m[0][0]/m[2][0] would be the SIDE, which is the wall. */
		d = CHK_SPAWN_OFFSET * (placed + 1);

		/* The engine's own rule (PingInCivCar, and cainescrossfire's spawn): a
		 * recolourable civilian body (model 0..4) takes a palette 0..5, anything
		 * else is single-palette. Different colours per car so they can be told
		 * apart at a glance. */
		palette = (residentCarModels[i] >= 0 && residentCarModels[i] <= 4) ? (placed % 6) : 0;
		pos[0] = pcp->hd.where.t[0] + (int)(((long long)pcp->hd.where.m[0][2] * d) >> 12);
		pos[1] = pcp->hd.where.t[1];
		pos[2] = pcp->hd.where.t[2] + (int)(((long long)pcp->hd.where.m[2][2] * d) >> 12);
		pos[3] = 0;

		InitCar(car, pcp->hd.direction, &pos, CONTROL_TYPE_CUTSCENE, i, palette, &padId);

		/* The player's position and the car's are both logged: this is the one thing
		 * about the placement a reader cannot check from the numbers above. */
		printInfo("[carhacks] spawn: %s model %d (resident slot %d) in CAR_DATA slot %d, palette %d, %d ahead - player (%d,%d,%d) car (%d,%d,%d)\n",
			chkCityName(id.city), id.model, i, CAR_INDEX(car), palette, d,
			pcp->hd.where.t[0], pcp->hd.where.t[1], pcp->hd.where.t[2],
			pos[0], pos[1], pos[2]);

		placed++;
	}

	printInfo("[carhacks] spawn: %d imported car(s) placed ahead of the player\n", placed);

	return JER_RESULT_CONTINUE;
}

void chkSpawnRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, chkSpawnOnFrame, NULL, 0);
}
