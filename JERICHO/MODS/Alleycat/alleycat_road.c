/*
 * alleycat_road.c -- enumerate the engine's road tables into a node/edge graph.
 * See alleycat_road.h.
 *
 * A thin reader of three engine globals (dr2roads.h):
 *
 *   Driver2StraightsPtr[NumDriver2Straights]   each: Midx/Midz, ConnectIdx[4], lanes
 *   Driver2CurvesPtr[NumDriver2Curves]         each: Midx/Midz, ConnectIdx[4], lanes
 *   Driver2JunctionsPtr[NumDriver2Junctions]   each: ExitIdx[4] (no position)
 *
 * Surface ids follow the engine's encoding: straights are their table index;
 * curves are 0x4000 | index; junctions are 0x2000 | index (dr2roads.h). The
 * ConnectIdx values use the same encoding, so a link resolves straight to a
 * node.
 */

#include <string.h>

#include "jericho.h"
#include "driver2.h"
#include "dr2roads.h"	/* the road tables, MapHeight, ROAD_LANES_COUNT, IS_*_SURFACE */

#include "alleycat_road.h"

static int  sAccN[ALLEYCAT_ROAD_MAX_NODES];
static long sAccX[ALLEYCAT_ROAD_MAX_NODES];
static long sAccZ[ALLEYCAT_ROAD_MAX_NODES];

int AlleycatRoadNodeForSurf(const ALLEYCAT_ROAD_GRAPH* graph, int surfId)
{
	int i;

	if (graph == NULL || surfId < 0)
		return -1;

	for (i = 0; i < graph->nodeCount; i++)
	{
		if (graph->nodes[i].surfId == surfId)
			return i;
	}

	return -1;
}

static int AlleycatRoadAddNode(ALLEYCAT_ROAD_GRAPH* g, int surfId, int kind,
	int x, int y, int z, int lanes, int limit)
{
	ALLEYCAT_ROAD_NODE* n;

	if (g->nodeCount >= ALLEYCAT_ROAD_MAX_NODES)
	{
		g->truncated = 1;
		return -1;
	}

	n = &g->nodes[g->nodeCount];
	n->surfId = surfId;
	n->kind = kind;
	n->x = x;
	n->y = y;
	n->z = z;
	n->numLanes = lanes;
	n->speedLimit = limit;

	return g->nodeCount++;
}

static void AlleycatRoadAddEdge(ALLEYCAT_ROAD_GRAPH* g, int a, int b)
{
	int i;

	if (a < 0 || b < 0 || a == b)
		return;

	/* de-duplicate: a link A->B and the reverse B->A are the same street */
	for (i = 0; i < g->edgeCount; i++)
	{
		if ((g->edges[i].a == a && g->edges[i].b == b) ||
		    (g->edges[i].a == b && g->edges[i].b == a))
			return;
	}

	if (g->edgeCount >= ALLEYCAT_ROAD_MAX_EDGES)
	{
		g->truncated = 1;
		return;
	}

	g->edges[g->edgeCount].a = a;
	g->edges[g->edgeCount].b = b;
	g->edgeCount++;
}

static int AlleycatRoadHeight(int x, int z)
{
	VECTOR v;

	v.vx = x;
	v.vy = 0;
	v.vz = z;

	return MapHeight(&v);
}

/* surface id -> node index, tolerating an unresolved (-1) or unknown id */
static int AlleycatRoadNodeForId(const ALLEYCAT_ROAD_GRAPH* g, int surfId)
{
	if (surfId < 0)
		return -1;

	return AlleycatRoadNodeForSurf(g, surfId);
}

