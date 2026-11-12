/*
 * alleycat_local.c -- one flood of the window, two rungs of the answer. See alleycat_local.h.
 *
 * Lifted from mp/ai/ailocal.c and namespaced (Alleycat*).
 *
 * Static scratch rather than stack: this runs from the game loop, the arrays are sized
 * to the grid, and the module is single-threaded (the same reasoning as alleycat_star's node
 * pool). Nothing is allocated and nothing grows.
 */

#include <string.h>

#include "alleycat_map.h"
#include "alleycat_local.h"

#define ALLEYCAT_LOCAL_NODES	(ALLEYCAT_MAP_SIZE * ALLEYCAT_MAP_SIZE)

static unsigned char sSeen[ALLEYCAT_LOCAL_NODES];
static int sDist[ALLEYCAT_LOCAL_NODES];
static int sQueue[ALLEYCAT_LOCAL_NODES];

static void AlleycatLocalFail(ALLEYCAT_GOAL* out)
{
	if (out == NULL)
		return;

	out->kind = ALLEYCAT_GOAL_NONE;
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
static int AlleycatLocalFlood(const ALLEYCAT_MAP* map, int sx, int sz)
{
	int head = 0, tail = 0;
	int start = sz * ALLEYCAT_MAP_SIZE + sx;
	int dx, dz;

	memset(sSeen, 0, sizeof(sSeen));

	/* the start must be a real sample: an out-of-range start would index sSeen out of
	 * bounds on the very first write. */
	if (sx < 0 || sz < 0 || sx >= ALLEYCAT_MAP_SIZE || sz >= ALLEYCAT_MAP_SIZE)
		return 0;

	sSeen[start] = 1;
	sDist[start] = 0;
	sQueue[tail++] = start;

	while (head < tail)
	{
		int cur = sQueue[head++];
		int cx = cur % ALLEYCAT_MAP_SIZE;
		int cz = cur / ALLEYCAT_MAP_SIZE;
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

				if (nx < 0 || nz < 0 || nx >= ALLEYCAT_MAP_SIZE || nz >= ALLEYCAT_MAP_SIZE)
					continue;

				n = nz * ALLEYCAT_MAP_SIZE + nx;

				if (sSeen[n])
					continue;

				if (AlleycatMapBlocked(map, nx, nz))
					continue;

				if (dx != 0 && dz != 0 &&
					(AlleycatMapBlocked(map, cx + dx, cz) || AlleycatMapBlocked(map, cx, cz + dz)))
					continue;

				sSeen[n] = 1;
				sDist[n] = d + 1;
				sQueue[tail++] = n;
			}
		}
	}

	return tail;
}

