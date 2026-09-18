// ai/flow.c — Combat D2 shared pursuit flow field. See ai/flow.h.
//
// A 96x96 window of 256-unit cells (shift-based: >>8 / <<8) holding a
// fast-marching distance to the goal, propagated from the goal outward with a
// binary heap and a per-frame budget (like the engine's cop map, but shared by
// every opponent and re-seeded from a moving goal). When the goal moves more
// than a cell the field is re-seeded from the new goal cell and re-propagates
// over the following frames; a large move re-centres the whole window.

#include "driver2.h"
#include "objcoll.h"
#include "dr2roads.h"
#include "dr2math.h"
#include "cainescrossfire.h"
#include "ai/flow.h"

#include <string.h>

#define CD2_FLOW_CELL		256
#define CD2_FLOW_SHIFT		8	// 256 = 1 << 8
#define CD2_FLOW_DIM		96
#define CD2_FLOW_CELLS		(CD2_FLOW_DIM * CD2_FLOW_DIM)
#define CD2_FLOW_HEAP		(CD2_FLOW_CELLS * 4)	// slack for duplicate heap pushes
#define CD2_FLOW_UNSET		0xFFFF
#define CD2_FLOW_RECENTRE	2500	// goal move that re-centres the window
#define CD2_FLOW_BUDGET		64	// default cells propagated per frame
#define CD2_FLOW_RESEED_EVERY	8	// min frames between goal-cell re-seeds
#define CD2_FLOW_SAMPLE		120	// clearance radius per cell

static unsigned short sDist[CD2_FLOW_CELLS];
static unsigned char  sState[CD2_FLOW_CELLS];	// 0 unknown, 1 clear, 2 blocked
static int sOriginX, sOriginZ;			// world (x,z) of cell (0,0) corner
static int sGoalX, sGoalZ;
static int sValidSeeded;
static int sSeedCellX = -99999, sSeedCellZ = -99999;
static int sSinceSeed;		// frames since the last re-seed (rate limit)

static int sHeap[CD2_FLOW_HEAP];
static int sHeapSize;
static int sPropagated;	// cells with a known distance (field coverage)

static int cd2FlowClear(int wx, int wz)
{
	int cx = (wx - sOriginX) >> CD2_FLOW_SHIFT;
	int cz = (wz - sOriginZ) >> CD2_FLOW_SHIFT;
	int idx;
	VECTOR p;

	if (cx < 0 || cz < 0 || cx >= CD2_FLOW_DIM || cz >= CD2_FLOW_DIM)
		return 0;

	idx = cz * CD2_FLOW_DIM + cx;

	if (sState[idx] != 0)
		return sState[idx] == 1;

	p.vx = sOriginX + (cx << CD2_FLOW_SHIFT) + CD2_FLOW_CELL / 2;
	p.vz = sOriginZ + (cz << CD2_FLOW_SHIFT) + CD2_FLOW_CELL / 2;
	p.vy = 0;

	sState[idx] = CellAtPositionEmpty(&p, CD2_FLOW_SAMPLE) ? 1 : 2;

	return sState[idx] == 1;
}

static void cd2FlowHeapPush(int idx)
{
	int i = sHeapSize++;

	if (i >= CD2_FLOW_HEAP)
	{
		sHeapSize = CD2_FLOW_HEAP;
		return;	// heap full: drop this relaxation
	}

	sHeap[i] = idx;

	while (i > 0)
	{
		int p = (i - 1) / 2;

		if (sDist[sHeap[p]] <= sDist[sHeap[i]])
			break;

		{
			int t = sHeap[p];
			sHeap[p] = sHeap[i];
			sHeap[i] = t;
		}

		i = p;
	}
}

static int cd2FlowHeapPop(void)
{
	int top = sHeap[0];
	int i = 0;

	sHeap[0] = sHeap[--sHeapSize];

	for (;;)
	{
		int l = i * 2 + 1;
		int r = l + 1;
		int m = i;

		if (l < sHeapSize && sDist[sHeap[l]] < sDist[sHeap[m]])
			m = l;

		if (r < sHeapSize && sDist[sHeap[r]] < sDist[sHeap[m]])
			m = r;

		if (m == i)
			break;

		{
			int t = sHeap[m];
			sHeap[m] = sHeap[i];
			sHeap[i] = t;
		}

		i = m;
	}

	return top;
}

static void cd2FlowSeed(int goalX, int goalZ)
{
	int cx = (goalX - sOriginX) >> CD2_FLOW_SHIFT;
	int cz = (goalZ - sOriginZ) >> CD2_FLOW_SHIFT;
	int idx;

	sHeapSize = 0;
	memset(sDist, 0xFF, sizeof(sDist));	// all UNSET

	if (cx < 0 || cz < 0 || cx >= CD2_FLOW_DIM || cz >= CD2_FLOW_DIM)
	{
		sValidSeeded = 0;
		return;
	}

	idx = cz * CD2_FLOW_DIM + cx;
	sDist[idx] = 0;
	sPropagated = 1;
	cd2FlowHeapPush(idx);
	sValidSeeded = 1;
}

