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
#include "combatd2_internal.h"	/* cross-file decls within this module */
#include "cars.h"
#include "cosmetic.h"
#include "camera.h"
#include "convert.h"
#include "players.h"
#include "pad.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_hud.h"	// HUD messages (kill banners)
#include "jer_pause_menu.h"
#include "jer_config.h"
#include "jer_math.h"
#include "sound.h"
#include "gamesnd.h"
#include "mc_snd.h"
#include "weapons/core/weapon.h"	/* CD2_WEAPON_DEF + inventory API */
#include "ai/ai.h"			/* opponent AI (ai/opponent.c) */
#include "factions/factions.h"	/* the five teams (factions/factions.c) */
#include "carhacks/carhacks.h"		/* vehicle-availability hacks (own module later) */
#include <string.h>
// Registration helpers from the other source files of this (merged) module:
//   combatd2wreckfx.c  — wreck explosion + kill credit (cd2WreckFxRegister)
//   combatd2carfx.c    — totaled-car presentation      (cd2CarFxRegister)
//   combatd2gearbox.c  — gearbox/rev-curve tuner  (cd2GearboxRegister)
//   combatd2enginesnd.c— engine rev/idle channels (cd2EngineSndRegister)
//   combatd2camerafx.c — chase framing + speed FOV (cd2CameraFxRegister)
//   weapons/core      — weapon framework         (cd2WeaponsRegister)
void cd2WreckFxRegister(JERICHO_CONTEXT* ctx);	/* combatd2wreckfx.c */
void cd2CarFxRegister(JERICHO_CONTEXT* ctx);	/* combatd2carfx.c */
void cd2GearboxRegister(JERICHO_CONTEXT* ctx);	/* combatd2gearbox.c */
void cd2EngineSndRegister(JERICHO_CONTEXT* ctx);	/* combatd2enginesnd.c */
void cd2CameraFxRegister(JERICHO_CONTEXT* ctx);	/* combatd2camerafx.c */
void cd2WeaponsRegister(JERICHO_CONTEXT* ctx);
void cd2DebugRegister(JERICHO_CONTEXT* ctx);	/* TEMPORARY: cd2debug.c */
void cd2FxRegister(JERICHO_CONTEXT* ctx);
void cd2FreezeRegister(JERICHO_CONTEXT* ctx);
void cd2CrewRegister(JERICHO_CONTEXT* ctx);	/* weapons/core/crew.c */
void cd2FacRegister(JERICHO_CONTEXT* ctx);	/* factions/factions.c (declared in its header) */

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

CD2_CONFIG gCd2Cfg;
CD2_CAR gCd2Car[MAX_CARS];
int gCd2SceneryHits[MAX_CARS];	// scenery impacts per car this level
int gCd2TrafficLastHit[MAX_CARS];	// last scenery-hit count seen, per car



// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void cd2LoadConfig(void)
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

	// factions (factions/, see FACTIONS.md): give every car a team identity,
	// and which team the player is. 0 = TANNER, and a non-competing faction is
	// refused by cd2FacPlayerFaction (the player drives).
	gCd2Cfg.factions      = jer_config_get_int("combatd2", "factions", 1);
	gCd2Cfg.playerFaction = jer_config_get_int("combatd2", "player_faction", CD2_FAC_TANNER);

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
	gCd2Cfg.factions      = gCd2Cfg.factions ? 1 : 0;
	gCd2Cfg.playerFaction = jer_clamp_int(gCd2Cfg.playerFaction, 0, CD2_FAC_COUNT - 1);
	gCd2Cfg.carCarDamage  = jer_clamp_int(gCd2Cfg.carCarDamage, 10, 100);
	gCd2Cfg.aiDamageTaken = jer_clamp_int(gCd2Cfg.aiDamageTaken, 10, 400);
	gCd2Cfg.respawn       = gCd2Cfg.respawn ? 1 : 0;
	gCd2Cfg.missileScale  = jer_clamp_int(gCd2Cfg.missileScale, 512, 16384);
	gCd2Cfg.missileSound  = jer_clamp_int(gCd2Cfg.missileSound, 0, 34);
}

