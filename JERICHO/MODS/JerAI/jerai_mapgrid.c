/*
 * jerai_mapgrid.c -- the PURE half of the world model: everything that reads an
 * already-built grid and nothing that touches the engine.
 *
 * Lifted from mp/ai/aimapgrid.c and namespaced. The split is deliberate: the
 * decisions worth testing must not need the engine to exist, so a host test can
 * hand-build a grid and exercise the real cost model and the real pathfinder
 * against it (see test_ai_path.c).
 *
 * Nothing here calls the engine. If a change ever makes that awkward, the change
 * is in the wrong file.
 */

#include "jerai_map.h"

#include <math.h>

int JerAiMapValid(const JERAI_MAP* map)
{
	return (map != NULL) && map->valid;
}

int JerAiMapBlocked(const JERAI_MAP* map, int ix, int iz)
{
	if (map == NULL || ix < 0 || iz < 0 || ix >= JERAI_MAP_SIZE || iz >= JERAI_MAP_SIZE)
		return 1;	/* outside the window is not somewhere we drive */

	return (map->cell[iz][ix] & JERAI_MAP_BLOCKED) ? 1 : 0;
}

int JerAiMapRoad(const JERAI_MAP* map, int ix, int iz)
{
	if (map == NULL || ix < 0 || iz < 0 || ix >= JERAI_MAP_SIZE || iz >= JERAI_MAP_SIZE)
		return 0;

	return (map->cell[iz][ix] & JERAI_MAP_ROAD) ? 1 : 0;
}

int JerAiMapClearance(const JERAI_MAP* map, int ix, int iz)
{
	if (map == NULL || ix < 0 || iz < 0 || ix >= JERAI_MAP_SIZE || iz >= JERAI_MAP_SIZE)
		return 0;

	return map->clear[iz][ix];
}

int JerAiMapCost(const JERAI_MAP* map, int ix, int iz)
{
	int cost;

	if (!JerAiMapValid(map) || JerAiMapBlocked(map, ix, iz))
		return JERAI_MAP_COST_BLOCKED;

	cost = 100;	/* must stay JERAI_STAR_COST_MIN in jerai_star.h */

	/* Off the road is allowed and discouraged: the whole point of a road preference
	 * is that it must not make anywhere unreachable. How much it costs to be off it
	 * is the map's `offroadCost` (JERICHO/CONFIG/JerAI.ini `offroad_cost`), so the
	 * planner takes the road network WHEN THE ROAD GOES THE SAME WAY while still
	 * being able to leave it to reach a goal that is not on it. */
	if (!JerAiMapRoad(map, ix, iz))
		cost += map->offroadCost;

	/* Hug the middle HARD. The clearance term has to be able to outweigh extra
	 * distance, or A* chooses to squeeze a short gap past a building over driving
	 * around the block - which the car then grinds into. Squared, so a sample
	 * right against a wall costs several times an open one:
	 *   clearance 0 -> +144, 1 -> +100, 2 -> +64, 3 -> +36, 6 -> +0 (base 100). */
	{
		int tight = JERAI_MAP_CLEAR_MAX - JerAiMapClearance(map, ix, iz);

		cost += tight * tight * 4;
	}

	return cost;
}

/* Nearest sample, FLOORING for negative offsets. C integer division truncates toward
 * zero, so the obvious (v + step/2) / step pulls a point a whole step BELOW the window
 * back onto its edge and reports it as inside. */
static int JerAiMapSampleRound(int v, int step)
{
	if (v >= 0)
		return (v + step / 2) / step;

	return -(((-v) + step / 2) / step);
}

int JerAiMapSampleIndex(const JERAI_MAP* map, int x, int z, int* ix, int* iz)
{
	int sx, sz;

	if (map == NULL)
		return 0;

	sx = JerAiMapSampleRound(x - map->originX, JERAI_MAP_STEP);
	sz = JerAiMapSampleRound(z - map->originZ, JERAI_MAP_STEP);

	if (sx < 0 || sz < 0 || sx >= JERAI_MAP_SIZE || sz >= JERAI_MAP_SIZE)
		return 0;

	if (ix != NULL) *ix = sx;
	if (iz != NULL) *iz = sz;

	return 1;
}

void JerAiMapWorld(const JERAI_MAP* map, int ix, int iz, int* x, int* z)
{
	if (map == NULL)
	{
		if (x != NULL) *x = 0;
		if (z != NULL) *z = 0;
		return;
	}

	if (x != NULL) *x = map->originX + ix * JERAI_MAP_STEP;
	if (z != NULL) *z = map->originZ + iz * JERAI_MAP_STEP;
}

/* Ring by ring, so the first hit is the nearest one. Shared rather than duplicated: the
 * pathfinder and the local road search both need it, and two copies would eventually
 * disagree about what "nearest" means. */
