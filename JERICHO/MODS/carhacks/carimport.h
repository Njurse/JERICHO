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

/* The engine's resident car slots (MAX_CAR_RESIDENT_MODELS). Slots 0..4 are the
 * level's civilians, 5..6 spare, 7 SPECIAL_CAR_SLOT. */
#define CHK_IMPORT_MAX_SLOTS	8
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
