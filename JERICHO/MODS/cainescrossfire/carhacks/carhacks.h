#ifndef CARHACKS_H
#define CARHACKS_H

/* carhacks — vehicle-availability hacks for Driver 2.
 *
 * A home for code that makes vehicles the game does not normally offer
 * selectable/usable. Each hack is one row of the table in carhacks.c and is
 * gated by its own config key, so they switch on and off independently.
 *
 * EXTRACTION SEAM. This unit is deliberately self-contained so it can become
 * its own JERICHO module later without touching cainescrossfire:
 *   - it uses only engine globals (already exported to mods via exports.def)
 *     and the public JERICHO API - no cainescrossfire internals;
 *   - it exposes exactly one entry point, carhacks_register(ctx);
 *   - its symbols are carhacks_ or chk_ prefixed and its config section is
 *     "carhacks", so no cainescrossfire name is reachable from here.
 * To lift it out: copy this folder to JERICHO/MODS/carhacks/, give it its own
 * JER_MODULE_ENTRY that calls carhacks_register(ctx), move the single
 * carhacks_register(ctx) call out of cainescrossfire's module entry, and add it to
 * the modlist. Nothing else changes.
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
