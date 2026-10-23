/*
 * fulltwist_config.c -- FULL TWIST tunables.
 *
 * The ini keys, clamps and presets live here so the event code and the pause
 * menu share one definition. `JERICHO/CONFIG/fulltwist.ini` is the persistent
 * store; the defaults are also the Arcade preset.
 */

#include "driver2.h"
#include "jericho.h"
#include "jer_config.h"
#include "jer_math.h"
#include "fulltwist.h"

#define FT_DEF_INTENSITY	100
#define FT_DEF_MIN_IMPACT	72000
#define FT_DEF_OFF_CENTER	30
#define FT_DEF_MAX_RATE		4000000
#define FT_DEF_GROUND_KICK	50
#define FT_DEF_WALL_SNAG	45
#define FT_DEF_DAMPING		25
#define FT_DEF_WINDOW		120
#define FT_DEF_TRIP			20
#define FT_DEF_LAUNCH		80
#define FT_DEF_ROLLOVER_DAMPING	45
#define FT_DEF_ROLLOVER_CATCH	50
#define FT_DEF_SPIN_RETENTION	50

void ftClampConfig(void)
{
	FULLTWIST_CONFIG* c = &gFullTwist.cfg;

	c->enabled = c->enabled ? 1 : 0;
	c->intensity = jer_clamp_int(c->intensity, 0, 200);
	c->minImpact = jer_clamp_int(c->minImpact, 0, FT_FULL_IMPACT);
	c->offCenter = jer_clamp_int(c->offCenter, 0, 200);
	c->maxRate = jer_clamp_int(c->maxRate, 0, 4000000);
	c->groundKick = jer_clamp_int(c->groundKick, 0, 200);
	c->wallSnag = jer_clamp_int(c->wallSnag, 0, 100);
	c->damping = jer_clamp_int(c->damping, 0, 100);
	c->window = jer_clamp_int(c->window, 0, 300);
	c->trip = jer_clamp_int(c->trip, 0, 100);
	c->launch = jer_clamp_int(c->launch, 0, 200);
	c->rolloverDamping = jer_clamp_int(c->rolloverDamping, 0, 100);
	c->rolloverCatch = jer_clamp_int(c->rolloverCatch, 0, 100);
	c->spinRetention = jer_clamp_int(c->spinRetention, 0, 100);
	c->scenery3d = c->scenery3d ? 1 : 0;
	c->playerOnly = c->playerOnly ? 1 : 0;
	c->debugLog = c->debugLog ? 1 : 0;
	c->testKick = c->testKick ? 1 : 0;
	c->testCrash = c->testCrash ? 1 : 0;
	c->testRollover = c->testRollover ? 1 : 0;
	c->testRolloverSide = (c->testRolloverSide < 0) ? -1 : 1;
	c->preset = jer_clamp_int(c->preset, FT_PRESET_CINEMATIC, FT_PRESET_CUSTOM);
}

void ftLoadConfig(void)
{
	FULLTWIST_CONFIG* c = &gFullTwist.cfg;

	c->enabled = jer_config_get_bool(FT_MOD_ID, "enabled", 1);
	c->intensity = jer_config_get_int(FT_MOD_ID, "intensity", FT_DEF_INTENSITY);
	c->minImpact = jer_config_get_int(FT_MOD_ID, "min_impact", FT_DEF_MIN_IMPACT);
	c->offCenter = jer_config_get_int(FT_MOD_ID, "off_center", FT_DEF_OFF_CENTER);
	c->maxRate = jer_config_get_int(FT_MOD_ID, "max_rate", FT_DEF_MAX_RATE);
	c->groundKick = jer_config_get_int(FT_MOD_ID, "ground_kick", FT_DEF_GROUND_KICK);
	c->wallSnag = jer_config_get_int(FT_MOD_ID, "wall_snag", FT_DEF_WALL_SNAG);
	c->damping = jer_config_get_int(FT_MOD_ID, "damping", FT_DEF_DAMPING);
	c->window = jer_config_get_int(FT_MOD_ID, "window", FT_DEF_WINDOW);
	c->trip = jer_config_get_int(FT_MOD_ID, "trip", FT_DEF_TRIP);
	c->launch = jer_config_get_int(FT_MOD_ID, "launch", FT_DEF_LAUNCH);
	c->rolloverDamping = jer_config_get_int(FT_MOD_ID, "rollover_damping", FT_DEF_ROLLOVER_DAMPING);
	c->rolloverCatch = jer_config_get_int(FT_MOD_ID, "rollover_catch", FT_DEF_ROLLOVER_CATCH);
	c->spinRetention = jer_config_get_int(FT_MOD_ID, "spin_retention", FT_DEF_SPIN_RETENTION);
	c->scenery3d = jer_config_get_bool(FT_MOD_ID, "scenery3d", 1);
	c->playerOnly = jer_config_get_bool(FT_MOD_ID, "player_only", 0);
	c->debugLog = jer_config_get_bool(FT_MOD_ID, "debug_log", 0);
	c->testKick = jer_config_get_bool(FT_MOD_ID, "test_kick", 0);
	c->testCrash = jer_config_get_bool(FT_MOD_ID, "test_crash", 0);
	c->testRollover = jer_config_get_bool(FT_MOD_ID, "test_rollover", 0);
	c->testRolloverSide = jer_config_get_int(FT_MOD_ID, "test_rollover_side", 1);
	c->preset = jer_config_get_int(FT_MOD_ID, "preset", FT_PRESET_ARCADE);

	ftClampConfig();
}

