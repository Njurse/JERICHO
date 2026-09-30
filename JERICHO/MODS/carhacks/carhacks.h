#ifndef CARHACKS_H
#define CARHACKS_H

/* carhacks — vehicle-availability hacks for Driver 2.
 *
 * A home for code that makes vehicles the game does not normally offer
 * selectable/usable. Each hack is one row of the table in carhacks.c and is
 * gated by its own config key, so they switch on and off independently.
 *
 * MODULE BOUNDARY. This unit was lifted out of Caine's Crossfire (it used to
 * live in cainescrossfire/carhacks/ and be registered from that module's entry)
 * and is now its own module under JERICHO/MODS/carhacks/ - so it can be enabled
 * next to any other module, in particular the multiplayer module. It is kept
 * HOST-AGNOSTIC, which is what made the lift cheap and keeps it re-usable:
 *   - it uses only engine globals (already exported to mods via exports.def)
 *     and the public JERICHO API - no cainescrossfire internals;
 *   - it exposes exactly one entry point, carhacks_register(ctx), which
 *     carhacks_module.c (the module wrapper) calls;
 *   - its symbols are carhacks_ or chk_ prefixed and its config section is
 *     "carhacks", so no other module's name is reachable from here.
 * Because of that it can still be compiled into another module unchanged: delete
 * carhacks_module.c and call carhacks_register(ctx) from that module's entry.
 *
 * The docs that describe the cross-city mechanism and the city data formats
 * (CROSS_CITY.md, HACK.md, FORMATS.md, PALETTES.md, VEHICLES.md, VRAM.md) still
 * live in cainescrossfire/carhacks/ - they are referenced from engine source
 * comments there and from tools, so they were not moved with the code. This
 * module's own docs (README.md, CARSELECT.md, MP_ADAPTER.md) are here.
 *
 * LOGGING - one rule per call, so no future line has to guess:
 *   - printInfo(...)   every DIAGNOSTIC line (what the module did and why it
 *                      refused). This is the engine's own informational channel
 *                      (driver2.h -> PsyX_Log_Info) -- the channel that emits the
 *                      "cross-city:"/"JERICHO-*" lines carhacks' lines sit beside,
 *                      and the one the harnesses read from a run's captured
 *                      stdout. Prefix with "[carhacks]" or "[carhacks/net]".
 *   - jer_error(...)   a line the PLAYER must see (a dropped car, a refused
 *                      pick, a menu that will not come up). It logs the whole
 *                      message verbatim as "[error] ..." AND raises the on-screen
 *                      notice, so it is the single call for a player-facing
 *                      refusal -- do NOT pair it with a printInfo.
 *   - ctx->jer_log(...) ONLY inside a module entry/registration function, where
 *                      `ctx` is the handle in hand and calling it is the SDK
 *                      idiom (carhacks_register, chkNetRegister,
 *                      chkCarSelectRegister). It lands in the same log as
 *                      printInfo.
 */

#include "jericho.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register every car hack: its config keys and its engine hooks. Called from
 * the host module's entry (or from this unit's own entry once it is a module).
 * Safe to call more than once (re-registration on module reload). */
void carhacks_register(JERICHO_CONTEXT* ctx);

/* The hack table, for a menu or for logging. */
int         carhacks_count(void);
const char* carhacks_name(int index);
int         carhacks_enabled(int index);

/* The hack table rows. Named, so no handler (or sibling source) ever depends on
 * the table order. */
enum
{
	CHK_HACK_UNLOCK_EXTRA = 0,	/* lift the frontend's extra-vehicle gate */
	CHK_HACK_CROSS_CITY,		/* import another city's vehicles */
	CHK_HACK_CAR_SELECT		/* the car-select menu (carselect.c) */
};

/* ---------------------------------------------------------------------------
 * The car-select menu (carselect.c)
 * ------------------------------------------------------------------------- */

/* Register the frontend car-select menu and its hooks. Called from
 * carhacks_register. Safe to call more than once (a module reload re-runs it). */
void chkCarSelectRegister(JERICHO_CONTEXT* ctx);

/* The "see the imported cars" measurement lever (spawn.c). chkSpawnRegister
 * installs its FRAME hook; chkSpawnReset re-arms it for a new level, so the cars
 * are placed once per level rather than once per session. Called from
 * carhacks_register and the CAR_DATA_SOURCE handler respectively. */
void chkSpawnRegister(JERICHO_CONTEXT* ctx);
void chkSpawnReset(void);

/* The menu's id, for jer_frontend_find and for the boot log. */
const char* chkCarSelectMenuId(void);

/* Arm the menu: the stock car screen has just started its setup, so show ours
 * instead of it. Called from carhacks.c's JER_EVENT_CAR_AVAILABILITY hook.
 * Declines for a 2-player pick, a mission's car pick, or a live mp session. */
void chkCarSelectArm(void);

/* The player's last pick from the menu: the city its car must come from and the
 * model number inside that city. -1 = nothing picked. carhacks.c turns this
 * into the cross-city import. */
int chkCarSelectPickCity(void);
int chkCarSelectPickModel(void);

#ifdef __cplusplus
}
#endif

#endif /* CARHACKS_H */
