/* Host-side tests for the mp AI's PATHFINDER (MODS/mp/ai): the grid queries, the cost
 * model and A* itself, with no engine, no level and no game.
 *
 * This directory is excluded from the game build (premake5.lua), so the file only ever
 * builds on its own, linking the two PURE pieces of the library and NOT aimap.c (which is
 * nothing but engine probes):
 *
 *   gcc -std=c99 -Wall -Wextra -I JERICHO/MODS/mp -o /tmp/test_ai_path \
 *       src_rebuild/Game/C/JERICHO/test/test_ai_path.c \
 *       JERICHO/MODS/mp/ai/aimapgrid.c JERICHO/MODS/mp/ai/aistar.c \n *       JERICHO/MODS/mp/ai/ailocal.c && /tmp/test_ai_path
 *
 *   (as C++: the module itself is compiled as C++, so the same source must build either way)
 *
 * Exit status 0 = every check passed. */
#include <stdio.h>
#include <string.h>

#include "../../../../../JERICHO/MODS/mp/ai/aimap.h"
#include "../../../../../JERICHO/MODS/mp/ai/aistar.h"
#include "../../../../../JERICHO/MODS/mp/ai/ailocal.h"

static int gFails, gChecks;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ------------------------------------------------------------------ */
/* A grid to plan over                                                 */
/* ------------------------------------------------------------------ */

static AIMAP gMap;

static int W(int sample)	/* sample index -> world units (origin at 0,0) */
{
	return sample * AIMAP_STEP;
}

static void gridReset(void)
{
	int ix, iz;

	memset(&gMap, 0, sizeof(gMap));
	gMap.originX = 0;
	gMap.originZ = 0;
	gMap.valid = 1;

	/* wide open until a wall is put down */
	for (iz = 0; iz < AIMAP_SIZE; iz++)
		for (ix = 0; ix < AIMAP_SIZE; ix++)
			gMap.clear[iz][ix] = AIMAP_CLEAR_MAX;
}

static void gridWall(int ix, int iz)
{
	gMap.cell[iz][ix] |= AIMAP_BLOCKED;

	/* keep the clearance honest the way AiMapBuild's measurement would: a blocked
	 * sample reports 0, its neighbours report their distance to it */
	{
		int dx, dz;

		for (dz = -AIMAP_CLEAR_MAX; dz <= AIMAP_CLEAR_MAX; dz++)
		{
			for (dx = -AIMAP_CLEAR_MAX; dx <= AIMAP_CLEAR_MAX; dx++)
			{
				int sx = ix + dx, sz = iz + dz, d;

				if (sx < 0 || sz < 0 || sx >= AIMAP_SIZE || sz >= AIMAP_SIZE)
					continue;

				d = (dx < 0 ? -dx : dx);
				if ((dz < 0 ? -dz : dz) > d)
					d = (dz < 0 ? -dz : dz);

				if (d < gMap.clear[sz][sx])
					gMap.clear[sz][sx] = d;
			}
		}
	}
}

static void gridRoad(int ix, int iz)
{
	gMap.cell[iz][ix] |= AIMAP_ROAD;
}

static AIPATH gPath;

static int planTo(int fromS, int toS)
{
	return AiStarPlan(&gMap, W(fromS), W(fromS), W(toS), W(toS), &gPath);
}

static void gridWallRow(int y, int x0, int x1)
{
	int x;

	for (x = x0; x <= x1; x++)
		gridWall(x, y);
}

/* ------------------------------------------------------------------ */
/* The grid queries                                                    */
/* ------------------------------------------------------------------ */

