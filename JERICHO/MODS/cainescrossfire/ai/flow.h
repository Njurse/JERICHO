// ai/flow.h — Combat D2 shared pursuit flow field.
//
// A coarse fast-marching distance field (256-unit cells) propagated from a
// moving goal, updated a budgeted number of cells per frame. Any number of
// opponents can then read a heading "downhill" toward the goal in O(1), which
// is what makes a pack of opponents converge on the player cheaply.

#ifndef CD2_FLOW_H
#define CD2_FLOW_H

#include "driver2.h"

// Point the field at a goal (re-centres the window when the goal moves far).
void cd2FlowSetGoal(const VECTOR* goal);

// Advance the propagation by up to `budget` cells (call once per frame).
void cd2FlowUpdate(int budget);

// 1 once any part of the field has been propagated.
int cd2FlowReady(void);

// Number of cells currently carrying a distance (field coverage / cost probe).
int cd2FlowCoverage(void);

// Downhill heading from `from` toward the goal (PSX angle units).
// Returns 1 when a heading is available, 0 otherwise.
int cd2FlowDir(const VECTOR* from, int* outHeading);

// nav_debug: draw the field (a tick per propagated cell sized by distance, plus
// downhill arrows) into the world pass.
void cd2FlowDraw(void);

#endif /* CD2_FLOW_H */
