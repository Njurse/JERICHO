// hud/lockon.h — the lock-on readout (hud/lockon.c).
#ifndef CD2_HUD_LOCKON_H
#define CD2_HUD_LOCKON_H

#include "driver2.h"
#include "jericho.h"

// Register the readout: the top-right name panel + the map's corner. Called
// once from the module entry (cainescrossfire.c).
void cd2LockOnRegister(JERICHO_CONTEXT* ctx);

// The car the player is currently locked onto (-1 = none). For tests/debug.
int cd2LockOnTarget(void);

#endif /* CD2_HUD_LOCKON_H */