static void test_queries(void)
{
	int ix = -1, iz = -1;

	gridReset();

	CHECK(AiMapValid(&gMap) == 1);
	CHECK(AiMapValid(NULL) == 0);

	/* inside the window */
	CHECK(AiMapSampleIndex(&gMap, W(4), W(7), &ix, &iz) == 1);
	CHECK(ix == 4 && iz == 7);

	/* the very edge is in, one sample past it is not */
	CHECK(AiMapSampleIndex(&gMap, W(AIMAP_SIZE - 1), W(0), &ix, &iz) == 1);
	CHECK(AiMapSampleIndex(&gMap, W(AIMAP_SIZE), W(0), &ix, &iz) == 0);
	CHECK(AiMapSampleIndex(&gMap, -W(1), W(0), &ix, &iz) == 0);

	/* world <-> sample round trip */
	{
		int x = 0, z = 0;

		AiMapWorld(&gMap, 9, 3, &x, &z);
		CHECK(x == W(9) && z == W(3));
	}

	/* outside the window is "blocked", not "clear": we do not drive off the map */
	CHECK(AiMapBlocked(&gMap, AIMAP_SIZE, 3) == 1);
	CHECK(AiMapBlocked(&gMap, 3, -1) == 1);

	/* a wall, and the cost of the samples around it */
	gridWall(10, 10);
	CHECK(AiMapBlocked(&gMap, 10, 10) == 1);
	CHECK(AiMapCost(&gMap, 10, 10) == AIMAP_COST_BLOCKED);
	CHECK(AiMapClearance(&gMap, 10, 10) == 0);
	CHECK(AiMapClearance(&gMap, 11, 10) == 1);

	/* touching a wall costs more than the middle of the road */
	CHECK(AiMapCost(&gMap, 11, 10) > AiMapCost(&gMap, 12, 10));

	/* a road sample is cheaper than an off-road one with the same clearance */
	gridReset();
	gridRoad(5, 5);
	CHECK(AiMapRoad(&gMap, 5, 5) == 1);
	CHECK(AiMapRoad(&gMap, 5, 6) == 0);
	CHECK(AiMapCost(&gMap, 5, 5) < AiMapCost(&gMap, 5, 6));

	/* an invalid grid is never planable */
	{
		AIMAP bad;

		memset(&bad, 0, sizeof(bad));
		CHECK(AiMapValid(&bad) == 0);
		CHECK(AiMapCost(&bad, 1, 1) == AIMAP_COST_BLOCKED);
	}
}

/* ------------------------------------------------------------------ */
/* Line of sight: what the smoothing and the bots both rely on         */
/* ------------------------------------------------------------------ */

static void test_line_clear(void)
{
	gridReset();

	CHECK(AiStarLineClear(&gMap, W(2), W(2), W(20), W(20)) == 1);
	CHECK(AiStarLineClear(&gMap, W(2), W(2), W(2), W(2)) == 1);

	gridWallRow(10, 0, AIMAP_SIZE - 1);
	CHECK(AiStarLineClear(&gMap, W(2), W(10), W(20), W(10)) == 0);	/* straight through */
	CHECK(AiStarLineClear(&gMap, W(2), W(2), W(2), W(20)) == 0);	/* crosses it */

	gridReset();
	gridWall(3, 3);
	gridWall(4, 3);
	gridWall(5, 3);
	CHECK(AiStarLineClear(&gMap, W(4), W(2), W(4), W(4)) == 0);	/* a thumb of wall */

	/* not knowing is not clear: a line that leaves the window is refused */
	gridReset();
	CHECK(AiStarLineClear(&gMap, W(2), W(2), W(AIMAP_SIZE + 4), W(2)) == 0);

	/* and an invalid grid refuses everything */
	{
		AIMAP bad;

		memset(&bad, 0, sizeof(bad));
		CHECK(AiStarLineClear(&bad, 0, 0, W(4), W(4)) == 0);
	}
}

/* ------------------------------------------------------------------ */
/* A*: the cases that matter                                           */
/* ------------------------------------------------------------------ */

