/*
 * testmode.c — JERICHO test mode: a quiet world and a camera on a subject.
 *
 * Asset work (textures, palettes, models) needs to look at ONE thing closely:
 * ambient traffic, police and pedestrians are in the way, and the chase camera
 * keeps moving. This module turns all of that off and replaces the camera with
 * an orbit around a chosen car or Tanner.
 *
 * Phase 1 (this file): options only. The module reads its flags and config,
 * logs the resolution, and registers nothing else — so enabling it changes
 * nothing yet.
 *
 * The flags are recognised by the engine's argument parser (like the mp mod's
 * -host/-join) so they are not reported as invalid arguments; the values are
 * read here, from JER_EVENT_CMDLINE.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_math.h"

#include "driver2.h"		/* engine base types, needed before mission.h */
#include "mission.h"		/* maxCivCars, CopsAllowed, numCivCars, numCopCars */
#include "cars.h"			/* car_data, MAX_CARS */
#include "civ_ai.h"		/* PingOutCar */

#include "testmode_internal.h"

#include <stdlib.h>		/* atoi */
#include <string.h>		/* strcmp */

TESTMODE_CFG gTestCfg;

static JERICHO_CONTEXT* gCtx = NULL;

/* ---- the quiet world ------------------------------------------------------ */

static int gQuiet = 0;				/* we are holding the spawn globals down */
static int gSavedMaxCivCars = 0;
static int gSavedCopsAllowed = 0;
static int gQuietPingedCars = 0;	/* civilian cars removed from the scene */

#define TESTMODE_LOG(...)	do { if (gCtx != NULL) gCtx->jer_log(gCtx, __VA_ARGS__); } while (0)

// [D] [T]
static const char* TestmodeSubjectName(void)
{
	return (gTestCfg.subject == TESTMODE_SUBJECT_PED) ? "tanner" : "car";
}

// [D] [T]
static void TestmodeLogOptions(const char* when)
{
	TESTMODE_LOG("[testmode] options (%s): %s, subject=%s",
		when,
		gTestCfg.enabled ? "ENABLED" : "idle",
		TestmodeSubjectName());

	if (gTestCfg.subject == TESTMODE_SUBJECT_CAR)
	{
		if (gTestCfg.car < 0)
			TESTMODE_LOG(", car=<the boot's -car>");
		else
			TESTMODE_LOG(", car=%d", gTestCfg.car);
	}

	TESTMODE_LOG(", camera distance=%d height=%d orbit=%d deg\n",
		gTestCfg.distance, gTestCfg.height, gTestCfg.orbit);
}

// [D] [T]
static void TestmodeClamp(void)
{
	if (gTestCfg.subject != TESTMODE_SUBJECT_PED)
		gTestCfg.subject = TESTMODE_SUBJECT_CAR;

	gTestCfg.distance = jer_clamp_int(gTestCfg.distance, TESTMODE_MIN_DISTANCE, TESTMODE_MAX_DISTANCE);
	gTestCfg.height = jer_clamp_int(gTestCfg.height, 0, TESTMODE_MAX_DISTANCE);

	/* keep the yaw in one turn so the stored value cannot drift */
	gTestCfg.orbit %= 360;

	if (gTestCfg.orbit < 0)
		gTestCfg.orbit += 360;
}

// [D] [T]
static void TestmodeReadConfig(void)
{
	gTestCfg.enabled   = jer_config_get_bool("testmode", "enabled", 0);
	gTestCfg.subject   = jer_config_get_int("testmode", "subject", TESTMODE_SUBJECT_CAR);
	gTestCfg.car       = jer_config_get_int("testmode", "car", -1);
	gTestCfg.pedAction = jer_config_get_int("testmode", "ped_action", 0);
	gTestCfg.distance  = jer_config_get_int("testmode", "distance", TESTMODE_DEFAULT_DISTANCE);
	gTestCfg.height    = jer_config_get_int("testmode", "height", TESTMODE_DEFAULT_HEIGHT);
	gTestCfg.orbit     = jer_config_get_int("testmode", "orbit", TESTMODE_DEFAULT_ORBIT);

	TestmodeClamp();
}

// JERICHO-HOOK: persist the resolved options, so testmode.ini materialises with
// every knob visible and editable (the combatd2 pattern). Reading first means a
// value the player edited is written back unchanged.
static void TestmodeSaveConfig(void)
{
	jer_config_set_bool("testmode", "enabled", gTestCfg.enabled);
	jer_config_set_int("testmode", "subject", gTestCfg.subject);
	jer_config_set_int("testmode", "car", gTestCfg.car);
	jer_config_set_int("testmode", "ped_action", gTestCfg.pedAction);
	jer_config_set_int("testmode", "distance", gTestCfg.distance);
	jer_config_set_int("testmode", "height", gTestCfg.height);
	jer_config_set_int("testmode", "orbit", gTestCfg.orbit);
}

