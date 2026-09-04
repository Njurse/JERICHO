/*
 * collisiondevil.c — COLLISIONDEVIL arcade handling overhaul.
 *
 * A compiled-in JERICHO deep mod. It listens to the five car-handling events
 * this package adds (JER_EVENT_CAR_*) and re-shapes the stock physics with a
 * handful of macro sliders:
 *
 *   Arcade Aggression  -> engine force (acceleration + emergent top speed)
 *   Drift Eagerness    -> rear-grip drop + yaw kick (brake-tap powerslides)
 *   Boost Intensity    -> extra engine multiplier
 *   Visual Drama       -> render-only body roll/pitch/yaw + FOV pull
 *
 * Golden rule: DERIVE, DON'T INVENT. Every value scales an existing chassis
 * stat (car_cosmetics[].powerRatio / traction / twistRateY, cp->wheel_angle,
 * the frontFS/rearFS friction scales) — there are no per-car tuning tables.
 *
 * The visual pass never touches physics. The body model is a *projection* of
 * a model onto the car, not the car itself: drama rotates a render-only copy
 * of cp->hd.drawCarMat (see the JER_EVENT_CAR_DRAW call site in cars.c), while
 * the physics/collision matrix cp->hd.where is never written.
 *
 * Two-page pause menu:
 *   Page 1 "COLLISIONDEVIL": toggle + aggression/eagerness/boost + preset
 *   Page 2 "Visual Theater": drama + FOV pull + reset
 */

#include "driver2.h"
#include "collisiondevil.h"
#include "cars.h"
#include "cosmetic.h"
#include "camera.h"
#include "convert.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_pause_menu.h"
#include "jer_config.h"
#include "jer_math.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

COLLISIONDEVIL_CONFIG gCdCfg;
static COLLISIONDEVIL_DRIFT  gDrift[MAX_CARS];
static COLLISIONDEVIL_VISUAL gVisual[MAX_CARS];

static const char* const kPresetNames[] = { "Burnout 3", "Paradise", "Custom" };

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

static void cdLoadConfig(void)
{
	gCdCfg.enabled    = jer_config_get_bool("collisiondevil", "enabled", 1);
	gCdCfg.aggression = jer_config_get_int("collisiondevil", "aggression", 65);
	gCdCfg.eagerness  = jer_config_get_int("collisiondevil", "eagerness", 65);
	gCdCfg.drama      = jer_config_get_int("collisiondevil", "drama", 65);
	gCdCfg.boost      = jer_config_get_int("collisiondevil", "boost", 65);
	gCdCfg.preset     = jer_config_get_int("collisiondevil", "preset", CD_PRESET_BURNOUT);
	gCdCfg.fovPull    = jer_config_get_int("collisiondevil", "fov_pull", 40);

	gCdCfg.enabled    = gCdCfg.enabled ? 1 : 0;
	gCdCfg.aggression = jer_clamp_int(gCdCfg.aggression, 0, 100);
	gCdCfg.eagerness  = jer_clamp_int(gCdCfg.eagerness, 0, 100);
	gCdCfg.drama      = jer_clamp_int(gCdCfg.drama, 0, 100);
	gCdCfg.boost      = jer_clamp_int(gCdCfg.boost, 0, 100);
	gCdCfg.preset     = jer_clamp_int(gCdCfg.preset, CD_PRESET_BURNOUT, CD_PRESET_CUSTOM);
	gCdCfg.fovPull    = jer_clamp_int(gCdCfg.fovPull, 0, 100);
}

static void cdSaveConfig(void)
{
	jer_config_set_bool("collisiondevil", "enabled", gCdCfg.enabled);
	jer_config_set_int("collisiondevil", "aggression", gCdCfg.aggression);
	jer_config_set_int("collisiondevil", "eagerness", gCdCfg.eagerness);
	jer_config_set_int("collisiondevil", "drama", gCdCfg.drama);
	jer_config_set_int("collisiondevil", "boost", gCdCfg.boost);
	jer_config_set_int("collisiondevil", "preset", gCdCfg.preset);
	jer_config_set_int("collisiondevil", "fov_pull", gCdCfg.fovPull);
}

