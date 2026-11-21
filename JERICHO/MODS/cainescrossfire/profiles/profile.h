// profiles/profile.h — VEHICLE PROFILES: the Twisted Metal roster.
//
// A vehicle profile is the *identity card* of one contestant's car. Where the
// engine knows a car only as "resident slot N, cosmetics from the level file",
// a profile names it, maps it back to the exact vehicle in a city's (or car
// bank's) asset files, gives it a set of 1..5 core stats, its own special
// weapon, and a set of native-physics overrides it applies to that vehicle's
// CAR_COSMETICS.
//
// The four axes of a profile:
//
//   IDENTITY             internalName / displayName and homeCity — which of the
//                        four stock arenas the contestant belongs to. This is
//                        what the default "field the home city's roster" rule
//                        reads. It does NOT have to be the vehicle's source.
//   VEHICLE SOURCE       vehicleCityName + modelSlot: the model NUMBER inside
//                        that city's or bank's CARMODEL_<n> set. A stock city
//                        is a LevelNames[] index (CHICAGO 0, HAVANA 1, VEGAS 2,
//                        RIO 3); a carhacks bank is named (e.g. "CROSSFIRE")
//                        and resolved through the engine's city registry.
//                        Optional bankLev/bankLcf let the profile register the
//                        bank itself when carhacks/cities.ini have not.
//   CORE STATS           Armor / Speed / Handling / Special Power, each 1..5.
//                        These are the design + HUD numbers (and the AI's
//                        weighting); they do not themselves drive the sim.
//   PHYSICS OVERRIDES    per-field substitutes for the vehicle's CAR_COSMETICS
//                        (mass, powerRatio, traction, ...) so a profile can
//                        dictate the actual handling — or inherit the model's
//                        own values (CD2_VEH_INHERIT) and change nothing.
//
// The roster is DATA: one JSON file per profile under profiles/, generated
// into profiles/generated_profiles.c by tools/gen_profiles.py. A new contestant
// is a new JSON file; the build regenerates the table (JERICHO/build.py).
//
// Special weapons are INTERNALLY named special_<carname> (special_hornet
// carries `special_hornet`) and carry their own display name for the HUD —
// internal id and on-screen name are deliberately separate fields.

#ifndef CD2_PROFILE_H
#define CD2_PROFILE_H

#include "driver2.h"
#include "cainescrossfire.h"
#include "jericho.h"			/* JERICHO_CONTEXT (cd2VehRegister) */
#include "weapons/core/weapon.h"	/* CD2_WID_* — a profile's special weapon */

// ---------------------------------------------------------------------------
// Profile ids are runtime indices into the generated table. There is no
// compile-time per-car enum any more: a profile is identified by its JSON
// "internal" name (cd2VehFindByName) or by its index (cd2VehDef).
// ---------------------------------------------------------------------------
#define CD2_VEH_MAX_PROFILES 64
#define CD2_VEH_NONE         (-1)

// ---------------------------------------------------------------------------
// Cities — LevelNames[] indices (system.c). Named so a row reads as its city.
// These are the four stock arenas; a car-data BANK is identified by name and
// resolved at runtime (see cd2VehVehicleCityOf).
// ---------------------------------------------------------------------------
enum
{
	CD2_VEH_CITY_CHICAGO = 0,
	CD2_VEH_CITY_HAVANA  = 1,
	CD2_VEH_CITY_VEGAS   = 2,
	CD2_VEH_CITY_RIO     = 3
};

// A profile's model is the NUMBER in its source's CARMODEL_<n> set (0..12),
// the scheme in carhacks/docs/VEHICLES.md. -1 = "no fixed model" (any the
// level loaded).

// ---------------------------------------------------------------------------
// Core stats — the Twisted Metal 1..5 scale (fields of CD2_VEH_PROFILE).
// ---------------------------------------------------------------------------

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
	int wheelSize;		// wheel size — stock cars run ~49-53; keep the same scale
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
// Crew (mounted driver/passenger) mount + pose offsets.
//
// A body whose doors sit somewhere the default mount does not fit - a limo, a
// truck, a very wide low car - can move the crew's mount point, turn the body,
// raise the seated passenger and scale the weapon arm. EVERY field is a DELTA
// on the module default and uses CD2_VEH_INHERIT (0) to leave that default
// alone, so a profile only states what it has to change.
//
// Sides: index 0 = the DRIVER (left door), index 1 = the GUNNER/passenger
// (right door). Distances are the same world units the default placement uses.
// ---------------------------------------------------------------------------
typedef struct CD2_VEH_CREW
{
	int lat[2];		// further out from the door centre (+ = further out)
	int fwd[2];		// further ahead of the cabin centre (+ = forward)
	int up[2];		// higher (+ = up; render frame is Y-down)
	int yaw[2];		// extra body yaw, PSX angle units (window out of true)
	int sillRaise;		// extra perch height for the seated passenger
	int armScale;		// arm reach, % of the default (0 = 100)
} CD2_VEH_CREW;

