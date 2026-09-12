// ai/nav.c — Combat D2 navigation layer. See ai/nav.h.
//
// The graph is the level's Driver2 road segments in one node index space:
//   [0 .. NumStraights)                                  straights
//   [NumStraights .. +NumCurves)                         curves
//   [+NumCurves .. +NumJunctions)                        junctions
// Edges come from each segment's ConnectIdx/ExitIdx (via GetSurfaceRoadInfo),
// made undirected so a route is traversable either way. Node positions are the
// segment midpoints; junctions, which carry no midpoint, take the average of
// their neighbours.

#include "driver2.h"
#include "dr2roads.h"
#include "dr2math.h"
#include "system.h"
#include "combatd2.h"
#include "ai/nav.h"
#include "ai/grid.h"

#include <string.h>

#define CD2_NAV_EDGE_NONE	(-1)

typedef struct CD2_NAV_NODE
{
	int   surfId;
	int   type;		// 0 straight, 1 curve, 2 junction
	int   hasPos;		// 0 until a position is known (junctions)
	int   x, z;
	int   y;		// world height (MapHeight at build time)
	short adj[4];		// undirected neighbour node indices
} CD2_NAV_NODE;

static CD2_NAV_NODE sNodes[CD2_NAV_MAX_NODES];
static int  sNodeCount;
static int  sEdgeCount;
static int  sBuilt;

// A* scratch (static: no per-frame allocation)
static int   sG[CD2_NAV_MAX_NODES];
static int   sF[CD2_NAV_MAX_NODES];
static short sParent[CD2_NAV_MAX_NODES];
static char  sClosed[CD2_NAV_MAX_NODES];
static int   sOpen[CD2_NAV_MAX_NODES * 4];	// slack for duplicate heap pushes
static int   sOpenSize;

typedef struct CD2_NAV_CACHE
{
	int  valid;
	int  goalNode;
	int  goalX, goalZ;
	int  count;
	int  source;
	int  expanded;
	VECTOR wp[CD2_NAV_MAX_ROUTE];
} CD2_NAV_CACHE;

static CD2_NAV_CACHE sCache[CD2_NAV_MAX_CARS];
static CD2_NAV_ROUTE sLastRoute[CD2_NAV_MAX_CARS];
static int           sLastValid[CD2_NAV_MAX_CARS];

static int cd2NavIsqrt(int v)
{
	int r = 0;
	int bit = 1 << 30;

	if (v <= 0)
		return 0;

	while (bit > v)
		bit >>= 2;

	while (bit != 0)
	{
		if (v >= r + bit)
		{
			v -= r + bit;
			r = (r >> 1) + bit;
		}
		else
			r >>= 1;

		bit >>= 2;
	}

	return r;
}

// ---------------------------------------------------------------------------
// Densify: insert intermediate waypoints so no leg is longer than
// CD2_NAV_WP_STEP. A road route is only the A* segment midpoints, which can be
// thousands of units apart; steering at such a midpoint makes the car beeline
// across the corner and into whatever is on it. A dense polyline is what lets
// pure-pursuit steering actually follow the road.
// ---------------------------------------------------------------------------
static int cd2NavDist2D(int ax, int az, int bx, int bz);	// defined below

static void cd2NavDensify(CD2_NAV_ROUTE* out)
{
	VECTOR tmp[CD2_NAV_MAX_ROUTE];
	int i, n = 0;

	if (out->count <= 1)
		return;

	for (i = 0; i < out->count - 1; i++)
	{
		int ax = out->wp[i].vx, ay = out->wp[i].vy, az = out->wp[i].vz;
		int bx = out->wp[i + 1].vx, by = out->wp[i + 1].vy, bz = out->wp[i + 1].vz;
		int dx = bx - ax, dz = bz - az;
		int dist = cd2NavDist2D(ax, az, bx, bz);
		int steps = dist / CD2_NAV_WP_STEP;
		int s;

		if (n < CD2_NAV_MAX_ROUTE)
			tmp[n++] = out->wp[i];

		for (s = 1; s < steps && n < CD2_NAV_MAX_ROUTE - 1; s++)
		{
			tmp[n].vx = ax + (int)(((long long)dx * s) / steps);
			tmp[n].vy = ay + (int)(((long long)(by - ay) * s) / steps);
			tmp[n].vz = az + (int)(((long long)dz * s) / steps);
			n++;
		}

		if (n >= CD2_NAV_MAX_ROUTE - 1)
			break;
	}

	if (n < CD2_NAV_MAX_ROUTE)
		tmp[n++] = out->wp[out->count - 1];

	for (i = 0; i < n; i++)
		out->wp[i] = tmp[i];

	out->count = n;
}