static void test_open_grid(void)
{
	gridReset();

	/* wide open: the goal is reached, and the route is SMOOTHED - a straight
	 * corridor must not come back as 29 cell-by-cell waypoints, which is what a
	 * car would weave down */
	CHECK(planTo(1, 30) == 1);
	CHECK(gPath.complete == 1);
	CHECK(gPath.waypoints >= 2);
	CHECK(gPath.waypoints <= 4);
	CHECK(gPath.cost > 0);

	/* the ends are the real ends */
	CHECK(gPath.wx[0] == W(1) && gPath.wz[0] == W(1));
	CHECK(gPath.wx[gPath.waypoints - 1] == W(30) && gPath.wz[gPath.waypoints - 1] == W(30));

	/* every leg is actually drivable, which is the whole point of smoothing */
	{
		int k;

		for (k = 1; k < gPath.waypoints; k++)
			CHECK(AiStarLineClear(&gMap, gPath.wx[k - 1], gPath.wz[k - 1], gPath.wx[k], gPath.wz[k]) == 1);
	}

	/* the work is bounded */
	CHECK(gPath.expanded <= AISTAR_EXPANSIONS);

	/* planning to where we already are is not a failure */
	CHECK(planTo(7, 7) == 1);
	CHECK(gPath.waypoints >= 1);
}

static void test_wall_with_a_gap(void)
{
	int k, legs = 0;

	gridReset();

	/* a wall across the middle with one gap at x = 20 */
	gridWallRow(16, 1, 19);
	gridWallRow(16, 21, AIMAP_SIZE - 1);

	CHECK(planTo(3, 30) == 1);
	CHECK(gPath.complete == 1);

	/* and every leg of the way round is clear */
	for (k = 1; k < gPath.waypoints; k++)
	{
		legs++;
		CHECK(AiStarLineClear(&gMap, gPath.wx[k - 1], gPath.wz[k - 1], gPath.wx[k], gPath.wz[k]) == 1);
	}

	CHECK(legs >= 2);	/* it had to turn: a straight line is blocked */

	/* no waypoint is inside a wall */
	for (k = 0; k < gPath.waypoints; k++)
	{
		int ix, iz;

		CHECK(AiMapSampleIndex(&gMap, gPath.wx[k], gPath.wz[k], &ix, &iz) == 1);
		CHECK(AiMapBlocked(&gMap, ix, iz) == 0);
	}
}

static void test_unreachable(void)
{
	/* the goal walled in on every side: the answer is NOT "nothing", it is the
	 * best partial route to the closest we can get - which is what a stuck car
	 * needs to head for while it re-plans */
	gridReset();

	{
		int dx, dz;

		/* every neighbour walled, so the goal itself is an island. (Not the goal
		 * sample: a blocked goal is simply moved to the nearest open sample, which is
		 * a different case and is tested below.) */
		for (dz = -1; dz <= 1; dz++)
			for (dx = -1; dx <= 1; dx++)
				if (dx || dz)
					gridWall(20 + dx, 20 + dz);
	}

	CHECK(planTo(3, 20) == 0);	/* (3,3) -> (20,20), an island */
	CHECK(gPath.complete == 0);
	CHECK(gPath.waypoints > 0);		/* a partial route, not silence */

	/* the partial route starts where we are and heads the right way */
	CHECK(gPath.wx[0] == W(3) && gPath.wz[0] == W(3));

	{
		int lastX = gPath.wx[gPath.waypoints - 1];
		int lastZ = gPath.wz[gPath.waypoints - 1];
		int nearX = (lastX - W(20)); if (nearX < 0) nearX = -nearX;
		int nearZ = (lastZ - W(20)); if (nearZ < 0) nearZ = -nearZ;

		/* it got closer to the goal than it started (both axes, since the pocket is
		 * at (20,20) and we start at (3,3)) */
		CHECK(nearX + nearZ < (W(20) - W(3)) * 2);
	}
}

static void test_corner_cut(void)
{
	/* An inside corner: (5,4) and (4,5) are walls, and the goal (5,5) is only one
	 * diagonal step away. A pathfinder that shaves corners takes that step; a car
	 * that may not clip a wall corner must go round. So the route must not be a
	 * single hop. */
	gridReset();

	gridWall(5, 4);
	gridWall(4, 5);

	CHECK(planTo(4, 5) == 1);	/* it is reachable the long way */
	CHECK(gPath.complete == 1);
	CHECK(gPath.waypoints >= 2);	/* more than one leg: the corner was not shaved */

	/* and the first leg does not end on the goal */
	CHECK(!(gPath.wx[0] == W(4) && gPath.wz[0] == W(4) && gPath.waypoints == 1));
}