// ---------------------------------------------------------------------------
// A quiet world: no civilian traffic and no police.
//
// maxCivCars and CopsAllowed are plain engine globals (mission.h) and a LEVEL
// LOAD rewrites them from the mission header, so this is asserted every frame
// rather than set once. Any value the engine writes while we are quiet is
// ADOPTED as the stock value before being zeroed, which is what makes the
// restore correct across level changes without knowing the mission's numbers.
// ---------------------------------------------------------------------------
static void TestmodeQuietHold(void)
{
	int i;

	/* the level (or another module) set a count: that is the stock value */
	if (maxCivCars != 0)
		gSavedMaxCivCars = maxCivCars;

	if (CopsAllowed != 0)
		gSavedCopsAllowed = CopsAllowed;

	maxCivCars = 0;
	CopsAllowed = 0;

	/* Stopping new spawns is not enough: a level load puts a few civilian cars
	 * in the scene before the first frame, and they stay. PingOutCar is the
	 * engine's own removal - it clears the CAR_DATA and sets CONTROL_TYPE_NONE -
	 * so this is idempotent and self-healing. Only the first MAX_CARS entries are
	 * cars at all; the last two are the Tanner and camera collision boxes. */
	for (i = 0; i < MAX_CARS; i++)
	{
		if (car_data[i].controlType == CONTROL_TYPE_CIV_AI)
		{
			PingOutCar(&car_data[i]);
			gQuietPingedCars++;
		}
	}

	if (!gQuiet)
	{
		gQuiet = 1;
		TESTMODE_LOG("[testmode] quiet: traffic off (was %d cars), cops off\n", gSavedMaxCivCars);
	}
}

// [D] [T]
static void TestmodeQuietRelease(void)
{
	if (!gQuiet)
		return;

	maxCivCars = gSavedMaxCivCars;
	CopsAllowed = gSavedCopsAllowed;
	gQuiet = 0;

	TESTMODE_LOG("[testmode] loud again: traffic %d cars, cops %s\n",
		gSavedMaxCivCars, gSavedCopsAllowed ? "on" : "off");
}

// ---------------------------------------------------------------------------
// A census, so the quiet world can be checked rather than assumed: the engine's
// own live counters (mission.h) are logged periodically while the mode is on.
// ---------------------------------------------------------------------------
static int gCensusFrames = 0;
static int gCensusDespawned = 0;

static void TestmodeCensus(void)
{
	if (++gCensusFrames < 300)		/* ~10s at 30fps */
		return;

	gCensusFrames = 0;

	TESTMODE_LOG("[testmode] census: civcars=%d copcars=%d (maxCivCars=%d CopsAllowed=%d) pinged_cars=%d peds_despawned=%d\n",
		numCivCars, numCopCars, maxCivCars, CopsAllowed, gQuietPingedCars, gCensusDespawned);
}

// ---------------------------------------------------------------------------
// JER_EVENT_FRAME — hold the world quiet while the mode is on.
// ---------------------------------------------------------------------------
static int TestmodeOnFrame(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (gTestCfg.enabled)
	{
		TestmodeQuietHold();
		TestmodeCensus();
	}
	else
	{
		TestmodeQuietRelease();
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_CMDLINE — fired once, after the engine has parsed its own argv, so a
// module can claim its own shortcuts. The engine recognises these flags in its
// parser (see main.c's -testmode branch) so they are not reported as invalid.
// ---------------------------------------------------------------------------
static int TestmodeOnCmdline(void* ud, void* args)
{
	JER_ARGS_CMDLINE* cl = (JER_ARGS_CMDLINE*)args;
	int i;

	(void)ud;

	if (cl == NULL || cl->argv == NULL)
		return JER_RESULT_CONTINUE;

	for (i = 1; i < cl->argc; i++)
	{
		if (strcmp(cl->argv[i], "-testmode") == 0)
		{
			gTestCfg.enabled = 1;
		}
		else if (strcmp(cl->argv[i], "-testped") == 0)
		{
			/* a standing Tanner instead of the car */
			gTestCfg.enabled = 1;
			gTestCfg.subject = TESTMODE_SUBJECT_PED;
		}
		else if (strcmp(cl->argv[i], "-testcar") == 0)
		{
			/* only take a value that is not the next flag - the engine's own
			 * parser guards the same way, and a missing value must not swallow
			 * the argument after it */
			if (i + 1 < cl->argc && cl->argv[i + 1][0] != '-')
			{
				gTestCfg.enabled = 1;
				gTestCfg.subject = TESTMODE_SUBJECT_CAR;
				gTestCfg.car = atoi(cl->argv[++i]);
			}
			else
			{
				TESTMODE_LOG("[testmode] -testcar needs a car slot or model number\n");
			}
		}
	}

	TestmodeClamp();
	TestmodeSaveConfig();
	TestmodeLogOptions("command line");

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_SHUTDOWN — the game is going away; put the globals back so a
// reload/restart is not left with a world we muzzled.
// ---------------------------------------------------------------------------
static int TestmodeOnShutdown(void* ud, void* args)
{
	(void)ud;
	(void)args;

	TestmodeQuietRelease();

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------
JER_MODULE_ENTRY(jer_module_testmode_entry)(JERICHO_CONTEXT* ctx)
{
	gCtx = ctx;

	TestmodeReadConfig();

	ctx->jer_register_module(ctx,
		"testmode",			/* id */
		"Test Mode",		/* name */
		"0.1.0",			/* version */
		"JERICHO",			/* author */
		"Debug/test-suite mode: a quiet world (no traffic, police or ambient "
		"pedestrians) with an orbiting camera on a chosen car or Tanner.",	/* description */
		"",					/* dependencies */
		JERICHO_SDK_VERSION);

	ctx->jer_register_hook(ctx, JER_EVENT_CMDLINE, TestmodeOnCmdline, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, TestmodeOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_SHUTDOWN, TestmodeOnShutdown, NULL, 0);

	TestmodeLogOptions("config");

	if (!gTestCfg.enabled)
		TESTMODE_LOG("[testmode] idle: pass -testmode, -testcar <n> or -testped to arm it\n");

	TESTMODE_LOG("[testmode] registered (SDK v%d)\n", ctx->sdkVersion);
}
