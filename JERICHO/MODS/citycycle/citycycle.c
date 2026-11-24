/*
 * citycycle.c - cross-city car stress test.
 *
 * Keeps four cars in front of the player. Every CYC_INTERVAL frames it removes them and
 * builds four new ones drawn from N different cities, N cycling 1,2,3,4,1,2,... with the
 * cities and the models picked at random. Each car is made resident through the same
 * JER_CARQ_LOAD request mp's Change car uses, so what it exercises is carhacks' import,
 * the page-handle allocator and the release path - the "1, 2, 3 and 4 cities at once"
 * cases. CYC_CHECK_DELAY frames after a spawn it reports, per car, whether the slot is
 * ready and drawable, and the import-handle census.
 *
 * Arm it: citycycle = 1 in JERICHO/CONFIG/modlist.ini. Read the log (and the in-game
 * console) for "[citycycle]" lines. JERICHO_CYCLE_SEED=<n> makes a run repeatable.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_carquery.h"
#include "jer_console.h"

#include "driver2.h"
#include "cars.h"
#include "civ_ai.h"
#include "players.h"
#include "mission.h"
#include "system.h"
#include "models.h"
#include "texture.h"
#include "dr2math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CYC_CARS		4
#define CYC_START_FRAME		120	/* let the level settle first */
#define CYC_INTERVAL		200	/* frames between rounds - slightly quicker iteration */
#define CYC_CHECK_DELAY		150	/* frames after a spawn until the check */
#define CYC_MAX_CITIES		16

typedef struct CYC_CAR
{
	int id;		/* car_data index, -1 = none */
	int slot;	/* resident slot it was built from */
	int city;
	int model;
} CYC_CAR;

static CYC_CAR gCar[CYC_CARS];
/* the last round's cars, asked to unload again a little later: the first request can come
 * back "a car is still on it", and carhacks only retries a deferred release inside an mp
 * session - a sandbox run has none, so the retry is ours */
static CYC_CAR gOld[CYC_CARS];
static int gOldAt;
static int gFrame, gRound, gCheckAt, gPlacedRound;
static unsigned int gRng = 1;
static int gCities[CYC_MAX_CITIES], gCityCount;

static unsigned int CycRand(void)
{
	gRng ^= gRng << 13;
	gRng ^= gRng >> 17;
	gRng ^= gRng << 5;
	return gRng;
}

static const char* CycCityName(int city)
{
	return (city >= 0 && city < CITY_COUNT && LevelNames[city] != NULL) ? LevelNames[city] : "level";
}

/* The cities carhacks can serve, or the level's own when nobody answers. */
static void CycRefreshCities(void)
{
	JER_CARQ_CITIES_ARGS a;

	memset(&a, 0, sizeof(a));
	a.cities = gCities;
	a.max = CYC_MAX_CITIES;
	jer_fire(JER_CARQ_CITIES, &a);

	gCityCount = (a.count > 0) ? a.count : 0;

	if (gCityCount > CYC_MAX_CITIES)
		gCityCount = CYC_MAX_CITIES;

	if (gCityCount == 0)
	{
		gCities[0] = GameLevel;
		gCityCount = 1;
	}
}

/* The resident slot holding (city, model), or -1. Looking never loads. */
static int CycSlotFor(int city, int model)
{
	int i;

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		int src;

		if (residentCarModels[i] != model || !JerCarSlotUsable(i))
			continue;

		src = GetCarModelSourceCity(i);

		if (src == city || (src < 0 && city == GameLevel))
			return i;
	}

	return -1;
}

static int CycPickModel(int city)
{
	extern char carNumLookup[CITY_COUNT][10];
	int models[10], n = 0, i, k;

	if (city < 0 || city >= CITY_COUNT)
		return -1;

	for (i = 0; i < 10; i++)
	{
		int m = (int)(signed char)carNumLookup[city][i];

		if (m <= 0 || m >= CAR_MODEL_LUMP_ENTRIES)
			continue;

		for (k = 0; k < n; k++)
		{
			if (models[k] == m)
				break;
		}

		if (k == n)
			models[n++] = m;
	}

	return (n > 0) ? models[CycRand() % n] : -1;
}

static void CycRemoveCars(void)
{
	int i;

	for (i = 0; i < CYC_CARS; i++)
	{
		int id = gCar[i].id;

		if (id >= 0 && id < MAX_CARS && car_data[id].controlType == CONTROL_TYPE_CIV_AI &&
			car_data[id].ap.model == gCar[i].slot)
			PingOutCar(&car_data[id]);

		if (gCar[i].slot >= 0)
		{
			JER_CARQ_UNLOAD_ARGS ua;

			ua.city = gCar[i].city;
			ua.model = gCar[i].model;
			jer_fire(JER_CARQ_UNLOAD, &ua);	/* give the slot, block and handles back */
			gOld[i] = gCar[i];
			gOldAt = gFrame + 30;
		}

		gCar[i].id = -1;
	}
}

static int CycSpawn(int slot, int palette, int lane)
{
	CAR_DATA* pc = &car_data[player[0].playerCarId];
	CAR_DATA* cp = NULL;
	EXTRA_CIV_DATA civ;
	LONGVECTOR4 pos;
	int dir = pc->hd.direction, i;
	int side = (lane * 2 - (CYC_CARS - 1)) * 120;	/* centred row, 240 apart */

	for (i = 0; i < MAX_CARS; i++)
	{
		if (car_data[i].controlType == CONTROL_TYPE_NONE)
		{
			cp = &car_data[i];
			break;
		}
	}

	if (cp == NULL)
		return -1;

	pos[0] = pc->hd.where.t[0] + FIXEDH(RSIN(dir) * 700) + FIXEDH(RCOS(dir) * side);
	pos[1] = pc->hd.where.t[1];
	pos[2] = pc->hd.where.t[2] + FIXEDH(RCOS(dir) * 700) - FIXEDH(RSIN(dir) * side);

	memset(&civ, 0, sizeof(civ));
	civ.palette = palette;
	InitCar(cp, dir, &pos, CONTROL_TYPE_CIV_AI, slot, palette, (char*)&civ);

	return (int)(cp - car_data);
}

