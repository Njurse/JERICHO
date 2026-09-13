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
#include "sound.h"
#include "gamesnd.h"
#include "mc_snd.h"
#include "weapons/core/weapon.h"	/* CD2_WEAPON_DEF + inventory API */
#include "ai/ai.h"			/* opponent AI (ai/opponent.c) */
#include <string.h>
// Registration helpers from the other source files of this (merged) module:
//   combatd2combat.c — wreck/explosion effects   (cd2CombatRegister)
//   combatd2media.c   — presentation tuners      (cd2MediaRegister)
//   weapons/core      — weapon framework         (cd2WeaponsRegister)
void cd2CombatRegister(JERICHO_CONTEXT* ctx);
void cd2MediaRegister(JERICHO_CONTEXT* ctx);
void cd2WeaponsRegister(JERICHO_CONTEXT* ctx);
void cd2FxRegister(JERICHO_CONTEXT* ctx);
void cd2FreezeRegister(JERICHO_CONTEXT* ctx);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

CD2_CONFIG gCd2Cfg;
static CD2_CAR gCd2Car[MAX_CARS];
static int gCd2SceneryHits[MAX_CARS];	// scenery impacts per car this level
static int gCd2TrafficLastHit[MAX_CARS];	// last scenery-hit count seen, per car

// Traffic (civ) cars are cannon fodder. They get no handling override and
// are deliberately EXEMPT from the roll-over limiter, so a weapon hit or a
// wall scrape can throw them properly instead of being clamped back down.
#define CD2_TRAFFIC_TUMBLE_MIN_SPEED	60		// units/frame below which a scrape is ignored
#define CD2_TRAFFIC_TUMBLE_RATE		26000	// angular impulse per unit/frame of speed
#define CD2_TRAFFIC_TUMBLE_MAX		0x500000	// ceiling on that impulse

// With no gas and no brake, below this speed (units/frame) the point-mass
// velocity snaps to a dead stop so the car can't creep / micro-roll a few
// units forever.
#define CD2_MICRO_STOP_SPEED		20
static unsigned int gDbgFrame; // telemetry frame counter
static char gPendingTotalCar;   // set by the pause-menu "Total Car", applied next physics frame

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
	gCd2Cfg.allWeapons    = jer_config_get_int("combatd2", "all_weapons", 1);
	gCd2Cfg.rollLimit     = jer_config_get_int("combatd2", "roll_limit", CD2_ROLL_LIMIT_DEFAULT);
	gCd2Cfg.sceneryDamage = jer_config_get_int("combatd2", "scenery_damage", CD2_SCENERY_DAMAGE_DEFAULT);
	gCd2Cfg.aiOpponent    = jer_config_get_int("combatd2", "ai_opponent", 1);
	gCd2Cfg.aiForceState  = jer_config_get_int("combatd2", "ai_force_state", CD2_AI_AUTO);
	gCd2Cfg.aiDebug       = jer_config_get_int("combatd2", "ai_debug", 0);
	gCd2Cfg.aiRole        = jer_config_get_int("combatd2", "ai_role", -1);
	gCd2Cfg.navDebug      = jer_config_get_int("combatd2", "nav_debug", 0);

	// car-vs-car damage as % of stock. Migrate the old car_car_nerf (% reduction).
	gCd2Cfg.carCarDamage  = jer_config_get_int("combatd2", "car_car_damage", -1);
	gCd2Cfg.aiDamageTaken = jer_config_get_int("combatd2", "ai_damage_taken", CD2_AI_DAMAGE_TAKEN_DEFAULT);
	gCd2Cfg.respawn       = jer_config_get_int("combatd2", "respawn", 1);
	gCd2Cfg.respawnDelay  = CD2_RESPAWN_DELAY;	// fixed 5s, see CD2_RESPAWN_DELAY

	if (gCd2Cfg.carCarDamage < 0)
		gCd2Cfg.carCarDamage = 100 - jer_config_get_int("combatd2", "car_car_nerf",
			100 - CD2_CAR_CAR_DAMAGE_DEFAULT);

	{
		const char* mm = jer_config_get_str("combatd2", "missile_model", "BOMB");
		strncpy(gCd2Cfg.missileModel, (mm != NULL) ? mm : "", sizeof(gCd2Cfg.missileModel) - 1);
		gCd2Cfg.missileModel[sizeof(gCd2Cfg.missileModel) - 1] = 0;
	}
	gCd2Cfg.missileScale  = jer_config_get_int("combatd2", "missile_scale", 4096);
	gCd2Cfg.missileSound  = jer_config_get_int("combatd2", "missile_sound", 6);

	gCd2Cfg.enabled  = gCd2Cfg.enabled ? 1 : 0;
	gCd2Cfg.topSpeed = jer_clamp_int(gCd2Cfg.topSpeed, 60, 600);
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
	gCd2Cfg.allWeapons    = gCd2Cfg.allWeapons ? 1 : 0;
	gCd2Cfg.rollLimit     = jer_clamp_int(gCd2Cfg.rollLimit, 0, 89);
	gCd2Cfg.sceneryDamage = jer_clamp_int(gCd2Cfg.sceneryDamage, 0, 100);
	gCd2Cfg.aiOpponent    = gCd2Cfg.aiOpponent ? 1 : 0;
	gCd2Cfg.aiForceState  = jer_clamp_int(gCd2Cfg.aiForceState, 0, CD2_AI_STATE_COUNT - 1);
	gCd2Cfg.aiDebug       = gCd2Cfg.aiDebug ? 1 : 0;
	gCd2Cfg.aiRole        = jer_clamp_int(gCd2Cfg.aiRole, -1, CD2_AI_ROLE_COUNT - 1);
	gCd2Cfg.navDebug      = gCd2Cfg.navDebug ? 1 : 0;
	gCd2Cfg.carCarDamage  = jer_clamp_int(gCd2Cfg.carCarDamage, 10, 100);
	gCd2Cfg.aiDamageTaken = jer_clamp_int(gCd2Cfg.aiDamageTaken, 10, 400);
	gCd2Cfg.respawn       = gCd2Cfg.respawn ? 1 : 0;
	gCd2Cfg.missileScale  = jer_clamp_int(gCd2Cfg.missileScale, 512, 16384);
	gCd2Cfg.missileSound  = jer_clamp_int(gCd2Cfg.missileSound, 0, 34);
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
	jer_config_set_int("combatd2", "all_weapons", gCd2Cfg.allWeapons);
	jer_config_set_int("combatd2", "roll_limit", gCd2Cfg.rollLimit);
	jer_config_set_int("combatd2", "scenery_damage", gCd2Cfg.sceneryDamage);
	jer_config_set_int("combatd2", "ai_opponent", gCd2Cfg.aiOpponent);
	jer_config_set_int("combatd2", "ai_force_state", gCd2Cfg.aiForceState);
	jer_config_set_int("combatd2", "ai_debug", gCd2Cfg.aiDebug);
	jer_config_set_int("combatd2", "ai_role", gCd2Cfg.aiRole);
	jer_config_set_int("combatd2", "nav_debug", gCd2Cfg.navDebug);
	jer_config_set_int("combatd2", "car_car_damage", gCd2Cfg.carCarDamage);
	jer_config_set_int("combatd2", "ai_damage_taken", gCd2Cfg.aiDamageTaken);
	jer_config_set_int("combatd2", "respawn", gCd2Cfg.respawn);
	jer_config_set_str("combatd2", "missile_model", gCd2Cfg.missileModel);
	jer_config_set_int("combatd2", "missile_scale", gCd2Cfg.missileScale);
	jer_config_set_int("combatd2", "missile_sound", gCd2Cfg.missileSound);
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

	// The raw slider is in "speed-units/frame"; apply the fixed-point
	// CD2_SPEED_SCALE (2048/4096 = 0.5x) to get the effective top so the
	// point-mass model doesn't out-run the level scale. Reverse is derived
	// from the SAME scaled top (TM drives backwards as fast as forwards) and
	// must stay POSITIVE: the brake pass tests `fwdSpeed > -reverseSpeed`.
	s.topSpeed = (int)(((long long)s.topSpeed * CD2_SPEED_SCALE) >> 12);
	s.reverseSpeed = (int)(((long long)s.topSpeed * CD2_REVERSE_FRAC) >> 12);

	return s;
}

