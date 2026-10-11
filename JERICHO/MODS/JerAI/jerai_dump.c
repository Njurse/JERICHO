/*
 * jerai_dump.c -- write the road graph to files for offline inspection.
 *
 * Two outputs, written into the game's working directory:
 *
 *   JerAI_roadgraph.json   the graph as data: nodes (surfId, kind, world x/y/z,
 *                          lanes, speed limit) and edges (node index pairs);
 *   JerAI_roadgraph.obj    the same graph as geometry: one vertex per node and
 *                          one `l` (line) per edge, for the level tool.
 *
 * The OBJ uses the convention the DriverLevelTool's own export uses - world
 * units / 4096, with X mirrored - so it lines up with the level geometry the
 * tool already knows how to place. The JSON keeps the raw engine world units.
 *
 * This module deliberately does not guess the transform at load time: it is a
 * straight read of the file-format note, and both files carry the numbers so a
 * mismatch is visible rather than silent.
 */

#include <stdio.h>

#include "jerai_road.h"

#define JERAI_DUMP_JSON	"JerAI_roadgraph.json"
#define JERAI_DUMP_OBJ	"JerAI_roadgraph.obj"

static const char* JerAiRoadKindName(int kind)
{
	switch (kind)
	{
	case JERAI_ROAD_STRAIGHT:	return "straight";
	case JERAI_ROAD_CURVE:		return "curve";
	case JERAI_ROAD_JUNCTION:	return "junction";
	default:			return "unknown";
	}
}

int JerAiDumpRoadGraph(const JERAI_ROAD_GRAPH* g)
{
	FILE* f;
	int i;

	if (g == NULL || !g->valid)
		return 0;

	f = fopen(JERAI_DUMP_JSON, "w");

	if (f != NULL)
	{
		fprintf(f, "{\n");
		fprintf(f, "  \"nodes\": %d, \"edges\": %d,\n", g->nodeCount, g->edgeCount);
		fprintf(f, "  \"straights\": %d, \"curves\": %d, \"junctions\": %d,\n",
			g->straights, g->curves, g->junctions);
		fprintf(f, "  \"node\": [\n");

		for (i = 0; i < g->nodeCount; i++)
		{
			const JERAI_ROAD_NODE* n = &g->nodes[i];

			fprintf(f, "    {\"id\":%d,\"surf\":%d,\"kind\":\"%s\",\"x\":%d,\"y\":%d,\"z\":%d,\"lanes\":%d,\"speed\":%d}%s\n",
				i, n->surfId, JerAiRoadKindName(n->kind),
				n->x, n->y, n->z, n->numLanes, n->speedLimit,
				(i + 1 < g->nodeCount) ? "," : "");
		}

		fprintf(f, "  ],\n  \"edge\": [\n");

		for (i = 0; i < g->edgeCount; i++)
		{
			fprintf(f, "    {\"a\":%d,\"b\":%d}%s\n",
				g->edges[i].a, g->edges[i].b,
				(i + 1 < g->edgeCount) ? "," : "");
		}

		fprintf(f, "  ]\n}\n");
		fclose(f);
	}

	f = fopen(JERAI_DUMP_OBJ, "w");

	if (f != NULL)
	{
		fprintf(f, "# JerAI road graph: nodes as vertices, edges as lines.\n");
		fprintf(f, "# Coordinates are world/4096 with X mirrored (the DriverLevelTool convention).\n");
		fprintf(f, "# %d nodes, %d edges (straights=%d curves=%d junctions=%d)\n",
			g->nodeCount, g->edgeCount, g->straights, g->curves, g->junctions);

		for (i = 0; i < g->nodeCount; i++)
		{
			const JERAI_ROAD_NODE* n = &g->nodes[i];

			fprintf(f, "v %d %d %d\n", -n->x / 4096, n->y / 4096, n->z / 4096);
		}

		for (i = 0; i < g->edgeCount; i++)
			fprintf(f, "l %d %d\n", g->edges[i].a + 1, g->edges[i].b + 1);

		fclose(f);
	}

	return 1;
}
