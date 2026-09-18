// ai/grid.h — Combat D2 scenery navigation: a coarse local drivability grid
// with an A* search, used when the goal is off the road graph (or the road
// route is much longer than the straight line).
//
// The grid is a window around the start/goal bounding box, rasterised on
// demand at CD2_GRID_CELL world units per cell: a cell is drivable when it is
// clear of scenery objects and passable from its neighbours (no cliff).

#ifndef CD2_GRID_H
#define CD2_GRID_H

#include "driver2.h"

// Search a drivable path from `from` to `to`. Fills up to maxWp world waypoints
// (excluding `from`, including `to`) and returns the count (0 = no path).
// *expanded receives the number of cells expanded by the search.
int cd2GridPath(const VECTOR* from, const VECTOR* to, VECTOR* outWp, int maxWp, int* expanded);

// nav_debug: draw the last searched grid window (drivable/blocked cells and the
// path) into the world pass.
void cd2GridDraw(void);

#endif /* CD2_GRID_H */
