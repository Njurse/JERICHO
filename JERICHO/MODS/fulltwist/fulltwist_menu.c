/*
 * fulltwist_menu.c -- the FULL TWIST page under JERICHO Addons.
 *
 * Every adjustment writes through ftSaveConfig(), so the ini and the menu never
 * disagree. A hand adjustment switches the preset to Custom; Reset to Defaults
 * returns to the Free Physics defaults.
 */

#include "driver2.h"
#include "jericho.h"
#include "jer_pause_menu.h"
#include "jer_math.h"
#include "fulltwist.h"

#include <stdio.h>

static const char* const ftPresetNames[] =
{
	"Cinematic",
	"Arcade",
	"Off",
	"Custom",
	"Free Physics"
};

static void ftLabelEnabled(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Enabled: %s", gFullTwist.cfg.enabled ? "ON" : "OFF");
}

static int ftToggleEnabled(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.cfg.enabled = !gFullTwist.cfg.enabled;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();

	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelPreset(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Preset: %s", ftPresetNames[jer_clamp_int(gFullTwist.cfg.preset, 0, 4)]);
}

static int ftCyclePreset(void* ud, int dir)
{
	/* Skip Custom: it is the "hand-tuned" state, not a destination. */
	static const int cycle[] =
	{
		FT_PRESET_CINEMATIC, FT_PRESET_ARCADE, FT_PRESET_FREE_PHYSICS, FT_PRESET_OFF
	};
	int i, next = 0;

	(void)ud;

	for (i = 0; i < (int)(sizeof(cycle) / sizeof(cycle[0])); i++)
	{
		if (cycle[i] == gFullTwist.cfg.preset)
		{
			next = (i + 1) % (int)(sizeof(cycle) / sizeof(cycle[0]));
			break;
		}
	}

	gFullTwist.cfg.preset = cycle[next];
	ftApplyPreset();
	ftSaveConfig();

	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelIntensity(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Intensity: %d%%", gFullTwist.cfg.intensity);
}

static int ftAdjIntensity(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.intensity = jer_clamp_int(gFullTwist.cfg.intensity + dir * 5, 0, 200);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelMinImpact(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Min impact: %d", gFullTwist.cfg.minImpact);
}

static int ftAdjMinImpact(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.minImpact = jer_clamp_int(gFullTwist.cfg.minImpact + dir * 10000, 0, FT_FULL_IMPACT);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelOffCenter(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Off-centre: %d%%", gFullTwist.cfg.offCenter);
}

static int ftAdjOffCenter(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.offCenter = jer_clamp_int(gFullTwist.cfg.offCenter + dir * 5, 0, 200);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelMaxRate(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Max rate: %d", gFullTwist.cfg.maxRate);
}

static int ftAdjMaxRate(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.maxRate = jer_clamp_int(gFullTwist.cfg.maxRate + dir * 100000, 0, 4000000);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelGroundKick(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Ground kick: %d%%", gFullTwist.cfg.groundKick);
}

static int ftAdjGroundKick(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.groundKick = jer_clamp_int(gFullTwist.cfg.groundKick + dir * 5, 0, 200);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelWallSnag(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Wall snag: %d%%", gFullTwist.cfg.wallSnag);
}

static int ftAdjWallSnag(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.wallSnag = jer_clamp_int(gFullTwist.cfg.wallSnag + dir * 5, 0, 100);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelDamping(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Damping: %d%%", gFullTwist.cfg.damping);
}

static int ftAdjDamping(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.damping = jer_clamp_int(gFullTwist.cfg.damping + dir * 5, 0, 100);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelWindow(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Window: %d frames", gFullTwist.cfg.window);
}

static int ftAdjWindow(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.window = jer_clamp_int(gFullTwist.cfg.window + dir * 10, 0, 300);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelTrip(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Trip: %d%%", gFullTwist.cfg.trip);
}

static int ftAdjTrip(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.trip = jer_clamp_int(gFullTwist.cfg.trip + dir * 5, 0, 100);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelLaunch(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Scenery launch: %d%%", gFullTwist.cfg.launch);
}

static int ftAdjLaunch(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.launch = jer_clamp_int(gFullTwist.cfg.launch + dir * 5, 0, 200);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelRolloverDamping(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Rollover damping: %d%%", gFullTwist.cfg.rolloverDamping);
}

static int ftAdjRolloverDamping(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.rolloverDamping = jer_clamp_int(gFullTwist.cfg.rolloverDamping + dir * 5, 0, 100);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelRolloverCatch(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Rollover catch: %d%%", gFullTwist.cfg.rolloverCatch);
}

static int ftAdjRolloverCatch(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.rolloverCatch = jer_clamp_int(gFullTwist.cfg.rolloverCatch + dir * 5, 0, 100);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelSpinRetention(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Spin retention: %d%%", gFullTwist.cfg.spinRetention);
}

static int ftAdjSpinRetention(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.spinRetention = jer_clamp_int(gFullTwist.cfg.spinRetention + dir * 5, 0, 100);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

/* ---- free spin / physics ------------------------------------------- */

static void ftLabelFreeSpin(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Free spin: %s", gFullTwist.cfg.freeSpin ? "ON" : "OFF");
}

static int ftToggleFreeSpin(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gFullTwist.cfg.freeSpin = !gFullTwist.cfg.freeSpin;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelAngularDamping(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Angular damping: %d", gFullTwist.cfg.angularDamping);
}

static int ftAdjAngularDamping(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.angularDamping = jer_clamp_int(gFullTwist.cfg.angularDamping + dir * 8, 0, 128);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelCancelExtra(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Cancel lift damping: %d%%", gFullTwist.cfg.cancelExtraDamping);
}

static int ftAdjCancelExtra(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.cancelExtraDamping = jer_clamp_int(gFullTwist.cfg.cancelExtraDamping + dir * 10, 0, 100);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelKeepSpin(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Keep spin at rest: %s", gFullTwist.cfg.keepSpinAtRest ? "ON" : "OFF");
}

static int ftToggleKeepSpin(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gFullTwist.cfg.keepSpinAtRest = !gFullTwist.cfg.keepSpinAtRest;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelFreeSpinWindow(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Free spin window: %d", gFullTwist.cfg.freeSpinWindow);
}

static int ftAdjFreeSpinWindow(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.freeSpinWindow = jer_clamp_int(gFullTwist.cfg.freeSpinWindow + dir * 15, 0, 300);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelHoldCamera(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Hold tumble camera: %s", gFullTwist.cfg.holdCamera ? "ON" : "OFF");
}

static int ftToggleHoldCamera(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gFullTwist.cfg.holdCamera = !gFullTwist.cfg.holdCamera;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelHullContact(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Hull contact assist: %s", gFullTwist.cfg.hullContact ? "ON" : "OFF");
}

static int ftToggleHullContact(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gFullTwist.cfg.hullContact = !gFullTwist.cfg.hullContact;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelHullFootprint(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Hull footprint: %s", gFullTwist.cfg.hullFootprint ? "ON" : "OFF");
}

static int ftToggleHullFootprint(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gFullTwist.cfg.hullFootprint = !gFullTwist.cfg.hullFootprint;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelSceneryMode(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Scenery mode: %s", gFullTwist.cfg.sceneryMode ? "Hull" : "Assist");
}

static int ftToggleSceneryMode(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gFullTwist.cfg.sceneryMode = !gFullTwist.cfg.sceneryMode;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelWallBounce(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Wall bounce: %d%%", gFullTwist.cfg.wallBounce);
}

static int ftAdjWallBounce(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.wallBounce = jer_clamp_int(gFullTwist.cfg.wallBounce + dir * 5, 0, 100);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelCarCarImpulse(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Car-car impulse: %d%%", gFullTwist.cfg.carCarImpulse);
}

static int ftAdjCarCarImpulse(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.carCarImpulse = jer_clamp_int(gFullTwist.cfg.carCarImpulse + dir * 5, 0, 100);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelCarCarLaunch(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Car-car launch: %d%%", gFullTwist.cfg.carCarLaunch);
}

static int ftAdjCarCarLaunch(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.carCarLaunch = jer_clamp_int(gFullTwist.cfg.carCarLaunch + dir * 10, 0, 200);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelCarCarTumble(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Car-car tumble: %d%%", gFullTwist.cfg.carCarTumble);
}

static int ftAdjCarCarTumble(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.carCarTumble = jer_clamp_int(gFullTwist.cfg.carCarTumble + dir * 10, 0, 200);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelBodyKick(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Body kick: %d%%", gFullTwist.cfg.bodyKick);
}

static int ftAdjBodyKick(void* ud, int dir)
{
	(void)ud;
	gFullTwist.cfg.bodyKick = jer_clamp_int(gFullTwist.cfg.bodyKick + dir * 10, 0, 200);
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelPlayerOnly(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Player only: %s", gFullTwist.cfg.playerOnly ? "ON" : "OFF");
}

static int ftTogglePlayerOnly(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.cfg.playerOnly = !gFullTwist.cfg.playerOnly;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();

	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelScenery3d(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "3D scenery: %s", gFullTwist.cfg.scenery3d ? "ON" : "OFF");
}

static int ftToggleScenery3d(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.cfg.scenery3d = !gFullTwist.cfg.scenery3d;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();

	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelDebug(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Debug log: %s", gFullTwist.cfg.debugLog ? "ON" : "OFF");
}

static int ftToggleDebug(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.cfg.debugLog = !gFullTwist.cfg.debugLog;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();

	return JER_PAUSE_QUIT_NONE;
}

static int ftTestKickAction(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.testKickPending = 1;

	return JER_PAUSE_QUIT_NONE;
}

static int ftTestCrashAction(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.testCrashPending = 1;
	gFullTwist.testCrashDone = 1;

	return JER_PAUSE_QUIT_NONE;
}

static int ftTestSmashAction(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.testSmashPending = 1;
	gFullTwist.testSmashDone = 1;

	return JER_PAUSE_QUIT_NONE;
}

static int ftTestRolloverAction(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.testRolloverPending = 1;
	gFullTwist.testRolloverDone = 1;

	return JER_PAUSE_QUIT_NONE;
}

static void ftLabelRolloverSide(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Rollover side: %s", gFullTwist.cfg.testRolloverSide < 0 ? "Left" : "Right");
}

static int ftToggleRolloverSide(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.cfg.testRolloverSide = (gFullTwist.cfg.testRolloverSide < 0) ? 1 : -1;
	gFullTwist.cfg.preset = FT_PRESET_CUSTOM;
	ftSaveConfig();

	return JER_PAUSE_QUIT_NONE;
}

static int ftResetDefaults(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.cfg.preset = FT_PRESET_FREE_PHYSICS;
	ftApplyPreset();
	ftSaveConfig();

	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM ftTuningItems[] =
{
	{ NULL, ftLabelIntensity,  ftAdjIntensity,  NULL, NULL, 1 },
	{ NULL, ftLabelMinImpact,  ftAdjMinImpact,  NULL, NULL, 1 },
	{ NULL, ftLabelOffCenter,  ftAdjOffCenter,  NULL, NULL, 1 },
	{ NULL, ftLabelMaxRate,    ftAdjMaxRate,    NULL, NULL, 1 },
	{ NULL, ftLabelGroundKick, ftAdjGroundKick, NULL, NULL, 1 },
	{ NULL, ftLabelWallSnag, ftAdjWallSnag, NULL, NULL, 1 },
	{ NULL, ftLabelDamping,    ftAdjDamping,    NULL, NULL, 1 },
	{ NULL, ftLabelWindow,     ftAdjWindow,     NULL, NULL, 1 },
	{ NULL, ftLabelTrip,       ftAdjTrip,       NULL, NULL, 1 },
	{ NULL, ftLabelLaunch,     ftAdjLaunch,     NULL, NULL, 1 },
	{ NULL, ftLabelRolloverDamping, ftAdjRolloverDamping, NULL, NULL, 1 },
	{ NULL, ftLabelRolloverCatch, ftAdjRolloverCatch, NULL, NULL, 1 },
	{ NULL, ftLabelSpinRetention, ftAdjSpinRetention, NULL, NULL, 1 },
};

static const JER_PAUSE_MENU ftTuningMenu =
{
	"Tuning",
	ftTuningItems,
	(int)(sizeof(ftTuningItems) / sizeof(ftTuningItems[0])),
	"fulltwist.tuning"
};

static const JER_PAUSE_MENU_ITEM ftDebugItems[] =
{
	{ "Test Kick", NULL, ftTestKickAction, NULL, NULL, 0 },
	{ "Test Crash", NULL, ftTestCrashAction, NULL, NULL, 0 },
	{ "Test Smash", NULL, ftTestSmashAction, NULL, NULL, 0 },
	{ "Test Rollover", NULL, ftTestRolloverAction, NULL, NULL, 0 },
	{ NULL, ftLabelRolloverSide, ftToggleRolloverSide, NULL, NULL, 0 },
	{ NULL, ftLabelDebug, ftToggleDebug, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU ftDebugMenu =
{
	"Debug",
	ftDebugItems,
	(int)(sizeof(ftDebugItems) / sizeof(ftDebugItems[0])),
	"fulltwist.debug"
};

static const JER_PAUSE_MENU_ITEM ftPhysicsItems[] =
{
	{ NULL, ftLabelFreeSpin,        ftToggleFreeSpin,     NULL, NULL, 0 },
	{ NULL, ftLabelAngularDamping,  ftAdjAngularDamping,  NULL, NULL, 1 },
	{ NULL, ftLabelCancelExtra,     ftAdjCancelExtra,     NULL, NULL, 1 },
	{ NULL, ftLabelKeepSpin,        ftToggleKeepSpin,     NULL, NULL, 0 },
	{ NULL, ftLabelFreeSpinWindow,  ftAdjFreeSpinWindow,  NULL, NULL, 1 },
	{ NULL, ftLabelHoldCamera,      ftToggleHoldCamera,   NULL, NULL, 0 },
	{ NULL, ftLabelHullContact,     ftToggleHullContact,  NULL, NULL, 0 },
	{ NULL, ftLabelHullFootprint,   ftToggleHullFootprint,NULL, NULL, 0 },
	{ NULL, ftLabelSceneryMode,     ftToggleSceneryMode,  NULL, NULL, 0 },
	{ NULL, ftLabelWallBounce,      ftAdjWallBounce,      NULL, NULL, 1 },
	{ NULL, ftLabelCarCarImpulse,   ftAdjCarCarImpulse,   NULL, NULL, 1 },
	{ NULL, ftLabelCarCarLaunch,    ftAdjCarCarLaunch,    NULL, NULL, 1 },
	{ NULL, ftLabelCarCarTumble,    ftAdjCarCarTumble,    NULL, NULL, 1 },
	{ NULL, ftLabelBodyKick,        ftAdjBodyKick,        NULL, NULL, 1 },
};

static const JER_PAUSE_MENU ftPhysicsMenu =
{
	"Physics",
	ftPhysicsItems,
	(int)(sizeof(ftPhysicsItems) / sizeof(ftPhysicsItems[0])),
	"fulltwist.physics"
};

static const JER_PAUSE_MENU_ITEM ftMainItems[] =
{
	{ NULL, ftLabelEnabled,   ftToggleEnabled,   NULL, NULL, 0 },
	{ NULL, ftLabelPreset,    ftCyclePreset,     NULL, NULL, 0 },
	{ "Physics",              NULL,              NULL, NULL, &ftPhysicsMenu, 0 },
	{ "Tuning",               NULL,              NULL, NULL, &ftTuningMenu, 0 },
	{ "Debug",                NULL,              NULL, NULL, &ftDebugMenu, 0 },
	{ NULL, ftLabelPlayerOnly, ftTogglePlayerOnly, NULL, NULL, 0 },
	{ NULL, ftLabelScenery3d, ftToggleScenery3d, NULL, NULL, 0 },
	{ "Reset to Defaults",    NULL,              ftResetDefaults, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU ftMainMenu =
{
	"FULL TWIST",
	ftMainItems,
	(int)(sizeof(ftMainItems) / sizeof(ftMainItems[0])),
	"fulltwist.main"
};

void ftRegisterMenu(void)
{
	jer_pause_menu_register(&ftMainMenu);
}
