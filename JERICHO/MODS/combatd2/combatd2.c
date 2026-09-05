/*
 * combatd2.c — Combat D2: Twisted Metal 2 style arcade handling.
 *
 * A compiled-in JERICHO deep mod. It replaces the horizontal motion of the
 * stock Driver 2 sim with a point-mass rigid body at JER_EVENT_CAR_TORQUE
 * (the tail of StepOneCar, right before the engine integrates velocity and
 * orientation):
 *
 *   * throttle -> direct forward/backward velocity (no engine/gears)
 *   * steering -> direct yaw rate (target yaw = handling * steer, works at
 *     zero speed: rotate in place)
 *   * lateral velocity -> damped by a grip force that falls off when you
 *     steer hard at speed, producing controlled drifts
 *
 * Vertical motion (gravity + ground lift) and roll/pitch are left to the
 * stock code so the car still rides the terrain. Collisions stay stock
 * (mass-based push); the grip + drag make recovery forgiving — the TM2 feel.
 */

#include "driver2.h"
#include "combatd2.h"
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

// reference powerRatio (short) that maps to a 1.0x accel/top-speed scale.
#define CD2_REF_POWER 3000

CD2_CONFIG gCd2Cfg;
static CD2_CAR gCd2Car[MAX_CARS];

static const char* const kPresetNames[] = { "Default", "Turbo", "Drifty", "Custom" };
static const char* const kTightInputNames[] = { "Handbrake", "Wheelspin", "Off" };

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

static void cd2LoadConfig(void)
{
	gCd2Cfg.enabled  = jer_config_get_bool("combatd2", "enabled", 1);
	gCd2Cfg.topSpeed = jer_config_get_int("combatd2", "top_speed", CD2_TOP_SPEED);
	gCd2Cfg.accel    = jer_config_get_int("combatd2", "accel", CD2_ACCEL);
	gCd2Cfg.handling = jer_config_get_int("combatd2", "handling", CD2_HANDLING);
	gCd2Cfg.grip     = jer_config_get_int("combatd2", "grip", CD2_GRIP);
	gCd2Cfg.preset   = jer_config_get_int("combatd2", "preset", CD2_PRESET_DEFAULT);
	gCd2Cfg.fovPull  = jer_config_get_int("combatd2", "fov_pull", 40);
	gCd2Cfg.tightTurn     = jer_config_get_int("combatd2", "tight_enabled", CD2_TIGHT_ENABLED_DEFAULT);
	gCd2Cfg.tightStrength = jer_config_get_int("combatd2", "tight_strength", CD2_TIGHT_STRENGTH_DEFAULT);
	gCd2Cfg.tightInput    = jer_config_get_int("combatd2", "tight_input", CD2_TIGHT_INPUT_DEFAULT);

	gCd2Cfg.enabled  = gCd2Cfg.enabled ? 1 : 0;
	gCd2Cfg.topSpeed = jer_clamp_int(gCd2Cfg.topSpeed, 60, 400);
	gCd2Cfg.accel    = jer_clamp_int(gCd2Cfg.accel, 1, 24);
	gCd2Cfg.handling = jer_clamp_int(gCd2Cfg.handling, 10, 120);
	gCd2Cfg.grip     = jer_clamp_int(gCd2Cfg.grip, 128, 4096);
	gCd2Cfg.preset   = jer_clamp_int(gCd2Cfg.preset, CD2_PRESET_DEFAULT, CD2_PRESET_CUSTOM);
	gCd2Cfg.fovPull  = jer_clamp_int(gCd2Cfg.fovPull, 0, 100);
	gCd2Cfg.tightTurn     = gCd2Cfg.tightTurn ? 1 : 0;
	gCd2Cfg.tightStrength = jer_clamp_int(gCd2Cfg.tightStrength, 0, 100);
	gCd2Cfg.tightInput    = jer_clamp_int(gCd2Cfg.tightInput, CD2_TIGHT_INPUT_HANDBRAKE, CD2_TIGHT_INPUT_OFF);
}

static void cd2SaveConfig(void)
{
	jer_config_set_bool("combatd2", "enabled", gCd2Cfg.enabled);
	jer_config_set_int("combatd2", "top_speed", gCd2Cfg.topSpeed);
	jer_config_set_int("combatd2", "accel", gCd2Cfg.accel);
	jer_config_set_int("combatd2", "handling", gCd2Cfg.handling);
	jer_config_set_int("combatd2", "grip", gCd2Cfg.grip);
	jer_config_set_int("combatd2", "preset", gCd2Cfg.preset);
	jer_config_set_int("combatd2", "fov_pull", gCd2Cfg.fovPull);
	jer_config_set_int("combatd2", "tight_enabled", gCd2Cfg.tightTurn);
	jer_config_set_int("combatd2", "tight_strength", gCd2Cfg.tightStrength);
	jer_config_set_int("combatd2", "tight_input", gCd2Cfg.tightInput);
}

