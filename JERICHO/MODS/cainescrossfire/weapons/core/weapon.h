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

#include "cainescrossfire.h"

// ---------------------------------------------------------------------------
// Functional classes
// ---------------------------------------------------------------------------
enum
{
	CD2_WCLS_RAYCAST = 0,	// fast particle, per-frame hit test (machine gun)
	CD2_WCLS_PROJECTILE,	// moving projectile with impact explosion (missile)
	CD2_WCLS_AOE,		// area burst (reserved; explosion helper today)
	CD2_WCLS_DROP,		// placed object with a proximity trigger (mine)
	CD2_WCLS_SHOTGUN,	// multi-pellet scatter from both fenders
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
	CD2_WID_HOMING,		// primary (projectile) that homes in
	CD2_WID_CLUSTER,	// primary (projectile): impact bursts into bomblets
	CD2_WID_ZOOMY,		// primary (projectile): a burst of weakly-homing shots
	CD2_WID_FREEZE,		// primary (projectile): freezes the car it hits
	CD2_WID_SHOTGUN,	// primary (shotgun): pellet spread from both fenders
	CD2_WID_SMG,		// primary (projectile): a fast 6-shot burst sidearm
	CD2_WID_SPECIAL_HORNET,		// profile specials — each is one vehicle's
	CD2_WID_SPECIAL_AVALANCHE,	// unique weapon, internally named
	CD2_WID_SPECIAL_CORVO,		// "special_<car>" and displayed under its own
	CD2_WID_SPECIAL_BRUXA,		// custom name (see the vehicle profiles in
	CD2_WID_SPECIAL_HIGHWAYMAN,	// profiles/ and profiles/SPECIALS.md)
	CD2_WID_SPECIAL_DEADSTAR,
	CD2_WID_SPECIAL_OBELISK,
	CD2_WID_COUNT,
	CD2_WID_NONE = -1
};

// ---------------------------------------------------------------------------
// Common per-weapon definition (the shared field set for a class)
// ---------------------------------------------------------------------------
typedef struct CD2_WEAPON_DEF
{
	int id;			// CD2_WID_*
	const char* name;	// short HUD name (the code-facing label, e.g. "MG")
	const char* displayName;	// full on-screen name; NULL = use `name`. A
					// special weapon's custom name lives here
					// ("Napalm Cone"), separate from its internal id.
	int cls;		// CD2_WCLS_*

	int isBase;		// 1 = always-available sidearm, excluded from the cycle
	int isSpecial;		// 1 = one vehicle's unique special (per-car ammo/cooldown)
	int hidden;		// 1 = never in pickups and never in a starting arsenal
	int pickupEnabled;	// 1 = may be spawned as a drive-over map pickup
	int pickupAmmo;		// rounds a pickup grants (field only for now)

	int maxAmmo;		// capacity (0 = infinite / never tracked)
	int fireInterval;	// frames between shots while the trigger is held
	int refireCooldown;	// MINIMUM frames between refires, any shooter (0 = none)
	int fireCone;		// firing tolerance: heading error the AI will still
					// shoot through (PSX angle units). Belongs to the weapon, not
					// to the AI: one that homes can be launched way off-axis, one
					// that only flies where it is pointed cannot.

	int leanOut;		// mounted-crew flags: a BITMASK of which crew member
					// leans out of a window WHILE THIS WEAPON IS FIRING.
					//   bit 0 (1) = the driver leans out the LEFT window
					//   bit 1 (2) = the gunner leans out the RIGHT window
					//   3         = both
					//   0         = nobody (e.g. the base machine gun)
					// Several weapons can fire in the same frame (the base MG
					// plus a primary), so the crew module ORs the flags of
					// EVERY weapon a car is firing this frame: a side that is
					// "true" for any firing weapon stays out, even while
					// another firing weapon (the MG, leanOut 0) wants nobody.
					// The crew module hangs that car's ped out of the matching
					// window in a held get-out pose and the weapon fires from
					// there instead of the car's centre.

	int damage;		// direct-hit damage (ApplyDamage units)
	int speed;		// world units / frame
	int range;		// max travel before the shot fizzles (world units)
	int life;		// frames a shot/animation lives

	int radius;		// proximity trigger radius (drops)
	int splashRadius;	// explosion radius
	int splashDamage;	// explosion damage at the blast centre
	int explosionEffect;	// BIG_BANG / LITTLE_BANG (dr2types.h)
	int impactFx;		// CD2_FX_* explosion profile id for the impact FX
				// (0 = use the stock explosionEffect above). See
				// weapons/fx/fx.h. Themed so a hit reads as its weapon.
	int homing;		// 1 = the shot steers itself toward a target
	int homingRate;		// homing turn per frame (fraction of 1024 blended,
				// same units as CD2_PROJ_HOME_TURN); 0 = use the
				// default. Lower = weaker (the seeker's default 340;
				// "extremely weak" is ~40).
	int collideScenery;	// 1 = shots stop on buildings/scenery (default ON)

	// -------------------------------------------------------------------
	// Barrage (cluster missile): when barrageCount > 0 the impact ALSO
	// schedules a burst of delayed blasts (see weapons/fx/fx.c). The parent
	// explosion still plays (via impactFx); the burst then scatters small
	// explosions around the hit point.
	// -------------------------------------------------------------------
	int barrageCount;	// blasts in the burst (0 = none)
	int barrageInterval;	// frames between burst blasts
	int barrageJitter;	// +/- world units of scatter per blast
	int barrageFx;		// CD2_FX_* profile for the burst blasts
	int barrageRadius;	// burst blast radius (0 = FX only, no damage)
	int barrageDamage;	// burst blast damage at centre
	int barrageStick;	// 1 = stick the burst to a hit car's hit point

	// -------------------------------------------------------------------
	// Volley (zoomy missiles): when burstCount > 0 the trigger launches that
	// many projectiles, burstInterval frames apart (the projectile pool runs
	// the burst). If ALL of them land on a car, the last to land deals
	// volleyBonusDamage and a volleyBonusKnock-sized shove.
	// -------------------------------------------------------------------
	int burstCount;		// projectiles per trigger (0 = single shot)
	int burstInterval;	// frames between burst projectiles
	int volleyBonusDamage;	// bonus damage the final landing shot deals
	int volleyBonusKnock;	// bonus knockback strength for that final shot

	// -------------------------------------------------------------------
	// Freeze (freeze missile): when freezeFrames > 0, a car hit encases the
	// car in ice for that many frames (cyan bright body, low grip, controls
	// locked). The weapon itself normally does 0 damage.
	// -------------------------------------------------------------------
	int freezeFrames;	// frozen duration on a car hit (0 = none)

	// -------------------------------------------------------------------
	// Shotgun: when pelletCount > 0 the trigger fires that many pellets at
	// once, ALTERNATING between the LEFT and RIGHT fenders, each jittered
	// across a spread cone and biased outward per fender. Pellets are
	// raycast-class particles, so `damage` is PER PELLET and `range` is the
	// (short) shotgun range.
	// -------------------------------------------------------------------
	int pelletCount;	// pellets per shot (0 = a single particle)
	int pelletSpread;	// cone half-width, car-relative fixed-point units
	int pelletFanout;	// outward bias per fender (0 = straight ahead)

	int colR, colG, colB;	// draw colour

	// Spawn the weapon (muzzle + direction + velocity come from the car).
	// Lives in the weapon's own class folder.
	void (*fire)(void* car);	// CAR_DATA*
} CD2_WEAPON_DEF;