static void test_road_preference(void)
{
	int k, onRoad = 0;
	int ix, iz;

	/* Two parallel corridors of the same length, the upper one road and the lower
	 * one not. Both routes are equal in distance, so cost is the only thing that
	 * can choose between them: the road must win. */
	gridReset();

	for (ix = 1; ix <= 30; ix++)
	{
		gridRoad(ix, 10);
		gridRoad(ix, 11);
	}

	CHECK(AiStarPlan(&gMap, W(1), W(10), W(30), W(10), &gPath) == 1);
	CHECK(gPath.complete == 1);

	for (k = 0; k < gPath.waypoints; k++)
	{
		CHECK(AiMapSampleIndex(&gMap, gPath.wx[k], gPath.wz[k], &ix, &iz) == 1);

		if (AiMapRoad(&gMap, ix, iz))
			onRoad++;
	}

	/* every waypoint of a road corridor is on the road */
	CHECK(onRoad == gPath.waypoints);
	CHECK(gPath.onRoad == gPath.waypoints);

	/* and a route that must cross off-road ground still gets there: the preference
	 * is a cost, never a wall */
	gridReset();

	for (ix = 1; ix <= 20; ix++)
		gridRoad(ix, 10);

	CHECK(AiStarPlan(&gMap, W(1), W(10), W(30), W(30), &gPath) == 1);
	CHECK(gPath.complete == 1);
}

static void test_start_inside_a_wall_sample(void)
{
	/* A car parked against a wall sits inside a "blocked" sample by its own probe
	 * radius. That is not "trapped": the planner walks out to the nearest open
	 * sample instead of refusing to plan. */
	gridReset();

	gridWall(8, 8);
	gridWall(8, 9);

	/* start exactly on the wall sample */
	CHECK(AiStarPlan(&gMap, W(8), W(8), W(20), W(20), &gPath) == 1);
	CHECK(gPath.complete == 1);
	CHECK(gPath.waypoints > 0);

	/* walled in completely: then, and only then, there is nothing to say */
	gridReset();

	{
		int dx, dz;

		for (dz = -1; dz <= 1; dz++)
			for (dx = -1; dx <= 1; dx++)
				if (dx || dz)
					gridWall(8 + dx, 8 + dz);
	}

	CHECK(AiStarPlan(&gMap, W(8), W(8), W(20), W(20), &gPath) == 0);
	CHECK(gPath.waypoints == 0);
}

static void test_invalid_map(void)
{
	AIMAP bad;

	memset(&bad, 0, sizeof(bad));	/* valid == 0 */

	CHECK(AiStarPlan(&bad, 0, 0, W(20), W(20), &gPath) == 0);
	CHECK(gPath.waypoints == 0);
	CHECK(gPath.complete == 0);

	/* a goal outside the window is not reachable either */
	gridReset();
	CHECK(AiStarPlan(&gMap, W(2), W(2), W(AIMAP_SIZE + 5), W(2), &gPath) == 0);
}

static void test_determinism(void)
{
	int firstW[2], firstCost, firstCount, k;

	gridReset();

	/* a maze with plenty of equal-cost options, so a non-deterministic tie break
	 * would show up as a different answer */
	gridWallRow(6, 1, 25);
	gridWallRow(12, 5, AIMAP_SIZE - 1);
	gridWallRow(18, 1, 28);

	CHECK(planTo(2, 30) == 1);
	firstCount = gPath.waypoints;
	firstCost = gPath.cost;
	firstW[0] = gPath.wx[0];
	firstW[1] = gPath.wz[0];

	for (k = 0; k < 4; k++)
	{
		CHECK(planTo(2, 30) == 1);
		CHECK(gPath.waypoints == firstCount);
		CHECK(gPath.cost == firstCost);
		CHECK(gPath.wx[0] == firstW[0] && gPath.wz[0] == firstW[1]);
	}
}