static void cd2ApplyPreset(void)
{
	switch (gCd2Cfg.preset)
	{
		case CD2_PRESET_TURBO:
			gCd2Cfg.topSpeed = CD2_TOP_SPEED * 13 / 10;
			gCd2Cfg.accel    = CD2_ACCEL * 13 / 10;
			gCd2Cfg.handling = CD2_HANDLING * 9 / 10;
			gCd2Cfg.grip     = CD2_GRIP * 12 / 10;
			break;
		case CD2_PRESET_DRIFTY:
			gCd2Cfg.topSpeed = CD2_TOP_SPEED;
			gCd2Cfg.accel    = CD2_ACCEL;
			gCd2Cfg.handling = CD2_HANDLING * 12 / 10;
			gCd2Cfg.grip     = CD2_GRIP * 7 / 10;
			break;
		case CD2_PRESET_DEFAULT:
		case CD2_PRESET_CUSTOM:
		default:
			gCd2Cfg.topSpeed = CD2_TOP_SPEED;
			gCd2Cfg.accel    = CD2_ACCEL;
			gCd2Cfg.handling = CD2_HANDLING;
			gCd2Cfg.grip     = CD2_GRIP;
			break;
	}
}

// ---------------------------------------------------------------------------
// Per-vehicle stats
// ---------------------------------------------------------------------------

static CD2_STATS cd2GetStats(CAR_DATA* cp)
{
	CD2_STATS s;

	s.topSpeed     = gCd2Cfg.topSpeed;
	s.reverseSpeed = CD2_REVERSE_SPEED;
	s.accel        = gCd2Cfg.accel;
	s.brake        = CD2_BRAKE;
	s.drag         = CD2_DRAG;
	s.handling     = gCd2Cfg.handling;
	s.angularAccel = CD2_ANGULAR_ACCEL;
	s.grip         = gCd2Cfg.grip;
	s.control      = 4096; // average baseline; derived per-car later

	if (cp->ap.carCos != NULL)
	{
		// Vehicle variety from existing chassis stats (no hardcoded table):
		//   powerRatio (short) -> accel + a milder top-speed nudge
		//   traction (fixed 4096 = stock) -> grip
		int power = cp->ap.carCos->powerRatio;
		if (power > 0)
		{
			int ps = jer_clamp_int((power * 4096) / CD2_REF_POWER, 2048, 6144); // 0.5x..1.5x
			s.accel    = (int)(((long long)s.accel * ps) >> 12);
			s.topSpeed = (int)(((long long)s.topSpeed * (4096 + (ps - 4096) / 2)) >> 12);
		}

		int traction = cp->ap.carCos->traction;
		if (traction > 0 && traction != 4096)
			s.grip = (int)(((long long)s.grip * traction) >> 12);
	}

	return s;
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

static int cd2OnBoot(void* ud, void* args)
{
	(void)ud;
	(void)args;
	cd2LoadConfig();
	cd2ApplyPreset();
	cd2SaveConfig();
	return JER_RESULT_CONTINUE;
}

static int cd2OnResetCar(void* ud, void* args)
{
	JER_ARGS_RESET_CAR* a = (JER_ARGS_RESET_CAR*)args;
	(void)ud;

	if (a->carId >= 0 && a->carId < MAX_CARS)
	{
		gCd2Car[a->carId].yawRate = 0;
		gCd2Car[a->carId].slip = 0;
		gCd2Car[a->carId].roll = 0;
		gCd2Car[a->carId].throttle = 0;
		gCd2Car[a->carId].pivotDir = 0;
	}
	return JER_RESULT_CONTINUE;
}

// CAR_STEP: capture the raw throttle BEFORE the stock wheel-force code can
// change it. AddWheelForcesDriver1 -> GetFrictionScalesDriver1 forces
// cp->thrust = 0 while the handbrake is held (so the handbrake alone would
// otherwise read as "coast"); the tight turn needs to know gas is still down.
static int cd2OnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!gCd2Cfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	gCd2Car[cp->id].throttle = (cp->thrust > 0) ? 1 : (cp->thrust < 0) ? -1 : 0;
	return JER_RESULT_CONTINUE;
}

