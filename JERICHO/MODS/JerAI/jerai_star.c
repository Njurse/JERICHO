/*
 * jerai_star.c -- A* over the jerai_map grid. See jerai_star.h.
 *
 * Lifted from mp/ai/aistar.c and namespaced (JerAi*). Deliberately plain: a
 * fixed node pool indexed by sample, a linear scan for the best open node, and
 * an octile heuristic scaled to the cheapest possible step. No allocation, no
 * recursion, no engine calls - which is what lets the self-test drive it against
 * a hand-built grid.
 */

#include <string.h>

#include "jerai_map.h"
#include "jerai_star.h"

#define JERAI_STAR_OPEN		1
#define JERAI_STAR_CLOSED	2

typedef struct JERAI_STAR_NODE
{
	int	g;
	int	f;
	int	parent;
	unsigned char state;
} JERAI_STAR_NODE;

/* The pool is the whole grid: no node is ever visited twice, so nothing has to
 * grow. A local static rather than a struct field because a plan is a
 * short-lived, single-threaded operation and the game loop is single-threaded. */
static JERAI_STAR_NODE sNodes[JERAI_STAR_MAX_NODES];

static int JerAiStarIndex(int ix, int iz)
{
	return iz * JERAI_MAP_SIZE + ix;
}

/* Octile distance between two samples, in 10/14 units, times the cheapest
 * sample cost. Admissible: no move can cost less than the min sample cost per
 * step, so this can never overestimate and A* stays optimal within its budget. */
#define JERAI_STAR_COST_MIN	100	/* must match the base cost in JerAiMapCost */

static int JerAiStarHeuristic(int ax, int az, int bx, int bz)
{
	int dx = (ax > bx) ? ax - bx : bx - ax;
	int dz = (az > bz) ? az - bz : bz - az;
	int diag = (dx < dz) ? dx : dz;
	int straight = ((dx > dz) ? dx : dz) - diag;

	return (straight * JERAI_STAR_STEP_STRAIGHT + diag * JERAI_STAR_STEP_DIAGONAL) * JERAI_STAR_COST_MIN / 10;
}

/* Is the midpoint of a diagonal step blocked? This is the corner-cut rule: a
 * car that may not clip a wall corner must not be routed through one. */
static int JerAiStarCornerBlocked(const JERAI_MAP* map, int ix, int iz, int dx, int dz)
{
	if (dx == 0 || dz == 0)
		return 0;

	return JerAiMapBlocked(map, ix + dx, iz) || JerAiMapBlocked(map, ix, iz + dz);
}

int JerAiStarLineClear(const JERAI_MAP* map, int fromX, int fromZ, int toX, int toZ)
{
	int dx, dz, span, k;
	int ix, iz;

	if (!JerAiMapValid(map))
		return 0;

	/* both ends have to be inside the window: off it we know nothing, and "I do
	 * not know" must never be reported as clear */
	if (!JerAiMapSampleIndex(map, fromX, fromZ, &ix, &iz))
		return 0;

	if (!JerAiMapSampleIndex(map, toX, toZ, &ix, &iz))
		return 0;

	dx = toX - fromX;
	dz = toZ - fromZ;

	if (dx == 0 && dz == 0)
		return !JerAiMapBlocked(map, ix, iz);

	/* sampled at half a grid step, so a thin obstacle lying across the line cannot
	 * slip between two tests */
	{
		int adx = (dx < 0) ? -dx : dx;
		int adz = (dz < 0) ? -dz : dz;

		span = ((adx > adz) ? adx : adz) / (JERAI_MAP_STEP / 2);
	}

	if (span < 1)
		span = 1;

	for (k = 0; k <= span; k++)
	{
		int x = fromX + (int)(((long)dx * k) / span);
		int z = fromZ + (int)(((long)dz * k) / span);

		if (!JerAiMapSampleIndex(map, x, z, &ix, &iz))
			return 0;

		if (JerAiMapBlocked(map, ix, iz))
			return 0;
	}

	return 1;
}

/* Reconstruct from a node back to the start, then smooth: keep a waypoint only
 * where the straight run to the following kept one stops being clear. */
