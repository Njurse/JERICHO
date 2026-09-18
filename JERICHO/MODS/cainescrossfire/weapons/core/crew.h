// weapons/core/crew.h — Combat D2 MOUNTED CREW: per-car lean request state.
//
// The crew module turns the weapon lean flags (CD2_WEAPON_DEF.leanOut) into
// per-car, per-side "a crew member is hanging out of this window" state. Every
// weapon fire — the player's and the AI's — goes through cd2WpnTryFire, which
// calls cd2CrewNotifyFire with that weapon's lean flags; the flags are ORed
// into the car's request and refresh that side's hold timer. A side stays out
// while ANY leaning weapon keeps firing (the base MG, leanOut 0, never clears
// it). The ped spawn/pose/despawn work lives in crew.c.

#ifndef CD2_CREW_H
#define CD2_CREW_H

#include "driver2.h"
#include "cainescrossfire.h"

// Lean flag bits (same values as CD2_WEAPON_DEF.leanOut).
#define CD2_CREW_DRIVER	1	// the driver, out of the LEFT window
#define CD2_CREW_GUNNER	2	// the gunner, out of the RIGHT window

// A weapon fired from `car` with lean flags `leanMask` (its leanOut). ORs the
// flags into that car's crew request and refreshes the hold on each set side.
// Safe on any car (traffic simply never carries a leaning weapon).
void cd2CrewNotifyFire(const CAR_DATA* car, int leanMask);

// The car's currently SELECTED/armed weapon's lean flags, refreshed every frame.
// Unlike a single shot, this keeps the sides out for as long as that weapon
// stays selected — selecting a leaning weapon brings the crew out and it stays
// out until another weapon is selected (or the weapon fires and the fire hold
// bridges the gap). Same OR semantics as cd2CrewNotifyFire.
void cd2CrewArmed(const CAR_DATA* car, int leanMask);

// Whether a car has that side currently out of the window:
// 1 while out, 0 while in. side = CD2_CREW_DRIVER or CD2_CREW_GUNNER.
int cd2CrewSideOut(const CAR_DATA* car, int side);

// How many crew peds are currently spawned (0..2*MAX_CARS). For tests/debug.
int cd2CrewPedCount(void);

// The world position (engine Y-down format: x, y, z) of a car's crew ped on
// `side`, or 0 if there is no ped. For tests/debug.
int cd2CrewPedPos(const CAR_DATA* car, int side, int out[3]);

// Register the crew hooks (FRAME decay + GAME_START reset). Called once from
// the module entry (cainescrossfire.c).
void cd2CrewRegister(JERICHO_CONTEXT* ctx);

#endif /* CD2_CREW_H */
