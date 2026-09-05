/*
 * combatd2.c — Combat D2: Twisted Metal: Black style arcade handling.
 *
 * A compiled-in JERICHO deep mod. It replaces the horizontal motion of the
 * stock Driver 2 sim with a point-mass rigid body at JER_EVENT_CAR_TORQUE
 * (the tail of StepOneCar, right before the engine integrates velocity and
 * orientation):
 *
 *   * throttle -> direct forward/backward velocity (no engine/gears)
 *   * steering -> direct yaw rate (target yaw = handling * steer, works at
 *     zero speed: rotate in place); authority is retained through any slide
 *   * Tight Turn (Triangle/handbrake) -> an acute forced pivot, its own yaw
 *     authority that bleeds a little speed (drift-slide with gas)
 *   * brakes -> fast and proportional (strong at speed, smooth taper)
 *   * lateral grip -> high and shallow-sagging, so skids are brief and never
 *     a loss-of-control spiral
 *
 * Vertical motion (gravity + ground lift) and roll/pitch are left to the
 * stock code so the car still rides the terrain. A gated engine query
 * (JER_EVENT_GET_WALL_RESTITUTION) makes scenery hits absorb momentum;
 * collisions are otherwise stock mass-based push.
 */

#include "driver2.h"
#include "combatd2.h"
#include "cars.h"
#include "cosmetic.h"
#include "camera.h"
#include "convert.h"
#include "players.h"
#include "pad.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_pause_menu.h"
#include "jer_config.h"
#include "jer_math.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

CD2_CONFIG gCd2Cfg;
static CD2_CAR gCd2Car[MAX_CARS];
static unsigned int gDbgFrame; // telemetry frame counter

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
	gCd2Cfg.brake    = jer_config_get_int("combatd2", "brake", CD2_BRAKE);
	gCd2Cfg.handling = jer_config_get_int("combatd2", "handling", CD2_HANDLING);
	gCd2Cfg.grip     = jer_config_get_int("combatd2", "grip", CD2_GRIP);
	gCd2Cfg.preset   = jer_config_get_int("combatd2", "preset", CD2_PRESET_DEFAULT);
	gCd2Cfg.fovPull  = jer_config_get_int("combatd2", "fov_pull", 40);
	gCd2Cfg.tightTurn     = jer_config_get_int("combatd2", "tight_enabled", CD2_TIGHT_ENABLED_DEFAULT);
	gCd2Cfg.tightStrength = jer_config_get_int("combatd2", "tight_strength", CD2_TIGHT_STRENGTH_DEFAULT);
	gCd2Cfg.tightInput    = jer_config_get_int("combatd2", "tight_input", CD2_TIGHT_INPUT_DEFAULT);
	gCd2Cfg.tmbButtons    = jer_config_get_int("combatd2", "tmb_buttons", CD2_TMB_BUTTONS_DEFAULT);
	gCd2Cfg.tmbTight      = jer_config_get_int("combatd2", "tmb_tight", CD2_TMB_TIGHT_DEFAULT);
	gCd2Cfg.debugLog      = jer_config_get_int("combatd2", "debug_log", 0);

	gCd2Cfg.enabled  = gCd2Cfg.enabled ? 1 : 0;
	gCd2Cfg.topSpeed = jer_clamp_int(gCd2Cfg.topSpeed, 60, 400);
	gCd2Cfg.accel    = jer_clamp_int(gCd2Cfg.accel, 1, 24);
	gCd2Cfg.brake    = jer_clamp_int(gCd2Cfg.brake, 2, 30);
	gCd2Cfg.handling = jer_clamp_int(gCd2Cfg.handling, 10, 120);
	gCd2Cfg.grip     = jer_clamp_int(gCd2Cfg.grip, 128, 4096);
	gCd2Cfg.preset   = jer_clamp_int(gCd2Cfg.preset, CD2_PRESET_DEFAULT, CD2_PRESET_CUSTOM);
	gCd2Cfg.fovPull  = jer_clamp_int(gCd2Cfg.fovPull, 0, 100);
	gCd2Cfg.tightTurn     = gCd2Cfg.tightTurn ? 1 : 0;
	gCd2Cfg.tightStrength = jer_clamp_int(gCd2Cfg.tightStrength, 0, 100);
	gCd2Cfg.tightInput    = jer_clamp_int(gCd2Cfg.tightInput, CD2_TIGHT_INPUT_HANDBRAKE, CD2_TIGHT_INPUT_OFF);
	gCd2Cfg.tmbButtons    = gCd2Cfg.tmbButtons ? 1 : 0;
	gCd2Cfg.tmbTight      = gCd2Cfg.tmbTight ? 1 : 0;
	gCd2Cfg.debugLog      = gCd2Cfg.debugLog ? 1 : 0;
}

