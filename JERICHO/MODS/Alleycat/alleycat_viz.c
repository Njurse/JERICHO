/*
 * alleycat_viz.c -- the in-game debug drawing. See alleycat_internal.h.
 *
 * Everything is drawn with the engine's depth-sorted world line
 * (Debug_AddLineDepth, DebugOverlay.cpp): buildings occlude the lines, so the
 * graph reads as being IN the world rather than pasted over it. The engine also
 * caps the buffer at 512 lines and culls anything beyond ~15000 units from the
 * camera, so this file can draw the whole road graph and let the engine decide
 * what is visible - but it still distance-gates the dense layers so the cap is
 * spent on what is actually near the car.
 *
 * Layers, each its own toggle:
 *   viz_road  the engine road graph: nodes (crosses, coloured by kind) + edges
 *   viz_grid  the occupancy grid: blocked samples (walls) and road samples
 *   viz_path  the current A* plan, the goal cross, and the aim line
 *   viz_hud   a one-line text readout (drawn separately, in the frame hook)
 *
 * Hooked at JER_EVENT_DRAW_WORLD, which fires right before the engine flushes
 * its overlay line buffer.
 */

#include "alleycat_internal.h"
#include "alleycat_drive.h"
#include "alleycat_behavior.h"

#include "jer_events.h"

/* the engine's depth-sorted world line (DebugOverlay.cpp, always linked) */
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);

/* how many lines this module issued this frame, for the readout */
static int sLines;

static void AlleycatSeg(int x0, int y0, int z0, int x1, int y1, int z1, CVECTOR& col)
{
	VECTOR a, b;

	a.vx = x0; a.vy = y0; a.vz = z0;
	b.vx = x1; b.vy = y1; b.vz = z1;

	Debug_AddLineDepth(a, b, col);
	sLines++;
}

static void AlleycatCross(int x, int y, int z, int r, CVECTOR& col)
{
	AlleycatSeg(x - r, y, z, x + r, y, z, col);
	AlleycatSeg(x, y, z - r, x, y, z + r, col);
}

/* distances are AlleycatDist2 (alleycat_mapgrid.c): 64-bit, because a 32-bit `long`
 * (Windows) wrapped on a big map and let far edges past the gates below, where
 * they ate the engine's 512-line cap */

/* ---- the engine road graph ---------------------------------------- */

static void AlleycatDrawRoad(CAR_DATA* car)
{
	CVECTOR cEdge = { 40, 160, 255 };
	CVECTOR cKind[3] = { { 255, 200, 40 }, { 255, 120, 40 }, { 140, 255, 140 } };
	int cx = car->hd.where.t[0];
	int cz = car->hd.where.t[2];
	int i;
	const long long edgeR2 = 13000LL * 13000LL;
	const long long nodeR2 = 7000LL * 7000LL;

	for (i = 0; i < gAlleycat.road.edgeCount; i++)
	{
		const ALLEYCAT_ROAD_NODE* a = &gAlleycat.road.nodes[gAlleycat.road.edges[i].a];
		const ALLEYCAT_ROAD_NODE* b = &gAlleycat.road.nodes[gAlleycat.road.edges[i].b];

		if (AlleycatDist2((a->x + b->x) / 2, (a->z + b->z) / 2, cx, cz) > edgeR2)
			continue;

		AlleycatSeg(a->x, a->y + 60, a->z, b->x, b->y + 60, b->z, cEdge);
	}

	for (i = 0; i < gAlleycat.road.nodeCount; i++)
	{
		const ALLEYCAT_ROAD_NODE* n = &gAlleycat.road.nodes[i];
		int k = (n->kind < 0) ? 0 : (n->kind > 2 ? 2 : n->kind);

		if (AlleycatDist2(n->x, n->z, cx, cz) > nodeR2)
			continue;

		AlleycatCross(n->x, n->y + 60, n->z, 120, cKind[k]);
	}
}

/* ---- the occupancy grid ------------------------------------------- */

static void AlleycatDrawGrid(CAR_DATA* car)
{
	CVECTOR cWall = { 255, 40, 40 };
	CVECTOR cRoad = { 40, 200, 40 };
	int y = gAlleycat.map.centreY + 40;
	int cx = car->hd.where.t[0];
	int cz = car->hd.where.t[2];
	const long long r2 = 6000LL * 6000LL;
	int ix, iz;

	/* stride 2: a sample every ~1024 units is plenty to read a wall line */
	for (iz = 0; iz < ALLEYCAT_MAP_SIZE; iz += 2)
	{
		for (ix = 0; ix < ALLEYCAT_MAP_SIZE; ix += 2)
		{
			int wx, wz;

			if (AlleycatMapBlocked(&gAlleycat.map, ix, iz))
			{
				AlleycatMapWorld(&gAlleycat.map, ix, iz, &wx, &wz);

				if (AlleycatDist2(wx, wz, cx, cz) > r2)
					continue;

				AlleycatCross(wx, y, wz, 90, cWall);
			}
			else if (AlleycatMapRoad(&gAlleycat.map, ix, iz))
			{
				AlleycatMapWorld(&gAlleycat.map, ix, iz, &wx, &wz);

				if (AlleycatDist2(wx, wz, cx, cz) > r2)
					continue;

				AlleycatCross(wx, y, wz, 40, cRoad);
			}
		}
	}
}

/* ---- the current plan --------------------------------------------- */

static void AlleycatDrawPath(CAR_DATA* car)
{
	CVECTOR cRoute = { 255, 60, 60 };
	CVECTOR cGoal = { 255, 255, 60 };
	CVECTOR cAim = { 60, 255, 60 };
	int y = car->hd.where.t[1] + 60;
	int k;

	for (k = 1; k < gAlleycat.path.waypoints; k++)
	{
		AlleycatSeg(gAlleycat.path.wx[k - 1], y, gAlleycat.path.wz[k - 1],
			 gAlleycat.path.wx[k], y, gAlleycat.path.wz[k], cRoute);
	}

	/* the goal as a ground cross, and the line to where we are aiming */
	AlleycatCross(gAlleycat.goalX, gAlleycat.map.centreY + 60, gAlleycat.goalZ, 500, cGoal);

	AlleycatSeg(car->hd.where.t[0], y, car->hd.where.t[2],
		 gAlleycat.aimX, y, gAlleycat.aimZ, cAim);
}

/* ---- the hook ------------------------------------------------------ */

int AlleycatOnDrawWorld(void* userdata, void* args)
{
	CAR_DATA* car;

	(void)userdata;
	(void)args;

	sLines = 0;

	if (!gAlleycat.enabled)
		return JER_RESULT_CONTINUE;

	car = AlleycatOwnCar();

	if (car == NULL)
		return JER_RESULT_CONTINUE;

	if (gAlleycat.vizRoad && gAlleycat.roadBuilt)
		AlleycatDrawRoad(car);

	if (gAlleycat.vizGrid && AlleycatMapValid(&gAlleycat.map))
		AlleycatDrawGrid(car);

	if (gAlleycat.vizPath && gAlleycat.havePlan)
		AlleycatDrawPath(car);

	return JER_RESULT_CONTINUE;
}

int AlleycatVizLines(void)
{
	return sLines;
}