// Exported for the presentation source file (combatd2media.c) of this merged
// module: the effective top speed of a car (config slider x CD2_SPEED_SCALE x
// the per-vehicle power/weight derivation), so its gearbox can level the
// engine pitch at this car's real top speed.
int cd2CarTopSpeed(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	if (cp == NULL)
		return 0;
	return cd2GetStats(cp).topSpeed;
}

// The car's own peak braking. The AI's speed governor needs this: a fixed
// frame-count margin assumes stock brakes, and combatd2 overrides them.
int cd2CarBrake(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL)
		return 0;

	return cd2GetStats(cp).brake;
}

// AI-requested acute in-place pivot (tight turn) for a car. The torque reads
// this at CAR_TORQUE; the AI sets it every frame in its CAR_STEP hook.
void cd2CarSetAiPivot(void* vcp, int dir)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	gCd2Car[cp->id].aiPivot = dir;
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
		gCd2Car[a->carId].aiPivot = 0;
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

	if (!gCd2Cfg.enabled || !a->live)
		return JER_RESULT_CONTINUE;

	// The shoulders + triggers are the weapon controls now (L1/R1 = prev/next
	// weapon, L2/R2 = fire). Strip them from the car's action bits so the
	// stock fast-steer (L1) never fires alongside; the stock horn (R1) is
	// cleared in the weapons FRAME handler.
	a->pad &= ~(MPAD_L1 | MPAD_L2 | MPAD_R1 | MPAD_R2);

	if (!gCd2Cfg.tmbButtons)
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

// Roll-over suppression. Runs at CAR_STEP (after combatd2combat's wreck toss,
// before the engine integrates velocity + orientation): the pitch/roll rates
// are capped so no single impulse can flip a car in one frame, and the car is
// stopped rolling once it leans past gCd2Cfg.rollLimit degrees. Weapons can
// still knock a car onto two wheels, but it can't end up on its roof.
static void cd2LimitRoll(CAR_DATA* cp)
{
	int* av = cp->st.n.angularVelocity;	// [0] pitch, [1] yaw, [2] roll
	int dotUp, cosLimit;

	if (gCd2Cfg.rollLimit <= 0)
		return;

	av[0] = jer_clamp_int(av[0], -CD2_ROLL_MAX_AV, CD2_ROLL_MAX_AV);
	av[2] = jer_clamp_int(av[2], -CD2_ROLL_MAX_AV, CD2_ROLL_MAX_AV);

	// car up . world up: 4096 upright, 0 fully on its side, -4096 on the roof
	dotUp = cp->hd.where.m[1][1];
	cosLimit = RCOS((gCd2Cfg.rollLimit * 4096) / 360);

	if (dotUp < cosLimit)
	{
		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] roll clamp: car=%d dotUp=%d limit=%d\n",
				cp->id, dotUp, cosLimit);

		// past the allowed lean: stop rolling; the stock suspension + gravity
		// settle the car back onto its wheels
		av[0] = 0;
		av[2] = 0;
	}
}

extern void RebuildCarMatrix(RigidBodyState* st, CAR_DATA* cp);

static int cd2RollIsqrt(int v)
{
	int r = 0;
	int bit = 1 << 30;

	if (v <= 0)
		return 0;

	while (bit > v)
		bit >>= 2;

	while (bit != 0)
	{
		if (v >= r + bit)
		{
			v -= r + bit;
			r = (r >> 1) + bit;
		}
		else
			r >>= 1;

		bit >>= 2;
	}

	return r;
}

