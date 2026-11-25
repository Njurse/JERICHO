/*
 * alleycat_mapgrid.c -- the PURE half of the world model: everything that reads an
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

#include "alleycat_map.h"

#include <math.h>

int AlleycatMapValid(const ALLEYCAT_MAP* map)
{
	return (map != NULL) && map->valid;
}

int AlleycatMapBlocked(const ALLEYCAT_MAP* map, int ix, int iz)
{
	if (map == NULL || ix < 0 || iz < 0 || ix >= ALLEYCAT_MAP_SIZE || iz >= ALLEYCAT_MAP_SIZE)
		return 1;	/* outside the window is not somewhere we drive */

	return (map->cell[iz][ix] & ALLEYCAT_MAP_BLOCKED) ? 1 : 0;
}

int AlleycatMapRoad(const ALLEYCAT_MAP* map, int ix, int iz)
{
	if (map == NULL || ix < 0 || iz < 0 || ix >= ALLEYCAT_MAP_SIZE || iz >= ALLEYCAT_MAP_SIZE)
		return 0;

	return (map->cell[iz][ix] & ALLEYCAT_MAP_ROAD) ? 1 : 0;
}

int AlleycatMapClearance(const ALLEYCAT_MAP* map, int ix, int iz)
{
	if (map == NULL || ix < 0 || iz < 0 || ix >= ALLEYCAT_MAP_SIZE || iz >= ALLEYCAT_MAP_SIZE)
		return 0;

	return map->clear[iz][ix];
}

int AlleycatMapCost(const ALLEYCAT_MAP* map, int ix, int iz)
{
	int cost;

	if (!AlleycatMapValid(map) || AlleycatMapBlocked(map, ix, iz))
		return ALLEYCAT_MAP_COST_BLOCKED;

	cost = 100;	/* must stay ALLEYCAT_STAR_COST_MIN in alleycat_star.h */

	/* Off the road is allowed and discouraged: the whole point of a road preference
	 * is that it must not make anywhere unreachable. How much it costs to be off it
	 * is the map's `offroadCost` (JERICHO/CONFIG/Alleycat.ini `offroad_cost`), so the
	 * planner takes the road network WHEN THE ROAD GOES THE SAME WAY while still
	 * being able to leave it to reach a goal that is not on it. */
	if (!AlleycatMapRoad(map, ix, iz))
		cost += map->offroadCost;

	/* Hug the middle HARD. The clearance term has to be able to outweigh extra
	 * distance, or A* chooses to squeeze a short gap past a building over driving
	 * around the block - which the car then grinds into. Squared, so a sample
	 * right against a wall costs several times an open one:
	 *   clearance 0 -> +144, 1 -> +100, 2 -> +64, 3 -> +36, 6 -> +0 (base 100). */
	{
		int tight = ALLEYCAT_MAP_CLEAR_MAX - AlleycatMapClearance(map, ix, iz);

		cost += tight * tight * 4;
	}

	return cost;
}

/* Nearest sample, FLOORING for negative offsets. C integer division truncates toward
 * zero, so the obvious (v + step/2) / step pulls a point a whole step BELOW the window
 * back onto its edge and reports it as inside. */
static int AlleycatMapSampleRound(int v, int step)
{
	if (v >= 0)
		return (v + step / 2) / step;

	return -(((-v) + step / 2) / step);
}

int AlleycatMapSampleIndex(const ALLEYCAT_MAP* map, int x, int z, int* ix, int* iz)
{
	int sx, sz;

	if (map == NULL)
		return 0;

	sx = AlleycatMapSampleRound(x - map->originX, ALLEYCAT_MAP_STEP);
	sz = AlleycatMapSampleRound(z - map->originZ, ALLEYCAT_MAP_STEP);

	if (sx < 0 || sz < 0 || sx >= ALLEYCAT_MAP_SIZE || sz >= ALLEYCAT_MAP_SIZE)
		return 0;

	if (ix != NULL) *ix = sx;
	if (iz != NULL) *iz = sz;

	return 1;
}

void AlleycatMapWorld(const ALLEYCAT_MAP* map, int ix, int iz, int* x, int* z)
{
	if (map == NULL)
	{
		if (x != NULL) *x = 0;
		if (z != NULL) *z = 0;
		return;
	}

	if (x != NULL) *x = map->originX + ix * ALLEYCAT_MAP_STEP;
	if (z != NULL) *z = map->originZ + iz * ALLEYCAT_MAP_STEP;
}

/* Ring by ring, so the first hit is the nearest one. Shared rather than duplicated: the
 * pathfinder and the local road search both need it, and two copies would eventually
 * disagree about what "nearest" means. */
int AlleycatMapNearestOpen(const ALLEYCAT_MAP* map, int ix, int iz, int* ox, int* oz)
{
	int r, dx, dz;

	if (!AlleycatMapBlocked(map, ix, iz))
	{
		if (ox != NULL) *ox = ix;
		if (oz != NULL) *oz = iz;
		return 1;
	}

	for (r = 1; r <= ALLEYCAT_MAP_OPEN_SNAP; r++)
	{
		for (dz = -r; dz <= r; dz++)
		{
			for (dx = -r; dx <= r; dx++)
			{
				if ((dx < 0 ? -dx : dx) != r && (dz < 0 ? -dz : dz) != r)
					continue;	/* the ring, not the disc */

				if (!AlleycatMapBlocked(map, ix + dx, iz + dz))
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
/* polyline geometry (see alleycat_map.h): 64-bit throughout               */
/* ------------------------------------------------------------------ */

long long AlleycatDist2(int ax, int az, int bx, int bz)
{
	long long dx = (long long)ax - (long long)bx;
	long long dz = (long long)az - (long long)bz;

	return dx * dx + dz * dz;
}

static long long AlleycatSegLen(long long ax, long long az, long long bx, long long bz)
{
	double dx = (double)(bx - ax);
	double dz = (double)(bz - az);

	return (long long)sqrt(dx * dx + dz * dz);
}

long long AlleycatPathProjectArc(const int* wx, const int* wz, int n, int cx, int cz, long long* crossD2)
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
		long long segLen = AlleycatSegLen(ax, az, bx, bz);
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

int AlleycatPathAimPoint(const int* wx, const int* wz, int n, int cx, int cz,
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
		long long segLen = AlleycatSegLen(ax, az, bx, bz);
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