static void cd2SaveConfig(void)
{
	jer_config_set_bool("combatd2", "enabled", gCd2Cfg.enabled);
	jer_config_set_int("combatd2", "top_speed", gCd2Cfg.topSpeed);
	jer_config_set_int("combatd2", "accel", gCd2Cfg.accel);
	jer_config_set_int("combatd2", "brake", gCd2Cfg.brake);
	jer_config_set_int("combatd2", "handling", gCd2Cfg.handling);
	jer_config_set_int("combatd2", "grip", gCd2Cfg.grip);
	jer_config_set_int("combatd2", "preset", gCd2Cfg.preset);
	jer_config_set_int("combatd2", "fov_pull", gCd2Cfg.fovPull);
	jer_config_set_int("combatd2", "tight_enabled", gCd2Cfg.tightTurn);
	jer_config_set_int("combatd2", "tight_strength", gCd2Cfg.tightStrength);
	jer_config_set_int("combatd2", "tight_input", gCd2Cfg.tightInput);
	jer_config_set_int("combatd2", "tmb_buttons", gCd2Cfg.tmbButtons);
	jer_config_set_int("combatd2", "tmb_tight", gCd2Cfg.tmbTight);
	jer_config_set_int("combatd2", "debug_log", gCd2Cfg.debugLog);
}

static void cd2ApplyPreset(void)
{
	gCd2Cfg.brake = CD2_BRAKE; // brake peak is preset-independent

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
	s.brake        = gCd2Cfg.brake;
	s.drag         = CD2_DRAG;
	s.handling     = gCd2Cfg.handling;
	s.angularAccel = CD2_ANGULAR_ACCEL;
	s.grip         = gCd2Cfg.grip;
	s.control      = 4096; // average baseline; derived per-car later

	if (cp->ap.carCos != NULL)
	{
		// Per-vehicle axes derived from the existing chassis stats:
		//   power-to-weight (powerRatio / mass) -> accel, top speed AND control
		//     (light + powerful = fast & nimble like Spectre; heavy = slow & wide
		//     like Darkside, but always kept driveable)
		//   traction (fixed 4096 = stock) -> grip
		//   mass -> drag (heavy cars coast longer: momentum is felt)
		// Collision "push" is already mass-based in the engine's impulse code.
		int power = cp->ap.carCos->powerRatio;
		int mass  = cp->ap.carCos->mass;
		if (mass < 1) mass = 1;

		if (power > 0)
		{
			// ~4096 for an average car in this data set
			int pw = (int)(((long long)power * 4096) / mass);

			// acceleration/top-speed scale: 0.375x .. 1.5x
			int as = jer_clamp_int((pw * 4096) / CD2_REF_PW, 1536, 6144);
			s.accel    = (int)(((long long)s.accel * as) >> 12);
			s.topSpeed = (int)(((long long)s.topSpeed * (4096 + (as - 4096) / 2)) >> 12);

			// steering/pivot authority: 0.5x .. 1.5x (heavy stays driveable)
			s.control = jer_clamp_int(pw, 2048, 6144);
		}

		// heavy cars shed speed slower (coast = momentum), capped at normal drag
		{
			int dm = jer_clamp_int(((long long)CD2_REF_MASS * 4096) / mass, 2048, 4096);
			s.drag = (int)(((long long)s.drag * dm) >> 12);
		}

		int traction = cp->ap.carCos->traction;
		if (traction > 0 && traction != 4096)
			s.grip = (int)(((long long)s.grip * traction) >> 12);
	}

	// combatd2 drives ~25% slower than the raw slider: the point-mass
	// top speed would otherwise out-run the level scale and feel frantic.
	// (Scales every preset + per-vehicle derivation uniformly.)
	s.topSpeed = (int)(((long long)s.topSpeed * CD2_SPEED_SCALE) >> 1);

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
		gCd2Car[a->carId].slideTicks = 0;
	}
	return JER_RESULT_CONTINUE;
}

