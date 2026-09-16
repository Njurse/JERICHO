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
#include "carhacks/carhacks.h"		/* vehicle-availability hacks (own module later) */
#include <string.h>
// Registration helpers from the other source files of this (merged) module:
//   combatd2combat.c — wreck/explosion effects   (cd2CombatRegister)
//   combatd2media.c   — presentation tuners      (cd2MediaRegister)
//   weapons/core      — weapon framework         (cd2WeaponsRegister)
void cd2CombatRegister(JERICHO_CONTEXT* ctx);
void cd2MediaRegister(JERICHO_CONTEXT* ctx);
void cd2WeaponsRegister(JERICHO_CONTEXT* ctx);
void cd2DebugRegister(JERICHO_CONTEXT* ctx);	/* TEMPORARY: cd2debug.c */
void cd2FxRegister(JERICHO_CONTEXT* ctx);
void cd2FreezeRegister(JERICHO_CONTEXT* ctx);
void cd2CrewRegister(JERICHO_CONTEXT* ctx);	/* weapons/core/crew.c */

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

CD2_CONFIG gCd2Cfg;
CD2_CAR gCd2Car[MAX_CARS];
int gCd2SceneryHits[MAX_CARS];	// scenery impacts per car this level
int gCd2TrafficLastHit[MAX_CARS];	// last scenery-hit count seen, per car

// Traffic (civ) cars are cannon fodder. They get no handling override and
// are deliberately EXEMPT from the roll-over limiter, so a weapon hit or a
// wall scrape can throw them properly instead of being clamped back down.
#define CD2_TRAFFIC_TUMBLE_MIN_SPEED	60		// units/frame below which a scrape is ignored
#define CD2_TRAFFIC_TUMBLE_RATE		26000	// angular impulse per unit/frame of speed
#define CD2_TRAFFIC_TUMBLE_MAX		0x500000	// ceiling on that impulse


static const char* const kPresetNames[] = { "Default", "Turbo", "Drifty", "Custom" };
static const char* const kTightInputNames[] = { "Handbrake", "Wheelspin", "Off" };

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
void cd2TrafficTumble(CAR_DATA* cp)
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

void cd2RespawnTick(CAR_DATA* cp)
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

			// wrecked cars are lighter until they come back, and they go out with
			// the casino bang
			cd2MassQuarter(cp);
			cd2DeathBang(cp);

			printInfo("[combatd2] respawn: car=%d DESTROYED (type=%d) - control stripped, mass now %d, returning in %d frames\n",
				cp->id, cp->controlType, (cp->ap.carCos != NULL) ? cp->ap.carCos->mass : -1, r->timer);
		}

		// A wreck must not keep its throttle or spin its wheels while it burns
		// down. The stock pad path re-applies these every frame (the damaged-car
		// fallback selects the handbrake, whose branch never clears wheelspin,
		// so a car that was mid-burnout when it died would keep its wheels
		// spinning), so strip the controls on EVERY waiting frame, not once.
		cp->thrust = 0;
		cp->handbrake = 0;
		cp->wheelspin = 0;
		cp->wheel_angle = 0;
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
int cd2OnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	memset(gCd2Respawn, 0, sizeof(gCd2Respawn));
	memset(gCd2SceneryHits, 0, sizeof(gCd2SceneryHits));
	memset(gCd2TrafficLastHit, 0, sizeof(gCd2TrafficLastHit));
	memset(gCd2MassMod, 0, sizeof(gCd2MassMod));	// masses are level data, re-read on load

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
int cd2OnDamageScale(void* ud, void* args)
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
int cd2OnCarVsCar(void* ud, void* args)
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

	// defer: combatd2sim.c applies it on the next physics frame (after
	// unpausing) so the wreck + explosion don't trigger while the menu is up.
	cd2CarQueueTotal();

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
	cd2CrewRegister(ctx);
	cd2FxRegister(ctx);
	cd2FreezeRegister(ctx);
	cd2AiRegister(ctx);

	/* vehicle-availability hacks - self-contained, hosted here for now and
	 * intended to move to its own module (see carhacks/carhacks.h) */
	carhacks_register(ctx);

	/* TEMPORARY: scripted debug driver, active only when debug_script is set
	 * (delete cd2debug.c and these two lines when done) */
	cd2DebugRegister(ctx);

	ctx->jer_log(ctx, "[combatd2] registered (SDK v%d)\n", ctx->sdkVersion);
}
