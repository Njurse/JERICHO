// motion/shove.h — one-shot velocity pushes for the module's own car sim.
#ifndef CD2_MOTION_SHOVE_H
#define CD2_MOTION_SHOVE_H

#include "driver2.h"
#include "jericho.h"

// Push `carId` by `pct` percent of its OWN top speed, along the world-space unit
// vector (dirX, dirZ) *4096. Several requests in one frame accumulate, and the
// sim spends them at the top of its step - the same place the turbo's engagement
// shove is applied, and for the same reason: that is where velX/velZ ARE the
// car's velocity.
//
// A weapon that writes cp->st.n.linearVelocity instead is writing to a field the
// sim overwrites from its own state before the car moves; the write is simply
// gone. The magnitude also has to be in these terms - a percentage of the car's
// top speed - because a raw velocity delta is meaningless without knowing what
// the sim is holding.
//
// A negative `pct` pushes the other way, so "backwards" is the car's own forward
// vector negated. dirX/dirZ need not be normalised to anything in particular as
// long as the caller and the meaning agree: a 4096-scaled unit vector is the
// convention.
void cd2ShovePush(int carId, int dirX, int dirZ, int pct);

// Take (and clear) this frame's accumulated push for `carId`. Returns 1 when
// there is one, filling dirX/dirZ (a *4096 unit vector) and pct. The sim calls
// this; modules only ever call cd2ShovePush.
int cd2ShoveTake(int carId, int* dirX, int* dirZ, int* pct);

// Drop every pending push (level start / return to the frontend).
void cd2ShoveReset(void);

void cd2ShoveRegister(JERICHO_CONTEXT* ctx);

#endif /* CD2_MOTION_SHOVE_H */
