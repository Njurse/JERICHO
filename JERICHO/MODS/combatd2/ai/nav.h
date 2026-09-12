// ai/nav.h — Combat D2 navigation layer (module-owned, replaces the greedy
// road-walking the stock civ/lead AI does).
//
// Phase 1 (this file): a graph over the level's Driver2 road segments
// (straights / curves / junctions) with an A* router and a per-car route cache.
// Later phases add a scenery (off-road) grid A* and a shared pursuit flow
// field, and the router arbitrates between them.
//
// The graph is rebuilt per level; node positions come straight from the
// segment midpoints (junctions derive theirs from their neighbours).

#ifndef CD2_NAV_H
#define CD2_NAV_H

#include "driver2.h"

#define CD2_NAV_MAX_ROUTE	64	// waypoints in one route
#define CD2_NAV_WP_STEP		512	// max spacing between route waypoints (world units)
#define CD2_NAV_MAX_NODES	4096	// graph node capacity
#define CD2_NAV_MAX_CARS	32	// route-cache slots (indexed by car id)
#define CD2_NAV_DRAW_RADIUS	4500	// nav_debug: draw nodes/edges within this of the player

// Where a route came from (also drawn by the nav debug overlay).
enum
{
	CD2_NAV_SRC_NONE = 0,
	CD2_NAV_SRC_ROAD,	// A* over the road graph
	CD2_NAV_SRC_SCENERY,	// A* over the off-road drivability grid
	CD2_NAV_SRC_FLOW	// shared pursuit flow field
};

typedef struct CD2_NAV_ROUTE
{
	int count;		// number of waypoints (0 = none)
	int source;		// CD2_NAV_SRC_*
	int goalNode;		// road node index of the goal (-1 if not on the graph)
	int expanded;		// nodes expanded by the last search
	int length;		// total route length, world units
	VECTOR wp[CD2_NAV_MAX_ROUTE];
} CD2_NAV_ROUTE;

// Build (lazily) / drop the graph. Reset on level start.
int  cd2NavReady(void);
void cd2NavReset(void);

int  cd2NavNodeCount(void);
int  cd2NavEdgeCount(void);

// Nearest graph node to a position: exact road surface first, else the closest
// node within maxDist (-1 when nothing is close enough).
int  cd2NavNodeAt(const VECTOR* pos);
int  cd2NavNearestRoadNode(const VECTOR* pos, int maxDist);
int  cd2NavNodePos(int node, VECTOR* out);

// Route from `from` toward `goal` (road graph A*). Cached per car: re-planned
// only when the goal node changes or the goal has moved more than a threshold.
// Returns out->count (0 = no route).
int  cd2NavRoute(int carId, const VECTOR* from, const VECTOR* goal, CD2_NAV_ROUTE* out);

// Last route computed for a car (for the debug overlay), or NULL.
const CD2_NAV_ROUTE* cd2NavLastRoute(int carId);

// nav_debug: draw the graph (nodes as vertical ticks, edges as lines, colour by
// road type) around `centre` within `radius` world units (0 = everything).
void cd2NavDraw(const VECTOR* centre, int radius);

#endif /* CD2_NAV_H */
