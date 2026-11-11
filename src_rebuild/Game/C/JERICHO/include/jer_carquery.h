#ifndef JER_CARQUERY_H
#define JER_CARQUERY_H

/* ------------------------------------------------------------------
 * jer_carquery.h — the LIVE CAR query contract (a CORE contract).
 *
 * Three mods need to agree on one question — "which cars can this session offer,
 * and can you make this one available?" — and none of them owns the answer:
 *
 *   ASKED BY   mp (its Change car row) and sandbox (its own car picker)
 *   ANSWERED BY carhacks, which is the mod that knows about cross-city car data
 *
 * It is a CONTRACT, not a function call. Issued and answered through jer_fire, so
 * a session with no car mod is not an error and nothing has to link: a module that
 * does not answer leaves `count` at 0 / `ok` at 0 and the caller falls back to what
 * the engine itself knows. That is what lets the three mods be built, released and
 * loaded separately — see docs/interfaces.md.
 *
 * This header lives in the CORE include dir (and is mirrored into the SDK), NOT in
 * any mod's folder: a mod including another mod's path is the tie that breaks when
 * that mod becomes its own repository, and tools/check_mod_interface.py fails on
 * it.
 *
 * The ids sit in the core-contracts block of the custom-event registry (+40..+49).
 *
 * ROLES and ROLEPICK run the other way round: the CAR SELECT (carhacks' join menu)
 * asks, and the SESSION (mp, from its game mode) answers.
 * ------------------------------------------------------------------ */

#include "jericho.h"

#define JER_CARQ_CITIES	(JER_EVENT_MODULE_CUSTOM + 40)	/* "which cities?" */
#define JER_CARQ_LOAD	(JER_EVENT_MODULE_CUSTOM + 41)	/* "make (city,model) available" */
#define JER_CARQ_CHOSEN	(JER_EVENT_MODULE_CUSTOM + 42)	/* "this is what we drive now" */
#define JER_CARQ_SLOTS	(JER_EVENT_MODULE_CUSTOM + 43)	/* "which slots may this city offer?" */
#define JER_CARQ_STANDIN	(JER_EVENT_MODULE_CUSTOM + 44)	/* "give me a stand-in body slot" */
#define JER_CARQ_CITYWIRE	(JER_EVENT_MODULE_CUSTOM + 47)	/* "spell this city for the wire / read it back" */
#define JER_CARQ_ROLES		(JER_EVENT_MODULE_CUSTOM + 48)	/* "which cars does the session want picked?" */
#define JER_CARQ_ROLEPICK	(JER_EVENT_MODULE_CUSTOM + 49)	/* "this is the car picked for role N" */

/* Handlers fill the result field of the struct and return
 * JER_RESULT_CONTINUE; the caller reads it. */

/* JER_CARQ_CITIES -> ARGS.count city indices written to ARGS.cities[].
 *
 * `city` is an index into the engine's city registry (CHICAGO/HAVANA/VEGAS/RIO, then
 * Driver 1's car-data cities), the same space JER_CARQ_SLOTS and carNumLookup use.
 * A handler that has nothing to add leaves count at 0 rather than repeating the
 * session's own city, because 0 is what tells the asker "nobody knows better": it
 * then offers the session's city alone. (carhacks answers with every city it can
 * serve, whether or not the config-driven cross-city hack is on -- a PICK is what
 * imports the car, so the city row and the car list are owned by the same roster
 * hack.) */
typedef struct JER_CARQ_CITIES_ARGS
{
	int* cities;	/* out: city indices, most preferred first */
	int  max;	/* in: capacity of cities[] */
	int  count;	/* out: how many were written; 0 = nobody knows */
} JER_CARQ_CITIES_ARGS;

