// ai/grid.c — Combat D2 off-road drivability grid + A*. See ai/grid.h.
//
// Window-based: for a start/goal pair we rasterise a bounded grid (at most
// CD2_GRID_MAXDIM x CD2_GRID_MAXDIM cells of CD2_GRID_CELL units) covering the
// two points plus a margin, then run an octile A* over it. This keeps memory
// tiny and needs no global level bounds.

#include "driver2.h"
#include "objcoll.h"
#include "dr2roads.h"
#include "dr2math.h"
#include "combatd2.h"
#include "ai/grid.h"

#include <string.h>

#define CD2_GRID_CELL		512	// world units per cell
#define CD2_GRID_MAXDIM		64	// cells per axis (window cap)
#define CD2_GRID_MAXCELLS	(CD2_GRID_MAXDIM * CD2_GRID_MAXDIM)
#define CD2_GRID_MARGIN		6	// cells of margin around the bbox
#define CD2_GRID_MAXSTEP	700	// max height change between neighbours (units)
#define CD2_GRID_SAMPLE		160	// clearance radius tested per cell

// cell state
enum { CD2_GRID_UNKNOWN = 0, CD2_GRID_CLEAR = 1, CD2_GRID_BLOCKED = 2 };

static unsigned char sCell[CD2_GRID_MAXCELLS];	// CD2_GRID_* state
static int  sCellY[CD2_GRID_MAXCELLS];		// MapHeight per cell
static char sOnRoad[CD2_GRID_MAXCELLS];		// 1 when the cell is on a road surface

static int  sG[CD2_GRID_MAXCELLS];
static int  sF[CD2_GRID_MAXCELLS];
static short sParent[CD2_GRID_MAXCELLS];
static char sClosed[CD2_GRID_MAXCELLS];
static int  sOpen[CD2_GRID_MAXCELLS];
static int  sOpenSize;

static int sDimX, sDimZ;
static int sOriginX, sOriginZ;			// world (x,z) of cell (0,0) centre

// last search, for the debug draw
static int sHaveLast;
static int sLastCount;
static int sLastPath[CD2_GRID_MAXCELLS];
static int sLastFromX, sLastFromZ, sLastToX, sLastToZ;

static int cd2GridIsqrt(int v)
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

static int cd2GridDist2D(int ax, int az, int bx, int bz)
{
	int dx = bx - ax;
	int dz = bz - az;

	return cd2GridIsqrt(dx * dx + dz * dz);
}

static int cd2GridCellX(int wx)
{
	int x = (wx - sOriginX + CD2_GRID_CELL / 2) / CD2_GRID_CELL;

	if (x < 0) x = 0;
	if (x >= sDimX) x = sDimX - 1;

	return x;
}

static int cd2GridCellZ(int wz)
{
	int z = (wz - sOriginZ + CD2_GRID_CELL / 2) / CD2_GRID_CELL;

	if (z < 0) z = 0;
	if (z >= sDimZ) z = sDimZ - 1;

	return z;
}

static int cd2GridWorldX(int cx)
{
	return sOriginX + cx * CD2_GRID_CELL;
}

static int cd2GridWorldZ(int cz)
{
	return sOriginZ + cz * CD2_GRID_CELL;
}

// Rasterise one cell (memoised).
static void cd2GridEval(int cx, int cz)
{
	int idx = cz * sDimX + cx;
	VECTOR p;

	if (sCell[idx] != CD2_GRID_UNKNOWN)
		return;

	p.vx = cd2GridWorldX(cx);
	p.vz = cd2GridWorldZ(cz);
	p.vy = 0;

	sCellY[idx] = MapHeight(&p);
	sOnRoad[idx] = IS_ROAD_SURFACE(GetSurfaceIndex(&p)) ? 1 : 0;

	// clear of scenery objects?
	if (!CellAtPositionEmpty(&p, CD2_GRID_SAMPLE))
	{
		sCell[idx] = CD2_GRID_BLOCKED;
		return;
	}

	sCell[idx] = CD2_GRID_CLEAR;
}

// Neighbour passability: clear, not blocked, and no big height step or wall
// between the two cell centres.
static int cd2GridStep(int aidx, int bidx)
{
	VECTOR a, b;
	int acx, acz, bcx, bcz;

	if (sCell[aidx] != CD2_GRID_CLEAR || sCell[bidx] != CD2_GRID_CLEAR)
		return 0;

	if (ABS(sCellY[aidx] - sCellY[bidx]) > CD2_GRID_MAXSTEP)
		return 0;

	acz = aidx / sDimX; acx = aidx - acz * sDimX;
	bcz = bidx / sDimX; bcx = bidx - bcz * sDimX;

	a.vx = cd2GridWorldX(acx); a.vz = cd2GridWorldZ(acz); a.vy = sCellY[aidx];
	b.vx = cd2GridWorldX(bcx); b.vz = cd2GridWorldZ(bcz); b.vy = sCellY[bidx];

	return lineClear(&a, &b) ? 1 : 0;
}