// The explosion type/fx id a weapon's impact should use: its themed CD2_FX_*
// profile when set, else the stock bang.
#define CD2_WPN_FX(d)	((d)->impactFx ? (d)->impactFx : (d)->explosionEffect)

// ---------------------------------------------------------------------------
// Registry + inventory API (implemented in core/weapons.c)
// ---------------------------------------------------------------------------
const CD2_WEAPON_DEF* cd2WpnDef(int weaponId);

int  cd2WpnOwns(int weaponId);		// 1 when this weapon is carried
int  cd2WpnAmmo(int weaponId);		// rounds left (0 for an infinite base weapon)
int  cd2WpnSelected(void);		// CD2_WID_* currently armed (never a base weapon)

int  cd2WpnCycle(int dir);		// move the selection (+1 next / -1 previous)

// Fire `weaponId` from `car`, honouring that weapon's minimum refire cooldown
// (per car). Returns 1 when the shot was made. Any shooter - the player's
// trigger or the AI - goes through this.
int  cd2WpnTryFire(void* car, int weaponId);void cd2WpnGrant(int weaponId, int ammo);	// give ammo (auto-select if new)
void cd2WpnClear(int weaponId);		// drop a carried weapon

void cd2WpnGrantAllMax(void);		// debug: every weapon to its max capacity
void cd2WpnResetAll(void);		// fresh level: reset inventory + all instances

// Human-readable name for the HUD ("MG", "MISSILE", ...). This is the short,
// code-facing label; cd2WpnDisplayName is the full on-screen name.
const char* cd2WpnName(int weaponId);
const char* cd2WpnDisplayName(int weaponId);

// ---------------------------------------------------------------------------
// Per-car inventory (a contestant's arsenal, above all its SPECIAL, is its own)
// ---------------------------------------------------------------------------
// The weaponId-only accessors above (cd2WpnOwns/Ammo/Grant/Clear) are the
// PLAYER's; these take any car, for a special's fire path and for modules.
int  cd2WpnCarAmmo(void* car, int weaponId);	// rounds left (base: -1 = infinite)
int  cd2WpnCarOwns(void* car, int weaponId);
void cd2WpnCarGrant(void* car, int weaponId, int ammo);
void cd2WpnCarConsume(void* car, int weaponId);	// burn one round after a real shot
void cd2WpnCarSelect(void* car, int weaponId);	// make it the car's weapon - for the
						// PLAYER, its STARTING weapon (a profile's special)

#endif /* CD2_WEAPON_H */