static void cdApplyPreset(void)
{
	switch (gCdCfg.preset)
	{
		case CD_PRESET_BURNOUT:
			gCdCfg.aggression = 85;
			gCdCfg.eagerness  = 90;
			gCdCfg.boost      = 85;
			gCdCfg.drama      = 70;
			gCdCfg.fovPull    = 55;
			break;
		case CD_PRESET_PARADISE:
			gCdCfg.aggression = 50;
			gCdCfg.eagerness  = 60;
			gCdCfg.boost      = 40;
			gCdCfg.drama      = 90;
			gCdCfg.fovPull    = 30;
			break;
		case CD_PRESET_CUSTOM:
			break;
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// scale a fixed-point value by a 0..4096 factor (4096 = 1.0x)
static int cdScale(int v, int scale)
{
	return (v * scale) >> 12;
}

static int cdPercent(int p)
{
	return (p * 4096) / 100;
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

static int cdOnBoot(void* ud, void* args)
{
	(void)ud;
	(void)args;
	cdLoadConfig();
	cdApplyPreset();
	cdSaveConfig();
	return JER_RESULT_CONTINUE;
}

static int cdOnResetCar(void* ud, void* args)
{
	JER_ARGS_RESET_CAR* a = (JER_ARGS_RESET_CAR*)args;
	(void)ud;

	if (a->carId >= 0 && a->carId < MAX_CARS)
	{
		gDrift[a->carId].active = 0;
		gDrift[a->carId].direction = 1;
		gDrift[a->carId].blend = 0;
		gVisual[a->carId].roll = 0;
		gVisual[a->carId].pitch = 0;
		gVisual[a->carId].yaw = 0;
	}
	return JER_RESULT_CONTINUE;
}

// CAR_ENGINE: overclock engine force + widen steering.
static int cdOnCarEngine(void* ud, void* args)
{
	JER_ARGS_CAR_ENGINE* a = (JER_ARGS_CAR_ENGINE*)args;
	(void)ud;

	if (!gCdCfg.enabled)
		return JER_RESULT_CONTINUE;

	// Acceleration: overclock engine force (raises accel + emergent top speed).
	if (a->thrust > 0)
	{
		int aggressionScale = 4096 + cdPercent(gCdCfg.aggression) / 2;  // 1.0x..1.5x
		int boostScale      = 4096 + cdPercent(gCdCfg.boost) / 2;       // 1.0x..1.5x
		a->thrust = cdScale(cdScale(a->thrust, aggressionScale), boostScale);
	}
	// Braking/reverse: soften it so the brake rotates the car into a drift
	// instead of killing its momentum (thrust stays < 0 so drift entry still
	// sees a braking state).
	else if (a->thrust < 0)
	{
		a->thrust = cdScale(a->thrust, CD_BRAKE_SOFTEN);
	}

	// Sharper turn-in: widen the existing steering angle as eagerness rises
	// (the manifesto's "Drift Angle = base_steering_angle * 1.8").
	if (gCdCfg.eagerness > 0 && a->wheelAngle != 0)
	{
		int widen = 4096 + (cdPercent(gCdCfg.eagerness) * 4) / 5;  // 1.0x..1.8x
		a->wheelAngle = cdScale(a->wheelAngle, widen);
	}

	return JER_RESULT_CONTINUE;
}

// CAR_STEP: per-car think tick — detect drift + compute visual drama targets.
static int cdOnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!gCdCfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	COLLISIONDEVIL_DRIFT* d = &gDrift[cp->id];
	COLLISIONDEVIL_VISUAL* v = &gVisual[cp->id];

	// --- drift detection (handbrake, or brake + steer, at speed) ---
	int steer = cp->wheel_angle;
	int braking = cp->thrust < 0;
	int handbrake = cp->handbrake != 0;
	int wantDrift = (handbrake || (braking && ABS(steer) > CD_STEER_MIN))
		&& a->speed > CD_DRIFT_MIN_SPEED;

	if (wantDrift)
	{
		if (!d->active)
		{
			d->direction = steer >= 0 ? 1 : -1;
			if (d->direction == 0)
				d->direction = 1;
		}
		d->active = 1;
	}
	else
	{
		d->active = 0;
	}

	d->blend = jer_lerp_int(d->blend, d->active ? 4096 : 0, CD_BLEND_LERP);

	// --- visual drama targets (render-only) ---
	int dramaFrac = cdPercent(gCdCfg.drama);
	int speedNorm = jer_clamp_int(a->speed, 0, CD_DRAMA_REF_SPEED) * 4096 / CD_DRAMA_REF_SPEED;

	int rollTarget = 0, pitchTarget = 0;
	if (dramaFrac > 0)
	{
		// roll: steering input + lateral momentum (side slip) — the body leans
		// into the corner and rolls further as it slides.
		int steerRoll = -cdScale(steer, dramaFrac) >> CD_DRAMA_ROLL_SHIFT;
		int slipRoll = cdScale(a->velX >> CD_SLIP_SHIFT, dramaFrac);
		rollTarget = cdScale(steerRoll + slipRoll, speedNorm);

		// pitch: nose UP under power (rear squats), nose DOWN under brake.
		int pitchSign = cp->thrust > 0 ? -1 : (cp->thrust < 0 ? 1 : 0);
		pitchTarget = cdScale(pitchSign * CD_DRAMA_PITCH_BASE, dramaFrac);
		pitchTarget = cdScale(pitchTarget, speedNorm);
	}

	// Clamp the body angle — snappy arcade lean, never absurd.
	rollTarget = jer_clamp_int(rollTarget, -CD_BODY_MAX_ROLL, CD_BODY_MAX_ROLL);
	pitchTarget = jer_clamp_int(pitchTarget, -CD_BODY_MAX_PITCH, CD_BODY_MAX_PITCH);

	v->roll  = jer_lerp_int(v->roll,  rollTarget,  CD_VISUAL_LERP);
	v->pitch = jer_lerp_int(v->pitch, pitchTarget, CD_VISUAL_LERP);

	return JER_RESULT_CONTINUE;
}

// CAR_FRICTION: drop rear grip while drifting.
static int cdOnCarFriction(void* ud, void* args)
{
	JER_ARGS_CAR_FRICTION* a = (JER_ARGS_CAR_FRICTION*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!gCdCfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	int blend = gDrift[cp->id].blend;
	if (blend <= 0)
		return JER_RESULT_CONTINUE;

	int eagernessFrac = cdPercent(gCdCfg.eagerness);
	int drop = cdScale(CD_GRIP_DROP_FRAC, blend);      // 0..~0.55 at full blend
	drop = cdScale(drop, eagernessFrac);               // scaled by eagerness

	// Oversteer: drop the REAR friction only (front keeps grip so the nose
	// bites and the rear steps out). rearFS is fixed point; dropping it toward
	// ~0.45x is the manifesto's "Drift Grip = base_friction * 0.45".
	a->rearFS = a->rearFS - cdScale(a->rearFS, drop);

	return JER_RESULT_CONTINUE;
}

// CAR_TORQUE: yaw kick into the drift.
static int cdOnCarTorque(void* ud, void* args)
{
	JER_ARGS_CAR_TORQUE* a = (JER_ARGS_CAR_TORQUE*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!gCdCfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	// Lower the general angular damping so cars twist and flip more freely
	// (ConvertTorqueToAngularAcceleration damps with -avel*128/4096).
	cp->hd.aacc[0] += cp->st.n.angularVelocity[0] >> CD_DAMP_SHIFT;
	cp->hd.aacc[1] += cp->st.n.angularVelocity[1] >> CD_DAMP_SHIFT;
	cp->hd.aacc[2] += cp->st.n.angularVelocity[2] >> CD_DAMP_SHIFT;

	// drift yaw kick (only while drifting)
	int blend = gDrift[cp->id].blend;
	if (blend > 0)
	{
		int eagernessFrac = cdPercent(gCdCfg.eagerness);
		int kick = cp->ap.carCos->twistRateY * CD_YAW_KICK_SCALE / 2;  // derived from yaw inertia
		kick *= gDrift[cp->id].direction;
		kick = cdScale(cdScale(kick, blend), eagernessFrac);
		a->yawTorque = kick;
	}

	return JER_RESULT_CONTINUE;
}

// CAR_DRAW: apply the render-only visual rotation. `matrix` is a COPY of
// cp->hd.drawCarMat made in DrawCar — rotating it tilts the body model's
// projection without ever touching cp->hd.where (the actual car / collision).
static int cdOnCarDraw(void* ud, void* args)
{
	JER_ARGS_CAR_DRAW* a = (JER_ARGS_CAR_DRAW*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	MATRIX* m = (MATRIX*)a->matrix;
	(void)ud;

	if (!gCdCfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	COLLISIONDEVIL_VISUAL* v = &gVisual[cp->id];
	if (v->pitch != 0)
		_RotMatrixX(m, (short)v->pitch);
	if (v->roll != 0)
		_RotMatrixZ(m, (short)v->roll);

	return JER_RESULT_CONTINUE;
}

// CAMERA: speed-based FOV pull (narrower scr_z = wider FOV).
static int cdOnCamera(void* ud, void* args)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	(void)ud;

	if (!gCdCfg.enabled || gCdCfg.fovPull <= 0 || !a->inCar)
		return JER_RESULT_CONTINUE;

	int speed = FIXEDH(a->carSpeed);
	int spd = jer_clamp_int(speed, 0, CD_DRAMA_REF_SPEED);
	int pull = spd * gCdCfg.fovPull * CD_FOV_PULL_SCRZ / (CD_DRAMA_REF_SPEED * 100);

	SetGeomScreen(gCameraDefaultScrZ - pull);
	return JER_RESULT_CONTINUE;
}

// COLLISION: throw more angular momentum into a car on impact so crashes
// twist and flip instead of bouncing flat.
static int cdOnCollision(void* ud, void* args)
{
	JER_ARGS_COLLISION* a = (JER_ARGS_COLLISION*)args;
	CAR_DATA* cp0 = (CAR_DATA*)a->car0;
	CAR_DATA* cp1 = (CAR_DATA*)a->car1;
	(void)ud;

	if (!gCdCfg.enabled || cp0 == NULL)
		return JER_RESULT_CONTINUE;

	int spin = a->howHard >> CD_CRASH_SPIN_SHIFT;
	if (spin <= 0)
		return JER_RESULT_CONTINUE;

	if (cp0->id >= 0 && cp0->id < MAX_CARS)
	{
		cp0->st.n.angularVelocity[0] += (cp0->id & 1) ? spin : -spin;
		cp0->st.n.angularVelocity[1] += (cp0->id & 2) ? spin : -spin;
		cp0->st.n.angularVelocity[2] += spin;
	}
	if (cp1 != NULL && cp1->id >= 0 && cp1->id < MAX_CARS)
	{
		cp1->st.n.angularVelocity[0] -= (cp1->id & 1) ? spin : -spin;
		cp1->st.n.angularVelocity[1] -= (cp1->id & 2) ? spin : -spin;
		cp1->st.n.angularVelocity[2] -= spin;
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Pause menu (2 pages)
// ---------------------------------------------------------------------------

static void cdLabelEnabled(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Enabled: %s", gCdCfg.enabled ? "ON" : "OFF"); }
static int  cdToggleEnabled(void* ud, int dir) { (void)ud; (void)dir; gCdCfg.enabled = !gCdCfg.enabled; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelAggression(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Arcade Aggression: %d%%", gCdCfg.aggression); }
static int  cdAdjAggression(void* ud, int dir) { (void)ud; gCdCfg.aggression = jer_clamp_int(gCdCfg.aggression + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelEagerness(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Drift Eagerness: %d%%", gCdCfg.eagerness); }
static int  cdAdjEagerness(void* ud, int dir) { (void)ud; gCdCfg.eagerness = jer_clamp_int(gCdCfg.eagerness + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelBoost(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Boost Intensity: %d%%", gCdCfg.boost); }
static int  cdAdjBoost(void* ud, int dir) { (void)ud; gCdCfg.boost = jer_clamp_int(gCdCfg.boost + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelPreset(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Preset: %s", kPresetNames[gCdCfg.preset]); }
static int  cdCyclePreset(void* ud, int dir) { (void)ud; (void)dir; gCdCfg.preset = (gCdCfg.preset + 1) % 3; cdApplyPreset(); cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelDrama(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Visual Drama: %d%%", gCdCfg.drama); }
static int  cdAdjDrama(void* ud, int dir) { (void)ud; gCdCfg.drama = jer_clamp_int(gCdCfg.drama + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelFov(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Camera FOV Pull: %d%%", gCdCfg.fovPull); }
static int  cdAdjFov(void* ud, int dir) { (void)ud; gCdCfg.fovPull = jer_clamp_int(gCdCfg.fovPull + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static int cdResetDefaults(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCdCfg.preset = CD_PRESET_BURNOUT;
	cdApplyPreset();
	cdSaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM cdVisualItems[] =
{
	{ NULL, cdLabelDrama, cdAdjDrama, NULL, NULL, 1 },
	{ NULL, cdLabelFov,   cdAdjFov,   NULL, NULL, 1 },
	{ "Reset to Defaults", NULL, cdResetDefaults, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU cdVisualMenu =
{ "Visual Theater", cdVisualItems, 3 };

static const JER_PAUSE_MENU_ITEM cdMainItems[] =
{
	{ NULL, cdLabelEnabled,   cdToggleEnabled, NULL, NULL, 0 },
	{ NULL, cdLabelAggression, cdAdjAggression, NULL, NULL, 1 },
	{ NULL, cdLabelEagerness,  cdAdjEagerness,  NULL, NULL, 1 },
	{ NULL, cdLabelBoost,      cdAdjBoost,      NULL, NULL, 1 },
	{ NULL, cdLabelPreset,     cdCyclePreset,   NULL, NULL, 1 },
	{ "Visual Theater", NULL, NULL, NULL, &cdVisualMenu, 0 },
};

static const JER_PAUSE_MENU cdMainMenu =
{ "COLLISIONDEVIL", cdMainItems, 6 };

// ---------------------------------------------------------------------------
// Module entry
// ---------------------------------------------------------------------------

JER_MODULE_ENTRY(jer_module_collisiondevil_entry)(JERICHO_CONTEXT* ctx)
{
	// Load + apply config here, not just on JER_EVENT_BOOT: the Mods menu
	// enables/reloads modules via jer_manager_reload, which re-runs this entry
	// but does NOT re-fire JER_EVENT_BOOT. Without this, a module enabled
	// mid-session would leave gCdCfg zero-initialised and stay inert.
	cdLoadConfig();
	cdApplyPreset();

	ctx->jer_register_module(ctx,
		"collisiondevil",			/* id */
		"COLLISIONDEVIL",			/* name */
		"0.1.0",					/* version */
		"JERICHO",					/* author */
		"Arcade handling overhaul: derived speed boost, brake-tap powerslides, render-only visual drama, and a 2-page pause menu.",	/* description */
		"",							/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, cdOnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, cdOnResetCar, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE, cdOnCarEngine, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cdOnCarStep, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_FRICTION, cdOnCarFriction, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_TORQUE, cdOnCarTorque, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW, cdOnCarDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_COLLISION, cdOnCollision, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, cdOnCamera, NULL, 0);

	jer_pause_menu_register(&cdMainMenu);

	ctx->jer_log(ctx, "[collisiondevil] registered (SDK v%d)\n", ctx->sdkVersion);
}