// CAR_TORQUE: the point-mass integrator. Runs at the very end of StepOneCar,
// after the stock wheel forces are computed but before the engine integrates
// velocity + orientation, so we can zero the stock horizontal forces and write
// our own velocity / yaw.
static int cd2OnCarTorque(void* ud, void* args)
{
	JER_ARGS_CAR_TORQUE* a = (JER_ARGS_CAR_TORQUE*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!gCd2Cfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	switch (cp->controlType)
	{
		case CONTROL_TYPE_PLAYER:
		case CONTROL_TYPE_CIV_AI:
		case CONTROL_TYPE_PURSUER_AI:
		case CONTROL_TYPE_LEAD_AI:
		case CONTROL_TYPE_CUTSCENE:
			break;
		default:
			return JER_RESULT_CONTINUE; // NONE / camera & tanner colliders
	}

	CD2_CAR* c = &gCd2Car[cp->id];
	CD2_STATS s = cd2GetStats(cp);

	// ---- steering: direct yaw control -------------------------------
	int steer = jer_clamp_int((int)cp->wheel_angle, -CD2_STEER_MAX, CD2_STEER_MAX);
	int steerFp = (steer * 4096) / CD2_STEER_MAX;           // -4096..4096
	int handlingNow = (int)(((long long)s.handling * s.control) >> 12);

	// ---- Tight Turn (TMB): acute pivot on its own yaw authority -----
	// Player-only. While the trigger is held, steering picks/latches a pivot
	// direction; releasing clears it. It is independent of the grip pass, so
	// the pivot keeps full authority through any slide (TMB: "overrides
	// normal physics for a brief moment").
	int tightActive = 0;
	if (gCd2Cfg.tightTurn && gCd2Cfg.tightInput != CD2_TIGHT_INPUT_OFF &&
		cp->controlType == CONTROL_TYPE_PLAYER)
	{
		if (gCd2Cfg.tightInput == CD2_TIGHT_INPUT_HANDBRAKE)
			tightActive = cp->handbrake;
		else if (gCd2Cfg.tightInput == CD2_TIGHT_INPUT_WHEELSPIN)
			tightActive = cp->wheelspin;
	}

	if (tightActive)
	{
		if ((steer < 0 ? -steer : steer) > CD2_TIGHT_STEER_MIN)
			c->pivotDir = steer > 0 ? 1 : -1;
	}
	else
	{
		c->pivotDir = 0;
	}

	int targetYaw;
	if (tightActive && c->pivotDir != 0)
	{
		// forced acute rotation: handling floor up to tight rate (x control)
		long long pivot = ((long long)CD2_TIGHT_RATE * s.control) >> 12;
		pivot = handlingNow + ((pivot - handlingNow) * gCd2Cfg.tightStrength) / 100;
		targetYaw = (int)pivot * c->pivotDir;
	}
	else
	{
		targetYaw = (handlingNow * steerFp) >> 12;           // PSX-units/frame
	}

	// yaw accelerates toward the target; snappier onset during a pivot
	int yawStep = s.angularAccel * (tightActive ? CD2_TIGHT_ANG_MULT : 1);
	int yaw = c->yawRate;
	if (yaw < targetYaw)
	{
		yaw += yawStep;
		if (yaw > targetYaw) yaw = targetYaw;
	}
	else if (yaw > targetYaw)
	{
		yaw -= yawStep;
		if (yaw < targetYaw) yaw = targetYaw;
	}
	c->yawRate = yaw;

	// ---- forward / right vectors (x-z plane, fixed-point unit) -------
	int fx = cp->hd.where.m[0][2];
	int fz = cp->hd.where.m[2][2];
	int rx = cp->hd.where.m[0][0];
	int rz = cp->hd.where.m[2][0];

	int velX = cp->st.n.linearVelocity[0];
	int velZ = cp->st.n.linearVelocity[2];

	long long fwdSpeed = ((long long)velX * fx + (long long)velZ * fz) >> 24; // speed units (vel and the unit vector are both 4096-scaled)

	// throttle is snapshotted at CAR_STEP: the stock wheel-force code zeroes
	// cp->thrust while the handbrake is held, which would otherwise read as
	// "coast" mid tight turn.
	int throttle = c->throttle;

	// ---- throttle / brake / reverse / drag --------------------------
	if (throttle > 0)
	{
		if (fwdSpeed < s.topSpeed)
		{
			velX += fx * s.accel;
			velZ += fz * s.accel;
		}
		else
		{
			velX -= (int)(((long long)velX * s.drag) >> 12);
			velZ -= (int)(((long long)velZ * s.drag) >> 12);
		}
	}
	else if (throttle < 0)
	{
		if (fwdSpeed > -s.reverseSpeed)
		{
			velX -= fx * s.brake;
			velZ -= fz * s.brake;
		}
		else
		{
			velX -= (int)(((long long)velX * s.drag) >> 12);
			velZ -= (int)(((long long)velZ * s.drag) >> 12);
		}
	}
	else
	{
		velX -= (int)(((long long)velX * s.drag) >> 12);
		velZ -= (int)(((long long)velZ * s.drag) >> 12);
	}

	// tight-turn momentum bleed: the forced pivot sheds horizontal speed, so
	// holding gas yields a short drift-slide arc rather than a dead stop
	if (tightActive && c->pivotDir != 0)
	{
		velX = (int)(((long long)velX * (4096 - CD2_TIGHT_BLEED)) >> 12);
		velZ = (int)(((long long)velZ * (4096 - CD2_TIGHT_BLEED)) >> 12);
	}

	// ---- lateral grip (drift) ---------------------------------------
	long long latVel = ((long long)velX * rx + (long long)velZ * rz) >> 24; // speed units (4096-scaled dot)

	int absSteer = steerFp < 0 ? -steerFp : steerFp;          // 0..4096
	long long absSpeed = fwdSpeed < 0 ? -fwdSpeed : fwdSpeed;
	if (absSpeed > s.topSpeed) absSpeed = s.topSpeed;

	// slipFactor 0..4096: hard steer + high speed -> grip falls off
	long long slipFactor = (absSteer * absSpeed) / s.topSpeed;
	int gripDrop = (int)((slipFactor * CD2_SLIP_REDUCTION) / 10);
	int grip = (int)(((long long)s.grip * (4096 - gripDrop)) >> 12);

	// damp the lateral component: vel -= right * latVel * grip
	velX -= (int)(((long long)rx * latVel * grip) >> 12);
	velZ -= (int)(((long long)rz * latVel * grip) >> 12);

	c->slip = (int)latVel; // for the visual lean

	// ---- write the point-mass state ---------------------------------
	cp->st.n.linearVelocity[0] = velX;
	cp->st.n.linearVelocity[2] = velZ;
	cp->st.n.angularVelocity[1] = yaw * CD2_AV_PER_UNIT;

	// neutralise the stock horizontal force + yaw torque so the engine's
	// later velocity += acc / angularVelocity += aacc add nothing on these axes
	cp->hd.acc[0] = 0;
	cp->hd.acc[2] = 0;
	cp->hd.aacc[1] = 0;

	// refresh the derived speed fields the rest of the game reads this frame
	{
		int ax = ABS(FIXEDH(velX));
		int az = ABS(FIXEDH(velZ));
		cp->hd.speed = (ax < az) ? (az + ax / 2) : (ax + az / 2);
		cp->hd.wheel_speed = (int)(((long long)velX * fx + (long long)velZ * fz) >> 12);
	}

	return JER_RESULT_CONTINUE;
}

// CAR_DRAW: render-only body lean into the slide (physics matrix untouched).
static int cd2OnCarDraw(void* ud, void* args)
{
	JER_ARGS_CAR_DRAW* a = (JER_ARGS_CAR_DRAW*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	MATRIX* m = (MATRIX*)a->matrix;
	(void)ud;

	if (!gCd2Cfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	CD2_CAR* c = &gCd2Car[cp->id];
	int target = jer_clamp_int(-c->slip * CD2_ROLL_GAIN, -CD2_BODY_MAX_ROLL, CD2_BODY_MAX_ROLL);
	c->roll = jer_lerp_int(c->roll, target, CD2_ROLL_LERP);

	if (c->roll != 0)
		_RotMatrixZ(m, (short)c->roll);

	return JER_RESULT_CONTINUE;
}

// CAMERA: speed-based FOV pull (narrower scr_z = wider FOV).
static int cd2OnCamera(void* ud, void* args)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	(void)ud;

	if (!gCd2Cfg.enabled || gCd2Cfg.fovPull <= 0 || !a->inCar)
		return JER_RESULT_CONTINUE;

	int speed = FIXEDH(a->carSpeed);
	int spd = jer_clamp_int(speed, 0, CD2_FOV_REF_SPEED);
	int pull = spd * gCd2Cfg.fovPull * CD2_FOV_PULL_SCRZ / (CD2_FOV_REF_SPEED * 100);

	SetGeomScreen(gCameraDefaultScrZ - pull);
	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Pause menu (single page)
// ---------------------------------------------------------------------------

static void cd2LabelEnabled(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Enabled: %s", gCd2Cfg.enabled ? "ON" : "OFF"); }
static int  cd2ToggleEnabled(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.enabled = !gCd2Cfg.enabled; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTopSpeed(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Top Speed: %d", gCd2Cfg.topSpeed); }
static int  cd2AdjTopSpeed(void* ud, int dir) { (void)ud; gCd2Cfg.topSpeed = jer_clamp_int(gCd2Cfg.topSpeed + dir * 10, 60, 400); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelAccel(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Acceleration: %d", gCd2Cfg.accel); }
static int  cd2AdjAccel(void* ud, int dir) { (void)ud; gCd2Cfg.accel = jer_clamp_int(gCd2Cfg.accel + dir, 1, 24); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelHandling(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Handling: %d", gCd2Cfg.handling); }
static int  cd2AdjHandling(void* ud, int dir) { (void)ud; gCd2Cfg.handling = jer_clamp_int(gCd2Cfg.handling + dir * 5, 10, 120); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelGrip(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Grip: %d", gCd2Cfg.grip); }
static int  cd2AdjGrip(void* ud, int dir) { (void)ud; gCd2Cfg.grip = jer_clamp_int(gCd2Cfg.grip + dir * 64, 128, 4096); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTightToggle(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Tight Turn: %s", gCd2Cfg.tightTurn ? "ON" : "OFF"); }
static int  cd2ToggleTight(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.tightTurn = !gCd2Cfg.tightTurn; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTightStrength(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Tight Pivot: %d%%", gCd2Cfg.tightStrength); }
static int  cd2AdjTightStrength(void* ud, int dir) { (void)ud; gCd2Cfg.tightStrength = jer_clamp_int(gCd2Cfg.tightStrength + dir * 5, 0, 100); cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTightInput(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Tight Input: %s", kTightInputNames[gCd2Cfg.tightInput]); }
static int  cd2CycleTightInput(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.tightInput = (gCd2Cfg.tightInput + 1) % 3; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelPreset(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Preset: %s", kPresetNames[gCd2Cfg.preset]); }
static int  cd2CyclePreset(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.preset = (gCd2Cfg.preset + 1) % 3; cd2ApplyPreset(); cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static int cd2ResetDefaults(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.preset = CD2_PRESET_DEFAULT;
	cd2ApplyPreset();
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM cd2MenuItems[] =
{
	{ NULL, cd2LabelEnabled,  cd2ToggleEnabled, NULL, NULL, 0 },
	{ NULL, cd2LabelTopSpeed, cd2AdjTopSpeed,   NULL, NULL, 1 },
	{ NULL, cd2LabelAccel,    cd2AdjAccel,      NULL, NULL, 1 },
	{ NULL, cd2LabelHandling, cd2AdjHandling,   NULL, NULL, 1 },
	{ NULL, cd2LabelGrip,     cd2AdjGrip,       NULL, NULL, 1 },
	{ NULL, cd2LabelTightToggle,   cd2ToggleTight,      NULL, NULL, 0 },
	{ NULL, cd2LabelTightStrength, cd2AdjTightStrength, NULL, NULL, 1 },
	{ NULL, cd2LabelTightInput,    cd2CycleTightInput,  NULL, NULL, 1 },
	{ NULL, cd2LabelPreset,   cd2CyclePreset,   NULL, NULL, 1 },
	{ "Reset to Defaults", NULL, cd2ResetDefaults, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU cd2Menu =
{ "Combat D2", cd2MenuItems, 10 };

// ---------------------------------------------------------------------------
// Module entry
// ---------------------------------------------------------------------------

JER_MODULE_ENTRY(jer_module_combatd2_entry)(JERICHO_CONTEXT* ctx)
{
	// Load + apply config here, not just on JER_EVENT_BOOT: the Mods menu
	// enables/reloads modules via jer_manager_reload, which re-runs this entry
	// but does NOT re-fire JER_EVENT_BOOT. Without this, a module enabled
	// mid-session would leave gCd2Cfg zero-initialised and stay inert.
	cd2LoadConfig();
	cd2ApplyPreset();

	ctx->jer_register_module(ctx,
		"combatd2",					/* id */
		"Combat D2",				/* name */
		"0.1.0",					/* version */
		"JERICHO",					/* author */
		"Twisted Metal 2 style arcade handling: point-mass velocity + yaw control, drift-heavy grip.",	/* description */
		"",							/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, cd2OnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, cd2OnResetCar, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2OnCarStep, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_TORQUE, cd2OnCarTorque, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW, cd2OnCarDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, cd2OnCamera, NULL, 0);

	jer_pause_menu_register(&cd2Menu);

	ctx->jer_log(ctx, "[combatd2] registered (SDK v%d)\n", ctx->sdkVersion);
}
