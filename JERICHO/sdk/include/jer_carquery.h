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
 * ------------------------------------------------------------------ */

#include "jericho.h"

#define JER_CARQ_CITIES	(JER_EVENT_MODULE_CUSTOM + 40)	/* "which cities?" */
#define JER_CARQ_LOAD	(JER_EVENT_MODULE_CUSTOM + 41)	/* "make (city,model) available" */
#define JER_CARQ_CHOSEN	(JER_EVENT_MODULE_CUSTOM + 42)	/* "this is what we drive now" */
#define JER_CARQ_SLOTS	(JER_EVENT_MODULE_CUSTOM + 43)	/* "which slots may this city offer?" */

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

#endif /* JER_CARQUERY_H */