int AlleycatLocalGoal(const ALLEYCAT_MAP* map, int fromX, int fromZ, ALLEYCAT_GOAL* out)
{
	int six, siz, q, reached;
	int bestClear = -1, bestIx = -1, bestIz = -1, bestRing = 0;

	if (out == NULL)
		return 0;

	AlleycatLocalFail(out);

	if (!AlleycatMapValid(map))
		return 0;

	if (!AlleycatMapSampleIndex(map, fromX, fromZ, &six, &siz))
		return 0;

	/* the car is very often inside a "blocked" sample - that is what a probe radius the
	 * width of a car does when the car is near a wall - so walk out before flooding */
	if (!AlleycatMapNearestOpen(map, six, siz, &six, &siz))
		return 0;

	reached = AlleycatLocalFlood(map, six, siz);

	for (q = 0; q < reached; q++)
	{
		int cur = sQueue[q];
		int cx = cur % ALLEYCAT_MAP_SIZE;
		int cz = cur / ALLEYCAT_MAP_SIZE;
		int d = sDist[cur];

		/* RUNG 1. Breadth-first, so the first road sample reached is the nearest
		 * reachable one - nearest along the ground the car can drive. */
		if (AlleycatMapRoad(map, cx, cz))
		{
			AlleycatMapWorld(map, cx, cz, &out->x, &out->z);
			out->kind = ALLEYCAT_GOAL_ROAD;
			out->samples = d;
			out->clearance = AlleycatMapClearance(map, cx, cz);

			return 1;
		}

		/* RUNG 2, gathered as we go: the most open ground, and (because BFS) the
		 * nearest of those when several are equally open. */
		if (AlleycatMapClearance(map, cx, cz) > bestClear)
		{
			bestClear = AlleycatMapClearance(map, cx, cz);
			bestIx = cx;
			bestIz = cz;
			bestRing = d;
		}
	}

	/* No road is reachable from here. Room to move still beats standing still. */
	if (bestIx >= 0)
	{
		AlleycatMapWorld(map, bestIx, bestIz, &out->x, &out->z);
		out->kind = ALLEYCAT_GOAL_OPEN;
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
#define ALLEYCAT_LOCAL_FLEE_RING	16

/* The road bonus has to stay COMPARABLE TO THE DISTANCE RANGE: too small and the flee
 * drifts off the road, too big and it drives straight AT the threat to reach a road. */
#define ALLEYCAT_LOCAL_ROAD_BONUS	120

int AlleycatLocalFleeGoal(const ALLEYCAT_MAP* map, int fromX, int fromZ, int threatX, int threatZ, ALLEYCAT_GOAL* out)
{
	int cix, ciz, tix, tiz, ix, iz, q, reached, best = -1, bestScore = 0, bestClear = 0;

	if (out == NULL)
		return 0;

	AlleycatLocalFail(out);

	if (!AlleycatMapValid(map))
		return 0;

	if (!AlleycatMapSampleIndex(map, fromX, fromZ, &cix, &ciz))
		return 0;

	if (!AlleycatMapNearestOpen(map, cix, ciz, &cix, &ciz))
		return 0;

	/* the threat may be off the window: then treat it as here, which makes "away" mean
	 * "the far side of the window" and still gives the flee a direction */
	if (!AlleycatMapSampleIndex(map, threatX, threatZ, &tix, &tiz))
	{
		tix = cix;
		tiz = ciz;
	}

	/* REACHABILITY, not line of sight: the flood answers "can the car actually drive
	 * there" for every candidate at once, and it is the SAME flood AlleycatLocalGoal uses,
	 * so the road rung and the flee rung agree about the map. */
	reached = AlleycatLocalFlood(map, cix, ciz);

	for (q = 0; q < reached; q++)
	{
		int score, dx, dz;
		int cur = sQueue[q];
		long away;

		ix = cur % ALLEYCAT_MAP_SIZE;
		iz = cur / ALLEYCAT_MAP_SIZE;

		dx = ix - cix;
		dz = iz - ciz;

		if (dx < 0) dx = -dx;
		if (dz < 0) dz = -dz;

		if (dx < ALLEYCAT_LOCAL_FLEE_RING / 2 && dz < ALLEYCAT_LOCAL_FLEE_RING / 2)
			continue;	/* it has to be somewhere worth going: near-here is not a flee */

		if (dx > ALLEYCAT_LOCAL_FLEE_RING || dz > ALLEYCAT_LOCAL_FLEE_RING)
			continue;	/* too far to be one plan's worth of driving */

		/* how far from the threat, in samples */
		{
			int tdx = ix - tix;
			int tdz = iz - tiz;

			away = (long)tdx * (long)tdx + (long)tdz * (long)tdz;
		}

		score = (int)(away / 4);

		if (AlleycatMapRoad(map, ix, iz))
			score += ALLEYCAT_LOCAL_ROAD_BONUS;

		score += AlleycatMapClearance(map, ix, iz) * 2;

		if (best < 0 || score > bestScore)
		{
			bestScore = score;
			best = cur;
			bestClear = AlleycatMapClearance(map, ix, iz);
		}
	}

	if (best < 0)
		return 0;

	{
		int bx = best % ALLEYCAT_MAP_SIZE;
		int bz = best / ALLEYCAT_MAP_SIZE;

		AlleycatMapWorld(map, bx, bz, &out->x, &out->z);
		out->kind = AlleycatMapRoad(map, bx, bz) ? ALLEYCAT_GOAL_ROAD : ALLEYCAT_GOAL_OPEN;
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

/* ------------------------------------------------------------------ */
/* The question the tangential escape answers: how to get AROUND the   */
/* obstacle between the car and its target                              */
/* ------------------------------------------------------------------ */

/* How far out a tangential escape is looked for, in samples. */
#define ALLEYCAT_LOCAL_TAN_RING	16

/* Where to go to get AROUND whatever is between the car and its target. Scores
 * every reachable sample in a ring and takes the one most SIDEWAYS to the
 * car->target line: the car keeps trying to drive at the target, so the thing
 * that un-wedges it is a spot perpendicular to that line, not another point on
 * it (which just rejoins the same obstacle). Returns 1 and fills `out`, or 0
 * when nothing in the ring is both reachable and worth going to. */
int AlleycatLocalTangentialGoal(const ALLEYCAT_MAP* map, int fromX, int fromZ, int targetX, int targetZ, ALLEYCAT_GOAL* out)
{
	int cix, ciz, ix, iz, q, reached, best = -1, bestScore = -1000000000, bestClear = 0;
	long tdx, tdz;

	if (out == NULL)
		return 0;

	AlleycatLocalFail(out);

	if (!AlleycatMapValid(map))
		return 0;

	if (!AlleycatMapSampleIndex(map, fromX, fromZ, &cix, &ciz))
		return 0;

	if (!AlleycatMapNearestOpen(map, cix, ciz, &cix, &ciz))
		return 0;

	/* the target direction, in SAMPLES so it is comparable with a candidate's
	 * sample offset. The target may be off the window; only the ratio matters. */
	tdx = ((long)targetX - (long)fromX) / ALLEYCAT_MAP_STEP;
	tdz = ((long)targetZ - (long)fromZ) / ALLEYCAT_MAP_STEP;

	reached = AlleycatLocalFlood(map, cix, ciz);

	for (q = 0; q < reached; q++)
	{
		int cur = sQueue[q];
		int dx, dz;
		int score;
		long cross, dot, dotAbs;

		ix = cur % ALLEYCAT_MAP_SIZE;
		iz = cur / ALLEYCAT_MAP_SIZE;

		dx = ix - cix;
		dz = iz - ciz;

		/* it has to be somewhere worth going: near-here is not an escape */
		{
			int adx = dx < 0 ? -dx : dx;
			int adz = dz < 0 ? -dz : dz;

			if (adx < ALLEYCAT_LOCAL_TAN_RING / 3 && adz < ALLEYCAT_LOCAL_TAN_RING / 3)
				continue;

			if (adx > ALLEYCAT_LOCAL_TAN_RING || adz > ALLEYCAT_LOCAL_TAN_RING)
				continue;
		}

		/* perpendicular component (signed) and along-the-line component of the
		 * offset, both against the target direction. */
		cross = (long)dx * tdz - (long)dz * tdx;
		dot = (long)dx * tdx + (long)dz * tdz;

		if (cross < 0)
			cross = -cross;

		if (dot < 0)
			dot = -dot;

		dotAbs = dot;

		/* sideways is good, along-the-line is not (it rejoins the wedge) */
		score = (int)cross - (int)dotAbs;

		if (AlleycatMapRoad(map, ix, iz))
			score += ALLEYCAT_LOCAL_ROAD_BONUS;

		score += AlleycatMapClearance(map, ix, iz) * 2;

		if (best < 0 || score > bestScore)
		{
			bestScore = score;
			best = cur;
			bestClear = AlleycatMapClearance(map, ix, iz);
		}
	}

	if (best < 0)
		return 0;

	{
		int bx = best % ALLEYCAT_MAP_SIZE;
		int bz = best / ALLEYCAT_MAP_SIZE;

		AlleycatMapWorld(map, bx, bz, &out->x, &out->z);
		out->kind = AlleycatMapRoad(map, bx, bz) ? ALLEYCAT_GOAL_ROAD : ALLEYCAT_GOAL_OPEN;
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
