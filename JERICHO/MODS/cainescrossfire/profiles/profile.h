// profiles/profile.h — VEHICLE PROFILES: the Twisted Metal roster.
//
// A vehicle profile is the *identity card* of one contestant's car. Where the
// engine knows a car only as "resident slot N, cosmetics from the level file",
// a profile names it, maps it back to the exact vehicle in a city's asset
// files, gives it a set of 1..5 core stats, its own special weapon, and a set
// of native-physics overrides it applies to that vehicle's CAR_COSMETICS.
//
// The three axes of a profile:
//
//   IDENTITY + MAPPING   internalName / displayName, and (originCity,
//                        modelSlot) — the model NUMBER inside that city's
//                        CARMODEL_<n> set. City is a LevelNames[] index
//                        (CHICAGO 0, HAVANA 1, VEGAS 2, RIO 3).
//   CORE STATS           Armor / Speed / Handling / Special Power, each 1..5.
//                        These are the design + HUD numbers (and the AI's
//                        weighting); they do not themselves drive the sim.
//   PHYSICS OVERRIDES    per-field substitutes for the vehicle's CAR_COSMETICS
//                        (mass, powerRatio, traction, ...) so a profile can
//                        dictate the actual handling — or inherit the model's
//                        own values (CD2_VEH_INHERIT) and change nothing.
//
// The registry is one row per vehicle. Each row lives in its OWN file under
// profiles/rows/ so a new contestant is a new file plus one line in the
// manifest (profiles/registry.c), never an edit to a shared table. The build
// globs MODS/<mod>/**/*.c, so dropping a file in is all the build needs.
//
// Special weapons are INTERNALLY named special_<carname> (special.hornet
// carries `special_hornet`) and carry their own display name for the HUD —
// internal id and on-screen name are deliberately separate fields.

#ifndef CD2_PROFILE_H
#define CD2_PROFILE_H

#include "driver2.h"
#include "cainescrossfire.h"
#include "jericho.h"			/* JERICHO_CONTEXT (cd2VehRegister) */
#include "weapons/core/weapon.h"	/* CD2_WID_* — a profile's special weapon */

// ---------------------------------------------------------------------------
// Profile ids (index into the manifest in profiles/registry.c)
// ---------------------------------------------------------------------------
enum
{
	CD2_VEH_HORNET = 0,	// Chicago's light spikes car
	CD2_VEH_AVALANCHE,	// the Vegas monster truck
	CD2_VEH_CORVO,		// Rio's siren/lightning car
	CD2_VEH_BRUXA,		// Rio's double-barrel shotgun car
	CD2_VEH_HIGHWAYMAN,	// Havana's flamethrower car
	CD2_VEH_DEADSTAR,	// Rio's turbo-ram car
	CD2_VEH_COUNT,
	CD2_VEH_NONE = -1
};

// ---------------------------------------------------------------------------
// Cities — LevelNames[] indices (system.c). Named so a row reads as its city.
// ---------------------------------------------------------------------------
enum
{
	CD2_VEH_CITY_CHICAGO = 0,
	CD2_VEH_CITY_HAVANA  = 1,
	CD2_VEH_CITY_VEGAS   = 2,
	CD2_VEH_CITY_RIO     = 3
};

// A profile's model is the NUMBER in that city's CARMODEL_<n> set (0..12), the
// scheme in carhacks/VEHICLES.md. -1 = "no fixed model" (any the level loaded).

// ---------------------------------------------------------------------------
// Core stats — the Twisted Metal 1..5 scale.
// ---------------------------------------------------------------------------
typedef struct CD2_VEH_STATS
{
	int armor;		// 1..5 — damage it can absorb
	int speed;		// 1..5 — top speed / acceleration
	int handling;		// 1..5 — grip + steering authority
	int specialPower;	// 1..5 — how hard the special hits
} CD2_VEH_STATS;

// ---------------------------------------------------------------------------
// Physics overrides — substitutes for the vehicle's CAR_COSMETICS fields.
//
// Every field uses CD2_VEH_INHERIT (0) to mean "leave the model's own value in
// place". A non-zero value is written over the loaded cosmetics, so a profile
// can be as heavy or as light as it likes on any single axis and still pull the
// rest from the original model.
// ---------------------------------------------------------------------------
#define CD2_VEH_INHERIT   0

