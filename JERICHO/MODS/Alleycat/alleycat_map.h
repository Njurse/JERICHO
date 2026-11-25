#ifndef ALLEYCAT_MAP_H
#define ALLEYCAT_MAP_H

/* ------------------------------------------------------------------
 * alleycat_map.h -- the AI's view of the world: a small occupancy grid.
 *
 * Lifted from the multiplayer module's ai/aimap and namespaced (Alleycat*)
 * so Alleycat carries its own copy: both modules are compiled into the same
 * exe, so identical symbol names would collide at link time. The world
 * model is unchanged:
 *
 *   - a FIXED ALLEYCAT_MAP_SIZE x ALLEYCAT_MAP_SIZE window of samples
 *     ALLEYCAT_MAP_STEP world units apart, re-centred on whoever asks;
 *   - every sample is one CellEmpty() call: the engine's own scenery
 *     test. FENCES AND BARRELS ARE NOT WALLS - CellEmpty skips
 *     MODEL_FLAG_SMASHABLE and chairs by design;
 *   - the road network is a PREFERENCE, never a wall: road/off-road is
 *     recorded and costed, not forbidden;
 *   - wall proximity is measured, so a route prefers the middle of a
 *     road instead of grazing its edge.
 *
 * NOTHING HERE DIVIDES BY MAP_CELL_SIZE. That is a level-header field
 * which is ZERO in the frontend; the window is positioned in world units
 * with a fixed step, and the world is only sampled once cells_across says
 * a level is loaded. When it is not, the build reports so and every query
 * answers "blocked".
 * ------------------------------------------------------------------ */

#include <stddef.h>	/* NULL: the pure half of this library has no engine headers to lean on */

#ifdef __cplusplus
extern "C" {
#endif

#define ALLEYCAT_MAP_SIZE		49	/* samples per side; odd, so one sits on the centre */
#define ALLEYCAT_MAP_STEP		256	/* world units between samples (an eighth of a cell) */
#define ALLEYCAT_MAP_RADIUS	350	/* the probe's radius: about a car's width */

/* Sample flags. */
#define ALLEYCAT_MAP_BLOCKED	0x01	/* CellEmpty found something a car cannot pass */
#define ALLEYCAT_MAP_ROAD		0x02	/* JerRoadAt: on the driveable road network */

/* How far clearance is measured, in samples. */
#define ALLEYCAT_MAP_CLEAR_MAX	6

/* The cost of a sample a route may not use. Big, but not INT_MAX, so
 * "cheaper" comparisons stay sane. */
#define ALLEYCAT_MAP_COST_BLOCKED	1000000

typedef struct ALLEYCAT_MAP
{
	int	originX, originZ;	/* world position of sample (0, 0) */
	int	centreX, centreZ;	/* the world position the window was centred on */
	int	centreY;		/* the height the probes used, for reference */
	int	valid;			/* 0 until a build has succeeded */
	int	sampled;		/* samples the engine answered for */
	int	roadSamples;		/* ...of which were on the road network */

	unsigned char cell[ALLEYCAT_MAP_SIZE][ALLEYCAT_MAP_SIZE];
	int	clear[ALLEYCAT_MAP_SIZE][ALLEYCAT_MAP_SIZE];	/* samples to the nearest blocked one, capped */
	int	turnCost;	/* extra cost for changing direction between samples (0 = off) */
	int	offroadCost;	/* extra cost for an OFF-ROAD sample (the road preference) */
} ALLEYCAT_MAP;

/* The extra cost an off-road sample carries. On-road samples cost 100, so this
 * only says how strongly the planner prefers the road network WHEN THE ROAD GOES
 * THE SAME WAY: off-road is still allowed, or a car that has wandered off the
 * road (or a goal that is off it) could never be planned to. */
#define ALLEYCAT_MAP_OFFROAD_DEFAULT	90

/* Build the window centred on (x, y, z). Returns 1 when the world was there to
 * sample, 0 when it was not (frontend, loading screen, between levels). */
int  AlleycatMapBuild(ALLEYCAT_MAP* map, int x, int y, int z);

int  AlleycatMapValid(const ALLEYCAT_MAP* map);
int  AlleycatMapBlocked(const ALLEYCAT_MAP* map, int ix, int iz);
int  AlleycatMapRoad(const ALLEYCAT_MAP* map, int ix, int iz);
int  AlleycatMapClearance(const ALLEYCAT_MAP* map, int ix, int iz);

/* The nearest sample a car could actually be: used when a probe radius leaves the car
 * inside a "blocked" sample, which is what happens whenever it is parked against a wall.
 * Returns 1 and writes the sample; 0 when there is nothing open within a few samples. */
#define ALLEYCAT_MAP_OPEN_SNAP	4	/* how far out to look, in samples */
int  AlleycatMapNearestOpen(const ALLEYCAT_MAP* map, int ix, int iz, int* ox, int* oz);

/* Traversal cost. Blocked samples cost ALLEYCAT_MAP_COST_BLOCKED. */
int  AlleycatMapCost(const ALLEYCAT_MAP* map, int ix, int iz);

/* World <-> sample conversion. AlleycatMapSampleIndex returns 1 and writes the
 * nearest sample when (x, z) is inside the window, 0 when it is not. */
int  AlleycatMapSampleIndex(const ALLEYCAT_MAP* map, int x, int z, int* ix, int* iz);
void AlleycatMapWorld(const ALLEYCAT_MAP* map, int ix, int iz, int* x, int* z);

/* 1 when a level is loaded, so the world is worth sampling. */
int  AlleycatMapWorldLoaded(void);

/* ------------------------------------------------------------------
 * PURE polyline geometry for the driver (alleycat_mapgrid.c), so the host test can
 * check it. Everything is 64-bit on purpose: `long` is 32 bits on Windows
 * (LLP64), and a squared world distance or a dx*t product wraps once a plan
 * segment is longer than ~1,300 units - which a string-pulled plan routinely is.
 * ------------------------------------------------------------------ */

/* Squared world distance, never wrapping. */
long long AlleycatDist2(int ax, int az, int bx, int bz);

/* Where (cx, cz) sits ALONG the polyline wx[]/wz[] (n points): the arc length to
 * the closest point of it. Writes that point's squared cross-track distance to
 * *crossD2 (-1 when n < 2, i.e. there is no segment to be off). 0 for n < 2. */
long long AlleycatPathProjectArc(const int* wx, const int* wz, int n, int cx, int cz, long long* crossD2);

/* The aim point: walk forward from `fromArc` in ~512-unit steps and take the
 * first point at least `lookAhead` further along that is not behind the car
 * (cosine against the heading fwdX/fwdZ, 4096 = 1.0, must exceed behindCos).
 * Falls back to the last waypoint. Writes the point and the direction of the
 * segment it sits on (0, 0 for the fallback). Returns 0 only for n <= 0. */
int  AlleycatPathAimPoint(const int* wx, const int* wz, int n, int cx, int cz,
	int fwdX, int fwdZ, long long fromArc, long long lookAhead, int behindCos,
	int* aimX, int* aimZ, int* segDx, int* segDz);

#ifdef __cplusplus
}
#endif

#endif /* ALLEYCAT_MAP_H */