/* JER_CARQ_SLOTS -> ARGS.count pairs written to slots[]/models[].
 *
 * "Which slots may `city` be offered in?" - the question a session's pause picker
 * needs and the engine's CarAvailability cannot answer once a session has started.
 * That table is written by the FRONTEND car screen (CarSelectScreen's setup), and a
 * session started with -host/-join never opens it, so in game the table still holds
 * its initialiser: four cars a city, and the picker hides everything else no matter
 * what the car mods can serve. They know the real answer.
 *
 * slots[] and models[] are PARALLEL: the car in models[i] is in slot slots[i]. Both
 * are written up to `max` entries.
 *
 * count = 0 means "nobody knows", the same convention as CITIES: a session with no
 * car mods falls back to the frontend table, i.e. exactly what it does today. A
 * handler must leave count at 0 rather than repeat a gated list, so the caller can
 * tell "no better answer" from "the answer is short". */
typedef struct JER_CARQ_SLOTS_ARGS
{
	int* slots;	/* out: slot indices, 0..9 */
	int* models;	/* out: model numbers, parallel to slots[] */
	int  city;	/* in: 0..3 */
	int  max;	/* in: capacity of BOTH arrays */
	int  count;	/* out: how many were written; 0 = nobody knows */
} JER_CARQ_SLOTS_ARGS;

/* JER_CARQ_LOAD -> ARGS.ok = 1 when this machine now holds (city, model) and the
 * session has been told, so the car can be driven here.
 *
 * This is a REQUEST, not a command: a handler that cannot place the car (no spare
 * resident slot, a palette block it cannot get) answers ok = 0, and the asker tells
 * the player the car is not available rather than driving a model it cannot
 * render. */
typedef struct JER_CARQ_LOAD_ARGS
{
	int  city;	/* in: 0..3 */
	int  model;	/* in: model number */
	int  ok;	/* out: 1 = held (or being loaded) here */
} JER_CARQ_LOAD_ARGS;

/* JER_CARQ_CHOSEN -> nothing to fill in: a NOTICE, fired AFTER a switch attempt (a
 * Change car pick, or the local player getting into a car again after being on
 * foot).
 *
 * LOAD only makes a car available; whether the player ends up driving it is decided
 * afterwards (the swap can still refuse). So the session must not be told about the
 * car at load time - it is told here, once the car on the road really is
 * (city, model). This is also the moment the car the player LEFT can be given back,
 * which LOAD cannot know.
 *
 *   changed = 1: the local player's car on the road is now (city, model), where
 *                city is the source city of the slot it is drawn from, or -1 for
 *                the level's own car. Also 1 when it already was that car.
 *   changed = 0: the switch to (city, model) did not happen (city as requested). */
typedef struct JER_CARQ_CHOSEN_ARGS
{
	int  city;	/* in: 0..3, or -1 = the level's own car */
	int  model;	/* in: model number */
	int  changed;	/* in: 1 = driving it now, 0 = the switch did not happen */
} JER_CARQ_CHOSEN_ARGS;

/* JER_CARQ_STANDIN -> ARGS.slot = a resident slot holding the stand-in body (the
 * placeholder car), or -1 when none can be placed. Asked by mp when a peer's
 * vehicle has no mesh yet; the body is DRAWN, never chosen, so nothing here tells
 * the session anything. */
typedef struct JER_CARQ_STANDIN_ARGS
{
	int slot;	/* out: resident slot for the stand-in body, -1 = none */
} JER_CARQ_STANDIN_ARGS;