typedef struct CD2_VEH_PHYS
{
	// direct CAR_COSMETICS fields (see dr2types.h struct CAR_COSMETICS)
	int mass;		// chassis mass (fixed-point scale) — heavier = more push
	int powerRatio;		// engine power; with mass it sets the derived speed
	int traction;		// grip, fixed point (4096 = stock)
	int susCoeff;		// suspension coefficient
	int wheelSize;		// wheel size
	int twistRateX;		// roll/pitch/yaw compliance
	int twistRateY;
	int twistRateZ;
	int cogY;		// centre-of-gravity height (SVECTOR cog.y)

	// sim-level nudge applied on top of the derivation in cd2GetStats, because
	// "lower top speed" cannot be expressed by mass alone (a heavy, powerful car
	// would still be fast). Percent of the derived top speed; 0 = inherit (100).
	int topSpeedPct;
} CD2_VEH_PHYS;

// ---------------------------------------------------------------------------
// Special weapon — the profile's unique weapon slot.
//
// internalName is the stable, code-facing id ("special_hornet"); displayName is
// what the HUD shows. weaponId is the CD2_WID_* the special maps to once its
// def row exists (phase 4); until then it stays CD2_WID_NONE.
// ---------------------------------------------------------------------------
typedef struct CD2_VEH_SPECIAL
{
	int weaponId;			// CD2_WID_* (CD2_WID_NONE until registered)
	const char* internalName;	// "special_<carname>"
	const char* displayName;	// custom on-screen name
	int capacity;			// charges carried
	int rechargeFrames;		// recharge after firing, at 30 Hz
} CD2_VEH_SPECIAL;

// ---------------------------------------------------------------------------
// The profile row.
// ---------------------------------------------------------------------------
typedef struct CD2_VEH_PROFILE
{
	int id;				// CD2_VEH_* (mirrors the manifest index)
	const char* internalName;	// "hornet" (code-facing)
	const char* displayName;	// "Hornet" (on-screen)

	int originCity;			// CD2_VEH_CITY_* (LevelNames index)
	int modelSlot;			// model number in that city's CARMODEL_<n> set

	CD2_VEH_STATS stats;
	CD2_VEH_SPECIAL special;
	CD2_VEH_PHYS phys;

	int palette;			// preferred paint palette index, -1 = any / none
} CD2_VEH_PROFILE;

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------
// The row for a profile id, or NULL when the id is not a CD2_VEH_*.
const CD2_VEH_PROFILE* cd2VehDef(int profileId);

// How many profiles are registered (the manifest length).
int cd2VehCount(void);

// The code-facing / on-screen names. Both fall back to "-" for CD2_VEH_NONE.
const char* cd2VehInternalName(int profileId);
const char* cd2VehDisplayName(int profileId);

// The profile whose (originCity, modelSlot) is this vehicle, or CD2_VEH_NONE.
// This is the (city, model) -> profile lookup a spawn path uses to decide which
// contestant a car is.
int cd2VehFindByVehicle(int city, int model);

// Log one line per registered profile: identity, mapping, stats, special and
// the physics overrides that are set. Unconditional and cheap — this is the
// evidence a headless run is read for.
void cd2VehDumpProfiles(void);

// ---------------------------------------------------------------------------
// Engine mapping (profiles/profiles_map.c)
// ---------------------------------------------------------------------------
// Which vehicle profiles this match fields, how each resolved into a resident
// car slot, and the per-car assignment.
//
// Fielded set: the [cainescrossfire] profiles list (internal names, comma
// separated), or — when that is empty — the profiles whose originCity is the
// level's own city (the level's home vehicle). CC_PROFILES overrides it for a
// headless run without touching the saved config.
int  cd2VehIsFielded(int profileId);
int  cd2VehFieldedCount(void);

// The resident slot a profile resolved to this level (-1 = not placed), and the
// reverse. A resident slot is an index into car_cosmetics[]/residentCarModels[];
// a live car's cp->ap.model is that slot, NOT the model number.
int  cd2VehSlotOf(int profileId);
int  cd2VehProfileOfSlot(int slot);

// The profile a live car belongs to, by its resident slot, or CD2_VEH_NONE.
int  cd2VehOfCar(void* car);

// Per-car profile assignment (mirrors the faction registry's gCd2CarFaction).
// Every car the module drives carries a profile; cd2VehOfCarId is the storage.
// CD2_VEH_NONE means "not assigned yet" — the CAR_STEP sync fills it from the
// car's resident slot.
int  cd2VehOfCarId(int carId);
void cd2VehSetCarProfile(int carId, int profileId);
void cd2VehForgetCar(int carId);

// Log the resolution (profile -> slot/import) and the cosmetic fields applied.
void cd2VehDumpResolution(void);

// Register the CAR_DATA_SOURCE / GAME_START / CAR_STEP / RESET_CAR handlers.
// Called once from the module entry.
void cd2VehRegister(JERICHO_CONTEXT* ctx);

#endif /* CD2_PROFILE_H */