void cd2FlowSetGoal(const VECTOR* goal)
{
	if (goal == NULL)
		return;

	if (!sValidSeeded || ABS(sGoalX - goal->vx) > CD2_FLOW_RECENTRE ||
	    ABS(sGoalZ - goal->vz) > CD2_FLOW_RECENTRE)
	{
		// re-centre the window on the goal and start over
		sOriginX = goal->vx - (CD2_FLOW_DIM / 2) * CD2_FLOW_CELL;
		sOriginZ = goal->vz - (CD2_FLOW_DIM / 2) * CD2_FLOW_CELL;
		memset(sState, 0, sizeof(sState));

		sGoalX = goal->vx;
		sGoalZ = goal->vz;
		cd2FlowSeed(goal->vx, goal->vz);
		sSeedCellX = (goal->vx - sOriginX) >> CD2_FLOW_SHIFT;
		sSeedCellZ = (goal->vz - sOriginZ) >> CD2_FLOW_SHIFT;
		sSinceSeed = 0;
		return;
	}

	sGoalX = goal->vx;
	sGoalZ = goal->vz;
	sSinceSeed++;

	// Re-seed only when the goal has actually entered a new cell, and at most
	// every CD2_FLOW_RESEED_EVERY frames - otherwise a fast-moving goal would
	// reset the field every frame and it would never propagate.
	{
		int cx = (goal->vx - sOriginX) >> CD2_FLOW_SHIFT;
		int cz = (goal->vz - sOriginZ) >> CD2_FLOW_SHIFT;

		if ((cx != sSeedCellX || cz != sSeedCellZ) && sSinceSeed >= CD2_FLOW_RESEED_EVERY)
		{
			cd2FlowSeed(goal->vx, goal->vz);
			sSeedCellX = cx;
			sSeedCellZ = cz;
			sSinceSeed = 0;
		}
	}
}

void cd2FlowUpdate(int budget)
{
	while (budget-- > 0 && sHeapSize > 0)
	{
		int idx = cd2FlowHeapPop();
		int cz = idx / CD2_FLOW_DIM;
		int cx = idx - cz * CD2_FLOW_DIM;
		int dz, dx;
		unsigned short base = sDist[idx];

		for (dz = -1; dz <= 1; dz++)
		{
			for (dx = -1; dx <= 1; dx++)
			{
				int nx = cx + dx, nz = cz + dz;
				int nidx, step;
				unsigned short nd;

				if ((dx == 0 && dz == 0) || nx < 0 || nz < 0 ||
				    nx >= CD2_FLOW_DIM || nz >= CD2_FLOW_DIM)
					continue;

				nidx = nz * CD2_FLOW_DIM + nx;

				if (sState[nidx] == 2)
					continue;		// known blocked

				step = (dx != 0 && dz != 0) ? 362 : 256;
				nd = base + step;

				if (nd < sDist[nidx])
				{
					if (sDist[nidx] == CD2_FLOW_UNSET)
						sPropagated++;

					sDist[nidx] = nd;
					cd2FlowHeapPush(nidx);
				}
			}
		}
	}
}

int cd2FlowReady(void)
{
	return (sValidSeeded && sPropagated > 1) ? 1 : 0;
}

int cd2FlowCoverage(void)
{
	return sPropagated;
}

int cd2FlowDir(const VECTOR* from, int* outHeading)
{
	int cx, cz, idx;
	int best = -1, bestDist = CD2_FLOW_UNSET;
	int dz, dx;

	if (from == NULL || outHeading == NULL || !sValidSeeded)
		return 0;

	cx = (from->vx - sOriginX) >> CD2_FLOW_SHIFT;
	cz = (from->vz - sOriginZ) >> CD2_FLOW_SHIFT;

	if (cx < 0 || cz < 0 || cx >= CD2_FLOW_DIM || cz >= CD2_FLOW_DIM)
		return 0;

	idx = cz * CD2_FLOW_DIM + cx;

	if (sDist[idx] == CD2_FLOW_UNSET)
		return 0;

	for (dz = -1; dz <= 1; dz++)
	{
		for (dx = -1; dx <= 1; dx++)
		{
			int nx = cx + dx, nz = cz + dz;
			int nidx;

			if ((dx == 0 && dz == 0) || nx < 0 || nz < 0 ||
			    nx >= CD2_FLOW_DIM || nz >= CD2_FLOW_DIM)
				continue;

			nidx = nz * CD2_FLOW_DIM + nx;

			if (sDist[nidx] < bestDist)
			{
				bestDist = sDist[nidx];
				best = nidx;
			}
		}
	}

	if (best < 0 || bestDist == CD2_FLOW_UNSET)
		return 0;

	{
		int bz = best / CD2_FLOW_DIM;
		int bx = best - bz * CD2_FLOW_DIM;

		*outHeading = ratan2((bx - cx) << CD2_FLOW_SHIFT, (bz - cz) << CD2_FLOW_SHIFT);
	}

	return 1;
}

// ---- debug draw ----------------------------------------------------------

#ifndef PSX
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
#endif

void cd2FlowDraw(void)
{
#ifndef PSX
	CVECTOR col = { 60, 200, 120 };
	int cx, cz;

	if (!sValidSeeded)
		return;

	for (cz = 0; cz < CD2_FLOW_DIM; cz++)
	{
		for (cx = 0; cx < CD2_FLOW_DIM; cx++)
		{
			int idx = cz * CD2_FLOW_DIM + cx;
			VECTOR a, b;
			int head;
			VECTOR p;

			if (sDist[idx] == CD2_FLOW_UNSET)
				continue;

			p.vx = sOriginX + (cx << CD2_FLOW_SHIFT) + CD2_FLOW_CELL / 2;
			p.vz = sOriginZ + (cz << CD2_FLOW_SHIFT) + CD2_FLOW_CELL / 2;

			if (!cd2FlowDir(&p, &head))
				continue;

			a.vx = p.vx;
			a.vz = p.vz;
			a.vy = MapHeight(&a) + 40;
			b.vx = p.vx + (int)(((long long)RSIN(head) * 150) >> 12);
			b.vz = p.vz + (int)(((long long)RCOS(head) * 150) >> 12);
			b.vy = a.vy;

			Debug_AddLineDepth(a, b, col);
		}
	}
#endif
}
