/*
 * jerai_viz.c -- the in-game debug drawing. See jerai_internal.h.
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

#include "jerai_internal.h"
#include "jerai_drive.h"
#include "jerai_behavior.h"

#include "jer_events.h"

/* the engine's depth-sorted world line (DebugOverlay.cpp, always linked) */
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);

/* how many lines this module issued this frame, for the readout */
static int sLines;

static void JerAiSeg(int x0, int y0, int z0, int x1, int y1, int z1, CVECTOR& col)
{
	VECTOR a, b;

	a.vx = x0; a.vy = y0; a.vz = z0;
	b.vx = x1; b.vy = y1; b.vz = z1;

	Debug_AddLineDepth(a, b, col);
	sLines++;
}

static void JerAiCross(int x, int y, int z, int r, CVECTOR& col)
{
	JerAiSeg(x - r, y, z, x + r, y, z, col);
	JerAiSeg(x, y, z - r, x, y, z + r, col);
}

static long JerAiDist2(int ax, int az, int bx, int bz)
{
	long dx = ax - bx;
	long dz = az - bz;

	return dx * dx + dz * dz;
}

/* ---- the engine road graph ---------------------------------------- */

static void JerAiDrawRoad(CAR_DATA* car)
{
	CVECTOR cEdge = { 40, 160, 255 };
	CVECTOR cKind[3] = { { 255, 200, 40 }, { 255, 120, 40 }, { 140, 255, 140 } };
	int cx = car->hd.where.t[0];
	int cz = car->hd.where.t[2];
	int i;
	const long edgeR2 = 13000L * 13000L;
	const long nodeR2 = 7000L * 7000L;

	for (i = 0; i < gJerAi.road.edgeCount; i++)
	{
		const JERAI_ROAD_NODE* a = &gJerAi.road.nodes[gJerAi.road.edges[i].a];
		const JERAI_ROAD_NODE* b = &gJerAi.road.nodes[gJerAi.road.edges[i].b];

		if (JerAiDist2((a->x + b->x) / 2, (a->z + b->z) / 2, cx, cz) > edgeR2)
			continue;

		JerAiSeg(a->x, a->y + 60, a->z, b->x, b->y + 60, b->z, cEdge);
	}

	for (i = 0; i < gJerAi.road.nodeCount; i++)
	{
		const JERAI_ROAD_NODE* n = &gJerAi.road.nodes[i];
		int k = (n->kind < 0) ? 0 : (n->kind > 2 ? 2 : n->kind);

		if (JerAiDist2(n->x, n->z, cx, cz) > nodeR2)
			continue;

		JerAiCross(n->x, n->y + 60, n->z, 120, cKind[k]);
	}
}

/* ---- the occupancy grid ------------------------------------------- */

static void JerAiDrawGrid(CAR_DATA* car)
{
	CVECTOR cWall = { 255, 40, 40 };
	CVECTOR cRoad = { 40, 200, 40 };
	int y = gJerAi.map.centreY + 40;
	int cx = car->hd.where.t[0];
	int cz = car->hd.where.t[2];
	const long r2 = 6000L * 6000L;
	int ix, iz;

	/* stride 2: a sample every ~1024 units is plenty to read a wall line */
	for (iz = 0; iz < JERAI_MAP_SIZE; iz += 2)
	{
		for (ix = 0; ix < JERAI_MAP_SIZE; ix += 2)
		{
			int wx, wz;

			if (JerAiMapBlocked(&gJerAi.map, ix, iz))
			{
				JerAiMapWorld(&gJerAi.map, ix, iz, &wx, &wz);

				if (JerAiDist2(wx, wz, cx, cz) > r2)
					continue;

				JerAiCross(wx, y, wz, 90, cWall);
			}
			else if (JerAiMapRoad(&gJerAi.map, ix, iz))
			{
				JerAiMapWorld(&gJerAi.map, ix, iz, &wx, &wz);

				if (JerAiDist2(wx, wz, cx, cz) > r2)
					continue;

				JerAiCross(wx, y, wz, 40, cRoad);
			}
		}
	}
}

/* ---- the current plan --------------------------------------------- */

static void JerAiDrawPath(CAR_DATA* car)
{
	CVECTOR cRoute = { 255, 60, 60 };
	CVECTOR cGoal = { 255, 255, 60 };
	CVECTOR cAim = { 60, 255, 60 };
	int y = car->hd.where.t[1] + 60;
	int k;

	for (k = 1; k < gJerAi.path.waypoints; k++)
	{
		JerAiSeg(gJerAi.path.wx[k - 1], y, gJerAi.path.wz[k - 1],
			 gJerAi.path.wx[k], y, gJerAi.path.wz[k], cRoute);
	}

	/* the goal as a ground cross, and the line to where we are aiming */
	JerAiCross(gJerAi.goalX, gJerAi.map.centreY + 60, gJerAi.goalZ, 500, cGoal);

	JerAiSeg(car->hd.where.t[0], y, car->hd.where.t[2],
		 gJerAi.aimX, y, gJerAi.aimZ, cAim);
}

/* ---- the hook ------------------------------------------------------ */

int JerAiOnDrawWorld(void* userdata, void* args)
{
	CAR_DATA* car;

	(void)userdata;
	(void)args;

	sLines = 0;

	if (!gJerAi.enabled)
		return JER_RESULT_CONTINUE;

	car = JerAiOwnCar();

	if (car == NULL)
		return JER_RESULT_CONTINUE;

	if (gJerAi.vizRoad && gJerAi.roadBuilt)
		JerAiDrawRoad(car);

	if (gJerAi.vizGrid && JerAiMapValid(&gJerAi.map))
		JerAiDrawGrid(car);

	if (gJerAi.vizPath && gJerAi.havePlan)
		JerAiDrawPath(car);

	return JER_RESULT_CONTINUE;
}

int JerAiVizLines(void)
{
	return sLines;
}
