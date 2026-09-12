// weapons/core/weapon_internal.h — Combat D2 weapon framework INTERNAL wiring.
//
// Not a public API: this header ties together the framework's own source
// files (the class pools and the per-weapon defs) which live in separate
// folders. Only the weapon framework includes it.

#ifndef CD2_WEAPON_INTERNAL_H
#define CD2_WEAPON_INTERNAL_H

#include "driver2.h"
#include "cars.h"
#include "weapons/core/weapon.h"

// ---------------------------------------------------------------------------
// From core/weapons.c — shared helpers used by every class folder
// ---------------------------------------------------------------------------

// Queue a world-space line (y-up) through the engine's depth-sorted debug
// overlay (the same API d2pl's laser uses: y-up -> y-down + OT depth sort).
void cd2WpnLine(const VECTOR* a, const VECTOR* b, int r, int g, int bl);

// Always-on-top '+' marker at p (y-up).
void cd2WpnMark(const VECTOR* p, int r, int g, int bl);

// Is world point p inside car cp's oriented box (y-up)?
int  cd2WpnPointInCar(const CAR_DATA* cp, const VECTOR* p);

// Apply damage to a car from a world impact (region picked from the impact
// direction in the car's local frame).
void cd2WpnDamageCar(CAR_DATA* cp, const VECTOR* at, int value);

// Weapon impact "lever action": add reactive angular knockback (world-space
// angular velocity) based on the impact point's offset from the car centre and
// the shot direction. `strength` is typically the weapon damage.
void cd2WpnKnock(CAR_DATA* cp, const VECTOR* at, const VECTOR* dir, int strength);

// The local player's CAR_DATA, or 0 (out untouched) when there is none.
int  cd2WpnPlayerCar(CAR_DATA** out);

// Fender muzzle for a given side (-1 left, +1 right), y-up world.
void cd2WpnMuzzle(const CAR_DATA* cp, int side, VECTOR* out);

// The car's forward unit vector *4096 (the m[][2] column).
void cd2WpnForward(const CAR_DATA* cp, VECTOR* out);

// The car's velocity in world units/frame (y-up). Weapons add this to their
// muzzle velocity (inertial launch) so a shot always pulls ahead of the car
// that fired it instead of lagging behind.
void cd2WpnCarVelocity(const CAR_DATA* cp, VECTOR* out);

// ---------------------------------------------------------------------------
// From aoe/aoe.c — explosion FX + radial damage (shared by projectile/drop)
// ---------------------------------------------------------------------------
void cd2AoeBlast(const VECTOR* at, int radius, int damage, int effect,
		 const CAR_DATA* skip);

// ---------------------------------------------------------------------------
// Class pools (one source folder each) — reset / step / draw all instances
// ---------------------------------------------------------------------------
void cd2RaycastReset(void);
void cd2RaycastStep(void);
void cd2RaycastDraw(void);

void cd2ProjectileReset(void);
void cd2ProjectileStep(void);
void cd2ProjectileDraw(void);

void cd2DropReset(void);
void cd2DropStep(void);
void cd2DropDraw(void);

// Spawn entry points (weapon files -> their class pool).
void cd2RaycastSpawn(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
		     const VECTOR* from, const VECTOR* dir);
void cd2ProjectileSpawn(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
			const VECTOR* from, const VECTOR* vel, const VECTOR* dir);
void cd2DropSpawn(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
		  const VECTOR* from, const VECTOR* vel);

// Diagnostic: forces + reports the missile model resolution (1 = a model was
// found for CD2_CONFIG.missileModel).
int cd2ProjectileModelValid(void);

// Weapon-threat queries (opponent-AI evasion): does an active shot, not owned
// by `car`, sit within CD2_THREAT_RANGE and close in on it? On a hit the
// nearest threat's position + velocity are written and 1 is returned.
#define CD2_THREAT_RANGE	2000
int cd2ProjectileThreat(const CAR_DATA* car, VECTOR* pos, VECTOR* vel);
int cd2RaycastThreat(const CAR_DATA* car, VECTOR* pos, VECTOR* vel);
int cd2WpnIncomingThreat(const CAR_DATA* car, VECTOR* pos, VECTOR* vel);

// ---------------------------------------------------------------------------
// Per-weapon defs (one source folder each)
// ---------------------------------------------------------------------------
extern const CD2_WEAPON_DEF cd2WdefMG;		// raycast/machinegun.c
extern const CD2_WEAPON_DEF cd2WdefMissile;	// projectile/missile.c
extern const CD2_WEAPON_DEF cd2WdefMine;	// drops/mine.c
extern const CD2_WEAPON_DEF cd2WdefHoming;	// projectile/homing.c

#endif /* CD2_WEAPON_INTERNAL_H */
