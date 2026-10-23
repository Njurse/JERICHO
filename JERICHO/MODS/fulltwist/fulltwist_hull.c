/*
 * fulltwist_hull.c -- the car's mesh hull and its convex footprint.
 *
 * The hull is built once per resident model slot from the CLEAN body mesh
 * (gCarCleanModelPtr). It is placed on the engine's collision-body centre, the
 * same `-cog` offset bcollide.c uses, so the lever a collision reports is in the
 * frame the stock response already uses.
 *
 * Two queries are exposed:
 *   ftHullSupport  -- the extreme vertex along a direction (the contact point)
 *   ftHullContact  -- the support point, tie-broken by the reported hit point
 *
 * The XZ convex footprint is the monotone-chain hull of the same vertices. It
 * is the shape the 3D scenery test will use; keeping it separate from the full
 * vertex list means the test does not need to walk every body vertex.
 */

#include "driver2.h"
#include "jericho.h"
#include "cars.h"
#include "fulltwist.h"

#include <stdlib.h>	/* qsort */

static int ftHullCompare(const void* pa, const void* pb)
{
	const SVECTOR* a = (const SVECTOR*)pa;
	const SVECTOR* b = (const SVECTOR*)pb;

	if (a->vx != b->vx)
		return (a->vx < b->vx) ? -1 : 1;

	if (a->vz != b->vz)
		return (a->vz < b->vz) ? -1 : 1;

	return 0;
}

static long long ftHullCross(const SVECTOR* o, const SVECTOR* a, const SVECTOR* b)
{
	return ((long long)a->vx - o->vx) * ((long long)b->vz - o->vz) -
		   ((long long)a->vz - o->vz) * ((long long)b->vx - o->vx);
}

static void ftHullBuild(FULLTWIST_HULL* hull, int slot, MODEL* mdl, CAR_COSMETICS* cos)
{
	SVECTOR* src;
	SVECTOR pts[FT_HULL_MAX_VERTS];
	SVECTOR out[FT_HULL_MAX_VERTS * 2];
	int n, i, k, lower;
	int minX, maxX, minY, maxY, minZ, maxZ;
	int cx, cy, cz;
	int cogX, cogY, cogZ;

	memset(hull, 0, sizeof(*hull));
	hull->model = mdl;
	hull->slot = slot;

	if (mdl == NULL)
		return;

	n = mdl->num_vertices;
	hull->modelVerts = n;

	if (n > FT_HULL_MAX_VERTS)
	{
		FT_LOG("[fulltwist] hull slot=%d: model has %d vertices, capped at %d\n",
			slot, n, FT_HULL_MAX_VERTS);
		n = FT_HULL_MAX_VERTS;
	}

	if (n <= 0)
		return;

	src = GET_MODEL_DATA(SVECTOR, mdl, vertices);

	if (src == NULL)
		return;

	cogX = (cos != NULL) ? cos->cog.vx : 0;
	cogY = (cos != NULL) ? cos->cog.vy : 0;
	cogZ = (cos != NULL) ? cos->cog.vz : 0;

	minX = maxX = src[0].vx;
	minY = maxY = src[0].vy;
	minZ = maxZ = src[0].vz;

	for (i = 1; i < n; i++)
	{
		if (src[i].vx < minX) minX = src[i].vx;
		if (src[i].vx > maxX) maxX = src[i].vx;
		if (src[i].vy < minY) minY = src[i].vy;
		if (src[i].vy > maxY) maxY = src[i].vy;
		if (src[i].vz < minZ) minZ = src[i].vz;
		if (src[i].vz > maxZ) maxZ = src[i].vz;
	}

	cx = (minX + maxX) / 2;
	cy = (minY + maxY) / 2;
	cz = (minZ + maxZ) / 2;

	/* Place the mesh centre on the collision-body centre (-cog). */
	for (i = 0; i < n; i++)
	{
		SVECTOR p;

		p.vx = (short)(src[i].vx - cx - cogX);
		p.vy = (short)(src[i].vy - cy - cogY);
		p.vz = (short)(src[i].vz - cz - cogZ);
		p.pad = 0;

		hull->verts[i] = p;
		pts[i] = p;
	}

	hull->count = n;
	hull->minX = minX - cx - cogX;
	hull->maxX = maxX - cx - cogX;
	hull->minY = minY - cy - cogY;
	hull->maxY = maxY - cy - cogY;
	hull->minZ = minZ - cz - cogZ;
	hull->maxZ = maxZ - cz - cogZ;
	hull->hx = (maxX - minX) / 2;
	hull->hy = (maxY - minY) / 2;
	hull->hz = (maxZ - minZ) / 2;
	hull->cogX = cogX;
	hull->cogY = cogY;
	hull->cogZ = cogZ;

	/* Monotone-chain hull of the XZ footprint. */
	qsort(pts, n, sizeof(SVECTOR), ftHullCompare);

	k = 0;

	for (i = 0; i < n; i++)
	{
		while (k >= 2 && ftHullCross(&out[k - 2], &out[k - 1], &pts[i]) <= 0)
			k--;

		out[k++] = pts[i];
	}

	lower = k + 1;

	for (i = n - 2; i >= 0; i--)
	{
		while (k >= lower && ftHullCross(&out[k - 2], &out[k - 1], &pts[i]) <= 0)
			k--;

		out[k++] = pts[i];
	}

	k--;	/* the last point repeats the first */

	if (k < 3)
	{
		/* Degenerate footprint (a line or a point): keep the source points. */
		k = n;
		for (i = 0; i < k; i++)
			out[i] = pts[i];
	}

	if (k > FT_HULL_MAX_VERTS)
		k = FT_HULL_MAX_VERTS;

	for (i = 0; i < k; i++)
	{
		hull->footprint[i] = out[i];
		hull->footprint[i].vy = 0;
	}

	hull->hullCount = k;

	FT_LOG("[fulltwist] hull slot=%d model=%p verts=%d size=(%d,%d,%d) centre=(%d,%d,%d) cog=(%d,%d,%d) colBox=(%d,%d,%d) footprint=%d\n",
		slot, (void*)mdl, hull->count, hull->hx * 2, hull->hy * 2, hull->hz * 2,
		cx, cy, cz, cogX, cogY, cogZ,
		(cos != NULL) ? cos->colBox.vx : 0,
		(cos != NULL) ? cos->colBox.vy : 0,
		(cos != NULL) ? cos->colBox.vz : 0,
		hull->hullCount);
}

