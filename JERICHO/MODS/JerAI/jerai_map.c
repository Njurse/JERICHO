/*
 * jerai_map.c -- build the AI's occupancy grid. See jerai_map.h.
 *
 * The engine side of the world model (lifted from mp/ai/aimap.c and namespaced).
 * Everything here is a THIN wrapper over the engine's own queries:
 *
 *   CellEmpty(p, radius)   the scenery test the whole game uses, which
 *                          deliberately ignores smashables and chairs;
 *   JerRoadAt(x, y, z)     JERICHO's road-network hook (dr2roads.c);
 *   MapHeight(p)           the ground height, so a sample is probed at the
 *                          height of the GROUND THERE rather than at the
 *                          car's height.
 */

#include <string.h>

#include "jericho.h"
#include "driver2.h"
#include "objcoll.h"	/* CellEmpty: the engine's own scenery test */
#include "map.h"	/* cells_across: the "is a level loaded" test */
#include "dr2roads.h"	/* JerRoadAt, MapHeight */

#include "jerai_map.h"

/* ------------------------------------------------------------------ */
/* The world                                                           */
/* ------------------------------------------------------------------ */

int JerAiMapWorldLoaded(void)
{
	/* cells_across is filled in from the level header when a level loads, and is 0
	 * before that - no level, no world to sample. */
	return cells_across > 0;
}

static void JerAiMapFill(JERAI_MAP* map, unsigned char flags)
{
	int ix, iz;

	for (iz = 0; iz < JERAI_MAP_SIZE; iz++)
	{
		for (ix = 0; ix < JERAI_MAP_SIZE; ix++)
		{
			map->cell[iz][ix] = flags;
			map->clear[iz][ix] = 0;
		}
	}
}

/* How many samples from the nearest blocked one, up to JERAI_MAP_CLEAR_MAX. A
 * bounded neighbourhood scan, done once per build rather than per query. */
static void JerAiMapMeasureClearance(JERAI_MAP* map)
{
	int ix, iz, dx, dz;

	for (iz = 0; iz < JERAI_MAP_SIZE; iz++)
	{
		for (ix = 0; ix < JERAI_MAP_SIZE; ix++)
		{
			int best = JERAI_MAP_CLEAR_MAX + 1;

			if (map->cell[iz][ix] & JERAI_MAP_BLOCKED)
			{
				map->clear[iz][ix] = 0;
				continue;
			}

			for (dz = -JERAI_MAP_CLEAR_MAX; dz <= JERAI_MAP_CLEAR_MAX && best > 1; dz++)
			{
				for (dx = -JERAI_MAP_CLEAR_MAX; dx <= JERAI_MAP_CLEAR_MAX; dx++)
				{
					int sx = ix + dx;
					int sz = iz + dz;
					int d;

					if (sx < 0 || sz < 0 || sx >= JERAI_MAP_SIZE || sz >= JERAI_MAP_SIZE)
						continue;	/* outside the window: unknown, not blocked */

					if ((map->cell[sz][sx] & JERAI_MAP_BLOCKED) == 0)
						continue;

					d = (dx < 0 ? -dx : dx);
					if ((dz < 0 ? -dz : dz) > d)
						d = (dz < 0 ? -dz : dz);

					if (d < best)
						best = d;
				}
			}

			map->clear[iz][ix] = (best > JERAI_MAP_CLEAR_MAX) ? JERAI_MAP_CLEAR_MAX : best;
		}
	}
}

int JerAiMapBuild(JERAI_MAP* map, int x, int y, int z)
{
	int ix, iz;

	if (map == NULL)
		return 0;

	map->valid = 0;
	map->sampled = 0;
	map->roadSamples = 0;
	map->centreX = x;
	map->centreY = y;
	map->centreZ = z;

	/* an odd grid, so sample JERAI_MAP_SIZE/2 IS the centre we were given */
	map->originX = x - (JERAI_MAP_SIZE / 2) * JERAI_MAP_STEP;
	map->originZ = z - (JERAI_MAP_SIZE / 2) * JERAI_MAP_STEP;

	if (!JerAiMapWorldLoaded())
	{
		/* No level: say so, and leave a grid that reports "blocked" everywhere. */
		JerAiMapFill(map, JERAI_MAP_BLOCKED);
		return 0;
	}

	for (iz = 0; iz < JERAI_MAP_SIZE; iz++)
	{
		for (ix = 0; ix < JERAI_MAP_SIZE; ix++)
		{
			int wx = map->originX + ix * JERAI_MAP_STEP;
			int wz = map->originZ + iz * JERAI_MAP_STEP;
			unsigned char f = 0;
			VECTOR v;

			v.vx = wx;
			v.vz = wz;

			/* OUTSIDE the loaded level is not drivable. CellEmpty reads an
			 * out-of-level cell as EMPTY (it has no objects), so without this
			 * test the A* cheerfully routes off the map edge and the car drives
			 * at fences and the boundary. units_across_halved is the level's
			 * half-extent in world units - the same term CellEmpty uses to build
			 * its cell number. */
			if (wx + units_across_halved < 0 || wx - units_across_halved >= 0 ||
			    wz + units_down_halved < 0 || wz - units_down_halved >= 0)
			{
				f |= JERAI_MAP_BLOCKED;
			}
			else
			{
				v.vy = MapHeight(&v);	/* probe at the ground there, not at our height */

				if (!CellEmpty(&v, JERAI_MAP_RADIUS))
					f |= JERAI_MAP_BLOCKED;

				if (!(f & JERAI_MAP_BLOCKED) && JerRoadAt(wx, v.vy, wz))
					f |= JERAI_MAP_ROAD;
			}

			map->cell[iz][ix] = f;
			map->clear[iz][ix] = 0;

			if (f & JERAI_MAP_ROAD)
				map->roadSamples++;

			map->sampled++;
		}
	}

	JerAiMapMeasureClearance(map);

	map->valid = 1;

	return 1;
}

/* The queries and the cost model live in jerai_mapgrid.c: they are PURE, so a host test
 * can exercise them (and the pathfinder) against a hand-built grid with no engine
 * present. Only the filling of the grid - which is nothing but engine probes - is
 * in this file. */
