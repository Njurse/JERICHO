/*
 * fulltwist_config.c -- FULL TWIST tunables.
 *
 * The ini keys, clamps and presets live here so the event code and the pause
 * menu share one definition. `JERICHO/CONFIG/fulltwist.ini` is the persistent
 * store; the defaults are also the Free Physics preset, the only one that
 * ships.
 */

#include "driver2.h"
#include "jericho.h"
#include "jer_config.h"
#include "jer_math.h"
#include "fulltwist.h"

#define FT_DEF_INTENSITY	100
#define FT_DEF_MIN_IMPACT	72000
#define FT_DEF_OFF_CENTER	20
#define FT_DEF_MAX_RATE		4000000
#define FT_DEF_GROUND_KICK	7
#define FT_DEF_WALL_SNAG	0
#define FT_DEF_DAMPING		0
#define FT_DEF_WINDOW		180
#define FT_DEF_TRIP			20
#define FT_DEF_LAUNCH		40
#define FT_DEF_ROLLOVER_DAMPING	50
#define FT_DEF_SPIN_RETENTION	0
#define FT_DEF_FREE_SPIN		1
#define FT_DEF_ANGULAR_DAMPING	6
#define FT_DEF_CANCEL_EXTRA		100
#define FT_DEF_KEEP_SPIN		1
#define FT_DEF_FREE_SPIN_WINDOW	120
#define FT_DEF_HOLD_CAMERA		1
#define FT_DEF_CAMERA_SNAP		250
#define FT_DEF_CAMERA_RELEASE	1
#define FT_DEF_CAMERA_RELEASE_AV	1800000
#define FT_DEF_HORN_ROLL		1
#define FT_DEF_HORN_ROLL_RATE	3000000
#define FT_DEF_HORN_ROLL_LAUNCH	15000
#define FT_DEF_HULL_CONTACT		0
#define FT_DEF_HULL_FOOTPRINT	1
#define FT_DEF_SCENERY_MODE		1
#define FT_DEF_WALL_BOUNCE		35
#define FT_DEF_CAR_CAR_IMPULSE	50
#define FT_DEF_CAR_CAR_LAUNCH	0
#define FT_DEF_CAR_CAR_TUMBLE	50
#define FT_DEF_BODY_KICK		0
#define FT_DEF_BODY_FRICTION	40
#define FT_DEF_BODY_HINGE		68
#define FT_DEF_BODY_GRAVITY		100

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
	c->spinRetention = jer_clamp_int(c->spinRetention, 0, 100);
	c->freeSpin = c->freeSpin ? 1 : 0;
	c->angularDamping = jer_clamp_int(c->angularDamping, 0, 128);
	c->cancelExtraDamping = jer_clamp_int(c->cancelExtraDamping, 0, 100);
	c->keepSpinAtRest = c->keepSpinAtRest ? 1 : 0;
	c->freeSpinWindow = jer_clamp_int(c->freeSpinWindow, 0, 300);
	c->holdCamera = c->holdCamera ? 1 : 0;
	c->cameraSnap = jer_clamp_int(c->cameraSnap, 0, 100);
	c->cameraRelease = c->cameraRelease ? 1 : 0;
	c->cameraReleaseAv = jer_clamp_int(c->cameraReleaseAv, 0, 8000000);
	c->hornRoll = c->hornRoll ? 1 : 0;
	c->hornRollRate = jer_clamp_int(c->hornRollRate, 0, 8000000);
	c->hornRollLaunch = jer_clamp_int(c->hornRollLaunch, 0, 200000);
	c->hullContact = c->hullContact ? 1 : 0;
	c->hullFootprint = c->hullFootprint ? 1 : 0;
	c->sceneryMode = c->sceneryMode ? 1 : 0;
	c->wallBounce = jer_clamp_int(c->wallBounce, 0, 100);
	c->carCarImpulse = jer_clamp_int(c->carCarImpulse, 0, 100);
	c->carCarLaunch = jer_clamp_int(c->carCarLaunch, 0, 200);
	c->carCarTumble = jer_clamp_int(c->carCarTumble, 0, 200);
	c->bodyKick = jer_clamp_int(c->bodyKick, 0, 200);
	c->bodyFriction = jer_clamp_int(c->bodyFriction, 0, 100);
	c->bodyHinge = jer_clamp_int(c->bodyHinge, 0, 200);
	c->bodyGravity = jer_clamp_int(c->bodyGravity, 0, 200);
	c->scenery3d = c->scenery3d ? 1 : 0;
	c->playerOnly = c->playerOnly ? 1 : 0;
	c->debugLog = c->debugLog ? 1 : 0;
	c->testKick = c->testKick ? 1 : 0;
	c->testCrash = c->testCrash ? 1 : 0;
	c->testSmash = c->testSmash ? 1 : 0;
	c->testRollover = c->testRollover ? 1 : 0;
	c->testRestFlip = c->testRestFlip ? 1 : 0;
	c->testHorn = c->testHorn ? 1 : 0;
	c->testRolloverSide = (c->testRolloverSide < 0) ? -1 : 1;
	c->preset = jer_clamp_int(c->preset, FT_PRESET_CINEMATIC, FT_PRESET_FREE_PHYSICS);
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
	{
		/* Deprecated no-op: rollover_catch used to scale a scripted uprighting
		 * assist. The key is still read so an old ini loads cleanly, but it no
		 * longer changes the physics. */
		int deprecatedRolloverCatch = jer_config_get_int(FT_MOD_ID, "rollover_catch", 0);
		(void)deprecatedRolloverCatch;
	}
	c->spinRetention = jer_config_get_int(FT_MOD_ID, "spin_retention", FT_DEF_SPIN_RETENTION);
	c->freeSpin = jer_config_get_bool(FT_MOD_ID, "free_spin", FT_DEF_FREE_SPIN);
	c->angularDamping = jer_config_get_int(FT_MOD_ID, "angular_damping", FT_DEF_ANGULAR_DAMPING);
	c->cancelExtraDamping = jer_config_get_int(FT_MOD_ID, "cancel_extra_damping", FT_DEF_CANCEL_EXTRA);
	c->keepSpinAtRest = jer_config_get_bool(FT_MOD_ID, "keep_spin_at_rest", FT_DEF_KEEP_SPIN);
	c->freeSpinWindow = jer_config_get_int(FT_MOD_ID, "free_spin_window", FT_DEF_FREE_SPIN_WINDOW);
	c->holdCamera = jer_config_get_bool(FT_MOD_ID, "hold_camera", FT_DEF_HOLD_CAMERA);
	c->cameraSnap = jer_config_get_int(FT_MOD_ID, "camera_snap", FT_DEF_CAMERA_SNAP);
	c->cameraRelease = jer_config_get_bool(FT_MOD_ID, "camera_release", FT_DEF_CAMERA_RELEASE);
	c->cameraReleaseAv = jer_config_get_int(FT_MOD_ID, "camera_release_av", FT_DEF_CAMERA_RELEASE_AV);
	c->hornRoll = jer_config_get_bool(FT_MOD_ID, "horn_roll", FT_DEF_HORN_ROLL);
	c->hornRollRate = jer_config_get_int(FT_MOD_ID, "horn_roll_rate", FT_DEF_HORN_ROLL_RATE);
	c->hornRollLaunch = jer_config_get_int(FT_MOD_ID, "horn_roll_launch", FT_DEF_HORN_ROLL_LAUNCH);
	c->hullContact = jer_config_get_bool(FT_MOD_ID, "hull_contact", FT_DEF_HULL_CONTACT);
	c->hullFootprint = jer_config_get_bool(FT_MOD_ID, "hull_footprint", FT_DEF_HULL_FOOTPRINT);
	c->sceneryMode = jer_config_get_bool(FT_MOD_ID, "scenery_mode", FT_DEF_SCENERY_MODE);
	c->wallBounce = jer_config_get_int(FT_MOD_ID, "wall_bounce", FT_DEF_WALL_BOUNCE);
	c->carCarImpulse = jer_config_get_int(FT_MOD_ID, "car_car_impulse", FT_DEF_CAR_CAR_IMPULSE);
	c->carCarLaunch = jer_config_get_int(FT_MOD_ID, "car_car_launch", FT_DEF_CAR_CAR_LAUNCH);
	c->carCarTumble = jer_config_get_int(FT_MOD_ID, "car_car_tumble", FT_DEF_CAR_CAR_TUMBLE);
	c->bodyKick = jer_config_get_int(FT_MOD_ID, "body_kick", FT_DEF_BODY_KICK);
	c->bodyFriction = jer_config_get_int(FT_MOD_ID, "body_friction", FT_DEF_BODY_FRICTION);
	c->bodyHinge = jer_config_get_int(FT_MOD_ID, "body_hinge", FT_DEF_BODY_HINGE);
	c->bodyGravity = jer_config_get_int(FT_MOD_ID, "body_gravity", FT_DEF_BODY_GRAVITY);
	c->scenery3d = jer_config_get_bool(FT_MOD_ID, "scenery3d", 1);
	c->playerOnly = jer_config_get_bool(FT_MOD_ID, "player_only", 1);
	c->debugLog = jer_config_get_bool(FT_MOD_ID, "debug_log", 0);
	c->testKick = jer_config_get_bool(FT_MOD_ID, "test_kick", 0);
	c->testCrash = jer_config_get_bool(FT_MOD_ID, "test_crash", 0);
	c->testSmash = jer_config_get_bool(FT_MOD_ID, "test_smash", 0);
	c->testRollover = jer_config_get_bool(FT_MOD_ID, "test_rollover", 0);
	c->testRestFlip = jer_config_get_bool(FT_MOD_ID, "test_rest_flip", 0);
	c->testHorn = jer_config_get_bool(FT_MOD_ID, "test_horn", 0);
	c->testRolloverSide = jer_config_get_int(FT_MOD_ID, "test_rollover_side", 1);
	c->preset = jer_config_get_int(FT_MOD_ID, "preset", FT_PRESET_FREE_PHYSICS);

	/* Cinematic and Arcade are retired: Free Physics is the only tuning the
	 * addon ships. An existing ini that still selects a legacy preset is
	 * folded into Free Physics on load, so every player gets the same body.
	 * Off and Custom keep their saved values. */
	if (c->preset == FT_PRESET_CINEMATIC || c->preset == FT_PRESET_ARCADE)
		c->preset = FT_PRESET_FREE_PHYSICS;

	ftClampConfig();
}