static void CycStartRound(void)
{
	int want = 1 + (gRound % 4), chosen[4], i;

	CycRemoveCars();
	CycRefreshCities();

	if (want > gCityCount)
		want = gCityCount;

	/* want distinct cities: a partial shuffle of the list */
	for (i = 0; i < gCityCount; i++)
	{
		int r = i + (int)(CycRand() % (unsigned)(gCityCount - i)), t = gCities[i];

		gCities[i] = gCities[r];
		gCities[r] = t;
	}

	for (i = 0; i < want && i < 4; i++)
		chosen[i] = gCities[i];

	jer_log("[citycycle] round %d: %d car(s) from %d city/cities\n", gRound, CYC_CARS, want);

	for (i = 0; i < CYC_CARS; i++)
	{
		JER_CARQ_LOAD_ARGS la;
		int city = chosen[i % want], model = CycPickModel(city), slot, pal;

		gCar[i].id = -1;
		gCar[i].city = city;
		gCar[i].model = model;
		gCar[i].slot = -1;

		if (model < 0)
			continue;

		slot = CycSlotFor(city, model);

		if (slot < 0)
		{
			memset(&la, 0, sizeof(la));
			la.city = city;
			la.model = model;
			jer_fire(JER_CARQ_LOAD, &la);

			if (la.ok)
				slot = CycSlotFor(city, model);
		}

		if (slot < 0)
		{
			jer_log("[citycycle]   car %d: %s model %d could not be made resident\n", i, CycCityName(city), model);
			jer_console_log("[citycycle] round %d: %s model %d would not load", gRound, CycCityName(city), model);
			continue;
		}

		pal = CarImportModelPaletteCount(city, model);
		pal = (pal > 1) ? (int)(CycRand() % (unsigned)pal) : 0;

		gCar[i].slot = slot;
		gCar[i].id = CycSpawn(slot, pal, i);

		jer_log("[citycycle]   car %d: %s model %d -> slot %d palette %d -> car %d\n",
			i, CycCityName(city), model, slot, pal, gCar[i].id);
	}

	gPlacedRound = gRound;
	gCheckAt = gFrame + CYC_CHECK_DELAY;
	gRound++;
}

static void CycCheckRound(void)
{
	int i, spawned = 0, drawable = 0;

	for (i = 0; i < CYC_CARS; i++)
	{
		int s = gCar[i].slot;

		if (gCar[i].id < 0 || s < 0)
			continue;

		spawned++;

		if (JerCarSlotDrawable(s))
			drawable++;
		else
			jer_log("[citycycle]   car %d (%s model %d, slot %d) is NOT drawable: %s\n", i,
				CycCityName(gCar[i].city), gCar[i].model, s, JerCarSlotRefusal(s) ? JerCarSlotRefusal(s) : "rows not uploaded yet");
	}

	JerImportIndexReport();

	jer_log("[citycycle] round %d check: %d/%d spawned, %d drawable\n", gPlacedRound, spawned, CYC_CARS, drawable);
	jer_console_log("[citycycle] round %d: %d/%d cars up, %d drawable%s", gPlacedRound, spawned, CYC_CARS, drawable,
		(spawned == CYC_CARS && drawable == CYC_CARS) ? "" : "  <-- CHECK THE LOG");

	gCheckAt = 0;
}

static int CycOnFrame(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	gFrame++;

	if (gFrame < CYC_START_FRAME || player[0].playerCarId < 0 ||
		car_data[player[0].playerCarId].controlType == CONTROL_TYPE_NONE)
		return JER_RESULT_CONTINUE;

	if (gOldAt != 0 && gFrame >= gOldAt)
	{
		int i;

		gOldAt = 0;

		for (i = 0; i < CYC_CARS; i++)
		{
			JER_CARQ_UNLOAD_ARGS ua;

			if (gOld[i].slot < 0)
				continue;

			ua.city = gOld[i].city;
			ua.model = gOld[i].model;
			jer_fire(JER_CARQ_UNLOAD, &ua);
			gOld[i].slot = -1;
		}
	}

	if (gCheckAt != 0 && gFrame >= gCheckAt)
		CycCheckRound();

	if (((gFrame - CYC_START_FRAME) % CYC_INTERVAL) == 0)
		CycStartRound();

	return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_citycycle_entry)(JERICHO_CONTEXT* ctx)
{
	const char* seed = getenv("JERICHO_CYCLE_SEED");
	int i;

	gRng = (seed != NULL && atoi(seed) != 0) ? (unsigned int)atoi(seed) : 2463534242u;

	for (i = 0; i < CYC_CARS; i++)
		gCar[i].id = gCar[i].slot = gOld[i].slot = -1;

	ctx->jer_register_module(ctx, "citycycle", "City Cycle", "0.1.0", "JERICHO",
		"Cross-city car stress test: four cars, 1-4 cities at a time, random cities each round.",
		"carhacks", JERICHO_SDK_VERSION);

	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, CycOnFrame, NULL, 0);

	jer_log("[citycycle] armed: %d cars, new round every %d frames (seed %u)\n", CYC_CARS, CYC_INTERVAL, gRng);
}