int JerAiMapNearestOpen(const JERAI_MAP* map, int ix, int iz, int* ox, int* oz)
{
	int r, dx, dz;

	if (!JerAiMapBlocked(map, ix, iz))
	{
		if (ox != NULL) *ox = ix;
		if (oz != NULL) *oz = iz;
		return 1;
	}

	for (r = 1; r <= JERAI_MAP_OPEN_SNAP; r++)
	{
		for (dz = -r; dz <= r; dz++)
		{
			for (dx = -r; dx <= r; dx++)
			{
				if ((dx < 0 ? -dx : dx) != r && (dz < 0 ? -dz : dz) != r)
					continue;	/* the ring, not the disc */

				if (!JerAiMapBlocked(map, ix + dx, iz + dz))
				{
					if (ox != NULL) *ox = ix + dx;
					if (oz != NULL) *oz = iz + dz;
					return 1;
				}
			}
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* polyline geometry (see jerai_map.h): 64-bit throughout               */
/* ------------------------------------------------------------------ */

long long JerAiDist2(int ax, int az, int bx, int bz)
{
	long long dx = (long long)ax - (long long)bx;
	long long dz = (long long)az - (long long)bz;

	return dx * dx + dz * dz;
}

static long long JerAiSegLen(long long ax, long long az, long long bx, long long bz)
{
	double dx = (double)(bx - ax);
	double dz = (double)(bz - az);

	return (long long)sqrt(dx * dx + dz * dz);
}

long long JerAiPathProjectArc(const int* wx, const int* wz, int n, int cx, int cz, long long* crossD2)
{
	long long bestD2 = -1, bestArc = 0, arc = 0;
	int k;

	if (crossD2 != NULL)
		*crossD2 = -1;

	if (wx == NULL || wz == NULL)
		return 0;

	for (k = 1; k < n; k++)
	{
		long long ax = wx[k - 1], az = wz[k - 1];
		long long bx = wx[k], bz = wz[k];
		long long dx = bx - ax, dz = bz - az;
		long long len2 = dx * dx + dz * dz;
		long long segLen = JerAiSegLen(ax, az, bx, bz);
		long long t = 0, px, pz, ex, ez, d2;

		if (len2 > 0)
		{
			t = (cx - ax) * dx + (cz - az) * dz;

			if (t < 0) t = 0;
			if (t > len2) t = len2;
		}

		px = ax + (len2 > 0 ? dx * t / len2 : 0);
		pz = az + (len2 > 0 ? dz * t / len2 : 0);
		ex = cx - px;
		ez = cz - pz;
		d2 = ex * ex + ez * ez;

		if (bestD2 < 0 || d2 < bestD2)
		{
			bestD2 = d2;
			bestArc = arc + (len2 > 0 ? segLen * t / len2 : 0);
		}

		arc += segLen;
	}

	if (crossD2 != NULL)
		*crossD2 = bestD2;

	return bestArc;
}

int JerAiPathAimPoint(const int* wx, const int* wz, int n, int cx, int cz,
	int fwdX, int fwdZ, long long fromArc, long long lookAhead, int behindCos,
	int* aimX, int* aimZ, int* segDx, int* segDz)
{
	long long arc = 0;
	long long bestX, bestZ, bestDx = 0, bestDz = 0;
	int k;

	if (wx == NULL || wz == NULL || n <= 0)
		return 0;

	bestX = wx[n - 1];
	bestZ = wz[n - 1];

	for (k = 1; k < n; k++)
	{
		long long ax = wx[k - 1], az = wz[k - 1];
		long long bx = wx[k], bz = wz[k];
		long long dx = bx - ax, dz = bz - az;
		long long segLen = JerAiSegLen(ax, az, bx, bz);
		long long steps, s;
		int done = 0;

		if (segLen <= 0)
			continue;

		steps = segLen / 512 + 1;

		for (s = 0; s <= steps; s++)
		{
			long long f = segLen * s / steps;
			long long px = ax + dx * f / segLen;
			long long pz = az + dz * f / segLen;
			long long thisArc = arc + f;

			if (thisArc < fromArc)
				continue;	/* still behind our own projection */

			if (thisArc - fromArc >= lookAhead)
			{
				long long rx = px - cx, rz = pz - cz;
				long long rlen2 = rx * rx + rz * rz;

				if (rlen2 <= 0)
					done = 1;
				else
				{
					long long cosScaled = ((long long)fwdX * rx + (long long)fwdZ * rz) /
						(long long)(sqrt((double)rlen2) + 1.0);

					if (cosScaled > behindCos)
						done = 1;
				}

				if (done)
				{
					bestX = px;
					bestZ = pz;
					bestDx = dx;
					bestDz = dz;
					break;
				}
			}
		}

		if (done)
			break;

		arc += segLen;
	}

	if (aimX != NULL) *aimX = (int)bestX;
	if (aimZ != NULL) *aimZ = (int)bestZ;
	if (segDx != NULL) *segDx = (int)bestDx;
	if (segDz != NULL) *segDz = (int)bestDz;

	return 1;
}
