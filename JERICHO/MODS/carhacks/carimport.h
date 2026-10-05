/* carhacks/carimport.h — the IMPORT SET: which car each resident slot holds and
 * where its data comes from, plus the player's pick. See carimport.c.
 *
 * This is the single source of truth for "what does this level's car list look
 * like, and which city does each entry come from" - the thing the engine's
 * JER_EVENT_CAR_DATA_SOURCE write is derived from, the thing the car-select menu
 * sets, and (next) the thing the multiplayer channel will agree between peers.
 */
#ifndef CHK_CARIMPORT_H
#define CHK_CARIMPORT_H

#include "carid.h"

/* The engine's resident car slots (MAX_CAR_RESIDENT_MODELS = 12). Slots 0..4 are
 * the level's civilians, the top slot is the engine's SPECIAL_CAR_SLOT, and the
 * rest are spares an import may take.
 *
 * The pool is as deep as the ENGINE's array: capping it at 8 left exactly three
 * spares (5, 6, 7) once mp had claimed its two, so a third player's car was
 * refused with "no spare resident slot is free" and the player drove whatever the
 * level assigned them instead of what they picked. */
#define CHK_IMPORT_MAX_SLOTS	11	/* 0..10; 11 is SPECIAL_CAR_SLOT */
#define CHK_IMPORT_SPARE_FIRST	5

/* ---- the set ----------------------------------------------------------- */

/* A new level (or a module reload): drop every entry. The local PICK is kept -
 * it belongs to the player's menu choice, not to the level - and is consumed
 * when the set is applied to a level. */
void chkImportReset(void);

/* Put `id` into `slot`. Returns 1 if the set changed; 0 = refused (bad range).
 * The set may name AS MANY source cities as it has slots -- the engine keeps a
 * source city PER SLOT (models.c/gCarModelSource), so a mix builds and spawns
 * like any other set. */
int chkImportSetSlot(int slot, CHK_CAR_ID id);

/* Put only a model number into `slot`, leaving the source alone (what the
 * traffic knock does). 1 = changed. */
int chkImportSetSlotModel(int slot, int model);

/* A city's name for a log line: "level" for CHK_CITY_NATIVE (-1), else LevelNames.
 * Shared by carimport.c, net.c and spawn.c. */
const char* chkCityName(int city);

/* The identity recorded for `slot` (city = CHK_CITY_NATIVE when nothing). */
CHK_CAR_ID chkImportSlotId(int slot);

/* JERICHO carhacks UNLOAD: give a resident slot back - the engine's pins, lower-half pool pages,
 * baked page index and geometry, plus the set entry and the engine arrays that claimed it, so the
 * slot is free for the next car (chkImportSlotFree says so again). Returns 1 if the slot held
 * anything. Called when the car's last driver goes away: a peer leaving, or a local re-pick. */
int chkImportReleaseSlot(int slot);

/* For the release decision (net.c, chkNetReleaseSlotIfUnused): the slot holding exactly
 * `car` (-1 = none), whether `slot` holds a car at all, and how many cars in the world are
 * drawn from `slot` right now (`first` = the first one's car_data index, or -1). */
int chkImportSlotOfCar(CHK_CAR_ID car);
int chkImportSlotHeld(int slot);
int chkImportCarsOnSlot(int slot, int* first);

/* Everything, for when the session ends and there is no map any more: the slots above, and the
 * engine's per-level AND per-city cross-city state (JerReleaseAllCrossCity). This is the path
 * that clears the pointers which used to outlive a level - the deferred palette lumps and the
 * parsed page lists - so leaving a session and rejoining with a car from another city no longer
 * reads freed data. */
void chkImportReleaseAll(void);

/* JERICHO carhacks: give the PLACEMENT back but keep the player's pick - everything the slots
 * hold, the parsed page lists, the deferred palette lumps and the imported city buffers. Used on
 * the way into the frontend (the menus need none of it, and the next level re-imports); the
 * pick survives because the level it starts still consumes it. */
void chkImportPurgePlacement(void);