void ftApplyPreset(void)
{
	FULLTWIST_CONFIG* c = &gFullTwist.cfg;

	switch (c->preset)
	{
	/* Cinematic and Arcade are retired. Their numeric ids still exist so an
	 * old ini loads, but ftLoadConfig folds them into Free Physics; there is
	 * no body here to select. */
	case FT_PRESET_FREE_PHYSICS:
		c->enabled = 1;
		c->intensity = 100;
		c->minImpact = 72000;
		c->offCenter = 20;
		c->maxRate = 4000000;
		c->groundKick = 25;
		c->wallSnag = 0;
		c->damping = 0;
		c->window = 180;
		c->trip = 20;
		c->launch = 40;
		c->rolloverDamping = 50;
		c->spinRetention = 0;
		c->freeSpin = 1;
		c->angularDamping = 16;
		c->cancelExtraDamping = 100;
		c->keepSpinAtRest = 1;
		c->freeSpinWindow = 120;
		c->hullContact = 0;
		c->hullFootprint = 1;
		c->sceneryMode = 1;
		c->wallBounce = 100;
		c->carCarImpulse = 50;
		c->carCarLaunch = 0;
		c->carCarTumble = 50;
		c->bodyKick = 0;
		c->bodyFriction = FT_DEF_BODY_FRICTION;
		c->bodyHinge = FT_DEF_BODY_HINGE;
		c->bodyGravity = FT_DEF_BODY_GRAVITY;
		c->holdCamera = 1;
		c->cameraSnap = 75;
		c->playerOnly = 1;
		c->cameraRelease = 1;
		c->cameraReleaseAv = 800000;
		c->hornRoll = 1;
		c->hornRollRate = 3000000;
		c->hornRollLaunch = 15000;
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
	jer_config_set_int(FT_MOD_ID, "spin_retention", c->spinRetention);
	jer_config_set_bool(FT_MOD_ID, "free_spin", c->freeSpin);
	jer_config_set_int(FT_MOD_ID, "angular_damping", c->angularDamping);
	jer_config_set_int(FT_MOD_ID, "cancel_extra_damping", c->cancelExtraDamping);
	jer_config_set_bool(FT_MOD_ID, "keep_spin_at_rest", c->keepSpinAtRest);
	jer_config_set_int(FT_MOD_ID, "free_spin_window", c->freeSpinWindow);
	jer_config_set_bool(FT_MOD_ID, "hold_camera", c->holdCamera);
	jer_config_set_int(FT_MOD_ID, "camera_snap", c->cameraSnap);
	jer_config_set_bool(FT_MOD_ID, "camera_release", c->cameraRelease);
	jer_config_set_int(FT_MOD_ID, "camera_release_av", c->cameraReleaseAv);
	jer_config_set_bool(FT_MOD_ID, "horn_roll", c->hornRoll);
	jer_config_set_int(FT_MOD_ID, "horn_roll_rate", c->hornRollRate);
	jer_config_set_int(FT_MOD_ID, "horn_roll_launch", c->hornRollLaunch);
	jer_config_set_bool(FT_MOD_ID, "hull_contact", c->hullContact);
	jer_config_set_bool(FT_MOD_ID, "hull_footprint", c->hullFootprint);
	jer_config_set_bool(FT_MOD_ID, "scenery_mode", c->sceneryMode);
	jer_config_set_int(FT_MOD_ID, "wall_bounce", c->wallBounce);
	jer_config_set_int(FT_MOD_ID, "car_car_impulse", c->carCarImpulse);
	jer_config_set_int(FT_MOD_ID, "car_car_launch", c->carCarLaunch);
	jer_config_set_int(FT_MOD_ID, "car_car_tumble", c->carCarTumble);
	jer_config_set_int(FT_MOD_ID, "body_kick", c->bodyKick);
	jer_config_set_int(FT_MOD_ID, "body_friction", c->bodyFriction);
	jer_config_set_int(FT_MOD_ID, "body_hinge", c->bodyHinge);
	jer_config_set_int(FT_MOD_ID, "body_gravity", c->bodyGravity);
	jer_config_set_bool(FT_MOD_ID, "scenery3d", c->scenery3d);
	jer_config_set_bool(FT_MOD_ID, "player_only", c->playerOnly);
	jer_config_set_bool(FT_MOD_ID, "debug_log", c->debugLog);
	jer_config_set_bool(FT_MOD_ID, "test_kick", c->testKick);
	jer_config_set_bool(FT_MOD_ID, "test_crash", c->testCrash);
	jer_config_set_bool(FT_MOD_ID, "test_smash", c->testSmash);
	jer_config_set_bool(FT_MOD_ID, "test_rollover", c->testRollover);
	jer_config_set_bool(FT_MOD_ID, "test_rest_flip", c->testRestFlip);
	jer_config_set_bool(FT_MOD_ID, "test_horn", c->testHorn);
	jer_config_set_int(FT_MOD_ID, "test_rollover_side", c->testRolloverSide);
	jer_config_set_int(FT_MOD_ID, "preset", c->preset);
}