FULLTWIST_HULL* ftHullForCar(const CAR_DATA* cp)
{
	int slot;
	MODEL* mdl;
	FULLTWIST_HULL* hull;

	if (cp == NULL)
		return NULL;

	slot = cp->ap.model;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;

	if (!JerCarSlotUsable(slot))
		return NULL;

	mdl = gCarCleanModelPtr[slot];

	if (mdl == NULL)
		return NULL;

	hull = &gFullTwist.hulls[slot];

	if (hull->model == mdl && hull->count > 0 && hull->modelVerts == mdl->num_vertices)
		return hull;

	ftHullBuild(hull, slot, mdl, cp->ap.carCos);

	return (hull->count > 0) ? hull : NULL;
}

int ftHullSupport(const FULLTWIST_HULL* hull, int nx, int ny, int nz, SVECTOR* out)
{
	int i;
	int best = -1;
	long long bestDot = 0;

	if (hull == NULL || hull->count <= 0 || out == NULL)
		return 0;

	for (i = 0; i < hull->count; i++)
	{
		long long dot = (long long)hull->verts[i].vx * nx +
						(long long)hull->verts[i].vy * ny +
						(long long)hull->verts[i].vz * nz;

		if (best < 0 || dot > bestDot)
		{
			best = i;
			bestDot = dot;
		}
	}

	if (best < 0)
		return 0;

	*out = hull->verts[best];
	return 1;
}

int ftHullContact(const FULLTWIST_HULL* hull, int px, int py, int pz,
				  int nx, int ny, int nz, SVECTOR* out)
{
	int i;
	int best = -1;
	int planeEps;
	long long bestDot = 0;
	long long bestDist = 0;

	if (hull == NULL || hull->count <= 0 || out == NULL)
		return 0;

	for (i = 0; i < hull->count; i++)
	{
		long long dot = (long long)hull->verts[i].vx * nx +
						(long long)hull->verts[i].vy * ny +
						(long long)hull->verts[i].vz * nz;

		if (best < 0 || dot > bestDot)
			bestDot = dot;
	}

	/* Vertices this close to the support plane are the face that was hit.
	 * Pick the one nearest the engine's reported point, so a broad side hit
	 * lands on the bumper that was actually struck rather than an arbitrary
	 * corner of the same face. */
	planeEps = (abs(nx) + abs(ny) + abs(nz)) * 24;

	for (i = 0; i < hull->count; i++)
	{
		long long dot = (long long)hull->verts[i].vx * nx +
						(long long)hull->verts[i].vy * ny +
						(long long)hull->verts[i].vz * nz;
		long long dx, dy, dz, dist;

		if (bestDot - dot > planeEps)
			continue;

		dx = (long long)hull->verts[i].vx - px;
		dy = (long long)hull->verts[i].vy - py;
		dz = (long long)hull->verts[i].vz - pz;
		dist = dx * dx + dy * dy + dz * dz;

		if (best < 0 || dist < bestDist)
		{
			best = i;
			bestDist = dist;
		}
	}

	if (best < 0)
		return ftHullSupport(hull, nx, ny, nz, out);

	*out = hull->verts[best];
	return 1;
}