void ftApplyPreset(void)
{
	FULLTWIST_CONFIG* c = &gFullTwist.cfg;

	switch (c->preset)
	{
	case FT_PRESET_CINEMATIC:
		c->enabled = 1;
		c->intensity = 140;
		c->minImpact = 104000;
		c->offCenter = 40;
		c->maxRate = 4000000;
		c->groundKick = 40;
		c->damping = 35;
		c->window = 150;
		c->trip = 15;
		c->launch = 30;
		c->scenery3d = 1;
		break;

	case FT_PRESET_ARCADE:
		c->enabled = 1;
		c->intensity = FT_DEF_INTENSITY;
		c->minImpact = FT_DEF_MIN_IMPACT;
		c->offCenter = FT_DEF_OFF_CENTER;
		c->maxRate = FT_DEF_MAX_RATE;
		c->groundKick = FT_DEF_GROUND_KICK;
		c->damping = FT_DEF_DAMPING;
		c->window = FT_DEF_WINDOW;
		c->trip = FT_DEF_TRIP;
		c->launch = FT_DEF_LAUNCH;
		c->scenery3d = 1;
		break;

	case FT_PRESET_OFF:
		c->enabled = 0;
		break;

	case FT_PRESET_CUSTOM:
	default:
		break;
	}

	ftClampConfig();
}

void ftSaveConfig(void)
{
	FULLTWIST_CONFIG* c = &gFullTwist.cfg;

	ftClampConfig();

	jer_config_set_bool(FT_MOD_ID, "enabled", c->enabled);
	jer_config_set_int(FT_MOD_ID, "intensity", c->intensity);
	jer_config_set_int(FT_MOD_ID, "min_impact", c->minImpact);
	jer_config_set_int(FT_MOD_ID, "off_center", c->offCenter);
	jer_config_set_int(FT_MOD_ID, "max_rate", c->maxRate);
	jer_config_set_int(FT_MOD_ID, "ground_kick", c->groundKick);
	jer_config_set_int(FT_MOD_ID, "wall_snag", c->wallSnag);
	jer_config_set_int(FT_MOD_ID, "damping", c->damping);
	jer_config_set_int(FT_MOD_ID, "window", c->window);
	jer_config_set_int(FT_MOD_ID, "trip", c->trip);
	jer_config_set_int(FT_MOD_ID, "launch", c->launch);
	jer_config_set_int(FT_MOD_ID, "rollover_damping", c->rolloverDamping);
	jer_config_set_int(FT_MOD_ID, "rollover_catch", c->rolloverCatch);
	jer_config_set_int(FT_MOD_ID, "spin_retention", c->spinRetention);
	jer_config_set_bool(FT_MOD_ID, "scenery3d", c->scenery3d);
	jer_config_set_bool(FT_MOD_ID, "player_only", c->playerOnly);
	jer_config_set_bool(FT_MOD_ID, "debug_log", c->debugLog);
	jer_config_set_bool(FT_MOD_ID, "test_kick", c->testKick);
	jer_config_set_bool(FT_MOD_ID, "test_crash", c->testCrash);
	jer_config_set_bool(FT_MOD_ID, "test_rollover", c->testRollover);
	jer_config_set_int(FT_MOD_ID, "test_rollover_side", c->testRolloverSide);
	jer_config_set_int(FT_MOD_ID, "preset", c->preset);
}