// Post-physics roll recovery. JER_EVENT_DEBUG_TICK fires at the very end of
// GlobalTimeStep, after collisions, so this catches a car the crash code has
// already tipped past the limit: it rotates the car's up axis back toward world
// up by CD2_ROLL_RECOVER_DEG and kills the tilt rates. Uses the engine's own
// left-multiply quaternion convention (q += omega (x) q), axis = normalize(carUp
// x worldUp) = normalize((-cz, 0, cx)).
static void cd2RecoverRoll(CAR_DATA* cp)
{
	MATRIX* w = &cp->hd.where;
	int cx = w->m[0][1], cy = w->m[1][1], cz = w->m[2][1];
	int cosLimit, ax, az, len, ang, half, s, c, qx, qz, qw;
	int ox, oy, oz, ow;
	int* q = cp->st.n.orientation;

	cosLimit = RCOS((gCd2Cfg.rollLimit * 4096) / 360);
	if (cy >= cosLimit)
		return;		// within the allowed lean

	ax = -cz;
	az = cx;
	len = cd2RollIsqrt(ax * ax + az * az);
	if (len < 1)
		return;		// car is perfectly inverted on its forward axis: n/a

	ax = (int)(((long long)ax * 4096) / len);
	az = (int)(((long long)az * 4096) / len);

	ang = (CD2_ROLL_RECOVER_DEG * 4096) / 360;
	half = ang / 2;
	s = RSIN(half);
	c = RCOS(half);

	// q_corr = (axis*sin(half), cos(half)) as (x,y,z,w)
	qx = (int)(((long long)ax * s) >> 12);
	qz = (int)(((long long)az * s) >> 12);
	qw = c;

	// q = q_corr (x) q
	ox = q[0]; oy = q[1]; oz = q[2]; ow = q[3];
	q[0] = (qw * ox + qx * ow - qz * oy) >> 12;
	q[1] = (qw * oy - qx * oz + qz * ox) >> 12;
	q[2] = (qw * oz + qx * oy + qz * ow) >> 12;
	q[3] = (qw * ow - qx * ox - qz * oz) >> 12;

	cp->st.n.angularVelocity[0] = 0;
	cp->st.n.angularVelocity[2] = 0;

	RebuildCarMatrix(&cp->st, cp);

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] roll recover: car=%d dotUp=%d\n", cp->id, cy);
}

// ---- destroyed-car respawn ------------------------------------------------
// Every car the module owns (the player and the AI opponents) remembers the
// spot it started the level at; when it is wrecked (past the damage cap) it
// comes back there after gCd2Cfg.respawnDelay frames. Spawn points are a later
// feature - the start point stands in for now.

typedef struct CD2_RESPAWN
{
	int valid;		// home recorded
	int waiting;		// wrecked, counting down
	int timer;		// frames left until it returns
	int x, y, z;		// home position
	int dir;		// home heading
} CD2_RESPAWN;

static CD2_RESPAWN gCd2Respawn[MAX_CARS];

// A wrecked car should be light enough to get shoved around like the wreck it
// is, but ap.carCos points into the SHARED car_cosmetics[] table - one entry
// per MODEL, not per car. So a plain mass /= 4 would quarter every car of that
// model, then quarter it again if a second one died, and the first respawn
// would restore it out from under the second. Reference-count per model.
typedef struct CD2_MASS_MOD
{
	CAR_COSMETICS* cos;	// NULL when the slot is free
	int original;
	int count;		// cars currently wearing the reduced mass
} CD2_MASS_MOD;

static CD2_MASS_MOD gCd2MassMod[8];
static int gCd2DeathChannel = -1;

// 1 when this car is one the module drives (the player or an AI opponent).
static int cd2OwnsCar(CAR_DATA* cp)
{
	return (cp->controlType == CONTROL_TYPE_PLAYER) || cd2AiIsOpponent(cp);
}

// 1 for stock civ traffic: a car the module does NOT drive. Opponents are
// spawned as CUTSCENE, so CIV_AI is the stock-traffic case.
static int cd2IsTraffic(CAR_DATA* cp)
{
	return (cp->controlType == CONTROL_TYPE_CIV_AI) && !cd2AiIsOpponent(cp);
}

// Traffic that scrapes the world gets hurled. gCd2SceneryHits already ticks
// on every wall/building impact (see cd2OnDamageScale), so a CHANGE in it IS
// the contact - this needs no extra engine hook.
static void cd2TrafficTumble(CAR_DATA* cp)
{
	int hits = gCd2SceneryHits[cp->id];
	int* av = cp->st.n.angularVelocity;
	int vx, vz, spd, rate, sign;

	if (hits == gCd2TrafficLastHit[cp->id])
		return;		// no fresh contact

	gCd2TrafficLastHit[cp->id] = hits;

	// raw 16.16 velocity -> an approximate units/frame
	vx = ABS(cp->st.n.linearVelocity[0]) >> 12;
	vz = ABS(cp->st.n.linearVelocity[2]) >> 12;
	spd = (vx > vz) ? (vx + vz / 2) : (vz + vx / 2);

	if (spd < CD2_TRAFFIC_TUMBLE_MIN_SPEED)
		return;		// a nudge at walking pace is not worth a barrel roll

	rate = spd * CD2_TRAFFIC_TUMBLE_RATE;

	if (rate > CD2_TRAFFIC_TUMBLE_MAX)
		rate = CD2_TRAFFIC_TUMBLE_MAX;

	if (rate > CD2_TRAFFIC_SCRAPE_ROLL)
		rate = CD2_TRAFFIC_SCRAPE_ROLL;

	// Alternate the throw per contact so a car scraping down a long wall
	// does not settle into one steady spin.
	sign = (hits & 1) ? 1 : -1;

	av[0] += sign * rate;		// pitch
	av[2] += (sign * rate) / 2;	// roll

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] traffic tumble: car=%d spd=%d rate=%d sign=%d hits=%d\n",
			cp->id, spd, rate, sign, hits);
}

// Drop this car's model mass to a quarter until it respawns.
static void cd2MassQuarter(CAR_DATA* cp)
{
	int i, spare = -1;

	if (cp->ap.carCos == NULL)
		return;

	for (i = 0; i < 8; i++)
	{
		if (gCd2MassMod[i].cos == cp->ap.carCos)
		{
			gCd2MassMod[i].count++;
			return;
		}

		if (spare < 0 && gCd2MassMod[i].cos == NULL)
			spare = i;
	}

	if (spare < 0)
		return;			// table full; leave the mass alone rather than corrupt it

	gCd2MassMod[spare].cos = cp->ap.carCos;
	gCd2MassMod[spare].original = cp->ap.carCos->mass;
	gCd2MassMod[spare].count = 1;
	cp->ap.carCos->mass = gCd2MassMod[spare].original / 4;
}