int AlleycatRoadBuild(ALLEYCAT_ROAD_GRAPH* g)
{
	int i, j;

	if (g == NULL)
		return 0;

	memset(g, 0, sizeof(*g));

	/* No level: the tables are empty until the level lumps load. */
	if ((NumDriver2Straights <= 0 && NumDriver2Curves <= 0 && NumDriver2Junctions <= 0) ||
	    (Driver2StraightsPtr == NULL && Driver2CurvesPtr == NULL && Driver2JunctionsPtr == NULL))
		return 0;

	g->straights = NumDriver2Straights;
	g->curves = NumDriver2Curves;
	g->junctions = NumDriver2Junctions;

	/* 1) straights and curves carry their own midpoint */
	if (Driver2StraightsPtr != NULL)
	{
		for (i = 0; i < NumDriver2Straights; i++)
		{
			DRIVER2_STRAIGHT* s = &Driver2StraightsPtr[i];

			AlleycatRoadAddNode(g, i, ALLEYCAT_ROAD_STRAIGHT, s->Midx,
				AlleycatRoadHeight(s->Midx, s->Midz), s->Midz,
				(int)ROAD_LANES_COUNT(s), (int)ROAD_SPEED_LIMIT(s));
		}
	}

	if (Driver2CurvesPtr != NULL)
	{
		for (i = 0; i < NumDriver2Curves; i++)
		{
			DRIVER2_CURVE* c = &Driver2CurvesPtr[i];

			AlleycatRoadAddNode(g, 0x4000 | i, ALLEYCAT_ROAD_CURVE, c->Midx,
				AlleycatRoadHeight(c->Midx, c->Midz), c->Midz,
				(int)ROAD_LANES_COUNT(c), (int)ROAD_SPEED_LIMIT(c));
		}
	}

	/* 2) junctions have no position of their own: place each at the average of
	 * the surfaces that connect to it */
	if (Driver2JunctionsPtr != NULL)
	{
		for (i = 0; i < NumDriver2Junctions; i++)
			AlleycatRoadAddNode(g, 0x2000 | i, ALLEYCAT_ROAD_JUNCTION, 0, 0, 0, 0, 0);

		memset(sAccN, 0, sizeof(sAccN));

		for (i = 0; i < g->nodeCount; i++)
		{
			sAccX[i] = 0;
			sAccZ[i] = 0;
		}

		if (Driver2StraightsPtr != NULL)
		{
			for (i = 0; i < NumDriver2Straights; i++)
			{
				DRIVER2_STRAIGHT* s = &Driver2StraightsPtr[i];

				for (j = 0; j < 4; j++)
				{
					int c = s->ConnectIdx[j];
					int n;

					if (!IS_JUNCTION_SURFACE(c))
						continue;

					n = AlleycatRoadNodeForSurf(g, c);
					if (n >= 0)
					{
						sAccX[n] += s->Midx;
						sAccZ[n] += s->Midz;
						sAccN[n]++;
					}
				}
			}
		}

		if (Driver2CurvesPtr != NULL)
		{
			for (i = 0; i < NumDriver2Curves; i++)
			{
				DRIVER2_CURVE* c = &Driver2CurvesPtr[i];

				for (j = 0; j < 4; j++)
				{
					int cc = c->ConnectIdx[j];
					int n;

					if (!IS_JUNCTION_SURFACE(cc))
						continue;

					n = AlleycatRoadNodeForSurf(g, cc);
					if (n >= 0)
					{
						sAccX[n] += c->Midx;
						sAccZ[n] += c->Midz;
						sAccN[n]++;
					}
				}
			}
		}

		for (i = 0; i < g->nodeCount; i++)
		{
			if (g->nodes[i].kind != ALLEYCAT_ROAD_JUNCTION || sAccN[i] <= 0)
				continue;

			g->nodes[i].x = (int)(sAccX[i] / sAccN[i]);
			g->nodes[i].z = (int)(sAccZ[i] / sAccN[i]);
			g->nodes[i].y = AlleycatRoadHeight(g->nodes[i].x, g->nodes[i].z);
		}
	}

	/* 3) edges: every surface's connects, and every junction's exits */
	if (Driver2StraightsPtr != NULL)
	{
		for (i = 0; i < NumDriver2Straights; i++)
		{
			DRIVER2_STRAIGHT* s = &Driver2StraightsPtr[i];
			int a = AlleycatRoadNodeForSurf(g, i);

			for (j = 0; j < 4; j++)
				AlleycatRoadAddEdge(g, a, AlleycatRoadNodeForId(g, s->ConnectIdx[j]));
		}
	}

	if (Driver2CurvesPtr != NULL)
	{
		for (i = 0; i < NumDriver2Curves; i++)
		{
			DRIVER2_CURVE* c = &Driver2CurvesPtr[i];
			int a = AlleycatRoadNodeForSurf(g, 0x4000 | i);

			for (j = 0; j < 4; j++)
				AlleycatRoadAddEdge(g, a, AlleycatRoadNodeForId(g, c->ConnectIdx[j]));
		}
	}

	if (Driver2JunctionsPtr != NULL)
	{
		for (i = 0; i < NumDriver2Junctions; i++)
		{
			DRIVER2_JUNCTION* jn = &Driver2JunctionsPtr[i];
			int a = AlleycatRoadNodeForSurf(g, 0x2000 | i);

			for (j = 0; j < 4; j++)
				AlleycatRoadAddEdge(g, a, AlleycatRoadNodeForId(g, jn->ExitIdx[j]));
		}
	}

	g->valid = (g->nodeCount > 0);

	return g->valid;
}