/* ------------------------------------------------------------------ */
/* The local search: get back to a road                                */
/* ------------------------------------------------------------------ */

static AIGOAL gGoal;

static void test_local_goal(void)
{
	int ix;

	/* nothing but open ground: the answer is OPEN, not silence */
	gridReset();
	CHECK(AiLocalGoal(&gMap, W(5), W(5), &gGoal) == 1);
	CHECK(gGoal.kind == AIGOAL_OPEN);
	CHECK(gGoal.samples == 0);		/* already as open as it gets */

	/* a road strip to the right, reachable: ROAD, and the NEAREST road sample */
	gridReset();
	for (ix = 20; ix < AIMAP_SIZE; ix++)
		gridRoad(ix, 5);

	CHECK(AiLocalGoal(&gMap, W(5), W(5), &gGoal) == 1);
	CHECK(gGoal.kind == AIGOAL_ROAD);
	CHECK(gGoal.samples == 15);		/* (5,5) -> (20,5) */
	CHECK(gGoal.x == W(20) && gGoal.z == W(5));

	/* already on the road: the goal is here, and the caller can tell */
	gridReset();
	gridRoad(7, 7);
	CHECK(AiLocalGoal(&gMap, W(7), W(7), &gGoal) == 1);
	CHECK(gGoal.kind == AIGOAL_ROAD);
	CHECK(gGoal.samples == 0);

	/* THE ONE THAT MATTERS: a road behind a wall is not a way out. Reachability is the
	 * reason this floods instead of looking around - a line-of-sight search would pick
	 * that road and drive the car straight at the wall.
	 *
	 * The wall has to actually SEPARATE: a wall one sample thick along z = 5 blocks
	 * nothing, because the car walks round its end through z = 4. (My first version of
	 * this test made exactly that mistake and the search was right to ignore it.) So
	 * the wall is a full column, the whole height of the window. */
	gridReset();
	for (ix = 15; ix < AIMAP_SIZE; ix++)
		gridRoad(ix, 5);
	{
		int iz;

		for (iz = 0; iz < AIMAP_SIZE; iz++)
			gridWall(14, iz);
	}

	CHECK(AiLocalGoal(&gMap, W(5), W(5), &gGoal) == 1);
	CHECK(gGoal.kind == AIGOAL_OPEN);	/* NOT the unreachable road */
	CHECK(gGoal.x < W(14));			/* and on our side of the wall */

	/* a road reachable only the LONG way round is still the answer, and this is the
	 * case that separates a flood from a straight-line look: the wall covers z = 5 for
	 * x = 0..10, so the only way to the road is round its end. */
	gridReset();
	for (ix = 0; ix <= 10; ix++)
		gridWall(ix, 5);
	for (ix = 20; ix < AIMAP_SIZE; ix++)
		gridRoad(ix, 5);

	CHECK(AiLocalGoal(&gMap, W(5), W(5), &gGoal) == 1);
	CHECK(gGoal.kind == AIGOAL_ROAD);
	CHECK(gGoal.x >= W(20));

	/* walled in completely: nothing to say, and the caller finds out */
	gridReset();
	{
		int dx, dz;

		for (dz = -1; dz <= 1; dz++)
			for (dx = -1; dx <= 1; dx++)
				if (dx || dz)
					gridWall(8 + dx, 8 + dz);
	}
	CHECK(AiLocalGoal(&gMap, W(8), W(8), &gGoal) == 1);
	CHECK(gGoal.kind == AIGOAL_OPEN);
	CHECK(gGoal.samples == 0);		/* its own sample: the only open one it has */

	/* an invalid grid is never a goal */
	{
		AIMAP bad;

		memset(&bad, 0, sizeof(bad));
		CHECK(AiLocalGoal(&bad, 0, 0, &gGoal) == 0);
		CHECK(gGoal.kind == AIGOAL_NONE);
	}
}

