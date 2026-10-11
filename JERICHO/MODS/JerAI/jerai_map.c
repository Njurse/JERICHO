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
#include "map.h"	/* cells_across: the "is a level loaded" test; units_across_halved */
#include "dr2roads.h"	/* JerRoadAt, MapHeight */
#include "cell.h"	/* CELL_ITERATOR, GetFirstPackedCop, QuickUnpackCellObject */
#include "models.h"	/* modelpointers, GET_MODEL_DATA */
#include "draw.h"	/* matrixtable */
#include "dr2math.h"	/* RCOS / RSIN, for the object's rotated collision boxes */

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

/* The same collision test CellEmpty makes, with the SMASHABLE branch KEPT.
 *
 * CellEmpty deliberately IGNORES smashable scenery (objcoll.c:49 requires
 * flags2 & (MODEL_FLAG_CHAIR | MODEL_FLAG_SMASHABLE) == 0), so light posts,
 * chairs and a lot of fencing are INVISIBLE to it - and the occupancy grid used
 * to route the A* straight through them, which is what the car then grinds into.
 * This only ADDS the objects CellEmpty skips, so it can never report something
 * safe as blocked. */
int JerAiMapCellHasSmashable(VECTOR* p, int radius)
{
	int cell_x = (p->vx + units_across_halved) / MAP_CELL_SIZE;
	int cell_z = (p->vz + units_down_halved) / MAP_CELL_SIZE;
	CELL_ITERATOR ci;
	PACKED_CELL_OBJECT* ppco;

	if (p->vx + units_across_halved < 0 || p->vx - units_across_halved >= 0 ||
	    p->vz + units_down_halved < 0 || p->vz - units_down_halved >= 0)
		return 0;	/* out of level: no objects to find */

	for (ppco = GetFirstPackedCop(cell_x, cell_z, &ci, 0); ppco != NULL; ppco = GetNextPackedCop(&ci))
	{
		int type = (ppco->value >> 6) | ((ppco->pos.vy & 1) << 10);
		MODEL* pModel = modelpointers[type];
		CELL_OBJECT co;
		int num_cb, box_loop, xd, zd, xxd, zzd, yang, theta, cs, sn;
		int xs, zs, ypos;
		long sphere_sq;
		COLLISION_PACKET* collide;
		MATRIX2* mat;

		if (pModel == NULL || pModel->collision_block <= 0)
			continue;

		if ((pModel->flags2 & (MODEL_FLAG_CHAIR | MODEL_FLAG_SMASHABLE)) == 0)
			continue;	/* CellEmpty already handles the solid ones */

		QuickUnpackCellObject(ppco, &ci.nearCell, &co);

		xd = co.pos.vx - p->vx;
		zd = co.pos.vz - p->vz;

		sphere_sq = pModel->bounding_sphere + 580;
		sphere_sq = sphere_sq * sphere_sq;

		if ((long)xd * xd + (long)zd * zd >= sphere_sq)
			continue;

		num_cb = *GET_MODEL_DATA(int, pModel, collision_block);
		collide = GET_MODEL_DATA_OFS(COLLISION_PACKET, pModel, collision_block, sizeof(int));

		yang = -co.yang & 63;
		theta = (co.yang + collide->yang) * 64;
		mat = &matrixtable[yang];
		cs = RCOS(theta);
		sn = RSIN(theta);

		for (box_loop = 0; box_loop < num_cb; box_loop++, collide++)
		{
			xxd = FIXEDH(collide->xpos * mat->m[0][0] + collide->zpos * mat->m[2][0]);
			zzd = FIXEDH(collide->xpos * mat->m[0][2] + collide->zpos * mat->m[2][2]);

			xd = (co.pos.vx - p->vx) + xxd;
			zd = (co.pos.vz - p->vz) + zzd;

			zs = ((collide->zsize + 10) * 2048 + radius * 4096);
			xs = ((collide->xsize + 10) * 2048 + radius * 4096);

			ypos = p->vy + (co.pos.vy + collide->ypos);

			if (collide->ysize / 2 > ABS(ypos) &&
				xs > ABS(xd * cs - zd * sn) &&
				zs > ABS(zd * cs + xd * sn))
				return 1;
		}
	}

	return 0;
}

int JerAiMapBuild(JERAI_MAP* map, int x, int y, int z)
{
	int ix, iz;

	if (map == NULL)
		return 0;

	map->valid = 0;
	map->sampled = 0;
	map->roadSamples = 0;
	/* the road preference is the default until a caller overrides it (the driver
	 * sets it from `offroad_cost` before planning) */
	map->offroadCost = JERAI_MAP_OFFROAD_DEFAULT;
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

				/* BOTH scenery tests: the engine's own, and the same one with the
				 * smashables it skips (posts, fencing, chairs) put back. A car has to
				 * drive around those, so the planner must know they are there - that
				 * is the "never path through world geometry" half of the grid. */
				if (!CellEmpty(&v, JERAI_MAP_RADIUS) || JerAiMapCellHasSmashable(&v, JERAI_MAP_RADIUS))
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