static int JerAiStarBuildPath(const JERAI_MAP* map, int goalNode, JERAI_PATH* path)
{
	int chain[JERAI_STAR_MAX_NODES];
	int count = 0;
	int node = goalNode;
	int i, kept;

	/* walk the parents to the start */
	while (node >= 0 && count < JERAI_STAR_MAX_NODES)
	{
		chain[count++] = node;

		if (sNodes[node].parent == node)
			break;

		node = sNodes[node].parent;
	}

	if (count == 0)
		return 0;

	/* Waypoint 0 is where we ARE, so a route reads from here to the goal. After that the
	 * chain is walked with a jump as far as line of sight allows - that IS the smoothing:
	 * a car steers at the next turning point, not at every grid step. */
	{
		int wx, wz;

		JerAiMapWorld(map, chain[count - 1] % JERAI_MAP_SIZE, chain[count - 1] / JERAI_MAP_SIZE, &wx, &wz);

		path->wx[0] = wx;
		path->wz[0] = wz;

		if (JerAiMapRoad(map, chain[count - 1] % JERAI_MAP_SIZE, chain[count - 1] / JERAI_MAP_SIZE))
			path->onRoad++;

		kept = 1;
	}

	i = count - 1;	/* the start is already emitted */

	while (i > 0 && kept < JERAI_STAR_MAX_WAYPOINTS)
	{
		int ix = chain[i] % JERAI_MAP_SIZE;
		int iz = chain[i] / JERAI_MAP_SIZE;
		int j, best = i - 1;	/* at worst the next node along: always a legal move */

		for (j = i - 1; j >= 0; j--)
		{
			int nx = chain[j] % JERAI_MAP_SIZE;
			int nz = chain[j] / JERAI_MAP_SIZE;
			int ax, az, bx, bz;

			JerAiMapWorld(map, ix, iz, &ax, &az);
			JerAiMapWorld(map, nx, nz, &bx, &bz);

			if (!JerAiStarLineClear(map, ax, az, bx, bz))
				break;

			best = j;
		}

		{
			int wx, wz;

			JerAiMapWorld(map, chain[best] % JERAI_MAP_SIZE, chain[best] / JERAI_MAP_SIZE, &wx, &wz);

			path->wx[kept] = wx;
			path->wz[kept] = wz;

			if (JerAiMapRoad(map, chain[best] % JERAI_MAP_SIZE, chain[best] / JERAI_MAP_SIZE))
				path->onRoad++;

			kept++;
		}

		i = best;
	}

	path->waypoints = kept;
	path->goalX = path->wx[kept > 0 ? kept - 1 : 0];
	path->goalZ = path->wz[kept > 0 ? kept - 1 : 0];

	return kept > 0;
}