/* Build resident `slot`'s geometry at runtime, from the import data the set names
 * (mid-level). Returns the bytes built, 0 = not built (no level yet, not our slot,
 * or it does not fit - and then the slot is left as it was). */
int chkImportHotLoad(int slot);

/* The session's canonical spare resident slot for `car`: by the lowest owning player
 * id, ascending, with our own pick at OUR player id (chkImportCarOrder in carimport.c).
 * -1 = the session needs more spares than the range has. `outCount` (optional) gets the
 * number of cars the session needs. */
int chkImportCanonicalSlot(CHK_CAR_ID car, int* outCount);

/* The set's own slot holding (city, model), or -1. `city` < 0 matches any. */
int chkImportSlotForCar(int city, int model);

/* ---- what a car list may offer ----------------------------------------- */

/* The engine's car list table: CarAvailability[city][slot] and carNumLookup[city][slot]. */
#define CHK_CAR_CITY_COUNT	9
#define CHK_CAR_SLOT_COUNT	10

/* MAY (city, slot) BE OFFERED in a car list?
 *
 * CarAvailability starts as {1,1,1,1,0,...} for EVERY city and is lifted for the extras
 * only by finishing the game, by a cheat in single player, or by the unlock hook - so a
 * modded game offers four cars per city and hides everything else, including any car a
 * `car_list` was configured to put in a slot. This is the one question that replaces those
 * gates: the slot names a real car.
 *
 * WHY THAT IS THE WHOLE TEST, and not a check that the car's data is present:
 *
 *  - CarAvailability is a MENU GATE, not a load instruction. The engine's only reads of it
 *    are the stock screen's cursor walk (FEmain.c:2773,2786), so widening it cannot by
 *    itself ask for a car to be loaded.
 *  - Offering is free. The car list draws a 2D icon; nothing is loaded by listing, and
 *    moving the highlight triggers no load, so a list can show every car without bringing
 *    any of their data into the level.
 *  - Import happens only on a PICK, and the pick path already refuses in words when it
 *    cannot serve one ("needs a spare resident slot and none is free", "riding the level's
 *    own car of that number"). That is a better outcome than a slot the player cannot see.
 *  - A data probe CANNOT be answered here anyway, and getting it wrong hides cars rather
 *    than showing them: on this build the car data is not per-model files. A whole install
 *    holds two CARMODEL_*.dmodel overrides, while every level's cars come out of its own
 *    package - which is exactly what the mid-level city read does (carhacks.c logs "car data
 *    from HAVANA read MID-LEVEL"). Probing LEVELS\<CITY>\CARMODEL_<n>.COS (the PSX-era
 *    name that cars.c/cosmetic.c still carry) would report "no data" for almost every car
 *    in the game.
 *
 * So a slot is offered when it names a car, and whether that car is the level's own or has
 * to be imported is reported separately (see chkImportOfferedCount / the per-level log),
 * because that difference is useful to see and wrong to gate on. */
typedef enum CHK_OFFER
{
	CHK_OFFER_OK = 0,	/* offer it */
	CHK_OFFER_BAD,		/* not a city or slot this game has */
	CHK_OFFER_NO_CAR	/* the slot names no car (model 0, or -1 = none here) */
} CHK_OFFER;

/* Give a city with no frontend car list of its own the cars its IMPORTED data
 * actually carries. Fills nothing for a city that already has a list, so Driver
 * 2's four cities are untouched. */
void chkFillCarTableFromImport(int city);

CHK_OFFER chkImportCanOffer(int city, int slot);

/* The model number `city`'s `slot` names, read the same SIGNED way the offer rule reads it:
 * 0 for a hole in the table, -1 for "the level has no car of that number". One place reads
 * the table, so a caller cannot disagree with chkImportCanOffer about what is in a slot. */
int chkImportSlotModel(int city, int slot);

/* Short human name for a verdict, for the one line a level logs about its list. */
const char* chkOfferReason(CHK_OFFER why);

/* How many of `city`'s slots a car list would show. */
int chkImportOfferedCount(int city);