static void test_flee_goal(void)
{
	int ix;

	/* the threat is due north; the goal must be AWAY from it, and reachable */
	gridReset();
	CHECK(AiLocalFleeGoal(&gMap, W(16), W(16), W(16), W(10), &gGoal) == 1);

	{
		int sx = 0, sz = 0;
		int dStart, dGoal;

		CHECK(AiMapSampleIndex(&gMap, gGoal.x, gGoal.z, &sx, &sz) == 1);
		CHECK(AiMapBlocked(&gMap, sx, sz) == 0);
		CHECK(AiStarLineClear(&gMap, W(16), W(16), gGoal.x, gGoal.z) == 1);

		dStart = (16 - 16) * (16 - 16) + (16 - 10) * (16 - 10);
		dGoal = (sx - 16) * (sx - 16) + (sz - 10) * (sz - 10);

		CHECK(dGoal > dStart);		/* further from the threat than we are */
		CHECK(sz > 16);			/* and it went south, away from it */
	}

	/* A road is preferred over open ground a couple of samples further out: the road
	 * bonus (120) outweighs the extra distance from the threat (a couple of samples is
	 * worth a couple of points). The band goes at z = 24, which is inside the flee ring
	 * - a road at the far edge of the WINDOW is not a candidate at all, which is the
	 * ring's job. */
	gridReset();
	for (ix = 0; ix < AIMAP_SIZE; ix++)
		gridRoad(ix, 24);

	CHECK(AiLocalFleeGoal(&gMap, W(16), W(16), W(16), W(10), &gGoal) == 1);
	CHECK(gGoal.kind == AIGOAL_ROAD);

	/* reachability again: with a full wall down the middle, the goal cannot be on the
	 * far side, however attractive it looks */
	gridReset();
	{
		int wz;

		for (wz = 0; wz < AIMAP_SIZE; wz++)
			gridWall(20, wz);
	}

	CHECK(AiLocalFleeGoal(&gMap, W(16), W(16), W(16), W(10), &gGoal) == 1);
	CHECK(gGoal.x < W(20));

	/* an invalid grid is never a goal */
	{
		AIMAP bad;

		memset(&bad, 0, sizeof(bad));
		CHECK(AiLocalFleeGoal(&bad, 0, 0, W(4), W(4), &gGoal) == 0);
		CHECK(gGoal.kind == AIGOAL_NONE);
	}
}

/* REACHABILITY, NOT LINE OF SIGHT. A road that can only be reached by driving AROUND a
 * wall is still a goal. The old gate was AiStarLineClear, which asks "is the straight
 * line clear" - so this road was rejected outright and the flee never used the network
 * next to it (the "it does not follow the road" symptom). The wall below blocks the
 * direct line to every in-ring road sample, but the flood walks round its end and finds
 * the road, so the chosen goal must still be a ROAD. */
static void test_flee_goal_around_a_corner(void)
{
	int ix;

	gridReset();

	/* the threat is due north; the road lies due south, behind a wall that spans the whole
	 * line of sight to it but stops short of the window's east edge */
	for (ix = 0; ix <= 24; ix++)
		gridWall(ix, 20);

	for (ix = 0; ix < AIMAP_SIZE; ix++)
		gridRoad(ix, 24);

	CHECK(AiLocalFleeGoal(&gMap, W(16), W(16), W(16), W(10), &gGoal) == 1);
	CHECK(gGoal.kind == AIGOAL_ROAD);

	/* and prove the point: the direct line to the road straight ahead of the car really is
	 * blocked, so a line-of-sight gate could not have picked it */
	CHECK(AiStarLineClear(&gMap, W(16), W(16), W(16), W(24)) == 0);
}

int main(void)
{
	test_queries();
	test_line_clear();
	test_open_grid();
	test_wall_with_a_gap();
	test_unreachable();
	test_corner_cut();
	test_road_preference();
	test_start_inside_a_wall_sample();
	test_invalid_map();
	test_determinism();
	test_local_goal();
	test_flee_goal();
	test_flee_goal_around_a_corner();

	printf("%d check(s), %d failed\n", gChecks, gFails);

	return gFails ? 1 : 0;
}