static int cd2NavDist2D(int ax, int az, int bx, int bz)
{
	int dx = bx - ax;
	int dz = bz - az;

	return cd2NavIsqrt(dx * dx + dz * dz);
}

// surfId (as returned by GetSurfaceIndex/ConnectIdx) -> node index.
static int cd2NavSurfToNode(int surfId)
{
	if (surfId < 0)
		return -1;

	if (IS_STRAIGHT_SURFACE(surfId))
		return surfId & 0x1FFF;

	if (IS_CURVED_SURFACE(surfId))
		return NumDriver2Straights + (surfId & 0x1FFF);

	if (IS_JUNCTION_SURFACE(surfId))
		return NumDriver2Straights + NumDriver2Curves + (surfId & 0x1FFF);

	return -1;
}

// Speed-limit id of a segment (0 = slowest .. 3 = fastest).
static int cd2NavSpeedLimit(int surfId)
{
	DRIVER2_ROAD_INFO info;

	if (!GetSurfaceRoadInfo(&info, surfId))
		return 0;

	return ROAD_SPEED_LIMIT(&info);
}

static void cd2NavAddEdge(int a, int b)
{
	int k;

	if (a < 0 || b < 0 || a >= sNodeCount || b >= sNodeCount || a == b)
		return;

	for (k = 0; k < 4; k++)
		if (sNodes[a].adj[k] == b)
			return;			// already linked

	for (k = 0; k < 4; k++)
	{
		if (sNodes[a].adj[k] == CD2_NAV_EDGE_NONE)
		{
			sNodes[a].adj[k] = (short)b;
			sEdgeCount++;
			return;
		}
	}
}

