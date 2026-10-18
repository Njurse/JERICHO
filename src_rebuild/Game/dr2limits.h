#ifndef DRLIMITS_H
#define DRLIMITS_H

// DRIVER 2 game engine limits
// please populate this file only with engine limits during refactoring


#ifndef PSX
#define DYNAMIC_LIGHTING
#define MAX_DLIGHTS				32
#endif

#ifndef PSX
#define MAX_PLAYERS				16			// used for replay streams mostly
#else
#define MAX_PLAYERS				8			// used for replay streams mostly
#endif

#define MAX_MODEL_SLOTS			1536		// DO NOT CHANGE. No effect in upping it - limited by cell types

/* JERICHO: the car_data slot map.
 *
 * MAX_CARS was 20 and mp cut it in two at the player count: "the first
 * MP_MAX_PLAYERS slots are players, everything above is traffic". That ties the
 * traffic pool to the lobby: every extra player takes a citizen's slot, so a
 * busy session had no traffic left and the map emptied out as people joined.
 * Raising the cap makes the two INDEPENDENT - the traffic pool keeps its size
 * whatever the lobby does, and the surplus is reserved for JERICHO-managed
 * vehicles (players, imported cars, a module's own spawns) that must never be
 * confused with traffic.
 *
 *   [ JER_SLOT_PLAYERS_FIRST .. JER_SLOT_MANAGED_FIRST )   one slot per player id
 *   [ JER_SLOT_MANAGED_FIRST .. JER_SLOT_TRAFFIC_FIRST )   spare, JERICHO-managed
 *   [ JER_SLOT_TRAFFIC_FIRST .. MAX_CARS )                 civilians + police
 *
 * Only the traffic half is a shared identity between machines: mp mirrors a
 * traffic car by its car_data SLOT (the band section in mp_session.c), so that
 * slot has to mean the same car everywhere, which is exactly why traffic needs
 * a region nothing else grows into. A player car is replicated BY MODEL, so its
 * slot number is local and the player half is engine-side only.
 *
 * The #error checks below keep the arithmetic honest: a MAX_CARS change that
 * stops adding up fails the build instead of silently shrinking traffic. */
#define MAX_CARS				32

#define JER_SLOT_TRAFFIC_COUNT	12		/* unchanged: MAX_CARS 20 - 8 players, so the map's
						 * population does not depend on the lobby size */
#define JER_SLOT_TRAFFIC_FIRST	(MAX_CARS - JER_SLOT_TRAFFIC_COUNT)
#define JER_SLOT_PLAYERS_FIRST	0
#define JER_SLOT_MANAGED_FIRST	MAX_PLAYERS	/* PC: players 0..15, managed 16..19, traffic 20..31 */
#define JER_SLOT_MANAGED_COUNT	(JER_SLOT_TRAFFIC_FIRST - JER_SLOT_MANAGED_FIRST)

#if MAX_CARS > 32
#error "MAX_CARS is capped at 32: hd.mayBeColliding is a 32-bit car-index bitmask"
#endif
#if JER_SLOT_MANAGED_FIRST > JER_SLOT_TRAFFIC_FIRST
#error "MAX_CARS is too small for MAX_PLAYERS plus the traffic pool"
#endif
#if (JER_SLOT_TRAFFIC_FIRST + JER_SLOT_TRAFFIC_COUNT) != MAX_CARS
#error "the traffic pool must end exactly at MAX_CARS"
#endif

#ifndef PSX
/* 12 (was 8, and 6 before that): the extra slots beyond the mission header's five
 * exist so a module can keep MANY more distinct vehicles resident at once
 * (cross-city car data, extra models). They are filled with -1 = "no model" in
 * SetupResidentModels, so stock levels are unchanged - the capacity is only used
 * when a module claims a slot. SPECIAL_CAR_SLOT follows this value. Slots 0..4 keep
 * their normal meaning; slot 4 is the engine's own filler.
 *
 * 8 was not enough for a full roster in one battleground: slots 0..4 hold the
 * level's civilian models and SPECIAL_CAR_SLOT holds the special, which left only
 * 5 and 6 for a module to place into, and cainescrossfire's roster needs up to
 * seven distinct cars on the field at once (the player plus six contestants).
 * With 12 the module has six slots of its own.
 *
 * 12 -> 16 for a SIXTEEN-player mp lobby: sixteen players can each pick a
 * different car, and a model that is not resident cannot be built - the car is
 * simply not there. Slots 12..15 are the extra ones, so stock levels are still
 * unchanged (they are filled with -1 = "no model"). */
