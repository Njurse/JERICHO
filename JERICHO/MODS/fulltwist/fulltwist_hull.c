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
#include "cosmetic.h"	/* car_cosmetics[] for the preemptive hull build */
#include "dr2roads.h"
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

/* Monotone-chain convex hull of a 2D point set (x = u, z = v). Sorts pts in
 * place and writes the hull to out; returns the vertex count. Shared by the
 * cached local footprint and the world-projected footprint used by the
 * scenery overlap gate. */
static int ftConvexFootprint(SVECTOR* pts, int n, SVECTOR* out)
{
	int i, k, lower;

	if (n < 1)
		return 0;

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

	return (k < 0) ? 0 : k;
}

static void ftHullBuild(FULLTWIST_HULL* hull, int slot, MODEL* mdl, CAR_COSMETICS* cos)
{
	SVECTOR* src;
	SVECTOR pts[FT_HULL_MAX_VERTS];
	SVECTOR out[FT_HULL_MAX_VERTS * 2];
	int n, i, k;
	int minX, maxX, minY, maxY, minZ, maxZ;
	int cx, cy, cz;
	int cogX, cogY, cogZ;

	memset(hull, 0, sizeof(*hull));
	hull->model = mdl;
	hull->slot = slot;
	hull->ready = 0;

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
	k = ftConvexFootprint(pts, n, out);

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
	hull->ready = (hull->count > 0) ? 1 : 0;

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

	if (hull->model == mdl && hull->modelVerts == mdl->num_vertices)
		return (hull->ready && hull->count > 0) ? hull : NULL;

	/* A resident model changed since the prebuild. Never build from a
	 * collision callback: request a rebuild at the next JER_EVENT_FRAME and
	 * fall back to the colBox for this contact. */
	if (!gFullTwist.hullsDirty)
		FT_LOG("[fulltwist] hull pending slot=%d (model changed; colBox fallback)\n", slot);

	gFullTwist.hullsDirty = 1;
	return NULL;
}

/* Build every usable resident hull at a safe point. Called from
 * JER_EVENT_FRAME after a level start or a model swap; no collision callback
 * ever calls ftHullBuild. */