static void cd2NavBuild(void)
{
	int i, k;

	sNodeCount = 0;
	sEdgeCount = 0;
	memset(sCache, 0, sizeof(sCache));
	memset(sLastValid, 0, sizeof(sLastValid));

	if (Driver2StraightsPtr == NULL && Driver2CurvesPtr == NULL && Driver2JunctionsPtr == NULL)
	{
		sBuilt = 1;
		return;
	}

	// straights
	for (i = 0; i < NumDriver2Straights && sNodeCount < CD2_NAV_MAX_NODES; i++)
	{
		CD2_NAV_NODE* nd = &sNodes[sNodeCount++];

		nd->surfId = i;
		nd->type = 0;
		nd->hasPos = 1;
		nd->x = Driver2StraightsPtr[i].Midx;
		nd->z = Driver2StraightsPtr[i].Midz;

		for (k = 0; k < 4; k++)
			nd->adj[k] = CD2_NAV_EDGE_NONE;
	}

	// curves
	for (i = 0; i < NumDriver2Curves && sNodeCount < CD2_NAV_MAX_NODES; i++)
	{
		CD2_NAV_NODE* nd = &sNodes[sNodeCount++];

		nd->surfId = 0x4000 | i;
		nd->type = 1;
		nd->hasPos = 1;
		nd->x = Driver2CurvesPtr[i].Midx;
		nd->z = Driver2CurvesPtr[i].Midz;

		for (k = 0; k < 4; k++)
			nd->adj[k] = CD2_NAV_EDGE_NONE;
	}

	// junctions (no midpoint - position derived from neighbours below)
	for (i = 0; i < NumDriver2Junctions && sNodeCount < CD2_NAV_MAX_NODES; i++)
	{
		CD2_NAV_NODE* nd = &sNodes[sNodeCount++];

		nd->surfId = 0x2000 | i;
		nd->type = 2;
		nd->hasPos = 0;
		nd->x = 0;
		nd->z = 0;

		for (k = 0; k < 4; k++)
			nd->adj[k] = CD2_NAV_EDGE_NONE;
	}

	// edges (bidirectional)
	for (i = 0; i < sNodeCount; i++)
	{
		DRIVER2_ROAD_INFO info;

		if (!GetSurfaceRoadInfo(&info, sNodes[i].surfId))
			continue;

		for (k = 0; k < 4; k++)
		{
			int t = cd2NavSurfToNode(info.ConnectIdx[k]);

			if (t >= 0 && t < sNodeCount)
			{
				cd2NavAddEdge(i, t);
				cd2NavAddEdge(t, i);
			}
		}
	}

	// give junctions a position: the average of their neighbours
	for (i = 0; i < sNodeCount; i++)
	{
		long long sx = 0, sz = 0;
		int n = 0;

		if (sNodes[i].hasPos)
			continue;

		for (k = 0; k < 4; k++)
		{
			int nb = sNodes[i].adj[k];

			if (nb >= 0 && sNodes[nb].hasPos)
			{
				sx += sNodes[nb].x;
				sz += sNodes[nb].z;
				n++;
			}
		}

		if (n > 0)
		{
			sNodes[i].x = (int)(sx / n);
			sNodes[i].z = (int)(sz / n);
			sNodes[i].hasPos = 1;
		}
	}

	sBuilt = 1;

	// cache node heights (drawn / used as waypoint Y without a per-use MapHeight)
	for (i = 0; i < sNodeCount; i++)
	{
		VECTOR p;

		if (!sNodes[i].hasPos)
			continue;

		p.vx = sNodes[i].x;
		p.vy = 0;
		p.vz = sNodes[i].z;
		sNodes[i].y = MapHeight(&p);
	}

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] nav graph built: nodes=%d (S%d C%d J%d) edges=%d\n",
			sNodeCount, NumDriver2Straights, NumDriver2Curves, NumDriver2Junctions, sEdgeCount);
}

static void cd2NavEnsure(void)
{
	// Rebuild when the level changes (cd2NavReset) OR when a previous attempt
	// found no road data yet (the road lumps may not be resident at GAME_START
	// on every level) - so a later call picks it up instead of caching an empty
	// graph for the whole level.
	if (!sBuilt || sNodeCount == 0)
		cd2NavBuild();
}

int cd2NavReady(void)
{
	cd2NavEnsure();
	return sBuilt;
}

void cd2NavReset(void)
{
	sBuilt = 0;
	sNodeCount = 0;
	sEdgeCount = 0;
	memset(sCache, 0, sizeof(sCache));
	memset(sLastValid, 0, sizeof(sLastValid));
}

int cd2NavNodeCount(void)
{
	cd2NavEnsure();
	return sNodeCount;
}

int cd2NavEdgeCount(void)
{
	cd2NavEnsure();
	return sEdgeCount;
}

int cd2NavNodePos(int node, VECTOR* out)
{
	if (node < 0 || node >= sNodeCount || !sNodes[node].hasPos)
		return 0;

	out->vx = sNodes[node].x;
	out->vy = sNodes[node].y;
	out->vz = sNodes[node].z;

	return 1;
}

int cd2NavNodeAt(const VECTOR* pos)
{
	VECTOR p = *pos;
	int surf, n;

	cd2NavEnsure();

	surf = GetSurfaceIndex(&p);
	n = cd2NavSurfToNode(surf);

	if (n >= 0 && n < sNodeCount && sNodes[n].hasPos)
		return n;

	return -1;
}