/*
 * JER_OVERRIDE_SLOT_SCENERY_COLLISION handler.
 *
 * The stock check is 2D plus a coarse vertical gate, so a car passing under a
 * ceiling/overhang can be reported as a wall hit. This rebuilds the vertical
 * question from the actual car hull: if the hull's world Y span does not
 * intersect the building box's span, the hit is rejected. A real hit keeps the
 * stock normal but moves the contact point to the hull support, which gives the
 * stock response the correct height and lever.
 */
void ftOnSceneryCollision(JER_ARGS_SCENERY_COLLISION* args)
{
	CAR_DATA* cp;
	BUILDING_BOX* building;
	FULLTWIST_HULL* hull;
	VECTOR* point;
	VECTOR* normal;
	int m[3][3];
	int i;
	int as, ac;
	int bY0, bY1;
	int hullMinY, hullMaxY;

	if (args == NULL || !gFullTwist.cfg.enabled || !gFullTwist.cfg.scenery3d)
		return;

	cp = (CAR_DATA*)args->car;
	building = (BUILDING_BOX*)args->building;
	point = (VECTOR*)args->point;
	normal = (VECTOR*)args->normal;

	if (!ftIsTumbleCar(cp) || building == NULL || cp->ap.carCos == NULL)
		return;

	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(cp))
		return;

	hull = ftHullForCar(cp);

	if (hull == NULL || hull->count <= 0)
		return;

	for (i = 0; i < 3; i++)
	{
		m[i][0] = cp->hd.where.m[i][0];
		m[i][1] = cp->hd.where.m[i][1];
		m[i][2] = cp->hd.where.m[i][2];
	}

	as = RSIN(building->theta);
	ac = RCOS(building->theta);

	/* World Y span of the hull. */
	hullMinY = hullMaxY = cp->hd.where.t[1] +
		(int)(((long long)m[1][0] * hull->verts[0].vx +
			   (long long)m[1][1] * hull->verts[0].vy +
			   (long long)m[1][2] * hull->verts[0].vz) >> 12);

	for (i = 1; i < hull->count; i++)
	{
		int wy = cp->hd.where.t[1] +
			(int)(((long long)m[1][0] * hull->verts[i].vx +
				   (long long)m[1][1] * hull->verts[i].vy +
				   (long long)m[1][2] * hull->verts[i].vz) >> 12);

		if (wy < hullMinY) hullMinY = wy;
		if (wy > hullMaxY) hullMaxY = wy;
	}

	/* The engine's stock gate compares |carY + pos.vy|, so the building's
	 * vertical centre is -pos.vy. height is the full span. */
	bY0 = -building->pos.vy - (building->height >> 1);
	bY1 = -building->pos.vy + (building->height >> 1);

	if (hullMaxY < bY0 || hullMinY > bY1)
	{
		if (gFullTwist.cfg.debugLog)
		{
			FT_LOG("[fulltwist] scenery skip car=%d hullY=(%d,%d) buildingY=(%d,%d) pos=(%d,%d,%d) height=%d\n",
				(int)cp->id, hullMinY, hullMaxY, bY0, bY1,
				building->pos.vx, building->pos.vy, building->pos.vz, building->height);
		}

		args->handled = 1;
		return;
	}

	/* A real hit: move the point onto the hull's support along the building
	 * normal. The stock normal is kept -- it is the wall face, not the car. */
	if (point != NULL && normal != NULL)
	{
		int lx = (int)(((long long)normal->vx * m[0][0] +
						(long long)normal->vy * m[1][0] +
						(long long)normal->vz * m[2][0]) >> 12);
		int ly = (int)(((long long)normal->vx * m[0][1] +
						(long long)normal->vy * m[1][1] +
						(long long)normal->vz * m[2][1]) >> 12);
		int lz = (int)(((long long)normal->vx * m[0][2] +
						(long long)normal->vy * m[1][2] +
						(long long)normal->vz * m[2][2]) >> 12);
		SVECTOR contact;

		if (ftHullSupport(hull, lx, ly, lz, &contact))
		{
			point->vx = cp->hd.where.t[0] +
				(int)(((long long)m[0][0] * contact.vx +
					   (long long)m[0][1] * contact.vy +
					   (long long)m[0][2] * contact.vz) >> 12);
			point->vy = cp->hd.where.t[1] +
				(int)(((long long)m[1][0] * contact.vx +
					   (long long)m[1][1] * contact.vy +
					   (long long)m[1][2] * contact.vz) >> 12);
			point->vz = cp->hd.where.t[2] +
				(int)(((long long)m[2][0] * contact.vx +
					   (long long)m[2][1] * contact.vy +
					   (long long)m[2][2] * contact.vz) >> 12);
		}
	}

	if (gFullTwist.cfg.debugLog)
	{
		FT_LOG("[fulltwist] scenery hull car=%d hullY=(%d,%d) buildingY=(%d,%d) point=(%d,%d,%d)\n",
			(int)cp->id, hullMinY, hullMaxY, bY0, bY1,
			point ? point->vx : 0, point ? point->vy : 0, point ? point->vz : 0);
	}
}