void ftHullBuildAll(void)
{
	int slot;

	for (slot = 0; slot < MAX_CAR_RESIDENT_MODELS; slot++)
	{
		FULLTWIST_HULL* hull = &gFullTwist.hulls[slot];
		MODEL* mdl;

		if (!JerCarSlotUsable(slot))
		{
			FT_LOG("[fulltwist] hull skipped slot=%d (slot not usable)\n", slot);
			memset(hull, 0, sizeof(*hull));
			hull->slot = slot;
			hull->ready = 0;
			continue;
		}

		mdl = gCarCleanModelPtr[slot];

		if (mdl == NULL)
		{
			FT_LOG("[fulltwist] hull skipped slot=%d (no clean model)\n", slot);
			memset(hull, 0, sizeof(*hull));
			hull->slot = slot;
			hull->ready = 0;
			continue;
		}

		if (hull->model == mdl && hull->modelVerts == mdl->num_vertices && hull->ready)
			continue;

		ftHullBuild(hull, slot, mdl, &car_cosmetics[slot]);

		if (hull->ready)
		{
			FT_LOG("[fulltwist] hull ready slot=%d verts=%d size=(%d,%d,%d) footprint=%d\n",
				slot, hull->count, hull->hx * 2, hull->hy * 2, hull->hz * 2, hull->hullCount);
		}
		else
		{
			FT_LOG("[fulltwist] hull skipped slot=%d (build failed, model verts=%d)\n",
				slot, mdl->num_vertices);
		}
	}

	gFullTwist.hullsDirty = 0;
	gFullTwist.hullsBuilt = 1;
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

/* Lowest hull vertex in world space, for roof/side landings. The hull is
 * stored where-local about the collision-body centre, so the draw matrix is
 * enough to put the vertex back into the world. groundY is MapHeight at that
 * XZ; the caller compares it with out->vy to decide whether the body is on
 * the road. */
int ftHullLowest(const FULLTWIST_HULL* hull, const CAR_DATA* cp,
				 VECTOR* out, int* groundY, SVECTOR* localOut)
{
	int i;
	int best = -1;
	int bestY = 0;

	if (hull == NULL || cp == NULL || out == NULL || hull->count <= 0)
		return 0;

	for (i = 0; i < hull->count; i++)
	{
		const SVECTOR* v = &hull->verts[i];
		int wx = cp->hd.where.t[0] +
			(int)(((long long)cp->hd.where.m[0][0] * v->vx +
				   (long long)cp->hd.where.m[0][1] * v->vy +
				   (long long)cp->hd.where.m[0][2] * v->vz) >> 12);
		int wy = cp->hd.where.t[1] +
			(int)(((long long)cp->hd.where.m[1][0] * v->vx +
				   (long long)cp->hd.where.m[1][1] * v->vy +
				   (long long)cp->hd.where.m[1][2] * v->vz) >> 12);
		int wz = cp->hd.where.t[2] +
			(int)(((long long)cp->hd.where.m[2][0] * v->vx +
				   (long long)cp->hd.where.m[2][1] * v->vy +
				   (long long)cp->hd.where.m[2][2] * v->vz) >> 12);

		if (best < 0 || wy < bestY)
		{
			best = i;
			bestY = wy;
			out->vx = wx;
			out->vy = wy;
			out->vz = wz;

			if (localOut != NULL)
				*localOut = *v;
		}
	}

	if (best < 0)
		return 0;

	if (groundY != NULL)
		*groundY = MapHeight(out);

	return 1;
}

/* World-space Y span of the hull. */
int ftHullWorldSpan(const FULLTWIST_HULL* hull, const CAR_DATA* cp,
					int* minY, int* maxY)
{
	int i;
	int lo, hi;

	if (hull == NULL || cp == NULL || minY == NULL || maxY == NULL || hull->count <= 0)
		return 0;

	lo = hi = cp->hd.where.t[1] +
		(int)(((long long)cp->hd.where.m[1][0] * hull->verts[0].vx +
			   (long long)cp->hd.where.m[1][1] * hull->verts[0].vy +
			   (long long)cp->hd.where.m[1][2] * hull->verts[0].vz) >> 12);

	for (i = 1; i < hull->count; i++)
	{
		const SVECTOR* v = &hull->verts[i];
		int wy = cp->hd.where.t[1] +
			(int)(((long long)cp->hd.where.m[1][0] * v->vx +
				   (long long)cp->hd.where.m[1][1] * v->vy +
				   (long long)cp->hd.where.m[1][2] * v->vz) >> 12);

		if (wy < lo) lo = wy;
		if (wy > hi) hi = wy;
	}

	*minY = lo;
	*maxY = hi;
	return 1;
}

/* World-space hull contact for a hit at world point p with world normal n.
 * The hull is stored in the where-local collision-body frame, so the engine
 * point and normal are brought into that frame, ftHullContact picks the struck
 * face vertex, and the result is transformed back to world. The caller passes
 * the pre-negation (world-up) point from the scenery collision override. */
int ftHullContactWorld(const FULLTWIST_HULL* hull, const CAR_DATA* cp,
					   int px, int py, int pz, int nx, int ny, int nz,
					   VECTOR* out)
{
	int ax[3], ay[3], az[3];
	int rx, ry, rz;
	int lx, ly, lz;
	int lnx, lny, lnz;
	SVECTOR contact;

	if (hull == NULL || cp == NULL || out == NULL || hull->count <= 0)
		return 0;

	ftGetAxes(cp, ax, ay, az);

	rx = px - cp->hd.where.t[0];
	ry = py - cp->hd.where.t[1];
	rz = pz - cp->hd.where.t[2];

	lx = ftDotLocal(rx, ry, rz, ax);
	ly = ftDotLocal(rx, ry, rz, ay);
	lz = ftDotLocal(rx, ry, rz, az);

	lnx = ftDotLocal(nx, ny, nz, ax);
	lny = ftDotLocal(nx, ny, nz, ay);
	lnz = ftDotLocal(nx, ny, nz, az);

	if (!ftHullContact(hull, lx, ly, lz, lnx, lny, lnz, &contact))
		return 0;

	out->vx = cp->hd.where.t[0] +
		(int)(((long long)cp->hd.where.m[0][0] * contact.vx +
			   (long long)cp->hd.where.m[0][1] * contact.vy +
			   (long long)cp->hd.where.m[0][2] * contact.vz) >> 12);
	out->vy = cp->hd.where.t[1] +
		(int)(((long long)cp->hd.where.m[1][0] * contact.vx +
			   (long long)cp->hd.where.m[1][1] * contact.vy +
			   (long long)cp->hd.where.m[1][2] * contact.vz) >> 12);
	out->vz = cp->hd.where.t[2] +
		(int)(((long long)cp->hd.where.m[2][0] * contact.vx +
			   (long long)cp->hd.where.m[2][1] * contact.vy +
			   (long long)cp->hd.where.m[2][2] * contact.vz) >> 12);

	return 1;
}

/* SAT for a convex 2D polygon against the axis-aligned rectangle [-hw,hw] x
 * [-hh,hh] in building-local space. Returns 1 on overlap. */
static int ftSatAgainstRect(const SVECTOR* poly, int n,
							int ax, int az, int hw, int hh)
{
	int i;
	long long lo, hi, rect;

	if (ax == 0 && az == 0)
		return 1;

	lo = hi = (long long)poly[0].vx * ax + (long long)poly[0].vz * az;

	for (i = 1; i < n; i++)
	{
		long long d = (long long)poly[i].vx * ax + (long long)poly[i].vz * az;

		if (d < lo) lo = d;
		if (d > hi) hi = d;
	}

	rect = (long long)((ax < 0) ? -ax : ax) * hw +
		   (long long)((az < 0) ? -az : az) * hh;

	return !(hi < -rect || lo > rect);
}

/* 1 = the world-projected hull footprint overlaps the building box's XZ
 * rectangle. The box's xsize/zsize are half extents rotated by theta; the
 * margin matches the engine's +15 car-box inflation. The footprint is built
 * from the 3D hull at call time, so a tilted car projects correctly. */
int ftHullFootprintOverlapsBuilding(const FULLTWIST_HULL* hull, const CAR_DATA* cp,
									const BUILDING_BOX* building, int margin)
{
	SVECTOR pts[FT_HULL_MAX_VERTS];
	SVECTOR out[FT_HULL_MAX_VERTS * 2];
	int n, i, count;
	int as, ac;
	int hw, hh;

	if (hull == NULL || cp == NULL || building == NULL || hull->count <= 0)
		return 1;	/* cannot prove a miss: keep the engine's hit */

	n = hull->count;
	if (n > FT_HULL_MAX_VERTS)
		n = FT_HULL_MAX_VERTS;

	as = RSIN(building->theta);
	ac = RCOS(building->theta);
	hw = building->xsize + margin;
	hh = building->zsize + margin;

	for (i = 0; i < n; i++)
	{
		const SVECTOR* v = &hull->verts[i];
		int wx = cp->hd.where.t[0] +
			(int)(((long long)cp->hd.where.m[0][0] * v->vx +
				   (long long)cp->hd.where.m[0][1] * v->vy +
				   (long long)cp->hd.where.m[0][2] * v->vz) >> 12);
		int wz = cp->hd.where.t[2] +
			(int)(((long long)cp->hd.where.m[2][0] * v->vx +
				   (long long)cp->hd.where.m[2][1] * v->vy +
				   (long long)cp->hd.where.m[2][2] * v->vz) >> 12);
		int dx = wx - building->pos.vx;
		int dz = wz - building->pos.vz;

		/* bcollided2d's building axes: xsize runs along (sin, cos) and
		 * zsize along (cos, -sin). */
		pts[i].vx = (short)(((long long)dx * as + (long long)dz * ac) >> 12);
		pts[i].vy = 0;
		pts[i].vz = (short)(((long long)dx * ac - (long long)dz * as) >> 12);
		pts[i].pad = 0;
	}

	count = ftConvexFootprint(pts, n, out);

	if (count < 1)
		return 1;

	if (count < 3)
	{
		int minU = out[0].vx, maxU = out[0].vx;
		int minV = out[0].vz, maxV = out[0].vz;

		for (i = 1; i < count; i++)
		{
			if (out[i].vx < minU) minU = out[i].vx;
			if (out[i].vx > maxU) maxU = out[i].vx;
			if (out[i].vz < minV) minV = out[i].vz;
			if (out[i].vz > maxV) maxV = out[i].vz;
		}

		return !(maxU < -hw || minU > hw || maxV < -hh || minV > hh);
	}

	if (!ftSatAgainstRect(out, count, 4096, 0, hw, hh))
		return 0;

	if (!ftSatAgainstRect(out, count, 0, 4096, hw, hh))
		return 0;

	for (i = 0; i < count; i++)
	{
		int j = (i + 1) % count;
		int ax = out[j].vz - out[i].vz;
		int az = -(out[j].vx - out[i].vx);

		if (!ftSatAgainstRect(out, count, ax, az, hw, hh))
			return 0;
	}

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

	if (!ftHullWorldSpan(hull, cp, &hullMinY, &hullMaxY))
		return;

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

	/* Planar gate: the stock test is the car's colBox rectangle. Reject a hit
	 * whose world-projected hull does not actually reach the building. The
	 * +15 margin matches the engine's car-box inflation. */
	if (gFullTwist.cfg.hullFootprint &&
		!ftHullFootprintOverlapsBuilding(hull, cp, building, 15))
	{
		if (gFullTwist.cfg.debugLog)
		{
			FT_LOG("[fulltwist] scenery reject car=%d hullY=(%d,%d) buildingY=(%d,%d) point=(%d,%d,%d)\n",
				(int)cp->id, hullMinY, hullMaxY, bY0, bY1,
				point ? point->vx : 0, point ? point->vy : 0, point ? point->vz : 0);
		}

		args->handled = 1;
		return;
	}

	/* A real hit: the engine's stock response uses this point for its lever.
	 * The stock point sits at carY + 41 for every wall; replace it with the
	 * hull contact so the lever reflects where the car actually hit. This
	 * override runs before bcollide.c negates hit.vy, so write world-up Y. */
	if (gFullTwist.cfg.hullContact && point != NULL && normal != NULL)
	{
		VECTOR stock = *point;
		VECTOR contact;

		if (ftHullContactWorld(hull, cp, point->vx, point->vy, point->vz,
							   normal->vx, normal->vy, normal->vz, &contact))
		{
			/* Read-only. The engine's stock response expects its own contact
			 * frame; rewriting args->point was the crash fixed in 438fe59a.
			 * The hull contact is used by ftHandleCar for the FULL TWIST
			 * torque assist; the stock point is deliberately left untouched. */
			if (gFullTwist.cfg.debugLog)
			{
				FT_LOG("[fulltwist] scenery lever car=%d stock=(%d,%d,%d) hull=(%d,%d,%d) hullY=(%d,%d) buildingY=(%d,%d)\n",
					(int)cp->id, stock.vx, stock.vy, stock.vz,
					contact.vx, contact.vy, contact.vz,
					hullMinY, hullMaxY, bY0, bY1);
			}
		}
		else if (gFullTwist.cfg.debugLog)
		{
			FT_LOG("[fulltwist] scenery lever miss car=%d point=(%d,%d,%d) normal=(%d,%d,%d)\n",
				(int)cp->id, point->vx, point->vy, point->vz,
				normal->vx, normal->vy, normal->vz);
		}
	}
	else if (gFullTwist.cfg.debugLog)
	{
		FT_LOG("[fulltwist] scenery hull car=%d hullY=(%d,%d) buildingY=(%d,%d) point=(%d,%d,%d)\n",
			(int)cp->id, hullMinY, hullMaxY, bY0, bY1,
			point ? point->vx : 0, point ? point->vy : 0, point ? point->vz : 0);
	}

	/* Final hand-off to the engine: this point is what bcollide.c will use
	 * for the lever and the stock response. Log it so a crash inside that
	 * response names the exact frame and contact. */
	if (gFullTwist.cfg.debugLog)
	{
		FT_LOG("[fulltwist] scenery response car=%d point=(%d,%d,%d) hullContact=%d footprint=%d\n",
			(int)cp->id, point ? point->vx : 0, point ? point->vy : 0, point ? point->vz : 0,
			gFullTwist.cfg.hullContact, gFullTwist.cfg.hullFootprint);
	}
}