// ---- TMB in-car button layout (JER_EVENT_CAR_PAD override) --------------
//
// This module does NOT rebind physical buttons. Instead it takes over the
// car's pedal semantics at JER_EVENT_CAR_PAD (ProcessCarPad): when the TMB
// layout is on and the pad is a live player, we write cp->thrust/
// cp->handbrake/cp->wheelspin ourselves and set handled=1, so the engine
// SKIPS its stock face-button assignment for that car this frame -- the
// original car binds can never double-fire alongside ours. Engine steering
// (wheel_angle) is untouched, so the analog curve stays stock. On foot,
// AI/lead/cutscene cars and the clamped locked-car state are never overridden.
static int cd2OnCarPad(void* ud, void* args)
{
	JER_ARGS_CAR_PAD* a = (JER_ARGS_CAR_PAD*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	int pad, tight, gas, brake;
	(void)ud;

	if (!gCd2Cfg.enabled || !gCd2Cfg.tmbButtons || !a->live)
		return JER_RESULT_CONTINUE;

	pad = a->pad;

	if (gCd2Cfg.debugLog)
	{
		static unsigned int t = 0;
		if ((t++ & 15) == 0)
			printInfo("[combatd2] pad=0x%04X (Cross 0x%X/Square 0x%X/Circle 0x%X/Triangle 0x%X)\n",
				pad, MPAD_CROSS, MPAD_SQUARE, MPAD_CIRCLE, MPAD_TRIANGLE);
	}

	// PS-position reference (PlayStation face buttons):
	//   default : Cross(bottom) = Tight Turn, Square(left) = Gas, Circle = Brake
	//   tmbTight: Square(left)  = Tight Turn, Cross(bottom) = Gas, Circle = Brake
	gas   = gCd2Cfg.tmbTight ? (pad & MPAD_CROSS)  : (pad & MPAD_SQUARE);
	tight = gCd2Cfg.tmbTight ? (pad & MPAD_SQUARE) : (pad & MPAD_CROSS);
	brake = pad & MPAD_CIRCLE;

	cp->handbrake = 0;
	// Deliberately never set cp->wheelspin: holding the tight button would
	// trip the stock burnout path (rev scream, gear-0 wheelspin revs). The
	// torque reads the tight button straight from the raw pad instead.
	cp->wheelspin = 0;

	// sign convention matches stock (positive = drive force, negative =
	// brake/reverse); combatd2's torque only reads the sign at CAR_STEP.
	if (gas)
		cp->thrust = (short)CD2_TMB_THRUST;
	else if (brake)
		cp->thrust = -(short)CD2_TMB_THRUST;
	else
		cp->thrust = 0;

	a->handled = 1; // stock face-button binds are skipped this frame
	return JER_RESULT_CONTINUE;
}

// GEARBOX (JER_EVENT_CAR_GEARBOX, gamesnd.c GetEngineRevs): while a player
// drives, retune the rev model so gears feel SHORT and punchy but the tall
// top gear levels the pitch at the car's combatd2 top speed — revs never
// wind past CD2_REV_CEILING even as the point-mass speed keeps climbing.
static int cd2OnCarGearbox(void* ud, void* args)
{
	JER_ARGS_CAR_GEARBOX* g = (JER_ARGS_CAR_GEARBOX*)args;
	CAR_DATA* cp = (CAR_DATA*)g->car;
	int top, i, prevHi, wsTop;
	(void)ud;

	if (!gCd2Cfg.enabled || !CD2_GEAR_AUTO)
		return JER_RESULT_CONTINUE;

	if (cp->controlType != CONTROL_TYPE_PLAYER)
		return JER_RESULT_CONTINUE;

	top = cd2GetStats(cp).topSpeed;
	if (top < 40)
		return JER_RESULT_CONTINUE;

	wsTop = (int)(((long long)top * CD2_WS_PER_SPEED) >> 12); // ws at top speed
	if (wsTop < 30)
		return JER_RESULT_CONTINUE;

	{
		int up[4];
		int fracs[3] = { CD2_GEAR_1_FRAC, CD2_GEAR_2_FRAC, CD2_GEAR_3_FRAC };

		prevHi = 0;
		for (i = 0; i < 3; i++)
		{
			up[i] = (int)(((long long)top * fracs[i]) >> 12);
			up[i] = (int)(((long long)up[i] * CD2_WS_PER_SPEED) >> 12);
			if (up[i] <= prevHi)
				up[i] = prevHi + 1;
			prevHi = up[i];
		}
		up[3] = wsTop;
		if (up[3] <= prevHi)
			up[3] = prevHi + 1;

		for (i = 0; i < 4; i++)
		{
			g->hiWs[i] = up[i];

			if (i == 0)
			{
				g->lowWs[i] = 0;
				g->lowIdleWs[i] = 0;
			}
			else
			{
				int lo = (int)(((long long)up[i - 1] * CD2_GEAR_DOWN_FRAC) >> 12);
				if (lo >= up[i])
					lo = up[i] - 1;
				g->lowWs[i] = lo;
				g->lowIdleWs[i] = (int)(((long long)lo * 3) >> 2);
			}

			if (i < 3)
			{
				int ratio = CD2_GEAR_SHIFT_REVS / up[i];
				if (ratio < 8) ratio = 8;
				g->ratioAc[i] = ratio;
				g->ratioIdle[i] = ratio;
			}
			else
			{
				int ratio = CD2_REV_CEILING / up[i];
				if (ratio < 4) ratio = 4;
				g->ratioAc[i] = ratio;
				g->ratioIdle[i] = ratio;
			}
		}

		g->revCeiling = CD2_REV_CEILING;
	}

	return JER_RESULT_CONTINUE;
}

// ENGINE SOUND (JER_EVENT_CAR_ENGINE_SOUND, gamesnd.c SoundTasks): scale and
// offset the player car's rev + idle channel pitch and volume. Tuners live in
// combatd2.h as CD2_SND_*; neutral defaults mean no change when left alone.
static int cd2OnCarEngineSound(void* ud, void* args)
{
	JER_ARGS_CAR_ENGINE_SOUND* e = (JER_ARGS_CAR_ENGINE_SOUND*)args;
	long long p;
	long long v;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	p = ((long long)e->revPitch * CD2_SND_PITCH_SCALE) >> 12;
	e->revPitch = (int)p + CD2_SND_PITCH_BIAS;

	p = ((long long)e->idlePitch * CD2_SND_PITCH_SCALE) >> 12;
	e->idlePitch = (int)p + CD2_SND_IDLE_PITCH_BIAS;

	// Volume is PSX attenuation (0 loudest, -10000 silent): gain divides the
	// attenuation (8192 ≈ twice as loud), bias moves it toward 0 (louder).
	v = -e->revVolume;
	if (CD2_SND_REV_GAIN > 0 && v > 0)
		v = (v * 4096) / CD2_SND_REV_GAIN;
	v = -v + CD2_SND_REV_BIAS;
	if (v < CD2_SND_MIN_VOL) v = CD2_SND_MIN_VOL;
	if (v > CD2_SND_MAX_VOL) v = CD2_SND_MAX_VOL;
	e->revVolume = (int)v;

	v = -e->idleVolume;
	if (CD2_SND_IDLE_GAIN > 0 && v > 0)
		v = (v * 4096) / CD2_SND_IDLE_GAIN;
	v = -v + CD2_SND_IDLE_BIAS;
	if (v < CD2_SND_MIN_VOL) v = CD2_SND_MIN_VOL;
	if (v > CD2_SND_MAX_VOL) v = CD2_SND_MAX_VOL;
	e->idleVolume = (int)v;

	return JER_RESULT_CONTINUE;
}

// REV SLEW (JER_EVENT_CAR_REVS, gamesnd.c ControlCarRevs): scale how fast the
// engine pitch climbs toward its target revs (CD2_REV_RISE_SCALE) and how
// hard it falls on shifts / let-off (CD2_REV_DROP_SCALE). Applied to player
// cars only; traffic keeps the stock lag.
static int cd2OnCarRevs(void* ud, void* args)
{
	JER_ARGS_CAR_REVS* r = (JER_ARGS_CAR_REVS*)args;
	CAR_DATA* cp = (CAR_DATA*)r->car;
	(void)ud;

	if (!gCd2Cfg.enabled || cp->controlType != CONTROL_TYPE_PLAYER)
		return JER_RESULT_CONTINUE;

	r->revRise = (int)(((long long)r->revRise * CD2_REV_RISE_SCALE) >> 12);
	r->revDrop = (int)(((long long)r->revDrop * CD2_REV_DROP_SCALE) >> 12);

	if (r->revRise < 0) r->revRise = 0;
	if (r->revDrop < 0) r->revDrop = 0;

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
	int slideNow = 0; // traction-suspended slide active (computed once per frame)
	if (gCd2Cfg.tightTurn && cp->controlType == CONTROL_TYPE_PLAYER)
	{
		if (gCd2Cfg.tmbButtons)
		{
			// Tight Turn = the layout's tight face button, read from the raw
			// pad (engine-native bits). NOT cp->wheelspin: the CAR_PAD
			// override leaves wheelspin clear, so X never triggers burnout.
			int padm = 0;
			if (cp->ai.padid != NULL && *cp->ai.padid >= 0 && *cp->ai.padid < 2)
				padm = Pads[*cp->ai.padid].mapped;
			tightActive = ((gCd2Cfg.tmbTight ? (padm & MPAD_SQUARE) : (padm & MPAD_CROSS)) != 0);
		}
		else if (gCd2Cfg.tightInput == CD2_TIGHT_INPUT_HANDBRAKE)
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

	// Current horizontal speed (magnitude), used for the slide decision.
	// NOTE steering below is deliberately NOT inverted in reverse: like TMB
	// ("you steer the front of the car as-is"), steer input rotates the nose
	// the same way whether driving forward or backward — which is exactly what
	// gives reversing its mirrored, rear-led feel.
	{
		int ax = ABS(FIXEDH(velX));
		int az = ABS(FIXEDH(velZ));
		int speedNow = (ax < az) ? (az + ax / 2) : (ax + az / 2); // speed units
		slideNow = tightActive && c->pivotDir != 0 && speedNow > CD2_SLIDE_MIN_SPEED;
	}

	// throttle is snapshotted at CAR_STEP: the stock wheel-force code zeroes
	// cp->thrust while the handbrake is held, which would otherwise read as
	// "coast" mid tight turn.
	int throttle = c->throttle;

	// ---- throttle / brake / reverse / drag --------------------------
	if (throttle > 0)
	{
		int accel = s.accel;
		if (slideNow)
			accel = (int)(((long long)accel * CD2_SLIDE_ACCEL_FRAC) >> 12);
		if (fwdSpeed < s.topSpeed)
		{
			velX += fx * accel;
			velZ += fz * accel;
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
			// TMB "fast and proportional" brake: strong scrub at speed that
			// tapers near zero (a small constant floor keeps it from being
			// asymptotic), so stopping is quick but never a jarring dead stop.
			int brake = s.brake;
			if (fwdSpeed > 0)
			{
				long long ratio = fwdSpeed * 4096 / s.topSpeed;
				if (ratio > 4096) ratio = 4096;
				brake = (int)(((long long)s.brake *
					(CD2_BRAKE_FLOOR + (4096 - CD2_BRAKE_FLOOR) * ratio / 4096)) >> 12);
			}
			else
			{
				// reversing: gentler, continuous push (no dead zone at 0)
				brake = (int)(((long long)s.brake * CD2_REVERSE_ACCEL_FRAC) >> 12);
			}
			velX -= fx * brake;
			velZ -= fz * brake;
		}
		else
		{
			velX -= (int)(((long long)velX * s.drag) >> 12);
			velZ -= (int)(((long long)velZ * s.drag) >> 12);
		}
	}
	else if (!slideNow)
	{
		// no input: rolling drag — skipped during a slide so the car glides
		velX -= (int)(((long long)velX * s.drag) >> 12);
		velZ -= (int)(((long long)velZ * s.drag) >> 12);
	}

	// tight-turn momentum bleed: the forced pivot sheds a little horizontal
	// speed so holding gas yields a drift-slide arc rather than a dead stop.
	// While the slide is active the scrub is stronger: gas + Tight Turn must
	// shed speed (TMB), not build it.
	if (tightActive && c->pivotDir != 0)
	{
		int bleed = slideNow ? CD2_SLIDE_BLEED : CD2_TIGHT_BLEED;
		velX = (int)(((long long)velX * (4096 - bleed)) >> 12);
		velZ = (int)(((long long)velZ * (4096 - bleed)) >> 12);
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

	// Iconic TMB slide: while the Tight Turn pivot is active and the car is
	// fast enough, traction is suspended — grip drops to a few percent, so
	// the car keeps travelling along its ORIGINAL velocity vector while the
	// pivot rotates the heading underneath it (steerable slide). Letting off
	// RECOVERS the velocity toward the heading WITHOUT stopping: the old
	// lateral wipe scrubbed the car dead once the pivot had rotated the
	// heading away from the travel direction. The recovery instead rotates
	// the velocity back onto the heading while conserving its magnitude, so
	// you exit the turn carrying your speed (TMB momentum).
	if (slideNow)
	{
		c->slideTicks = CD2_RECOVER_FRAMES; // re-prime for the release
		grip = (int)(((long long)grip * CD2_SLIDE_GRIP_FRAC) >> 12);
	}
	else if (c->slideTicks > 0)
	{
		// velocity recovery: rotate toward the heading, keep the magnitude
		c->slideTicks--;

		{
			int ax = ABS(velX);
			int az = ABS(velZ);
			int mag = (ax < az) ? (az + ax / 2) : (ax + az / 2);

			if (mag > 0)
			{
				// target = heading scaled to the current speed
				int tx = (int)(((long long)fx * mag) >> 12);
				int tz = (int)(((long long)fz * mag) >> 12);
				long long rate = CD2_RECOVER_RATE;
				int nvx = (int)(velX + ((((long long)tx - velX) * rate) >> 12));
				int nvz = (int)(velZ + ((((long long)tz - velZ) * rate) >> 12));
				int nax = ABS(nvx);
				int naz = ABS(nvz);
				int nm = (nax < naz) ? (naz + nax / 2) : (nax + naz / 2);

				if (nm > 0)
				{
					// renormalise: the slide's speed is carried out, not lost
					velX = (int)(((long long)nvx * mag) / nm);
					velZ = (int)(((long long)nvz * mag) / nm);
				}
			}
		}

		grip = 0; // rotation above replaces the lateral scrub this frame

		if (c->slideTicks == 0)
			c->slideTicks = -CD2_GRIP_RAMP_FRAMES; // chain into the grip ramp
	}
	else if (c->slideTicks < 0)
	{
		// grip ramps back up gradually instead of snapping to full
		grip = (int)(((long long)grip * (CD2_GRIP_RAMP_FRAMES + c->slideTicks)) /
			CD2_GRIP_RAMP_FRAMES);
		c->slideTicks++;
	}

	// damp the lateral component: vel -= right * latVel * grip
	velX -= (int)(((long long)rx * latVel * grip) >> 12);
	velZ -= (int)(((long long)rz * latVel * grip) >> 12);

	c->slip = (int)latVel; // for the visual lean

	// hard ceiling: total horizontal speed never exceeds topSpeed. Without it,
	// a tight slide with gas keeps adding speed along a rotating heading and
	// can feel like it is accelerating out of control.
	{
		int ax = ABS(FIXEDH(velX));
		int az = ABS(FIXEDH(velZ));
		int mag = (ax < az) ? (az + ax / 2) : (ax + az / 2);
		if (mag > s.topSpeed)
		{
			long long k = ((long long)s.topSpeed * 4096) / mag;
			velX = (int)(((long long)velX * k) >> 12);
			velZ = (int)(((long long)velZ * k) >> 12);
		}
	}

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

		// In-place turning at a COMPLETE standstill: the engine treats a car
		// with hd.speed == 0 as fully stopped and wipes ALL angular velocity
		// before it integrates orientation, so the car can't rotate until it
		// rolls a tiny bit. While the driver is actually steering (or
		// pivoting) and the car is motionless, flag it as barely rolling so
		// the yaw we wrote above survives and the car spins on the spot.
		if (cp->hd.speed == 0 && cp->controlType == CONTROL_TYPE_PLAYER &&
			(steerFp != 0 || tightActive))
		{
			cp->hd.speed = 1;
		}
	}

	// ---- telemetry (opt-in): export input/velocity/twist for tuning ----
	if (gCd2Cfg.debugLog && cp->controlType == CONTROL_TYPE_PLAYER &&
		(gDbgFrame++ & 7) == 0)
	{
			int padm = 0;
			if (cp->ai.padid != NULL && *cp->ai.padid >= 0 && *cp->ai.padid < 2)
				padm = Pads[*cp->ai.padid].mapped; // engine-native bits (raw, pre-override)
			printInfo("[combatd2] fr=%u thr=%d steer=%d tight=%d slide=%d "
				"fwd=%d spd=%d lat=%d grip=%d yaw=%d hb=%d ws=%d pad=0x%04X hdspd=%d hdws=%d\n",
				gDbgFrame, c->throttle, steerFp / 4096,
				tightActive ? (c->pivotDir ? c->pivotDir : 8) : 0, slideNow ? 1 : 0,
				(int)fwdSpeed, (int)(((long long)velX * fx + (long long)velZ * fz) >> 24),
				(int)latVel, grip, yaw, cp->handbrake, cp->wheelspin, padm,
				cp->hd.speed, cp->hd.wheel_speed);
	}

	return JER_RESULT_CONTINUE;
}

// GET_WALL_RESTITUTION: TMB "walls absorb momentum". Returning a low scale
// makes building/scenery hits a hard stop (momentum bled) instead of the stock
// outward bounce + spin. No handler (combatd2 disabled) = stock behaviour.
static int cd2OnGetWallRestitution(void* ud, void* args)
{
	JER_ARGS_WALL_RESTITUTION* a = (JER_ARGS_WALL_RESTITUTION*)args;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	a->result = CD2_WALL_KEEP;
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

	if (!gCd2Cfg.enabled || !a->inCar)
		return JER_RESULT_CONTINUE;

	// TMB chase framing: after the engine places the main chase camera, pull
	// it closer to the car on the ground plane and ease it lower. Relative
	// nudges (fractions of the gap) applied to camera_position only, so the
	// engine's own re-place each frame keeps this a stable framing offset.
	if (a->cameraView == 0 && CD2_CAM_PULL > 0)
	{
		int* bp = (int*)a->basePos;
		int* cp = (int*)a->cameraPosition;
		long long dx = (long long)bp[0] - cp[0];
		long long dz = (long long)bp[2] - cp[2];

		cp[0] += (int)((dx * CD2_CAM_PULL) >> 12);
		cp[2] += (int)((dz * CD2_CAM_PULL) >> 12);

		if (CD2_CAM_LOW > 0)
		{
			long long dy = (long long)bp[1] - cp[1];
			cp[1] += (int)((dy * CD2_CAM_LOW) >> 12);
		}
	}

	if (gCd2Cfg.fovPull <= 0)
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

static void cd2LabelBrake(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Braking: %d", gCd2Cfg.brake); }
static int  cd2AdjBrake(void* ud, int dir) { (void)ud; gCd2Cfg.brake = jer_clamp_int(gCd2Cfg.brake + dir, 2, 30); cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelHandling(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Handling: %d", gCd2Cfg.handling); }
static int  cd2AdjHandling(void* ud, int dir) { (void)ud; gCd2Cfg.handling = jer_clamp_int(gCd2Cfg.handling + dir * 5, 10, 120); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelGrip(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Grip: %d", gCd2Cfg.grip); }
static int  cd2AdjGrip(void* ud, int dir) { (void)ud; gCd2Cfg.grip = jer_clamp_int(gCd2Cfg.grip + dir * 64, 128, 4096); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTightToggle(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Tight Turn: %s", gCd2Cfg.tightTurn ? "ON" : "OFF"); }
static int  cd2ToggleTight(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.tightTurn = !gCd2Cfg.tightTurn; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTmbButtons(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "TMB Buttons: %s", gCd2Cfg.tmbButtons ? "ON" : "OFF"); }
static int  cd2ToggleTmbButtons(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.tmbButtons = !gCd2Cfg.tmbButtons; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTightStrength(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Tight Pivot: %d%%", gCd2Cfg.tightStrength); }
static int  cd2AdjTightStrength(void* ud, int dir) { (void)ud; gCd2Cfg.tightStrength = jer_clamp_int(gCd2Cfg.tightStrength + dir * 5, 0, 100); cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTightInput(void* ud, char* out, int max)
{
	(void)ud;
	if (gCd2Cfg.tmbButtons)
	{
		// PS-position reference: X/Cross (bottom) is the default Tight Turn,
		// Square (left) is Gas. Flip for pads that label the left button "X".
		snprintf(out, max, "Tight Turn Button: %s",
			gCd2Cfg.tmbTight ? "Square (Gas on X/Cross)" : "X / Cross (Gas on Square)");
	}
	else
		snprintf(out, max, "Tight Input: %s", kTightInputNames[gCd2Cfg.tightInput]);
}
static int  cd2CycleTightInput(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	if (gCd2Cfg.tmbButtons)
		gCd2Cfg.tmbTight = !gCd2Cfg.tmbTight;
	else
		gCd2Cfg.tightInput = (gCd2Cfg.tightInput + 1) % 3;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelDebug(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Telemetry Log: %s", gCd2Cfg.debugLog ? "ON" : "OFF"); }
static int  cd2ToggleDebug(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.debugLog = !gCd2Cfg.debugLog; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

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
	{ NULL, cd2LabelBrake,    cd2AdjBrake,      NULL, NULL, 1 },
	{ NULL, cd2LabelHandling, cd2AdjHandling,   NULL, NULL, 1 },
	{ NULL, cd2LabelGrip,     cd2AdjGrip,       NULL, NULL, 1 },
	{ NULL, cd2LabelTightToggle,   cd2ToggleTight,      NULL, NULL, 0 },
	{ NULL, cd2LabelTmbButtons,    cd2ToggleTmbButtons, NULL, NULL, 0 },
	{ NULL, cd2LabelTightStrength, cd2AdjTightStrength, NULL, NULL, 1 },
	{ NULL, cd2LabelTightInput,    cd2CycleTightInput,  NULL, NULL, 1 },
	{ NULL, cd2LabelDebug, cd2ToggleDebug, NULL, NULL, 0 },
	{ NULL, cd2LabelPreset,   cd2CyclePreset,   NULL, NULL, 1 },
	{ "Reset to Defaults", NULL, cd2ResetDefaults, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU cd2Menu =
{ "Combat D2", cd2MenuItems, 13 };

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
		"0.5.0",					/* version */
		"JERICHO",					/* author */
		"Twisted Metal: Black style handling: point-mass velocity + yaw, Tight Turn pivot, proportional brakes, brief skids, momentum-absorbing walls.",	/* description */
		"",							/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, cd2OnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, cd2OnResetCar, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_PAD, cd2OnCarPad, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_GEARBOX, cd2OnCarGearbox, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_REVS, cd2OnCarRevs, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE_SOUND, cd2OnCarEngineSound, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2OnCarStep, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_TORQUE, cd2OnCarTorque, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_WALL_RESTITUTION, cd2OnGetWallRestitution, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW, cd2OnCarDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, cd2OnCamera, NULL, 0);

	jer_pause_menu_register(&cd2Menu);

	ctx->jer_log(ctx, "[combatd2] registered (SDK v%d)\n", ctx->sdkVersion);
}