static void cd2GridHeapPush(int node)
{
	int i = sOpenSize++;

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

static int cd2GridHeapPop(void)
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

// octile distance heuristic in cell units
static int cd2GridHeur(int ax, int az, int bx, int bz)
{
	int dx = ABS(ax - bx);
	int dz = ABS(az - bz);

	return 1000 * (dx + dz) + 414 * ((dx < dz) ? dx : dz);
}

int cd2GridPath(const VECTOR* from, const VECTOR* to, VECTOR* outWp, int maxWp, int* expanded)
{
	int minX, minZ, maxX, maxZ;
	int i, n = 0;
	int startIdx, goalIdx, cur;
	int scx, scz, gcx, gcz;

	*expanded = 0;
	sHaveLast = 0;

	if (from == NULL || to == NULL || maxWp <= 0)
		return 0;

	// window: bbox of from/to plus margin, at most CD2_GRID_MAXDIM per axis
	minX = (from->vx < to->vx) ? from->vx : to->vx;
	maxX = (from->vx > to->vx) ? from->vx : to->vx;
	minZ = (from->vz < to->vz) ? from->vz : to->vz;
	maxZ = (from->vz > to->vz) ? from->vz : to->vz;

	minX -= CD2_GRID_MARGIN * CD2_GRID_CELL;
	maxX += CD2_GRID_MARGIN * CD2_GRID_CELL;
	minZ -= CD2_GRID_MARGIN * CD2_GRID_CELL;
	maxZ += CD2_GRID_MARGIN * CD2_GRID_CELL;

	sDimX = (maxX - minX) / CD2_GRID_CELL + 1;
	sDimZ = (maxZ - minZ) / CD2_GRID_CELL + 1;

	if (sDimX > CD2_GRID_MAXDIM) sDimX = CD2_GRID_MAXDIM;
	if (sDimZ > CD2_GRID_MAXDIM) sDimZ = CD2_GRID_MAXDIM;
	if (sDimX < 2 || sDimZ < 2)
		return 0;

	sOriginX = minX;
	sOriginZ = minZ;

	memset(sCell, 0, (size_t)sDimX * sDimZ);

	scx = cd2GridCellX(from->vx); scz = cd2GridCellZ(from->vz);
	gcx = cd2GridCellX(to->vx);   gcz = cd2GridCellZ(to->vz);

	startIdx = scz * sDimX + scx;
	goalIdx = gcz * sDimX + gcx;

	cd2GridEval(scx, scz);
	cd2GridEval(gcx, gcz);

	// The car can be sitting right against scenery, which would block its own
	// cell; nudge start (like the goal below) to the nearest clear neighbour.
	if (sCell[startIdx] != CD2_GRID_CLEAR)
	{
		int dx, dz;
		int found = 0;

		for (dz = -1; dz <= 1 && !found; dz++)
			for (dx = -1; dx <= 1 && !found; dx++)
			{
				int nx = scx + dx, nz = scz + dz;

				if (nx < 0 || nz < 0 || nx >= sDimX || nz >= sDimZ)
					continue;

				cd2GridEval(nx, nz);

				if (sCell[nz * sDimX + nx] == CD2_GRID_CLEAR)
				{
					startIdx = nz * sDimX + nx;
					scx = nx;
					scz = nz;
					found = 1;
				}
			}

		if (!found)
			return 0;
	}

	// endpoints must be usable (if the goal cell is blocked, nudge to a clear
	// neighbour so a route can still end next to it)
	if (sCell[goalIdx] != CD2_GRID_CLEAR)
	{
		int dx, dz;
		int found = 0;

		for (dz = -1; dz <= 1 && !found; dz++)
			for (dx = -1; dx <= 1 && !found; dx++)
			{
				int nx = gcx + dx, nz = gcz + dz;

				if (nx < 0 || nz < 0 || nx >= sDimX || nz >= sDimZ)
					continue;

				cd2GridEval(nx, nz);

				if (sCell[nz * sDimX + nx] == CD2_GRID_CLEAR)
				{
					goalIdx = nz * sDimX + nx;
					found = 1;
				}
			}

		if (!found)
			return 0;
	}

	if (sCell[startIdx] != CD2_GRID_CLEAR)
		return 0;

	if (sCell[startIdx] != CD2_GRID_CLEAR)
		return 0;

	for (i = 0; i < sDimX * sDimZ; i++)
	{
		sG[i] = 0x7fffffff;
		sF[i] = 0x7fffffff;
		sParent[i] = -1;
		sClosed[i] = 0;
	}

	sOpenSize = 0;
	sG[startIdx] = 0;
	sF[startIdx] = cd2GridHeur(scx, scz, gcx, gcz);
	cd2GridHeapPush(startIdx);

	cur = startIdx;

	while (sOpenSize > 0)
	{
		int cx, cz, dz;

		cur = cd2GridHeapPop();

		if (cur == goalIdx)
			break;

		if (sClosed[cur])
			continue;

		sClosed[cur] = 1;
		(*expanded)++;

		cz = cur / sDimX;
		cx = cur - cz * sDimX;

		for (dz = -1; dz <= 1; dz++)
		{
			int dx;

			for (dx = -1; dx <= 1; dx++)
			{
				int nx = cx + dx, nz = cz + dz;
				int nidx, cost;
				int diagonal = (dx != 0 && dz != 0);

				if ((dx == 0 && dz == 0) || nx < 0 || nz < 0 || nx >= sDimX || nz >= sDimZ)
					continue;

				nidx = nz * sDimX + nx;
				cd2GridEval(nx, nz);

				if (sClosed[nidx] || !cd2GridStep(cur, nidx))
					continue;

				cost = sG[cur] + (diagonal ? 1448 : 1000) + (sOnRoad[nidx] ? 0 : 250);

				if (cost < sG[nidx])
				{
					sG[nidx] = cost;
					sParent[nidx] = (short)cur;
					sF[nidx] = cost + cd2GridHeur(nx, nz, gcx, gcz);
					cd2GridHeapPush(nidx);
				}
			}
		}
	}

	if (cur != goalIdx)
		return 0;

	// walk back, then sample every few cells into world waypoints (reversed)
	{
		int tmp[CD2_GRID_MAXCELLS];
		int cnt = 0;
		int step;

		for (i = goalIdx; i != startIdx && i >= 0 && cnt < CD2_GRID_MAXCELLS; i = sParent[i])
			tmp[cnt++] = i;

		if (i != startIdx)
			return 0;

		sLastCount = cnt;
		for (i = 0; i < cnt; i++)
			sLastPath[i] = tmp[i];
		sHaveLast = 1;
		sLastFromX = from->vx; sLastFromZ = from->vz;
		sLastToX = to->vx; sLastToZ = to->vz;

		// 1 cell between waypoints, but never more than maxWp-1 of them
		step = 1;
		while (cnt / step > maxWp - 1)
			step++;

		for (i = cnt - 1; i >= 0; i -= step)
		{
			VECTOR p;
			int idx = tmp[i];
			int cz = idx / sDimX;
			int cx = idx - cz * sDimX;

			p.vx = cd2GridWorldX(cx);
			p.vz = cd2GridWorldZ(cz);
			p.vy = sCellY[idx];

			outWp[n++] = p;

			if (n >= maxWp)
				break;
		}
	}

	// ensure the true goal is the final waypoint
	if (n == 0 || outWp[n - 1].vx != to->vx || outWp[n - 1].vz != to->vz)
	{
		if (n < maxWp)
			outWp[n++] = *to;
		else
			outWp[maxWp - 1] = *to;
	}

	return n;
}

// ---- debug draw ----------------------------------------------------------

#ifndef PSX
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
#endif

void cd2GridDraw(void)
{
#ifndef PSX
	CVECTOR colClear = { 40, 70, 40 };
	CVECTOR colBlock = { 120, 40, 40 };
	CVECTOR colPath = { 250, 60, 250 };
	int cx, cz, i;

	if (!sHaveLast)
		return;

	for (cz = 0; cz < sDimZ; cz++)
	{
		for (cx = 0; cx < sDimX; cx++)
		{
			int idx = cz * sDimX + cx;
			VECTOR a, b;

			if (sCell[idx] == CD2_GRID_UNKNOWN)
				continue;

			a.vx = cd2GridWorldX(cx) + CD2_GRID_CELL / 2 - 100;
			a.vz = cd2GridWorldZ(cz);
			a.vy = sCellY[idx] + 30;
			b.vx = a.vx - CD2_GRID_CELL / 2 + 100;
			b.vz = a.vz;
			b.vy = a.vy;

			Debug_AddLineDepth(a, b, (sCell[idx] == CD2_GRID_CLEAR) ? colClear : colBlock);
		}
	}

	for (i = 0; i < sLastCount; i++)
	{
		int idx = sLastPath[i];
		int pcz = idx / sDimX;
		int pcx = idx - pcz * sDimX;
		VECTOR a;

		a.vx = cd2GridWorldX(pcx);
		a.vz = cd2GridWorldZ(pcz);
		a.vy = sCellY[idx] + 260;

		{
			VECTOR b = a;
			b.vy = a.vy + 200;
			Debug_AddLineDepth(a, b, colPath);
		}
	}
#endif
}
