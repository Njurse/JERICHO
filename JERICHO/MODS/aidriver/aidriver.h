/*
 * aidriver.h — public contract of the aidriver module.
 *
 * The module reads the player's AI mode from the game-global
 * g_PlayerControlMode (overlay.h), which the sandbox menu already writes
 * when the user cycles Player AI Mode (0 = manual, 1 = Traffic, 2 = Cop,
 * 3 = Lead). No cross-module function calls are needed — the module
 * observes the game state and adjusts the AI-driven player car.
 *
 * Tunables are stored with the JERICHO config API under the "aidriver"
 * namespace (JERICHO/CONFIG/aidriver.ini) and are re-read live, so the
 * sandbox menu (or any module) can adjust them with
 * jer_config_set_int("aidriver", KEY, value).
 */

#ifndef AIDRIVER_H
#define AIDRIVER_H

/* master switch: 0 disables all behavior */
#define AIDRIVER_CFG_ENABLED        "enabled"

/* joyride: pace above the road's traffic limit, in percent (25 = +25%) */
#define AIDRIVER_CFG_PACE_PCT       "pace_boost_pct"

/* evade: civ-AI speed cap (u_char maxSpeed, 0..255; 255 = as fast as the
 * car can physically go) */
#define AIDRIVER_CFG_TOP_SPEED      "top_speed"

/* free-flowing: 1 = never park, never stop at lights/yields (hard
 * obstacles still brake); 0 = full traffic obedience */
#define AIDRIVER_CFG_RUN_LIGHTS     "run_lights"

/* pursuit tailing: hold this distance (world units) behind the mission
 * chase target (Mission.ChaseTarget) */
#define AIDRIVER_CFG_PURSUIT_DIST   "pursuit_dist"

/* pursuit tailing: deadband around pursuit_dist before the AI brakes or
 * accelerates */
#define AIDRIVER_CFG_PURSUIT_MARGIN "pursuit_margin"

/* evade trigger: a cop closer than this (world units) counts as a threat
 * even below pursuit level (getaway / take-a-ride) */
#define AIDRIVER_CFG_COP_CLEARANCE  "cop_clearance"

/* pursuit tailing: steering lock (game units, civ maxSteer = 512) */
#define AIDRIVER_CFG_MAX_STEER      "max_steer"

#endif /* AIDRIVER_H */