int cd2NavNearestRoadNode(const VECTOR* pos, int maxDist)
{
	int i, best = -1, bestD = maxDist;

	cd2NavEnsure();

	for (i = 0; i < sNodeCount; i++)
	{
		int d;

		if (!sNodes[i].hasPos)
			continue;

		d = cd2NavDist2D(pos->vx, pos->vz, sNodes[i].x, sNodes[i].z);

		if (d < bestD)
		{
			bestD = d;
			best = i;
		}
	}

	return best;
}

// ---- A* ------------------------------------------------------------------

static int cd2NavHeur(int a, int goal)
{
	// admissible: straight-line distance never exceeds the step cost, which is
	// dist * (4 - speedLimit) with (4 - speedLimit) >= 1
	return ABS(sNodes[a].x - sNodes[goal].x) + ABS(sNodes[a].z - sNodes[goal].z);
}

static int cd2NavEdgeCost(int a, int b)
{
	int dist = cd2NavDist2D(sNodes[a].x, sNodes[a].z, sNodes[b].x, sNodes[b].z);
	int factor = 4 - cd2NavSpeedLimit(sNodes[b].surfId);

	if (factor < 1)
		factor = 1;

	return dist * factor;
}

static void cd2NavHeapPush(int node)
{
	int i = sOpenSize++;

	if (i >= CD2_NAV_MAX_NODES * 4)
	{
		sOpenSize = CD2_NAV_MAX_NODES * 4;
		return;	// heap full: drop (best-effort)
	}

	sOpen[i] = node;

	while (i > 0)
	{
		int p = (i - 1) / 2;

		if (sF[sOpen[p]] <= sF[sOpen[i]])
			break;

		{
			int t = sOpen[p];
			sOpen[p] = sOpen[i];
			sOpen[i] = t;
		}

		i = p;
	}
}

static int cd2NavHeapPop(void)
{
	int top = sOpen[0];
	int i = 0;

	sOpen[0] = sOpen[--sOpenSize];

	for (;;)
	{
		int l = i * 2 + 1;
		int r = l + 1;
		int m = i;

		if (l < sOpenSize && sF[sOpen[l]] < sF[sOpen[m]])
			m = l;

		if (r < sOpenSize && sF[sOpen[r]] < sF[sOpen[m]])
			m = r;

		if (m == i)
			break;

		{
			int t = sOpen[m];
			sOpen[m] = sOpen[i];
			sOpen[i] = t;
		}

		i = m;
	}

	return top;
}

// A* from start to goal; fills `out` with the node indices (start excluded)
// and returns the count, or -1 when unreachable. *expanded gets the node count.
static int cd2NavSearch(int start, int goal, int* out, int maxOut, int* expanded)
{
	int i, n = 0;
	int cur;

	if (start < 0 || goal < 0 || start >= sNodeCount || goal >= sNodeCount)
		return -1;

	for (i = 0; i < sNodeCount; i++)
	{
		sG[i] = 0x7fffffff;
		sF[i] = 0x7fffffff;
		sParent[i] = -1;
		sClosed[i] = 0;
	}

	sOpenSize = 0;
	*expanded = 0;

	sG[start] = 0;
	sF[start] = cd2NavHeur(start, goal);
	cd2NavHeapPush(start);

	while (sOpenSize > 0)
	{
		int k;

		cur = cd2NavHeapPop();

		if (cur == goal)
			break;

		if (sClosed[cur])
			continue;

		sClosed[cur] = 1;
		(*expanded)++;

		for (k = 0; k < 4; k++)
		{
			int nb = sNodes[cur].adj[k];
			int cost;

			if (nb < 0 || sClosed[nb])
				continue;

			cost = sG[cur] + cd2NavEdgeCost(cur, nb);

			if (cost < sG[nb])
			{
				sG[nb] = cost;
				sParent[nb] = (short)cur;
				sF[nb] = cost + cd2NavHeur(nb, goal);
				cd2NavHeapPush(nb);
			}
		}
	}

	if (cur != goal)
		return -1;

	// walk parents back from the goal, then reverse into `out`
	for (i = goal; i != start && i >= 0 && n < CD2_NAV_MAX_NODES; i = sParent[i])
		out[n++] = i;

	if (i != start)
		return -1;		// parent chain broken

	// reverse
	for (i = 0; i < n / 2; i++)
	{
		int t = out[i];
		out[i] = out[n - 1 - i];
		out[n - 1 - i] = t;
	}

	if (n > maxOut)
		n = maxOut;

	return n;
}