// The profile row. The field order is the generated initializer order; see
// profiles/generated_profiles.c.
// ---------------------------------------------------------------------------
typedef struct CD2_VEH_PROFILE
{
	const char* internalName;	// "hornet" (code-facing)
	const char* displayName;	// "Hornet" (on-screen)

	int homeCity;			// CD2_VEH_CITY_* (-1 = no home arena)
	const char* vehicleCityName;	// "CHICAGO" or a carhacks bank name
	int modelSlot;			// model number in that source
	const char* bankLev;		// optional: register the bank from this .LEV
	const char* bankLcf;		// optional: its .LCF

	/* armor, speed, handling, specialPower (1..5) */
	int armor, speed, handling, specialPower;

	// The car's SPECIAL WEAPON, by its code name (e.g. "special_hornet").
	// Everything ABOUT it (name, ammo carried, recharge, behaviour) belongs to
	// the weapon, in its own file under weapons/special/. The id is resolved
	// once at boot by cd2VehSpecialOf.
	const char* specialName;

	CD2_VEH_PHYS phys;

	int palette;			// preferred paint palette index, -1 = any / none

	// Per-side crew mount/pose deltas (see CD2_VEH_CREW above).
	CD2_VEH_CREW crew;
} CD2_VEH_PROFILE;

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------
// The row for a profile id, or NULL when the id is out of range.
const CD2_VEH_PROFILE* cd2VehDef(int profileId);

// How many profiles are registered (the generated table length).
int cd2VehCount(void);

// The code-facing / on-screen names. Both fall back to "-" for CD2_VEH_NONE.
const char* cd2VehInternalName(int profileId);
const char* cd2VehDisplayName(int profileId);

// The profile whose vehicle source is (city, model), or CD2_VEH_NONE. A custom
// bank is matched by its resolved city index (see cd2VehVehicleCityOf).
int cd2VehFindByVehicle(int city, int model);

// The profile whose code name is `name` (case-sensitive), or CD2_VEH_NONE.
int cd2VehFindByName(const char* name);

// Identity vs vehicle source. `cd2VehHomeCityOf` is the static home arena;
// `cd2VehVehicleCityOf` resolves vehicleCityName through JerCityName() and,
// when the profile supplied bankLev, registers the bank on demand. Both answer
// -1 when unresolved.
int cd2VehHomeCityOf(int profileId);
int cd2VehVehicleCityOf(int profileId);

// The profile's special weapon id (CD2_WID_*), resolved from specialName at
// boot; CD2_WID_NONE when the profile has none or the name is unknown.
int cd2VehSpecialOf(int profileId);

// Resolve every profile's city and special. Called by cd2VehRegister before the
// hooks fire; safe to call again (bank registration is idempotent).
void cd2VehLoad(void);

// Log one line per registered profile: identity, home/vehicle mapping, stats,
// special and the physics overrides that are set. Unconditional and cheap —
// this is the evidence a headless run is read for.
void cd2VehDumpProfiles(void);

// ---------------------------------------------------------------------------
// Engine mapping (profiles/profiles_map.c)
// ---------------------------------------------------------------------------
// Which vehicle profiles this match fields, how each resolved into a resident
// car slot, and the per-car assignment.
//
// Fielded set: the [cainescrossfire] profiles list (internal names, comma
// separated), or — when that is empty — the profiles whose homeCity is the
// level's own city (the level's home vehicle). CC_PROFILES overrides it for a
// headless run without touching the saved config. The player's chosen profile
// (cd2VehSetPlayerProfile) is always fielded.
int  cd2VehIsFielded(int profileId);
int  cd2VehFieldedCount(void);

// The profile the PLAYER drives this match (the car-select pick), or
// CD2_VEH_NONE. Always fielded, so its model is placed into a resident slot.
int  cd2VehPlayerProfile(void);
void cd2VehSetPlayerProfile(int profileId);

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
