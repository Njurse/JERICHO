/* carhacks_module.c — the module wrapper for the carhacks unit.
 *
 * carhacks deliberately exists as two layers:
 *   - carhacks.c/.h is a self-contained UNIT (see carhacks.h) with exactly one
 *     entry point, carhacks_register(ctx). It uses only engine globals already
 *     exported to mods and the public JERICHO SDK, so it can be compiled into
 *     another module unchanged.
 *   - this file turns it into a JERICHO MODULE of its own, so it can be enabled
 *     alongside any other module - in particular the multiplayer module -
 *     instead of only through Caine's Crossfire, which used to host it.
 *
 * The generated registry (Game/C/JERICHO/gen/jer_registry.c) calls
 * jer_module_carhacks_entry to link and the loader calls it again on a reload,
 * so this must stay re-runnable: carhacks_register is documented safe to call
 * more than once.
 *
 * It must remain a COMPILED-IN module (no `runtime = "dll"` in mod.toml):
 * MSVC cannot import the game's data globals (CarAvailability, carNumLookup,
 * wantedCar) into a DLL, and carhacks reads and writes exactly those.
 */

#include "jericho.h"

#include "carhacks.h"

JER_MODULE_ENTRY(jer_module_carhacks_entry)(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_module(ctx,
		"carhacks",			/* id (must match the folder + mod.toml) */
		"Car Hacks",			/* name */
		"0.1.0",			/* version */
		"JERICHO",			/* author */
		"Vehicle-availability and cross-city vehicle hacks: unlock the normally-"
		"locked extra vehicles in the frontend car list, and import vehicles that "
		"belong to another city's data files.",
		"",				/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	/* the unit: its config keys, its engine hooks, and (later) its own
	 * frontend car-select menu. Safe on a reload. */
	carhacks_register(ctx);
}
