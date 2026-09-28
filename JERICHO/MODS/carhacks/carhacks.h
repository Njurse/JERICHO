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

#ifdef __cplusplus
}
#endif

#endif /* CARHACKS_H */
