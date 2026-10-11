#ifndef JERAI_MAP_H
#define JERAI_MAP_H

/* ------------------------------------------------------------------
 * jerai_map.h -- the AI's view of the world: a small occupancy grid.
 *
 * Lifted from the multiplayer module's ai/aimap and namespaced (JerAi*)
 * so JerAI carries its own copy: both modules are compiled into the same
 * exe, so identical symbol names would collide at link time. The world
 * model is unchanged:
 *
 *   - a FIXED JERAI_MAP_SIZE x JERAI_MAP_SIZE window of samples
 *     JERAI_MAP_STEP world units apart, re-centred on whoever asks;
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

#define JERAI_MAP_SIZE		49	/* samples per side; odd, so one sits on the centre */
#define JERAI_MAP_STEP		512	/* world units between samples (a quarter of a cell) */
#define JERAI_MAP_RADIUS	350	/* the probe's radius: about a car's width */

/* Sample flags. */
#define JERAI_MAP_BLOCKED	0x01	/* CellEmpty found something a car cannot pass */
#define JERAI_MAP_ROAD		0x02	/* JerRoadAt: on the driveable road network */

/* How far clearance is measured, in samples. */
#define JERAI_MAP_CLEAR_MAX	6

/* The cost of a sample a route may not use. Big, but not INT_MAX, so
 * "cheaper" comparisons stay sane. */
#define JERAI_MAP_COST_BLOCKED	1000000

typedef struct JERAI_MAP
{
	int	originX, originZ;	/* world position of sample (0, 0) */
	int	centreX, centreZ;	/* the world position the window was centred on */
	int	centreY;		/* the height the probes used, for reference */
	int	valid;			/* 0 until a build has succeeded */
	int	sampled;		/* samples the engine answered for */
	int	roadSamples;		/* ...of which were on the road network */

	unsigned char cell[JERAI_MAP_SIZE][JERAI_MAP_SIZE];
	int	clear[JERAI_MAP_SIZE][JERAI_MAP_SIZE];	/* samples to the nearest blocked one, capped */
	int	turnCost;	/* extra cost for changing direction between samples (0 = off) */
	int	offroadCost;	/* extra cost for an OFF-ROAD sample (the road preference) */
} JERAI_MAP;

/* The extra cost an off-road sample carries. On-road samples cost 100, so this
 * only says how strongly the planner prefers the road network WHEN THE ROAD GOES
 * THE SAME WAY: off-road is still allowed, or a car that has wandered off the
 * road (or a goal that is off it) could never be planned to. */
#define JERAI_MAP_OFFROAD_DEFAULT	90

/* Build the window centred on (x, y, z). Returns 1 when the world was there to
 * sample, 0 when it was not (frontend, loading screen, between levels). */
int  JerAiMapBuild(JERAI_MAP* map, int x, int y, int z);

int  JerAiMapValid(const JERAI_MAP* map);
int  JerAiMapBlocked(const JERAI_MAP* map, int ix, int iz);
int  JerAiMapRoad(const JERAI_MAP* map, int ix, int iz);
int  JerAiMapClearance(const JERAI_MAP* map, int ix, int iz);

/* The nearest sample a car could actually be: used when a probe radius leaves the car
 * inside a "blocked" sample, which is what happens whenever it is parked against a wall.
 * Returns 1 and writes the sample; 0 when there is nothing open within a few samples. */
#define JERAI_MAP_OPEN_SNAP	4	/* how far out to look, in samples */
int  JerAiMapNearestOpen(const JERAI_MAP* map, int ix, int iz, int* ox, int* oz);

/* Traversal cost. Blocked samples cost JERAI_MAP_COST_BLOCKED. */
int  JerAiMapCost(const JERAI_MAP* map, int ix, int iz);

/* World <-> sample conversion. JerAiMapSampleIndex returns 1 and writes the
 * nearest sample when (x, z) is inside the window, 0 when it is not. */
int  JerAiMapSampleIndex(const JERAI_MAP* map, int x, int z, int* ix, int* iz);
void JerAiMapWorld(const JERAI_MAP* map, int ix, int iz, int* x, int* z);

/* 1 when a level is loaded, so the world is worth sampling. */
int  JerAiMapWorldLoaded(void);

#ifdef __cplusplus
}
#endif

#endif /* JERAI_MAP_H */
