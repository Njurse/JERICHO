#ifndef ALLEYCAT_ROAD_H
#define ALLEYCAT_ROAD_H

/* ------------------------------------------------------------------
 * alleycat_road.h -- the engine's road GRAPH, as nodes and edges.
 *
 * The occupancy grid (alleycat_map) sees the road network only as a
 * yes/no flag per sample: it is the wrong shape for "where does this
 * street go". The engine keeps the topology directly - the three road
 * tables (straights / curves / junctions) and each surface's
 * ConnectIdx, the surfaces it joins (dr2roads.h). This module enumerates
 * that into a small fixed graph so it can be DRAWN (alleycat_viz) and
 * DUMPED (alleycat_dump).
 *
 *   - a NODE is a road surface. Straights and curves carry their own
 *     midpoint (Midx/Midz, world units - the same frame as
 *     hd.where.t). A junction has no position of its own (DRIVER2_JUNCTION
 *     is just ExitIdx[4] + flags), so its node is placed at the average of
 *     the surfaces that connect to it.
 *   - an EDGE is a ConnectIdx link: surface A joins surface B.
 *
 * Compiled-in only (JerRoadInfoAt / the Driver2* tables are not cheap to
 * reconstruct), which is part of why Alleycat is a deep mod.
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

#define ALLEYCAT_ROAD_MAX_NODES	4096
#define ALLEYCAT_ROAD_MAX_EDGES	8192

/* node kinds, matching the engine's surface encoding */
#define ALLEYCAT_ROAD_STRAIGHT	0
#define ALLEYCAT_ROAD_CURVE	1
#define ALLEYCAT_ROAD_JUNCTION	2

typedef struct ALLEYCAT_ROAD_NODE
{
	int	surfId;		/* the engine surface id (index | kind bits) */
	int	kind;		/* ALLEYCAT_ROAD_* */
	int	x, y, z;	/* world units */
	int	numLanes;	/* 0 for a junction (it holds exits, not lanes) */
	int	speedLimit;	/* 0..3, 0 for a junction */
} ALLEYCAT_ROAD_NODE;

typedef struct ALLEYCAT_ROAD_EDGE
{
	int	a, b;		/* node indices */
} ALLEYCAT_ROAD_EDGE;

typedef struct ALLEYCAT_ROAD_GRAPH
{
	int	valid;
	int	nodeCount;
	int	edgeCount;

	/* what the engine reported, for the log / dump cross-check */
	int	straights;
	int	curves;
	int	junctions;

	ALLEYCAT_ROAD_NODE	nodes[ALLEYCAT_ROAD_MAX_NODES];
	ALLEYCAT_ROAD_EDGE	edges[ALLEYCAT_ROAD_MAX_EDGES];

	int	truncated;	/* 1 when a table hit its cap and entries were dropped */
} ALLEYCAT_ROAD_GRAPH;

/* Enumerate the engine's road tables into `graph`. Returns 1 when a level's
 * road data is loaded and the graph was built, 0 otherwise (frontend, no level).
 * The graph is cleared first, so a failed build leaves counts at 0. */
int AlleycatRoadBuild(ALLEYCAT_ROAD_GRAPH* graph);

/* The node index for a surface id, or -1. Resolution is by scanning nodes; the
 * graph is small and this is used while building and by the dump. */
int AlleycatRoadNodeForSurf(const ALLEYCAT_ROAD_GRAPH* graph, int surfId);

/* Write `graph` to Alleycat_roadgraph.json + Alleycat_roadgraph.obj in the working
 * directory (the OBJ in world/4096, X mirrored). Returns 1 when something was
 * written. Defined in alleycat_dump.c. */
int AlleycatDumpRoadGraph(const ALLEYCAT_ROAD_GRAPH* graph);

#ifdef __cplusplus
}
#endif

#endif /* ALLEYCAT_ROAD_H */
