#ifndef JERAI_ROAD_H
#define JERAI_ROAD_H

/* ------------------------------------------------------------------
 * jerai_road.h -- the engine's road GRAPH, as nodes and edges.
 *
 * The occupancy grid (jerai_map) sees the road network only as a
 * yes/no flag per sample: it is the wrong shape for "where does this
 * street go". The engine keeps the topology directly - the three road
 * tables (straights / curves / junctions) and each surface's
 * ConnectIdx, the surfaces it joins (dr2roads.h). This module enumerates
 * that into a small fixed graph so it can be DRAWN (jerai_viz) and
 * DUMPED (jerai_dump).
 *
 *   - a NODE is a road surface. Straights and curves carry their own
 *     midpoint (Midx/Midz, world units - the same frame as
 *     hd.where.t). A junction has no position of its own (DRIVER2_JUNCTION
 *     is just ExitIdx[4] + flags), so its node is placed at the average of
 *     the surfaces that connect to it.
 *   - an EDGE is a ConnectIdx link: surface A joins surface B.
 *
 * Compiled-in only (JerRoadInfoAt / the Driver2* tables are not cheap to
 * reconstruct), which is part of why JerAI is a deep mod.
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

#define JERAI_ROAD_MAX_NODES	4096
#define JERAI_ROAD_MAX_EDGES	8192

/* node kinds, matching the engine's surface encoding */
#define JERAI_ROAD_STRAIGHT	0
#define JERAI_ROAD_CURVE	1
#define JERAI_ROAD_JUNCTION	2

typedef struct JERAI_ROAD_NODE
{
	int	surfId;		/* the engine surface id (index | kind bits) */
	int	kind;		/* JERAI_ROAD_* */
	int	x, y, z;	/* world units */
	int	numLanes;	/* 0 for a junction (it holds exits, not lanes) */
	int	speedLimit;	/* 0..3, 0 for a junction */
} JERAI_ROAD_NODE;

typedef struct JERAI_ROAD_EDGE
{
	int	a, b;		/* node indices */
} JERAI_ROAD_EDGE;

typedef struct JERAI_ROAD_GRAPH
{
	int	valid;
	int	nodeCount;
	int	edgeCount;

	/* what the engine reported, for the log / dump cross-check */
	int	straights;
	int	curves;
	int	junctions;

	JERAI_ROAD_NODE	nodes[JERAI_ROAD_MAX_NODES];
	JERAI_ROAD_EDGE	edges[JERAI_ROAD_MAX_EDGES];

	int	truncated;	/* 1 when a table hit its cap and entries were dropped */
} JERAI_ROAD_GRAPH;

/* Enumerate the engine's road tables into `graph`. Returns 1 when a level's
 * road data is loaded and the graph was built, 0 otherwise (frontend, no level).
 * The graph is cleared first, so a failed build leaves counts at 0. */
int JerAiRoadBuild(JERAI_ROAD_GRAPH* graph);

/* The node index for a surface id, or -1. Resolution is by scanning nodes; the
 * graph is small and this is used while building and by the dump. */
int JerAiRoadNodeForSurf(const JERAI_ROAD_GRAPH* graph, int surfId);

/* Write `graph` to JerAI_roadgraph.json + JerAI_roadgraph.obj in the working
 * directory (the OBJ in world/4096, X mirrored). Returns 1 when something was
 * written. Defined in jerai_dump.c. */
int JerAiDumpRoadGraph(const JERAI_ROAD_GRAPH* graph);

#ifdef __cplusplus
}
#endif

#endif /* JERAI_ROAD_H */