// ---- routing -------------------------------------------------------------

static int sScratchRoute[CD2_NAV_MAX_NODES];

int cd2NavRoute(int carId, const VECTOR* from, const VECTOR* goal, CD2_NAV_ROUTE* out)
{
	CD2_NAV_CACHE* cache = NULL;
	int start, goalNode;
	int i, straight, roadLen = 0;
	int replan = 1;

	cd2NavEnsure();

	out->count = 0;
	out->source = CD2_NAV_SRC_NONE;
	out->goalNode = -1;
	out->expanded = 0;
	out->length = 0;

	if (from == NULL || goal == NULL)
		return 0;

	if (carId >= 0 && carId < CD2_NAV_MAX_CARS)
		cache = &sCache[carId];

	straight = cd2NavDist2D(from->vx, from->vz, goal->vx, goal->vz);

	start = cd2NavNodeAt(from);

	if (start < 0)
		start = cd2NavNearestRoadNode(from, 2048);

	// goal node: exact surface, else a graph node only if the point is close to
	// it (a goal far off any road is routed off-road instead)
	goalNode = cd2NavNodeAt(goal);

	if (goalNode < 0)
		goalNode = cd2NavNearestRoadNode(goal, 512);

	if (cache != NULL && cache->valid && cache->goalNode == goalNode &&
	    ABS(cache->goalX - goal->vx) + ABS(cache->goalZ - goal->vz) < 1024)
		replan = 0;

	if (replan)
	{
		int roadOk = 0;
		int expanded = 0;

		if (cache != NULL)
			cache->valid = 0;

		// --- road-graph attempt ---
		if (sNodeCount > 0 && start >= 0 && goalNode >= 0)
		{
			int n = cd2NavSearch(start, goalNode, sScratchRoute, CD2_NAV_MAX_ROUTE - 1, &expanded);

			if (n >= 0)
			{
				for (i = 0; i < n && out->count < CD2_NAV_MAX_ROUTE - 1; i++)
				{
					VECTOR p;

					if (!cd2NavNodePos(sScratchRoute[i], &p))
						continue;

					out->wp[out->count++] = p;
				}

				for (i = 1; i < out->count; i++)
					roadLen += cd2NavDist2D(out->wp[i - 1].vx, out->wp[i - 1].vz, out->wp[i].vx, out->wp[i].vz);

				// include the final leg to the goal so the detour test below
				// compares like for like against the straight-line distance
				if (out->count > 0)
					roadLen += cd2NavDist2D(out->wp[out->count - 1].vx, out->wp[out->count - 1].vz, goal->vx, goal->vz);

				roadOk = 1;
			}
		}

		// --- off-road fallback: no road route, or the road detours badly ---
		if (!roadOk || (straight > 1200 && roadLen > straight * 2))
		{
			VECTOR gw[CD2_NAV_MAX_ROUTE];
			int gexp = 0;
			int gn = cd2GridPath(from, goal, gw, CD2_NAV_MAX_ROUTE, &gexp);

			if (gn > 0)
			{
				for (i = 0; i < gn; i++)
					out->wp[i] = gw[i];

				out->count = gn;
				out->source = CD2_NAV_SRC_SCENERY;
				out->expanded = gexp;
				roadOk = 1;
			}
			else if (roadOk)
			{
				out->source = CD2_NAV_SRC_ROAD;
				out->expanded = expanded;
				roadOk = 1;
			}
		}
		else
		{
			out->source = CD2_NAV_SRC_ROAD;
			out->expanded = expanded;
		}

		if (!roadOk)
			return 0;

		// always end at the actual goal position
		if (out->count == 0 || out->wp[out->count - 1].vx != goal->vx || out->wp[out->count - 1].vz != goal->vz)
		{
			if (out->count < CD2_NAV_MAX_ROUTE)
				out->wp[out->count++] = *goal;
			else
				out->wp[CD2_NAV_MAX_ROUTE - 1] = *goal;
		}

		// dense enough to steer along, then measure
		cd2NavDensify(out);

		out->goalNode = goalNode;

		for (i = 1; i < out->count; i++)
			out->length += cd2NavDist2D(out->wp[i - 1].vx, out->wp[i - 1].vz, out->wp[i].vx, out->wp[i].vz);

		if (cache != NULL)
		{
			cache->valid = 1;
			cache->source = out->source;
			cache->goalNode = goalNode;
			cache->goalX = goal->vx;
			cache->goalZ = goal->vz;
			cache->expanded = out->expanded;
			cache->count = out->count;

			for (i = 0; i < out->count; i++)
				cache->wp[i] = out->wp[i];
		}

		if (gCd2Cfg.debugLog)
		{
			static unsigned int t = 0;

			if ((t++ & 31) == 0)
				printInfo("[combatd2] nav route car=%d src=%s wp=%d len=%d straight=%d goalNode=%d expanded=%d\n",
					carId, (out->source == CD2_NAV_SRC_SCENERY) ? "scenery" : "road",
					out->count, out->length, straight, goalNode, out->expanded);
		}
	}
	else
	{
		out->source = cache->source;
		out->goalNode = cache->goalNode;
		out->expanded = cache->expanded;
		out->count = cache->count;

		for (i = 0; i < cache->count; i++)
			out->wp[i] = cache->wp[i];

		for (i = 1; i < out->count; i++)
			out->length += cd2NavDist2D(out->wp[i - 1].vx, out->wp[i - 1].vz, out->wp[i].vx, out->wp[i].vz);
	}

	if (carId >= 0 && carId < CD2_NAV_MAX_CARS)
	{
		sLastRoute[carId] = *out;
		sLastValid[carId] = 1;
	}

	return out->count;
}

