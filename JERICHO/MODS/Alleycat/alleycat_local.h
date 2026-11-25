#ifndef ALLEYCAT_LOCAL_H
#define ALLEYCAT_LOCAL_H

/* ------------------------------------------------------------------
 * alleycat_local.h -- the LOCAL search: "get me back to a road".
 *
 * Lifted from mp/ai/ailocal and namespaced (Alleycat*). This is a different
 * KIND of search from alleycat_star: A* answers "how do I get to that specific
 * place"; this answers "where is the nearest place worth being", which is the
 * question a car that has wandered off the road, or been shunted into a garden,
 * actually has.
 *
 * One flood of the window answers both rungs, in order:
 *
 *   1. ROAD  - the nearest sample on the road network that the car can
 *              actually REACH (breadth-first, so nearest in the graph);
 *   2. OPEN  - when no road is reachable, the most open ground that is;
 *   3. NONE  - nothing at all.
 *
 * Pure, like alleycat_mapgrid.c: no engine calls, so the host test covers it.
 * ------------------------------------------------------------------ */

#include "alleycat_map.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALLEYCAT_GOAL_NONE	0
#define ALLEYCAT_GOAL_ROAD	1	/* on the road network, and reachable from here */
#define ALLEYCAT_GOAL_OPEN	2	/* no road reachable: the most open ground instead */

typedef struct ALLEYCAT_GOAL
{
	int	kind;		/* ALLEYCAT_GOAL_* */
	int	x, z;		/* the destination, in world units */
	int	samples;	/* how far away it is, in samples (0 = here) */
	int	clearance;	/* wall clearance at the destination */
} ALLEYCAT_GOAL;

/* Fill `out` with where this car should head for. Returns 1 when there is a
 * goal (ROAD or OPEN), 0 when the window offers nothing (kind NONE). */
int AlleycatLocalGoal(const ALLEYCAT_MAP* map, int fromX, int fromZ, ALLEYCAT_GOAL* out);

/* Where to RUN to, given something to run from. Scores every reachable sample in
 * a ring and takes the one furthest from the threat. Returns 1 and fills `out`,
 * or 0 when nothing in the ring is both reachable and worth going to. */
int AlleycatLocalFleeGoal(const ALLEYCAT_MAP* map, int fromX, int fromZ, int threatX, int threatZ, ALLEYCAT_GOAL* out);

/* Where to go to get AROUND an obstacle on the way to a target: the reachable
 * spot most SIDEWAYS (perpendicular) to the car->target line, so a stuck follower
 * detours sideways before re-routing instead of rejoining the same wedge. */
int AlleycatLocalTangentialGoal(const ALLEYCAT_MAP* map, int fromX, int fromZ, int targetX, int targetZ, ALLEYCAT_GOAL* out);

#ifdef __cplusplus
}
#endif

#endif /* ALLEYCAT_LOCAL_H */
