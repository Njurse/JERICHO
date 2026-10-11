/*
 * jerai_local.c -- one flood of the window, two rungs of the answer. See jerai_local.h.
 *
 * Lifted from mp/ai/ailocal.c and namespaced (JerAi*).
 *
 * Static scratch rather than stack: this runs from the game loop, the arrays are sized
 * to the grid, and the module is single-threaded (the same reasoning as jerai_star's node
 * pool). Nothing is allocated and nothing grows.
 */

#include <string.h>

#include "jerai_map.h"
#include "jerai_local.h"

#define JERAI_LOCAL_NODES	(JERAI_MAP_SIZE * JERAI_MAP_SIZE)

static unsigned char sSeen[JERAI_LOCAL_NODES];
static int sDist[JERAI_LOCAL_NODES];
static int sQueue[JERAI_LOCAL_NODES];

static void JerAiLocalFail(JERAI_GOAL* out)
{
	if (out == NULL)
		return;

	out->kind = JERAI_GOAL_NONE;
	out->x = 0;
	out->z = 0;
	out->samples = 0;
	out->clearance = 0;
}

/* ------------------------------------------------------------------ */
/* One flood of the window, shared by every rung below                 */
/* ------------------------------------------------------------------ */
/* Breadth-first from the car's own sample, over the samples a car can actually drive
 * between, writing sSeen[]/sDist[] and the BFS-ordered sQueue[] (so the queue is also
 * "nearest first"). Both rungs read the same result: "reachable" has to mean ONE thing.
 *
 * The corner-cut rule is not decoration: a diagonal step between two blocked samples is
 * a gap narrower than a car, and letting the flood leak through one would call a road
 * "reachable" that the car cannot drive to. */
static int JerAiLocalFlood(const JERAI_MAP* map, int sx, int sz)
{
	int head = 0, tail = 0;
	int start = sz * JERAI_MAP_SIZE + sx;
	int dx, dz;

	memset(sSeen, 0, sizeof(sSeen));

	/* the start must be a real sample: an out-of-range start would index sSeen out of
	 * bounds on the very first write. */
	if (sx < 0 || sz < 0 || sx >= JERAI_MAP_SIZE || sz >= JERAI_MAP_SIZE)
		return 0;

	sSeen[start] = 1;
	sDist[start] = 0;
	sQueue[tail++] = start;

	while (head < tail)
	{
		int cur = sQueue[head++];
		int cx = cur % JERAI_MAP_SIZE;
		int cz = cur / JERAI_MAP_SIZE;
		int d = sDist[cur];

		for (dz = -1; dz <= 1; dz++)
		{
			for (dx = -1; dx <= 1; dx++)
			{
				int nx, nz, n;

				if (dx == 0 && dz == 0)
					continue;

				nx = cx + dx;
				nz = cz + dz;

				if (nx < 0 || nz < 0 || nx >= JERAI_MAP_SIZE || nz >= JERAI_MAP_SIZE)
					continue;

				n = nz * JERAI_MAP_SIZE + nx;

				if (sSeen[n])
					continue;

				if (JerAiMapBlocked(map, nx, nz))
					continue;

				if (dx != 0 && dz != 0 &&
					(JerAiMapBlocked(map, cx + dx, cz) || JerAiMapBlocked(map, cx, cz + dz)))
					continue;

				sSeen[n] = 1;
				sDist[n] = d + 1;
				sQueue[tail++] = n;
			}
		}
	}

	return tail;
}

