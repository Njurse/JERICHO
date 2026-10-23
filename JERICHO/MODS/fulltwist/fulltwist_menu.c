/*
 * fulltwist_menu.c -- the FULL TWIST page under JERICHO Addons.
 *
 * Every adjustment writes through ftSaveConfig(), so the ini and the menu never
 * disagree. A hand adjustment switches the preset to Custom; Reset to Defaults
 * returns to the Arcade defaults.
 */

#include "driver2.h"
#include "jericho.h"
#include "jer_pause_menu.h"
#include "jer_math.h"
#include "fulltwist.h"

#include <stdio.h>

static const char* const ftPresetNames[] = { "Cinematic", "Arcade", "Off", "Custom" };

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
	snprintf(out, max, "Preset: %s", ftPresetNames[jer_clamp_int(gFullTwist.cfg.preset, 0, 3)]);
}

static int ftCyclePreset(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gFullTwist.cfg.preset = (gFullTwist.cfg.preset + 1) % 3;
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
	snprintf(out, max, "Contact impulse: %d%%", gFullTwist.cfg.launch);
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

	gFullTwist.cfg.preset = FT_PRESET_ARCADE;
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

static const JER_PAUSE_MENU_ITEM ftMainItems[] =
{
	{ NULL, ftLabelEnabled,   ftToggleEnabled,   NULL, NULL, 0 },
	{ NULL, ftLabelPreset,    ftCyclePreset,     NULL, NULL, 0 },
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
