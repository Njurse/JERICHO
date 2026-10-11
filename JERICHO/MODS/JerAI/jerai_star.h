#ifndef JERAI_STAR_H
#define JERAI_STAR_H

/* ------------------------------------------------------------------
 * jerai_star.h -- the tiny pathfinder: A* over the jerai_map grid.
 *
 * Lifted from mp/ai/aistar and namespaced (JerAi*). Unchanged in behavior:
 *
 *   - NO ALLOCATION. The node pool is a fixed array sized to the biggest
 *     grid the map can produce;
 *   - DETERMINISTIC. Ties break on node index;
 *   - BOUNDED. JERAI_STAR_EXPANSIONS caps the work; hitting the cap is NOT
 *     an error - the path comes back INCOMPLETE with the best partial
 *     route to the closest reachable point;
 *   - it reads the grid and nothing else. No engine calls, which is what
 *     lets the self-test build a grid by hand and check it in isolation.
 *
 * The path that comes out is SMOOTHED: the cell-by-cell stair is collapsed
 * into straight runs by line of sight. Waypoint 0 is always where the plan
 * STARTED.
 * ------------------------------------------------------------------ */

#include "jerai_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Big enough for any grid the map produces, so the pool is never a limit. */
#define JERAI_STAR_MAX_NODES	(JERAI_MAP_SIZE * JERAI_MAP_SIZE)

/* Work ceiling per plan: the grid itself. Every sample is expanded at most once. */
#define JERAI_STAR_EXPANSIONS	JERAI_STAR_MAX_NODES

/* Straight and diagonal step costs, in the 10/14 integer scheme. */
#define JERAI_STAR_STEP_STRAIGHT	10
#define JERAI_STAR_STEP_DIAGONAL	14

/* Waypoints a plan may return. Beyond this the tail is dropped rather than the
 * path being rejected (the head is what matters). */
#define JERAI_STAR_MAX_WAYPOINTS	24

typedef struct JERAI_PATH
{
	int	waypoints;			/* how many are in wx[]/wz[] */
	int	wx[JERAI_STAR_MAX_WAYPOINTS];	/* world units */
	int	wz[JERAI_STAR_MAX_WAYPOINTS];
	int	complete;			/* 1 = the goal was reached */
	int	expanded;			/* nodes expanded, for the log */
	int	cost;				/* total path cost, 0 when there is none */
	int	onRoad;				/* waypoints that sit on the road network */
	int	goalX, goalZ;			/* what was actually reached */
} JERAI_PATH;

/* Plan from a world position to another over `map`.
 *
 * Returns 1 when the goal was reached, 0 when it was not - and in BOTH cases
 * path->waypoints / wx[] / wz[] hold the route (the best partial one when the
 * goal could not be reached). Returns 0 with waypoints == 0 only when there is
 * nowhere to go from here at all.
 *
 * The start sample being BLOCKED is handled rather than refused: the planner
 * walks out to the nearest open sample instead of declaring the car trapped. */
int JerAiStarPlan(const JERAI_MAP* map, int fromX, int fromZ, int toX, int toZ, JERAI_PATH* path);

/* Would a car get from one world point to another in a straight line? */
int JerAiStarLineClear(const JERAI_MAP* map, int fromX, int fromZ, int toX, int toZ);

#ifdef __cplusplus
}
#endif

#endif /* JERAI_STAR_H */