void cd2SaveConfig(void)
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
	jer_config_set_int("combatd2", "factions", gCd2Cfg.factions);
	jer_config_set_int("combatd2", "player_faction", gCd2Cfg.playerFaction);
	jer_config_set_int("combatd2", "car_car_damage", gCd2Cfg.carCarDamage);
	jer_config_set_int("combatd2", "ai_damage_taken", gCd2Cfg.aiDamageTaken);
	jer_config_set_int("combatd2", "respawn", gCd2Cfg.respawn);
	jer_config_set_str("combatd2", "missile_model", gCd2Cfg.missileModel);
	jer_config_set_int("combatd2", "missile_scale", gCd2Cfg.missileScale);
	jer_config_set_int("combatd2", "missile_sound", gCd2Cfg.missileSound);
}

void cd2ApplyPreset(void)
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

CD2_STATS cd2GetStats(CAR_DATA* cp)
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

extern void RebuildCarMatrix(RigidBodyState* st, CAR_DATA* cp);


// ---- car identity ---------------------------------------------------------
// 1 when this car is one the module drives (the player or an AI opponent).
int cd2OwnsCar(CAR_DATA* cp)
{
	return (cp->controlType == CONTROL_TYPE_PLAYER) || cd2AiIsOpponent(cp);
}

// 1 for stock civ traffic: a car the module does NOT drive. Opponents are
// spawned as CUTSCENE, so CIV_AI is the stock-traffic case.
// Non-static: the weapons layer scales damage per target class (see
// cd2WpnDamageCar), and the AI helper it leans on is extern'd the same way.
int cd2IsTraffic(CAR_DATA* cp)
{
	return (cp->controlType == CONTROL_TYPE_CIV_AI) && !cd2AiIsOpponent(cp);
}

// Traffic that scrapes the world gets hurled. gCd2SceneryHits already ticks
// on every wall/building impact (see cd2OnDamageScale), so a CHANGE in it IS
// the contact - this needs no extra engine hook.

#if CD2_ENFORCE_PURSUIT_MUSIC
// Force the music onto the "pursuit" segment each frame (see
// CD2_ENFORCE_PURSUIT_MUSIC). FunkUpDaBGMTunez is idempotent, so this just
// re-asserts the pursuit tune if the stock cop/felony logic switched it off.
int cd2OnFramePursuit(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	// Only while a game is actually running. This hook fires for EVERY frame,
	// including the frontend's and the menus', where the music tables are not
	// set up: FunkUpDaBGMTunez indexes xm_coptrackpos, which is still NULL there,
	// and that dereference crashed the frontend on its first frame. A player car
	// that is in the world is the cheapest reliable "in a game" test.
	if (MainPlayer.playerCarId < 0 || MainPlayer.playerCarId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (car_data[MainPlayer.playerCarId].controlType != CONTROL_TYPE_PLAYER)
		return JER_RESULT_CONTINUE;

	FunkUpDaBGMTunez(1);

	return JER_RESULT_CONTINUE;
}
#endif

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

	cd2MenuRegister(ctx);	/* combatd2menu.c */

	// the merged sibling sources (kept as separate files, registered here in
	// core -> combat -> presentation order so the dispatch order of the old
	// three-module layout is preserved: combat's CAR_STEP stays at priority -1)
	cd2WreckFxRegister(ctx);
	cd2CarFxRegister(ctx);
	cd2GearboxRegister(ctx);
	cd2EngineSndRegister(ctx);
	cd2CameraFxRegister(ctx);
	cd2WeaponsRegister(ctx);
	cd2CrewRegister(ctx);
	cd2FxRegister(ctx);
	cd2FreezeRegister(ctx);

	// The factions go in BEFORE the AI: a spawned opponent claims its roster
	// slot, so the field has to exist (and be reset for the new level) first.
	cd2FacRegister(ctx);
	cd2AiRegister(ctx);

	/* vehicle-availability hacks - self-contained, hosted here for now and
	 * intended to move to its own module (see carhacks/carhacks.h) */
	carhacks_register(ctx);

	/* TEMPORARY: scripted debug driver, active only when debug_script is set
	 * (delete cd2debug.c and these two lines when done) */
	cd2DebugRegister(ctx);

	ctx->jer_log(ctx, "[combatd2] registered (SDK v%d)\n", ctx->sdkVersion);
}