int JerAiStarPlan(const JERAI_MAP* map, int fromX, int fromZ, int toX, int toZ, JERAI_PATH* path)
{
	int six, siz, gix, giz;
	int startNode, goalNode;
	int expanded = 0;
	int bestPartial = -1, bestPartialH = 0;
	int found = 0;
	int i;

	if (path == NULL)
		return 0;

	path->waypoints = 0;
	path->complete = 0;
	path->expanded = 0;
	path->cost = 0;
	path->onRoad = 0;
	path->goalX = fromX;
	path->goalZ = fromZ;

	if (!JerAiMapValid(map))
		return 0;

	if (!JerAiMapSampleIndex(map, fromX, fromZ, &six, &siz))
		return 0;

	if (!JerAiMapSampleIndex(map, toX, toZ, &gix, &giz))
		return 0;

	if (!JerAiMapNearestOpen(map, six, siz, &six, &siz))
		return 0;	/* walled in: genuinely nowhere to go */

	if (JerAiMapBlocked(map, gix, giz))
		JerAiMapNearestOpen(map, gix, giz, &gix, &giz);

	startNode = JerAiStarIndex(six, siz);
	goalNode = JerAiStarIndex(gix, giz);

	for (i = 0; i < JERAI_STAR_MAX_NODES; i++)
	{
		sNodes[i].g = 0;
		sNodes[i].f = 0;
		sNodes[i].state = 0;
		sNodes[i].parent = -1;
	}

	sNodes[startNode].parent = startNode;	/* its own parent marks the start */
	sNodes[startNode].g = 0;
	sNodes[startNode].f = JerAiStarHeuristic(six, siz, gix, giz);
	sNodes[startNode].state = JERAI_STAR_OPEN;

	bestPartial = startNode;
	bestPartialH = sNodes[startNode].f;

	while (expanded < JERAI_STAR_EXPANSIONS)
	{
		int current = -1;
		int bestF = 0;
		int cx, cz, dx, dz;

		/* the best open node. A linear scan: the pool is ~2400 entries, so a heap
		 * would be more code for no measurable gain, and a scan on the node index
		 * breaks ties the same way every time. */
		for (i = 0; i < JERAI_STAR_MAX_NODES; i++)
		{
			if (sNodes[i].state != JERAI_STAR_OPEN)
				continue;

			if (current < 0 || sNodes[i].f < bestF)
			{
				current = i;
				bestF = sNodes[i].f;
			}
		}

		if (current < 0)
			break;	/* nothing left to expand */

		if (current == goalNode)
		{
			found = 1;
			break;
		}

		sNodes[current].state = JERAI_STAR_CLOSED;
		expanded++;

		cx = current % JERAI_MAP_SIZE;
		cz = current / JERAI_MAP_SIZE;

		/* remember the closest we ever got, for the incomplete case */
		{
			int h = JerAiStarHeuristic(cx, cz, gix, giz);

			if (h < bestPartialH)
			{
				bestPartialH = h;
				bestPartial = current;
			}
		}

		for (dz = -1; dz <= 1; dz++)
		{
			for (dx = -1; dx <= 1; dx++)
			{
				int nx, nz, n, step, g;

				if (dx == 0 && dz == 0)
					continue;

				nx = cx + dx;
				nz = cz + dz;

				if (nx < 0 || nz < 0 || nx >= JERAI_MAP_SIZE || nz >= JERAI_MAP_SIZE)
					continue;

				n = JerAiStarIndex(nx, nz);

				if (sNodes[n].state == JERAI_STAR_CLOSED)
					continue;

				if (JerAiMapBlocked(map, nx, nz))
					continue;

				if (JerAiStarCornerBlocked(map, cx, cz, dx, dz))
					continue;

				step = (dx != 0 && dz != 0) ? JERAI_STAR_STEP_DIAGONAL : JERAI_STAR_STEP_STRAIGHT;

				/* the step costs both ends, so a route pays for entering a bad
				 * sample as well as leaving one */
				g = sNodes[current].g +
					step * (JerAiMapCost(map, cx, cz) + JerAiMapCost(map, nx, nz)) / 20;

				/* turn penalty: a route that changes direction between samples is a
				 * staircase through the grid, i.e. a wiggly drive. Charging for the
				 * turn makes the planner prefer straight, fast roads even when the
				 * wiggly route is otherwise marginally shorter. */
				if (map->turnCost > 0 && sNodes[current].parent >= 0)
				{
					int px = sNodes[current].parent % JERAI_MAP_SIZE;
					int pz = sNodes[current].parent / JERAI_MAP_SIZE;

					if (cx - px != dx || cz - pz != dz)
						g += map->turnCost;
				}

				if (sNodes[n].state == JERAI_STAR_OPEN && g >= sNodes[n].g)
					continue;

				sNodes[n].parent = current;
				sNodes[n].g = g;
				sNodes[n].f = g + JerAiStarHeuristic(nx, nz, gix, giz);
				sNodes[n].state = JERAI_STAR_OPEN;
			}
		}
	}

	path->expanded = expanded;
	path->complete = found;

	if (found)
	{
		if (!JerAiStarBuildPath(map, goalNode, path))
			return 0;

		path->cost = sNodes[goalNode].g;
		return 1;
	}

	/* Out of budget, or the goal is walled off. Either way the useful answer is
	 * the closest point we did reach: a car that needs to be somewhere else
	 * drives there and re-plans. */
	if (bestPartial >= 0 && bestPartial != startNode)
	{
		if (JerAiStarBuildPath(map, bestPartial, path))
		{
			path->cost = sNodes[bestPartial].g;
			return 0;
		}
	}

	return 0;
}
