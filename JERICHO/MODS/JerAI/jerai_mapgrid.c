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

	/* off the road is allowed and discouraged: the whole point of a road
	 * preference is that it must not make anywhere unreachable */
	if (!JerAiMapRoad(map, ix, iz))
		cost += 40;

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