/* How many of those the level ALREADY has (chkImportLevelHoldsModel == 1), i.e. the ones a
 * pick can serve without bringing anything in. Reported, never gated on.
 *
 * Strictly == 1: that function is tri-state, and its -1 means "no level has told us its list
 * yet" - which is the whole frontend, before a level is loaded. Counting that as held made a
 * menu-time log line claim cars the level had never been asked about. */
int chkImportOfferedHeldCount(int city);

/* ---- slot ownership ---------------------------------------------------- */

/* Hand the set the ENGINE's live resident models for this level (the `models`
 * array from JER_EVENT_CAR_DATA_SOURCE), so its automatic slot choosers skip a
 * slot another module already claimed. carhacks answers that event AFTER mp
 * (priority 10 vs mp's 0), so mp's spare slots (5 and 6) are already written
 * here -- see chkImportSlotFree. Pass NULL/0 outside the hook. */
void chkImportSetEngineModels(int* models, int count);

/* Is resident slot `slot` free to import into -- nothing of OURS, and nothing
 * another module claimed? */
int chkImportSlotFree(int slot);

/* Does the LEVEL already hold this model in its resident pool? "This level's own
 * city" does not imply the level has that car (a level reads only the models its
 * own list names), so the callers that used to skip an own-city car ask this
 * instead. Hook-only, like chkImportSlotFree: 1 = the pool holds it, 0 = it
 * provably does not, -1 = cannot tell (no live resident list). A caller that would
 * IMPORT on a 0 must treat -1 as "leave it alone". */
int chkImportLevelHoldsModel(int model);

/* Distinguishable source cities the set names, for logging: the FIRST guest city
 * (-1 = none, the level's own) and how many distinct ones there are. */
int chkImportGuestCity(void);
int chkImportGuestCityCount(void);	/* distinct source cities the set names */

/* Bumps on every change, so a peer can tell whether it needs the whole set. */
int chkImportSetVersion(void);

/* ---- the player's pick (the car-select menu) --------------------------- */

void chkImportSetLocalPick(int city, int model);
CHK_CAR_ID chkImportLocalPick(void);
int chkImportLocalPickCity(void);
int chkImportLocalPickModel(void);

/* JERICHO carhacks: the car the local player CHOSE. Unlike chkImportLocalPick* this survives
 * the level consuming the pick, because the question "what is this player driving?" is about
 * the choice and not about the seat the engine put them in - an engine that re-seats a player
 * (a late-join spawn) must not silently change what the session thinks they drive. Unset until
 * they choose; check chkImportChosenIsSet(). */
CHK_CAR_ID chkImportChosenCar(void);
int chkImportChosenIsSet(void);

/* The local player switched to `id` mid-match (after the switch succeeded): record it as the
 * choice. Does NOT touch the frontend pick. Accepts a native car. */
void chkImportSetChosen(CHK_CAR_ID id);

/* Consume the pick - the level that imported it has read it, so a later level
 * does not import the same car again. */
void chkImportClearPick(void);

/* ---- building the set -------------------------------------------------- */

/* Read the [carhacks] config (import = slot:city:model, traffic_model,
 * traffic_slot) into the set. This is the fallback when no pick has been made,
 * and what the launchers and chk_suite.sh drive. `source_city` is NOT here: it is
 * a level-wide lever, applied by carhacks.c itself. Returns the number of
 * changes. */
int chkImportLoadConfig(const char* section, int count, int skipGuestEntries);

/* Apply the player's pick as an import: the pick's city + model into a spare
 * resident slot, so the engine reads that model from THAT city's files. A pick
 * from the level's own city needs no import (the level already lists it).
 * Returns 1 if the set changed. */
int chkImportApplyPick(int level, int count);

/* ---- the engine write -------------------------------------------------- */

/* Write the set into a JER_EVENT_CAR_DATA_SOURCE's arrays: models[slot] and
 * modelSource[slot] (-1 = leave the engine's own value). */
void chkImportApplyToCarData(int count, int* models, int* modelSource);

/* One line per entry, for the log. */
void chkImportDump(int level);

#endif /* CHK_CARIMPORT_H */