// Put it back, once the last car wearing that model has respawned.
static void cd2MassRestore(CAR_DATA* cp)
{
	int i;

	if (cp->ap.carCos == NULL)
		return;

	for (i = 0; i < 8; i++)
	{
		if (gCd2MassMod[i].cos == cp->ap.carCos)
		{
			if (--gCd2MassMod[i].count <= 0)
			{
				gCd2MassMod[i].cos->mass = gCd2MassMod[i].original;
				gCd2MassMod[i].cos = NULL;
			}

			return;
		}
	}
}

// The casino bang. ExplosionSound uses GetMissionSound(29) on missions 30 and
// 35 (the casino ones) and the missile WIP reached for the same sample. Guard
// the way the engine does: a mission sample that is not resident comes back as
// 255, so fall back to the always-loaded SFX impact rather than going silent.
static void cd2DeathBang(CAR_DATA* cp)
{
	int bang = (unsigned char)GetMissionSound(29);	// char return: 0xFF arrives as -1
	int bank = SOUND_BANK_MISSION;

	if (bang == 255)
	{
		bang = 5;
		bank = SOUND_BANK_SFX;
	}

	if (gCd2DeathChannel < 0)
	{
		gCd2DeathChannel = GetFreeChannel(1);
		LockChannel(gCd2DeathChannel);
	}

	Start3DSoundVolPitch(gCd2DeathChannel, bank, bang,
		cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], 0, 3584);
}

static void cd2RespawnCar(CAR_DATA* cp, CD2_RESPAWN* r)
{
	int i;

	cp->totalDamage = 0;

	for (i = 0; i < 6; i++)
		cp->ap.damage[i] = 0;

	cp->hd.where.t[0] = r->x;
	cp->hd.where.t[1] = r->y;
	cp->hd.where.t[2] = r->z;
	cp->hd.direction = r->dir;

	for (i = 0; i < 3; i++)
	{
		cp->st.n.linearVelocity[i] = 0;
		cp->st.n.angularVelocity[i] = 0;
	}

	cp->thrust = 0;
	cp->wheel_angle = 0;
	cp->handbrake = 0;
	cp->wheelspin = 0;
	cp->hd.speed = 0;
	cp->hd.wheel_speed = 0;

	RebuildCarMatrix(&cp->st, cp);

	// reset this module's per-car handling state so nothing carries over
	gCd2Car[cp->id].yawRate = 0;
	gCd2Car[cp->id].slip = 0;
	gCd2Car[cp->id].roll = 0;
	gCd2Car[cp->id].throttle = 0;
	gCd2Car[cp->id].pivotDir = 0;
	gCd2Car[cp->id].slideTicks = 0;
	gCd2Car[cp->id].aiPivot = 0;

	cd2MassRestore(cp);	// full weight again

	printInfo("[combatd2] respawn: car=%d back at start (%d,%d,%d), mass %d\n",
		cp->id, r->x, r->y, r->z, (cp->ap.carCos != NULL) ? cp->ap.carCos->mass : -1);
}

static void cd2RespawnTick(CAR_DATA* cp)
{
	CD2_RESPAWN* r = &gCd2Respawn[cp->id];

	if (!cd2OwnsCar(cp))
	{
		r->valid = 0;
		r->waiting = 0;
		return;
	}

	// record the home spot the first time the car is seen alive at the start
	if (!r->valid && !cd2CarTotaled(cp))
	{
		r->valid = 1;
		r->x = cp->hd.where.t[0];
		r->y = cp->hd.where.t[1];
		r->z = cp->hd.where.t[2];
		r->dir = cp->hd.direction;
	}

	if (!gCd2Cfg.respawn)
	{
		r->waiting = 0;
		return;
	}

	if (cd2CarTotaled(cp))
	{
		if (!r->waiting)
		{
			r->waiting = 1;
			r->timer = gCd2Cfg.respawnDelay;

			// Hand the car straight to the engine as a write-off instead of
			// letting damage creep up to the cap: slam totalDamage to the
			// engine's own maximum (the value ApplyDamage clamps to) and drop
			// every control input, so the engine's lockup / kill-patrol
			// handling takes over this frame.
			cp->totalDamage = USHRT_MAX;
			cp->thrust = 0;
			cp->wheel_angle = 0;
			cp->handbrake = 0;
			cp->wheelspin = 0;

			// wrecked cars are lighter until they come back, and they go out with
			// the casino bang
			cd2MassQuarter(cp);
			cd2DeathBang(cp);

			printInfo("[combatd2] respawn: car=%d DESTROYED (type=%d) - control stripped, mass now %d, returning in %d frames\n",
				cp->id, cp->controlType, (cp->ap.carCos != NULL) ? cp->ap.carCos->mass : -1, r->timer);
		}
	}
	else if (r->waiting)
	{
		r->waiting = 0;		// recovered some other way
	}

	if (r->waiting && r->valid && --r->timer <= 0)
	{
		cd2RespawnCar(cp, r);
		r->waiting = 0;
	}
}

// New level: forget every home.
static int cd2OnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	memset(gCd2Respawn, 0, sizeof(gCd2Respawn));
	memset(gCd2SceneryHits, 0, sizeof(gCd2SceneryHits));
	memset(gCd2TrafficLastHit, 0, sizeof(gCd2TrafficLastHit));
	memset(gCd2MassMod, 0, sizeof(gCd2MassMod));	// masses are level data, re-read on load

	return JER_RESULT_CONTINUE;
}

static int cd2OnDebugTick(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled || gCd2Cfg.rollLimit <= 0)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (cp->controlType == CONTROL_TYPE_NONE)
			continue;

		// Traffic is SUPPOSED to be tumbling. Standing it back up would undo
		// the whole point, and it fights the weapon knockback besides.
		if (cd2IsTraffic(cp))
			continue;

		cd2RecoverRoll(cp);
	}

	return JER_RESULT_CONTINUE;
}

