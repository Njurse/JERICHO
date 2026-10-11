#ifndef JERAI_LOCAL_H
#define JERAI_LOCAL_H

/* ------------------------------------------------------------------
 * jerai_local.h -- the LOCAL search: "get me back to a road".
 *
 * Lifted from mp/ai/ailocal and namespaced (JerAi*). This is a different
 * KIND of search from jerai_star: A* answers "how do I get to that specific
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
 * Pure, like jerai_mapgrid.c: no engine calls, so the host test covers it.
 * ------------------------------------------------------------------ */

#include "jerai_map.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JERAI_GOAL_NONE	0
#define JERAI_GOAL_ROAD	1	/* on the road network, and reachable from here */
#define JERAI_GOAL_OPEN	2	/* no road reachable: the most open ground instead */

typedef struct JERAI_GOAL
{
	int	kind;		/* JERAI_GOAL_* */
	int	x, z;		/* the destination, in world units */
	int	samples;	/* how far away it is, in samples (0 = here) */
	int	clearance;	/* wall clearance at the destination */
} JERAI_GOAL;

/* Fill `out` with where this car should head for. Returns 1 when there is a
 * goal (ROAD or OPEN), 0 when the window offers nothing (kind NONE). */
int JerAiLocalGoal(const JERAI_MAP* map, int fromX, int fromZ, JERAI_GOAL* out);

/* Where to RUN to, given something to run from. Scores every reachable sample in
 * a ring and takes the one furthest from the threat. Returns 1 and fills `out`,
 * or 0 when nothing in the ring is both reachable and worth going to. */
int JerAiLocalFleeGoal(const JERAI_MAP* map, int fromX, int fromZ, int threatX, int threatZ, JERAI_GOAL* out);

#ifdef __cplusplus
}
#endif

#endif /* JERAI_LOCAL_H */