const CD2_NAV_ROUTE* cd2NavLastRoute(int carId)
{
	if (carId < 0 || carId >= CD2_NAV_MAX_CARS || !sLastValid[carId])
		return NULL;

	return &sLastRoute[carId];
}

// ---- debug draw ----------------------------------------------------------

#ifndef PSX
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
#endif

void cd2NavDraw(const VECTOR* centre, int radius)
{
#ifndef PSX
	static const CVECTOR colNode[3] =
	{
		{ 250, 250, 70 },	// straight
		{ 70, 170, 250 },	// curve
		{ 250, 130, 40 }	// junction
	};
	CVECTOR colEdge = { 90, 90, 90 };
	int i, k;

	if (centre == NULL)
		return;

	for (i = 0; i < sNodeCount; i++)
	{
		VECTOR a, b;

		if (!sNodes[i].hasPos)
			continue;

		if (radius > 0 && cd2NavDist2D(centre->vx, centre->vz, sNodes[i].x, sNodes[i].z) > radius)
			continue;

		a.vx = sNodes[i].x;
		a.vy = sNodes[i].y;
		a.vz = sNodes[i].z;

		b = a;
		b.vy = a.vy + 220;
		Debug_AddLineDepth(a, b, (CVECTOR&)colNode[sNodes[i].type]);

		for (k = 0; k < 4; k++)
		{
			int nb = sNodes[i].adj[k];

			if (nb <= i || !sNodes[nb].hasPos)
				continue;	// draw each edge once

			if (radius > 0 && cd2NavDist2D(centre->vx, centre->vz, sNodes[nb].x, sNodes[nb].z) > radius)
				continue;

			a.vy = sNodes[i].y + 60;
			b.vx = sNodes[nb].x;
			b.vy = sNodes[nb].y + 60;
			b.vz = sNodes[nb].z;
			Debug_AddLineDepth(a, b, colEdge);
		}
	}
#endif
}