int JerAiLocalGoal(const JERAI_MAP* map, int fromX, int fromZ, JERAI_GOAL* out)
{
	int six, siz, q, reached;
	int bestClear = -1, bestIx = -1, bestIz = -1, bestRing = 0;

	if (out == NULL)
		return 0;

	JerAiLocalFail(out);

	if (!JerAiMapValid(map))
		return 0;

	if (!JerAiMapSampleIndex(map, fromX, fromZ, &six, &siz))
		return 0;

	/* the car is very often inside a "blocked" sample - that is what a probe radius the
	 * width of a car does when the car is near a wall - so walk out before flooding */
	if (!JerAiMapNearestOpen(map, six, siz, &six, &siz))
		return 0;

	reached = JerAiLocalFlood(map, six, siz);

	for (q = 0; q < reached; q++)
	{
		int cur = sQueue[q];
		int cx = cur % JERAI_MAP_SIZE;
		int cz = cur / JERAI_MAP_SIZE;
		int d = sDist[cur];

		/* RUNG 1. Breadth-first, so the first road sample reached is the nearest
		 * reachable one - nearest along the ground the car can drive. */
		if (JerAiMapRoad(map, cx, cz))
		{
			JerAiMapWorld(map, cx, cz, &out->x, &out->z);
			out->kind = JERAI_GOAL_ROAD;
			out->samples = d;
			out->clearance = JerAiMapClearance(map, cx, cz);

			return 1;
		}

		/* RUNG 2, gathered as we go: the most open ground, and (because BFS) the
		 * nearest of those when several are equally open. */
		if (JerAiMapClearance(map, cx, cz) > bestClear)
		{
			bestClear = JerAiMapClearance(map, cx, cz);
			bestIx = cx;
			bestIz = cz;
			bestRing = d;
		}
	}

	/* No road is reachable from here. Room to move still beats standing still. */
	if (bestIx >= 0)
	{
		JerAiMapWorld(map, bestIx, bestIz, &out->x, &out->z);
		out->kind = JERAI_GOAL_OPEN;
		out->samples = bestRing;
		out->clearance = bestClear;

		return 1;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* The other question the local search answers: where to RUN to        */
/* ------------------------------------------------------------------ */

/* How far out a flee goal is looked for, in samples. */
#define JERAI_LOCAL_FLEE_RING	16

/* The road bonus has to stay COMPARABLE TO THE DISTANCE RANGE: too small and the flee
 * drifts off the road, too big and it drives straight AT the threat to reach a road. */
#define JERAI_LOCAL_ROAD_BONUS	120

int JerAiLocalFleeGoal(const JERAI_MAP* map, int fromX, int fromZ, int threatX, int threatZ, JERAI_GOAL* out)
{
	int cix, ciz, tix, tiz, ix, iz, q, reached, best = -1, bestScore = 0, bestClear = 0;

	if (out == NULL)
		return 0;

	JerAiLocalFail(out);

	if (!JerAiMapValid(map))
		return 0;

	if (!JerAiMapSampleIndex(map, fromX, fromZ, &cix, &ciz))
		return 0;

	if (!JerAiMapNearestOpen(map, cix, ciz, &cix, &ciz))
		return 0;

	/* the threat may be off the window: then treat it as here, which makes "away" mean
	 * "the far side of the window" and still gives the flee a direction */
	if (!JerAiMapSampleIndex(map, threatX, threatZ, &tix, &tiz))
	{
		tix = cix;
		tiz = ciz;
	}

	/* REACHABILITY, not line of sight: the flood answers "can the car actually drive
	 * there" for every candidate at once, and it is the SAME flood JerAiLocalGoal uses,
	 * so the road rung and the flee rung agree about the map. */
	reached = JerAiLocalFlood(map, cix, ciz);

	for (q = 0; q < reached; q++)
	{
		int score, dx, dz;
		int cur = sQueue[q];
		long away;

		ix = cur % JERAI_MAP_SIZE;
		iz = cur / JERAI_MAP_SIZE;

		dx = ix - cix;
		dz = iz - ciz;

		if (dx < 0) dx = -dx;
		if (dz < 0) dz = -dz;

		if (dx < JERAI_LOCAL_FLEE_RING / 2 && dz < JERAI_LOCAL_FLEE_RING / 2)
			continue;	/* it has to be somewhere worth going: near-here is not a flee */

		if (dx > JERAI_LOCAL_FLEE_RING || dz > JERAI_LOCAL_FLEE_RING)
			continue;	/* too far to be one plan's worth of driving */

		/* how far from the threat, in samples */
		{
			int tdx = ix - tix;
			int tdz = iz - tiz;

			away = (long)tdx * (long)tdx + (long)tdz * (long)tdz;
		}

		score = (int)(away / 4);

		if (JerAiMapRoad(map, ix, iz))
			score += JERAI_LOCAL_ROAD_BONUS;

		score += JerAiMapClearance(map, ix, iz) * 2;

		if (best < 0 || score > bestScore)
		{
			bestScore = score;
			best = cur;
			bestClear = JerAiMapClearance(map, ix, iz);
		}
	}

	if (best < 0)
		return 0;

	{
		int bx = best % JERAI_MAP_SIZE;
		int bz = best / JERAI_MAP_SIZE;

		JerAiMapWorld(map, bx, bz, &out->x, &out->z);
		out->kind = JerAiMapRoad(map, bx, bz) ? JERAI_GOAL_ROAD : JERAI_GOAL_OPEN;
		out->clearance = bestClear;
		out->samples = ((bx - cix) < 0 ? (cix - bx) : (bx - cix));
		{
			int dzz = ((bz - ciz) < 0 ? (ciz - bz) : (bz - ciz));

			if (dzz > out->samples)
				out->samples = dzz;
		}
	}

	return 1;
}
