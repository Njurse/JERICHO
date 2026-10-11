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

/* the corner-speed model: a lateral-g budget (bigger = the car carries more
 * speed through the same curve), the braking authority used for the
 * braking-distance term, and how far ahead the plan is scanned for corners */
#define JERAI_CFG_CORNER_G          "corner_g"
#define JERAI_CFG_BRAKE_GAIN        "brake_gain"
#define JERAI_CFG_CORNER_LOOKAHEAD  "corner_lookahead"

/* overall aggression: the corner target speed as a percentage (100 = as the
 * grip model computes it, lower = more cautious) */
#define JERAI_CFG_SPEED_SCALE       "speed_scale"

/* A* route shaping: extra cost for changing direction between grid samples, so
 * the planner prefers straight roads over a wiggly staircase (0 = off) */
#define JERAI_CFG_TURN_COST         "turn_cost"

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

/* while JerAI is driving: make the player's car immune (no damage totals it,
 * via JER_EVENT_GET_BUDDHA) and keep the wanted level at zero, so the car is not
 * destroyed and no cop sirens chase the test drive (1 = on). */
#define JERAI_CFG_IMMUNE            "immune"

/* MULTIPLAYER: while a session is live (and this machine is NOT the host),
 * retarget the joyride behavior at the HOST's car, so the AI car tails the host
 * around the map and holds a clearance instead of driving into them. 0 keeps
 * the plain random wander (1 = on). See jerai_follow.c. */
#define JERAI_CFG_FOLLOW            "follow"

/* the clearance held from the followed car, in world units: within this the AI
 * car stops and idles rather than closing the last stretch into the host. */
#define JERAI_CFG_FOLLOW_CLEARANCE  "follow_clearance"

/* how far (world units) the followed car may get before the AI gives it up and
 * goes back to wandering. 0 (the default) = NO LIMIT: the chase is CHUNKED, not
 * truncated - the plan goal is clamped to the planner's window and re-planned as
 * the car closes, so an unlimited chase still crosses a whole city. */
#define JERAI_CFG_FOLLOW_RANGE      "follow_range"

/* TEST LEVER: a CAR_DATA index to follow with NO session at all (-1 = off), so
 * the approach + clearance + idle can be exercised in single player. */
#define JERAI_CFG_FOLLOW_CAR        "follow_car"

/* debug: override the aim with a FIXED heading (game units, -1 = off). Used to
 * settle the steering sign in isolation from a moving plan. */
#define JERAI_CFG_AIM_TEST          "aim_test"

/* How much the steering leads the heading error by the car's own yaw rate
 * (frames of yaw). It is the damper that stops the wheel sawing at the wheel and
 * the car oscillating up a straight - but too much of it and the wheel straightens
 * the moment the car starts turning, which reads as "it steers far too slowly". */
#define JERAI_CFG_YAW_LEAD          "yaw_lead"

#endif /* JERAI_H */
