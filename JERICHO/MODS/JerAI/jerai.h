/*
 * jerai.h — public contract of the JerAI module.
 *
 * JerAI drives the player's car with A* pathfinding over an engine occupancy
 * grid, plus a small set of modular behaviors (joyride/random-wander first).
 * It is a COMPILED-IN (deep) module: it reads the game's car/player globals,
 * the compiled-in road graph (JerRoadInfoAt) and draws 3D debug lines
 * (Debug_AddLineDepth) directly, like the mp module it borrows the AI search
 * code from.
 *
 * Tunables are stored with the JERICHO config API under the "JerAI"
 * namespace (JERICHO/CONFIG/JerAI.ini) and are re-read live, so the module's
 * own menu (or any other module) can adjust them with
 * jer_config_set_int("JerAI", KEY, value).
 */

#ifndef JERAI_H
#define JERAI_H

/* master switch: 0 disables all behavior */
#define JERAI_CFG_ENABLED           "enabled"

/* the active behavior (see jerai_behavior.h). Manual = 0. */
#define JERAI_CFG_MODE              "mode"

/* joyride: pace above the road's traffic limit, in percent (25 = +25%) */
#define JERAI_CFG_PACE_PCT          "pace_pct"

/* evade: civ-AI speed cap (u_char maxSpeed, 0..255; 255 = flat out) */
#define JERAI_CFG_TOP_SPEED         "top_speed"

/* free-flowing: 1 = never park, never stop at lights/yields */
#define JERAI_CFG_RUN_LIGHTS        "run_lights"

/* how often a route is refreshed, in ms */
#define JERAI_CFG_REPLAN_MS         "replan_ms"

/* world units a goal may drift before a replan */
#define JERAI_CFG_REPLAN_MOVE       "replan_move"

/* steer at the furthest waypoint at least this far out (world units) */
#define JERAI_CFG_LOOK_AHEAD        "look_ahead"

/* traffic/wall avoidance: start slowing for a car ahead within this range */
#define JERAI_CFG_AVOID_DIST        "avoid_dist"

/* evade trigger: a cop closer than this (world units) counts as a threat */
#define JERAI_CFG_COP_CLEARANCE     "cop_clearance"

/* steering lock used by direct-steer behaviors (game units, civ maxSteer = 512) */
#define JERAI_CFG_MAX_STEER         "max_steer"

/* visualization layers (0/1) */
#define JERAI_CFG_VIZ_GRID          "viz_grid"
#define JERAI_CFG_VIZ_ROAD          "viz_road"
#define JERAI_CFG_VIZ_PATH          "viz_path"
#define JERAI_CFG_VIZ_HUD           "viz_hud"

/* dump the road graph to a file the next chance the world is loaded (0/1);
 * the module clears it back to 0 after writing. */
#define JERAI_CFG_DUMP_NOW          "dump_now"

/* force the control menu open on the next config refresh (0/1), for a headless
 * capture; the module clears it back to 0. */
#define JERAI_CFG_MENU_OPEN         "menu_open"

#endif /* JERAI_H */