#define MAX_CAR_RESIDENT_MODELS	16			// use all permanently loaded car models available
#else
#define MAX_CAR_RESIDENT_MODELS	5
#endif

/* Deliberately NOT (MAX_CAR_RESIDENT_MODELS-1) any more. This is a MODEL number,
 * not "the last slot": it is compared against cp->ap.model (the Vegas ambulance
 * in cars.c, mission 33's limo in civ_ai.c), it names the slot levelSpecCosmetics
 * indexes, and residentCarModels[SPECIAL_CAR_SLOT] is how the special model is
 * spooled. It was 11 when the array was 12 long - pinning it keeps every one of
 * those working while the array grew past it. Slots 12..15 are the module's. */
#ifndef PSX
#define SPECIAL_CAR_SLOT        11
#else
#define SPECIAL_CAR_SLOT        (MAX_CAR_RESIDENT_MODELS-1)
#endif

#define MAX_PEDESTRIANS			28
#define MAX_SEATED_PEDS			20
#define MAX_PLACED_PEDS			15
// JERICHO: raised from 5 so a weapon barrage (cluster missiles fire 5 delayed
// bomblets) plus other concurrent impacts do not starve the pool. AddExplosion
// recycles the oldest slot if it ever fills, so this is a ceiling, not a risk.
#define MAX_EXPLOSION_OBJECTS	16
#define MAX_THROWN_BOMBS		5
#define MAX_MOTION_CAPTURE		24

// denting
#define MAX_DAMAGE_ZONE_VERTS	50
#define MAX_DAMAGE_ZONE_POLYS	70
#define MAX_DAMAGE_LEVELS		256

#ifndef PSX
#define MAX_DENTING_VERTS		256
#define MAX_DENTING_UVS			256
#define MAX_DENTING_LOD_UVS		256
#else
#define MAX_DENTING_VERTS		132
#define MAX_DENTING_UVS			255
#define MAX_DENTING_LOD_UVS		134
#endif

// Tyre track visuals
#define MAX_TYRE_PLAYERS		4
#ifndef PSX
#define MAX_TYRE_TRACK_WHEELS	4
#define MAX_TYRE_TRACK_PLAYERS	4
#else
#define MAX_TYRE_TRACK_WHEELS	2
#define MAX_TYRE_TRACK_PLAYERS	2
#endif

// Mission limits
// DO NOT EDIT, breaks compatibility!
#define MAX_MISSION_THREADS		16
#define MAX_MISSION_TARGETS		16

// replay definitions.
// DO NOT EDIT, breaks compatibility!
#define MAX_REPLAY_STREAMS		MAX_PLAYERS
#define MAX_REPLAY_CAMERAS		60
#define MAX_REPLAY_WAYPOINTS	150
#define MAX_REPLAY_PINGS		400

// debris limits
#define MAX_SMOKE				80
#define MAX_LEAVES				50
#define MAX_BIRDS				7
#define MAX_DEBRIS				60
#define MAX_GROUND_DEBRIS		16
#define MAX_DAMAGED_LAMPS		5
#define MAX_SMASHED_OBJECTS		8
#define MAX_LAMP_STREAKS		21
#define MAX_RAIN_DROPS			180

// driving games limit
#define MAX_SMASHED_CONES		6

// sound limits
#define MAX_SFX_CHANNELS		16		// DON'T CHANGE unless you have more than 24 channels (no PSX code)
#define MAX_LEVEL_ENVSOUNDS		32
#define MAX_SIREN_NOISES		2
#define MAX_CAR_NOISES			4


#endif // DRLIMITS_H