// Shared damage rediuction: `value` scaled to `pct` percent (both damage hooks
// route through this so player and opponent cars are treated identically).
int cd2ScaleDamage(int value, int pct)
{
	return (value * pct) / 100;
}

// Scenery impacts taken by `car` this level (see cd2OnDamageScale).
int cd2SceneryHits(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return 0;

	return gCd2SceneryHits[cp->id];
}

// Scenery (building/wall) damage scale: soften the damage a car takes from
// hitting solid objects (the momentum-absorbing walls make these hits bite
// hard). Fired from DamageCar (bcollide.c) before ApplyDamage.
static int cd2OnDamageScale(void* ud, void* args)
{
	JER_ARGS_DAMAGE_SCALE* a = (JER_ARGS_DAMAGE_SCALE*)args;

	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	a->result = cd2ScaleDamage(4096, gCd2Cfg.sceneryDamage);

	// Count real scenery impacts for EVERY car, not just opponents. The AI
	// readout uses it to measure "keeps smashing into walls", and the
	// traffic tumble detects a world contact from a CHANGE in it - so with
	// the increment buried in the opponent branch below, traffic could
	// never register a hit and never tumbled at all.
	if (((CAR_DATA*)a->car)->id >= 0 && ((CAR_DATA*)a->car)->id < MAX_CARS)
		gCd2SceneryHits[((CAR_DATA*)a->car)->id]++;

	// an opponent that keeps clipping walls needs the extra cushion, or a
	// single corner ends its run
	if (cd2AiIsOpponent(a->car))
		a->result = cd2ScaleDamage(a->result, gCd2Cfg.aiDamageTaken);

	// Traffic takes a further half off scenery impacts.
	if (cd2IsTraffic((CAR_DATA*)a->car))
		a->result = cd2ScaleDamage(a->result, CD2_TRAFFIC_SCENERY_EXTRA);

	if (gCd2Cfg.debugLog)
	{
		static unsigned int t = 0;
		if ((t++ & 63) == 0)
			printInfo("[combatd2] scenery dmg scale: car=%d type=%d -> %d%% (%d) dmg=%d\n",
				((CAR_DATA*)a->car)->id, ((CAR_DATA*)a->car)->controlType, gCd2Cfg.sceneryDamage, a->result,
				((CAR_DATA*)a->car)->totalDamage);
	}

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_CAR_VS_CAR: retune the damage two cars exchange. An owned opponent
// uses the player damage model (not the harsher traffic multiplier), and every
// car-to-car impact is scaled down by the car-to-car nerf.
static int cd2OnCarVsCar(void* ud, void* args)
{
	JER_ARGS_CAR_VS_CAR* a = (JER_ARGS_CAR_VS_CAR*)args;
	int v;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	v = a->value;

	if (cd2AiIsOpponent(a->car))
		v = a->playerValue;

	v = cd2ScaleDamage(v, gCd2Cfg.carCarDamage);

	// opponents take a further cut so they survive long enough to be a threat
	if (cd2AiIsOpponent(a->car))
		v = cd2ScaleDamage(v, gCd2Cfg.aiDamageTaken);

	if (gCd2Cfg.debugLog)
	{
		static unsigned int t = 0;
		if ((t++ & 63) == 0)
			printInfo("[combatd2] car-car dmg: car=%d type=%d opp=%d stock=%d -> %d (pct=%d) dmg=%d\n",
				((CAR_DATA*)a->car)->id, ((CAR_DATA*)a->car)->controlType,
				cd2AiIsOpponent(a->car), a->value, v, gCd2Cfg.carCarDamage,
				((CAR_DATA*)a->car)->totalDamage);
	}

	// Exactly one of the pair being ours means this is a combatd2 car
	// trading paint with civilian traffic. Take 80% off.
	if (a->car != NULL && a->other != NULL &&
		    (cd2OwnsCar((CAR_DATA*)a->car) != cd2OwnsCar((CAR_DATA*)a->other)))
		v = cd2ScaleDamage(v, CD2_CAR_TRAFFIC_DAMAGE);

	// Being shunted. A combatd2 car punting a civ car rolls it over: the
	// roll axis is horizontal and perpendicular to the shove, so it tumbles
	// end over end along the ground instead of spinning on the spot.
	{
		CAR_DATA* ca = (CAR_DATA*)a->car;
		CAR_DATA* ot = (CAR_DATA*)a->other;
		CAR_DATA* tc = NULL;
		CAR_DATA* sc = NULL;

		if (ca != NULL && ot != NULL)
		{
			if (cd2IsTraffic(ca) && cd2OwnsCar(ot))
			{
				tc = ca;
				sc = ot;
			}
			else if (cd2IsTraffic(ot) && cd2OwnsCar(ca))
			{
				tc = ot;
				sc = ca;
			}

			if (tc != NULL)
			{
				int dx = tc->hd.where.t[0] - sc->hd.where.t[0];
				int dz = tc->hd.where.t[2] - sc->hd.where.t[2];
				int adx = ABS(dx), adz = ABS(dz);
				int al = (adx > adz) ? (adx + adz / 2) : (adz + adx / 2);
				int rate = ABS(a->strikeVel) * CD2_TRAFFIC_ROLL_RATE;
				int* av = tc->st.n.angularVelocity;

				if (rate > CD2_TRAFFIC_ROLL_MAX)
					rate = CD2_TRAFFIC_ROLL_MAX;

				if (al < 1)
					al = 1;

				// roll about the horizontal axis perpendicular to the shove
				av[0] += (int)(((long long)(-dz) * rate) / al);
				av[2] += (int)(((long long)(-dx) * rate) / al);

				if (gCd2Cfg.debugLog)
					printInfo("[combatd2] traffic shove: car=%d by=%d strike=%d rate=%d\n",
						tc->id, sc->id, a->strikeVel, rate);
			}
		}
	}

	a->value = v;
	return JER_RESULT_CONTINUE;
}

#if CD2_ENFORCE_PURSUIT_MUSIC
// Force the music onto the "pursuit" segment each frame (see
// CD2_ENFORCE_PURSUIT_MUSIC). FunkUpDaBGMTunez is idempotent, so this just
// re-asserts the pursuit tune if the stock cop/felony logic switched it off.
static int cd2OnFramePursuit(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (gCd2Cfg.enabled)
		FunkUpDaBGMTunez(1);

	return JER_RESULT_CONTINUE;
}
#endif

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

	// deferred "Total Car" debug: applied on the first physics frame after
	// unpausing so the wreck/explosion don't fire while the pause menu is up.
	if (gPendingTotalCar && cp->id == MainPlayer.playerCarId)
	{
		gPendingTotalCar = 0;
		cp->totalDamage = 0xffff;
		Start3DSoundVolPitch(
			-1,
			SOUND_BANK_MISSION,
			29,
			cp->hd.where.t[0],
			cp->hd.where.t[1],
			cp->hd.where.t[2],
			-2000,
			4096 + 2048
		);
	}

	gCd2Car[cp->id].throttle = (cp->thrust > 0) ? 1 : (cp->thrust < 0) ? -1 : 0;

	// Traffic is exempt from the roll-over limiter: it is meant to be thrown
	// around, and clamping its pitch/roll is exactly what stopped that.
	if (cd2IsTraffic(cp))
		cd2TrafficTumble(cp);
	else
		cd2LimitRoll(cp);

	// wrecked cars return to their start after a delay
	cd2RespawnTick(cp);

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

	// Traffic keeps STOCK handling. The TMB handling / top-speed override is
	// for the player and the opponents; civilian traffic should stay slow and
	// unwieldy so it reads as scenery that can be knocked about.
	if (cd2IsTraffic(cp))
		return JER_RESULT_CONTINUE;

	CD2_CAR* c = &gCd2Car[cp->id];
	CD2_STATS s = cd2GetStats(cp);

	// ---- steering: direct yaw control -------------------------------
	int steer = jer_clamp_int((int)cp->wheel_angle, -CD2_STEER_MAX, CD2_STEER_MAX);
	int steerFp = (steer * 4096) / CD2_STEER_MAX;           // -4096..4096
	int handlingNow = (int)(((long long)s.handling * s.control) >> 12);

	// TM2 speed-sensitive yaw: turn authority tapers as speed rises so the
	// car can't spin out at top speed (opt-in via CD2_YAW_SPEED_FALLOFF).
	if (CD2_YAW_SPEED_FALLOFF)
	{
		int lvx = cp->st.n.linearVelocity[0];
		int lvz = cp->st.n.linearVelocity[2];
		int mag = (ABS(FIXEDH(lvx)) < ABS(FIXEDH(lvz)))
			? (ABS(FIXEDH(lvz)) + ABS(FIXEDH(lvx)) / 2)
			: (ABS(FIXEDH(lvx)) + ABS(FIXEDH(lvz)) / 2);
		if (mag > s.topSpeed) mag = s.topSpeed;
		int norm = (s.topSpeed > 0) ? (int)(((long long)mag * 4096) / s.topSpeed) : 0;
		handlingNow = (int)(((long long)handlingNow * (4096 - ((CD2_YAW_SPEED_FALLOFF * norm) >> 12))) >> 12);
	}

	// ---- Tight Turn (TMB): acute pivot on its own yaw authority -----
	// Player-only. While the trigger is held, steering picks/latches a pivot
	// direction; releasing clears it. It is independent of the grip pass, so
	// the pivot keeps full authority through any slide (TMB: "overrides
	// normal physics for a brief moment").
	int tightActive = 0;
	int slideNow = 0; // traction-suspended slide active (computed once per frame)
	if (gCd2Cfg.tightTurn)
	{
		if (c->aiPivot != 0)
		{
			// An AI car asked for an acute in-place pivot this frame.
			tightActive = 1;
		}
		else if (cp->controlType == CONTROL_TYPE_PLAYER)
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
		// forced acute rotation: normal handling x CD2_TIGHT_MULT, so the
		// pivot scales with the car's own steering authority; the strength
		// slider blends between plain handling and the full pivot.
		long long pivot = (handlingNow * CD2_TIGHT_MULT) >> 12;
		pivot = handlingNow + ((pivot - handlingNow) * gCd2Cfg.tightStrength) / 100;
		targetYaw = (int)pivot * c->pivotDir;
	}
	else
	{
		targetYaw = (handlingNow * steerFp) >> 12;           // PSX-units/frame
	}

	// yaw accelerates toward the target; snappier onset during a pivot, and a
	// stronger step while returning to center so the car stops spinning
	// promptly instead of carrying rotation (less angular momentum).
	int yawStep = s.angularAccel;
	if (tightActive)
		yawStep = (int)(yawStep * CD2_TIGHT_ANG_MULT);
	int yaw = c->yawRate;
	if (ABS(targetYaw) < ABS(yaw))
	{
		// stifle the tight-turn rotation velocity below speed: once the
		// trigger is released a barely-rolling car shouldn't keep spinning
		int lvx = cp->st.n.linearVelocity[0];
		int lvz = cp->st.n.linearVelocity[2];
		int mag = (ABS(FIXEDH(lvx)) < ABS(FIXEDH(lvz)))
			? (ABS(FIXEDH(lvz)) + ABS(FIXEDH(lvx)) / 2)
			: (ABS(FIXEDH(lvx)) + ABS(FIXEDH(lvz)) / 2);
		if (!tightActive && mag < CD2_TIGHT_ROT_STOP_SPEED)
			yaw = targetYaw; // snap the residual pivot spin off
		else
			yawStep = (int)(((long long)yawStep * CD2_YAW_DECAY) >> 12);
	}
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

	// Airborne: conserve momentum — no throttle/brake/drag and no tire
	// friction (grip/bleed). Yaw above still applies, so the player can
	// rotate mid-air (TM air control) without the car being "driven" or
	// air-braked in flight.
	int grounded = (cp->hd.wheel[0].susCompression | cp->hd.wheel[1].susCompression |
	                cp->hd.wheel[2].susCompression | cp->hd.wheel[3].susCompression) != 0;

	long long latVel = 0; // in scope for the telemetry below too
	int grip = 0;

	if (grounded)
	{
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

		// idle micro-roll guard: with no gas and no brake (this is the branch
		// where throttle == 0) and barely any speed left, snap to a full stop
		// so the car doesn't creep / roll a few units forever
		{
			int ax = ABS(FIXEDH(velX));
			int az = ABS(FIXEDH(velZ));

			if ((ax < az ? (az + ax / 2) : (ax + az / 2)) < CD2_MICRO_STOP_SPEED)
			{
				velX = 0;
				velZ = 0;
			}
		}
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
	latVel = ((long long)velX * rx + (long long)velZ * rz) >> 24; // speed units (4096-scaled dot)

	int absSteer = steerFp < 0 ? -steerFp : steerFp;          // 0..4096
	long long absSpeed = fwdSpeed < 0 ? -fwdSpeed : fwdSpeed;
	if (absSpeed > s.topSpeed) absSpeed = s.topSpeed;

	// slipFactor 0..4096: hard steer + high speed -> grip falls off
	long long slipFactor = (absSteer * absSpeed) / s.topSpeed;
	int gripDrop = (int)((slipFactor * CD2_SLIP_REDUCTION) / 10);
	grip = (int)(((long long)s.grip * (4096 - gripDrop)) >> 12);

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
		// grip during the slide: normal slides keep CD2_SLIDE_GRIP_FRAC of
		// traction (they arc a little); once the skid is big enough to be
		// sliding SIDEWAYS (|latVel| > CD2_SKID_LOCK_LAT) the slide LOCKS onto
		// its original momentum line — grip near zero so the rotating heading
		// never scrubs the velocity (pure TMB ice drift until you release).
		if (ABS(latVel) > CD2_SKID_LOCK_LAT)
			grip = (int)(((long long)grip * CD2_SKID_LOCK_GRIP) >> 12);
		else
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
	}

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

// GET_PHYSICS_PARAMS: gravity + faster angular settling + tighter suspension.
static int cd2OnPhysicsParams(void* ud, void* args)
{
	JER_ARGS_PHYSICS_PARAMS* a = (JER_ARGS_PHYSICS_PARAMS*)args;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	a->gravity = CD2_GRAVITY;
	a->angularDamping = CD2_ANGULAR_DAMPING;
	a->springRate = CD2_SPRING_RATE;
	a->springDamping = CD2_SPRING_DAMPING;
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

// ---- submenu: Tight Turn -------------------------------------------------
static const JER_PAUSE_MENU_ITEM cd2TightItems[] =
{
	{ NULL, cd2LabelTightToggle,   cd2ToggleTight,      NULL, NULL, 0 },
	{ NULL, cd2LabelTightStrength, cd2AdjTightStrength, NULL, NULL, 1 },
	{ NULL, cd2LabelTightInput,    cd2CycleTightInput,  NULL, NULL, 1 },
};

static const JER_PAUSE_MENU cd2TightMenu =
{ "Tight Turn", cd2TightItems, 3 };

// ---- submenu: Debug ------------------------------------------------------
static int cd2TotalCar(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	// defer: the damage is applied on the next physics frame (after unpausing)
	// so the wreck + explosion don't trigger while the pause menu is up.
	if (MainPlayer.playerCarId >= 0)
		gPendingTotalCar = 1;

	return JER_PAUSE_QUIT_NONE;
}

// ---- submenu: Weapons ----------------------------------------------------
// Test/dev helpers: grant every weapon at once, plus a per-weapon grant/clear
// toggle. Base weapons (the machine gun) are not listed - always carried.
static void cd2LabelAllWeapons(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "All Weapons (Test): %s", gCd2Cfg.allWeapons ? "ON" : "OFF");
}

static int cd2ToggleAllWeapons(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.allWeapons = !gCd2Cfg.allWeapons;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static int cd2GrantAllNow(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	cd2WpnGrantAllMax();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelWeapon(void* ud, char* out, int max)
{
	int id = (int)(size_t)ud;
	const CD2_WEAPON_DEF* d = cd2WpnDef(id);

	if (d == NULL)
	{
		snprintf(out, max, "(invalid)");
		return;
	}

	if (d->isBase || cd2WpnAmmo(id) < 0)
		snprintf(out, max, "%s: infinite", d->name);
	else
		snprintf(out, max, "%s: %d", d->name, cd2WpnAmmo(id));
}

static int cd2ToggleWeapon(void* ud, int dir)
{
	int id = (int)(size_t)ud;
	const CD2_WEAPON_DEF* d = cd2WpnDef(id);
	(void)dir;

	if (d == NULL)
		return JER_PAUSE_QUIT_NONE;

	if (cd2WpnOwns(id))
		cd2WpnClear(id);
	else
		cd2WpnGrant(id, (d->maxAmmo > 0) ? d->maxAmmo : 10);

	return JER_PAUSE_QUIT_NONE;
}

// ---- AI + balance test helpers -------------------------------------------
static void cd2LabelAi(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Opponent AI: %s", gCd2Cfg.aiOpponent ? "ON" : "OFF");
}

static int cd2ToggleAi(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.aiOpponent = !gCd2Cfg.aiOpponent;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelAiState(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "AI State: %s", cd2AiStateName());
}

static int cd2CycleAiState(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.aiForceState = (gCd2Cfg.aiForceState + 1) % CD2_AI_STATE_COUNT;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelAiDebug(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "AI Readout: %s", gCd2Cfg.aiDebug ? "ON" : "OFF");
}

static int cd2ToggleAiDebug(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.aiDebug = !gCd2Cfg.aiDebug;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelRespawn(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Car Respawn: %s", gCd2Cfg.respawn ? "ON" : "OFF");
}

static int cd2ToggleRespawn(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.respawn = !gCd2Cfg.respawn;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelAiRole(void* ud, char* out, int max)
{
	static const char* names[] = { "Auto", "Chaser", "Flanker", "Ambusher", "Harvester" };
	int r = gCd2Cfg.aiRole;
	(void)ud;

	if (r < -1 || r >= CD2_AI_ROLE_COUNT)
		r = -1;

	snprintf(out, max, "Opponent Role: %s", names[r + 1]);
}

static int cd2CycleAiRole(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gCd2Cfg.aiRole++;

	if (gCd2Cfg.aiRole >= CD2_AI_ROLE_COUNT)
		gCd2Cfg.aiRole = -1;

	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelNavDebug(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Nav Debug: %s", gCd2Cfg.navDebug ? "ON" : "OFF");
}

static int cd2ToggleNavDebug(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.navDebug = !gCd2Cfg.navDebug;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelCarCarNerf(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Car-Car Damage: %d%%", gCd2Cfg.carCarDamage);
}

static int cd2CycleCarCarNerf(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.carCarDamage = 10 + ((gCd2Cfg.carCarDamage - 10 + 5) % 91);
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelScenery(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Scenery Damage: %d%%", gCd2Cfg.sceneryDamage);
}

static int cd2CycleScenery(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.sceneryDamage = (gCd2Cfg.sceneryDamage + 5) % 105;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM cd2WeaponItems[] =
{
	{ NULL, cd2LabelAllWeapons, cd2ToggleAllWeapons, NULL, NULL, 0 },
	{ "Grant All Now", NULL, cd2GrantAllNow, NULL, NULL, 0 },
	{ NULL, cd2LabelWeapon, cd2ToggleWeapon, (void*)(size_t)CD2_WID_MISSILE, NULL, 0 },
	{ NULL, cd2LabelWeapon, cd2ToggleWeapon, (void*)(size_t)CD2_WID_MINE, NULL, 0 },
	{ NULL, cd2LabelAi, cd2ToggleAi, NULL, NULL, 0 },
	{ NULL, cd2LabelAiState, cd2CycleAiState, NULL, NULL, 0 },
	{ NULL, cd2LabelAiRole, cd2CycleAiRole, NULL, NULL, 0 },
	{ NULL, cd2LabelAiDebug, cd2ToggleAiDebug, NULL, NULL, 0 },
	{ NULL, cd2LabelRespawn, cd2ToggleRespawn, NULL, NULL, 0 },
	{ NULL, cd2LabelNavDebug, cd2ToggleNavDebug, NULL, NULL, 0 },
	{ NULL, cd2LabelScenery, cd2CycleScenery, NULL, NULL, 0 },
	{ NULL, cd2LabelCarCarNerf, cd2CycleCarCarNerf, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU cd2WeaponMenu =
{ "Weapons", cd2WeaponItems, 12 };

static const JER_PAUSE_MENU_ITEM cd2DebugItems[] =
{
	{ NULL, cd2LabelDebug, cd2ToggleDebug, NULL, NULL, 0 },
	{ "Total Car", NULL, cd2TotalCar, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU cd2DebugMenu =
{ "Debug", cd2DebugItems, 2 };

// ---- main menu -----------------------------------------------------------
static const JER_PAUSE_MENU_ITEM cd2MenuItems[] =
{
	{ NULL, cd2LabelEnabled,  cd2ToggleEnabled, NULL, NULL, 0 },
	{ NULL, cd2LabelTopSpeed, cd2AdjTopSpeed,   NULL, NULL, 1 },
	{ NULL, cd2LabelAccel,    cd2AdjAccel,      NULL, NULL, 1 },
	{ NULL, cd2LabelBrake,    cd2AdjBrake,      NULL, NULL, 1 },
	{ NULL, cd2LabelHandling, cd2AdjHandling,   NULL, NULL, 1 },
	{ NULL, cd2LabelGrip,     cd2AdjGrip,       NULL, NULL, 1 },
	{ NULL, cd2LabelTmbButtons, cd2ToggleTmbButtons, NULL, NULL, 0 },
	{ "Tight Turn...", NULL, NULL, NULL, &cd2TightMenu, 0 },
	{ NULL, cd2LabelPreset,   cd2CyclePreset,   NULL, NULL, 1 },
	{ "Weapons...", NULL, NULL, NULL, &cd2WeaponMenu, 0 },
	{ "Debug...", NULL, NULL, NULL, &cd2DebugMenu, 0 },
	{ "Reset to Defaults", NULL, cd2ResetDefaults, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU cd2Menu =
{ "Combat D2", cd2MenuItems, 12 };

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
		"Caine's Crossfire",		/* name */
		"0.6.0",					/* version */
		"JERICHO",					/* author */
		"Twisted Metal: Black style car combat: point-mass handling (velocity + yaw, Tight Turn pivot, TMB buttons, momentum-absorbing walls), totaled-car wreck effects, engine presentation tuners, and the weapon prototype.",	/* description */
		"",							/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, cd2OnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, cd2OnResetCar, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_PAD, cd2OnCarPad, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2OnCarStep, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_TORQUE, cd2OnCarTorque, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_WALL_RESTITUTION, cd2OnGetWallRestitution, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_PHYSICS_PARAMS, cd2OnPhysicsParams, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW, cd2OnCarDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_DAMAGE_SCALE, cd2OnDamageScale, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_VS_CAR, cd2OnCarVsCar, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DEBUG_TICK, cd2OnDebugTick, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2OnGameStart, NULL, 0);

#if CD2_ENFORCE_PURSUIT_MUSIC
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2OnFramePursuit, NULL, 0);
#endif

	jer_pause_menu_register(&cd2Menu);

	// the merged sibling sources (kept as separate files, registered here in
	// core -> combat -> presentation order so the dispatch order of the old
	// three-module layout is preserved: combat's CAR_STEP stays at priority -1)
	cd2CombatRegister(ctx);
	cd2MediaRegister(ctx);
	cd2WeaponsRegister(ctx);
	cd2FxRegister(ctx);
	cd2FreezeRegister(ctx);
	cd2AiRegister(ctx);

	ctx->jer_log(ctx, "[combatd2] registered (SDK v%d)\n", ctx->sdkVersion);
}