/* JER_CARQ_CITYWIRE - the session-wide spelling of a car's CITY on any wire.
 *
 * A city INDEX is only stable for the stock rows (0..10: Driver 2's four, Driver 1's
 * seven). Runtime banks (rows 11+, cities.ini or carhacks' pack auto-register) land
 * on whatever row was free on THAT machine, so the same index names different banks
 * on two machines with different packs. On the wire a bank is therefore spelled by
 * a code derived from its NAME:
 *
 *   0x00..0x0A   a stock city, by index (unchanged)
 *   0x80..0xFE   a runtime bank: JER_CITY_WIRE_CODE_FIRST + hash(name) % 127
 *   0xFF         the session's / level's own city (unchanged)
 *   0x7F         a city the sender could not spell (treat as not held)
 *
 * toWire = 1: in `city` (local index, -1 = the level's own), out `wire`.
 * toWire = 0: in `wire`, out `city` = the LOCAL index of the bank with that code, or
 *             the wire value itself (>= CITY_COUNT, so never a usable row) when this
 *             machine does not hold it or the code is ambiguous in this session.
 *             Passing such a value back through toWire returns it unchanged, so a
 *             relay (a host re-sending a roster, a dedicated server with no carhacks)
 *             forwards the original spelling instead of losing it.
 * `handled` = 1 when a handler answered; with no handler the caller keeps the
 * legacy raw-index behaviour. */
#define JER_CITY_WIRE_CODE_FIRST	0x80
#define JER_CITY_WIRE_CODE_LAST		0xFE
#define JER_CITY_WIRE_UNSPELLABLE	0x7F

typedef struct JER_CARQ_CITYWIRE_ARGS
{
	int toWire;	/* in */
	int city;	/* in (toWire) / out (!toWire) */
	int wire;	/* out (toWire) / in (!toWire) */
	int handled;	/* out */
} JER_CARQ_CITYWIRE_ARGS;

/* JER_CARQ_ROLES - the car select asks the session how many cars to ask for.
 *
 * A game mode can give a player more than one car: one per ROLE, each from its
 * own pool (Cops n Robbers: a patrol car from the police cars, then a getaway car
 * from the rest). The car select then walks the roles in order, one pick each,
 * with the roster filtered to the role's pool, and only the LAST pick launches
 * (the usual JER_EVENT_MP_FRONTEND START). The session drives the right one per
 * role and round itself.
 *
 * count = 0 means "one ordinary pick" -- every mode without roles, a session
 * that does not answer, no session at all. The asker must then behave exactly as
 * it did before this contract existed.
 *
 * `launch` names the role whose car the level loads with (0..count-1); the session
 * swaps cars itself when the role changes. */
#define JER_CARQ_ROLES_MAX	4
#define JER_CARQ_ROLE_TITLE_MAX	24

#define JER_CARQ_POOL_ANY	0	/* every car the roster offers */
#define JER_CARQ_POOL_POLICE	1	/* police cars only (JerCarIsPolice, models.h) */
#define JER_CARQ_POOL_CIVILIAN	2	/* everything that is not a police car */

typedef struct JER_CARQ_ROLE
{
	char title[JER_CARQ_ROLE_TITLE_MAX];	/* the step's heading, e.g. "PATROL CAR" */
	int  pool;				/* JER_CARQ_POOL_* */
} JER_CARQ_ROLE;

typedef struct JER_CARQ_ROLES_ARGS
{
	JER_CARQ_ROLE roles[JER_CARQ_ROLES_MAX];	/* out */
	int count;	/* out: 0 = one ordinary pick (nobody answered / no roles) */
	int launch;	/* out: the role whose car the level loads with */
} JER_CARQ_ROLES_ARGS;

/* JER_CARQ_ROLEPICK - a NOTICE from the car select: role `role` is (city, model).
 * Fired once per role as each step is confirmed, before the launch. `ok` = 1 when
 * the session kept it. A pick the session refuses (outside the role's pool) is
 * not an error for the asker: the session falls back to its own choice. */
typedef struct JER_CARQ_ROLEPICK_ARGS
{
	int role;	/* in: 0..count-1 */
	int city;	/* in: the roster city (an index, as JER_CARQ_SLOTS) */
	int model;	/* in: model number */
	int palette;	/* in: the paint picked with it */
	int ok;		/* out */
} JER_CARQ_ROLEPICK_ARGS;

#endif /* JER_CARQUERY_H */
