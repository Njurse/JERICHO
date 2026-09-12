// weapons/core/weapon.h — Combat D2 WEAPON FRAMEWORK: types + public API.
//
// Car weapons are DATA + behaviour grouped by a functional CLASS. Each
// weapon is one CD2_WEAPON_DEF row (a table in its own source folder) and
// is driven by the class's shared pool/step/draw code:
//
//   RAYCAST    - a fast travelling particle that tests a car/ground hit as
//                it flies (the machine gun bullet). NOT an instant hitscan.
//   PROJECTILE - a slower moving projectile that explodes where it lands
//                (the missile).
//   AOE        - an area burst helper (explosion FX + radial damage) shared
//                by the projectile and drop classes.
//   DROP       - a placed object that arms where it lands and triggers when
//                a NON-caster car comes near it (the mine). Hidden: never in
//                pickups and never part of a starting arsenal.
//
// Every weapon shares the SAME common field set (damage, speed, range,
// radius, splash*, explosionEffect, homing [field only for now], pickup
// amount, ...) so new weapons are added by writing one def row + a fire
// function in the matching class folder.

#ifndef CD2_WEAPON_H
#define CD2_WEAPON_H

#include "combatd2.h"

// ---------------------------------------------------------------------------
// Functional classes
// ---------------------------------------------------------------------------
enum
{
	CD2_WCLS_RAYCAST = 0,	// fast particle, per-frame hit test (machine gun)
	CD2_WCLS_PROJECTILE,	// moving projectile with impact explosion (missile)
	CD2_WCLS_AOE,		// area burst (reserved; explosion helper today)
	CD2_WCLS_DROP,		// placed object with a proximity trigger (mine)
	CD2_WCLS_COUNT
};

// ---------------------------------------------------------------------------
// Stable weapon ids (index into the registry)
// ---------------------------------------------------------------------------
enum
{
	CD2_WID_MG = 0,		// base sidearm (raycast) — always available
	CD2_WID_MISSILE,	// primary (projectile)
	CD2_WID_MINE,		// hidden drop
	CD2_WID_COUNT,
	CD2_WID_NONE = -1
};

// ---------------------------------------------------------------------------
// Common per-weapon definition (the shared field set for a class)
// ---------------------------------------------------------------------------
typedef struct CD2_WEAPON_DEF
{
	int id;			// CD2_WID_*
	const char* name;	// short HUD name
	int cls;		// CD2_WCLS_*

	int isBase;		// 1 = always-available sidearm, excluded from the cycle
	int hidden;		// 1 = never in pickups and never in a starting arsenal
	int pickupEnabled;	// 1 = may be spawned as a drive-over map pickup
	int pickupAmmo;		// rounds a pickup grants (field only for now)

	int maxAmmo;		// capacity (0 = infinite / never tracked)
	int fireInterval;	// frames between shots while the trigger is held

	int damage;		// direct-hit damage (ApplyDamage units)
	int speed;		// world units / frame
	int range;		// max travel before the shot fizzles (world units)
	int life;		// frames a shot/animation lives

	int radius;		// proximity trigger radius (drops)
	int splashRadius;	// explosion radius
	int splashDamage;	// explosion damage at the blast centre
	int explosionEffect;	// BIG_BANG / LITTLE_BANG (dr2types.h)
	int homing;		// RESERVED: field only, no logic this turn

	int colR, colG, colB;	// draw colour

	// Spawn the weapon (muzzle + direction + velocity come from the car).
	// Lives in the weapon's own class folder.
	void (*fire)(void* car);	// CAR_DATA*
} CD2_WEAPON_DEF;

// ---------------------------------------------------------------------------
// Registry + inventory API (implemented in core/weapons.c)
// ---------------------------------------------------------------------------
const CD2_WEAPON_DEF* cd2WpnDef(int weaponId);

int  cd2WpnOwns(int weaponId);		// 1 when this weapon is carried
int  cd2WpnAmmo(int weaponId);		// rounds left (0 for an infinite base weapon)
int  cd2WpnSelected(void);		// CD2_WID_* currently armed (never a base weapon)

int  cd2WpnCycle(int dir);		// move the selection (+1 next / -1 previous)
void cd2WpnGrant(int weaponId, int ammo);	// give ammo (auto-select if new)
void cd2WpnClear(int weaponId);		// drop a carried weapon

void cd2WpnGrantAllMax(void);		// debug: every weapon to its max capacity
void cd2WpnResetAll(void);		// fresh level: reset inventory + all instances

// Human-readable name for the HUD ("MG", "MISSILE", ...).
const char* cd2WpnName(int weaponId);

#endif /* CD2_WEAPON_H */
