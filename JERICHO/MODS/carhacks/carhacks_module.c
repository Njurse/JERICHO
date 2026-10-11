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
#include "jer_pause_menu.h"	/* jer_pause_menu_add_items: carhacks adds to mp's car page */

#include "carhacks.h"

/* carhacks contributes ONE row to mp's Car options page, as the demonstration of
 * jer_pause_menu_add_items: a module adding to a menu it does not own, matched by
 * the menu's STABLE id ("mp.car") so mp rewording a row cannot break the link.
 * The action is carhacks' own domain - the cross-city identity of the car being
 * driven - which is exactly the state mp's page only borrows from here. */
static int ChkCarInfo(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	jer_log("[carhacks] Car info (contributed row): the cross-city identity of your "
		"car is owned here - see the roster and import tables in JERICHO.log\n");

	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM kChkCarInfoItems[] =
{
	{ "Car info (carhacks)", NULL, ChkCarInfo, NULL, NULL, 0 },
};

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

	/* ...and one row in mp's Car options page, to prove the extension point. The
	 * target is matched by id, so mp's layout can move and this still lands. */
	jer_pause_menu_add_items("mp.car", kChkCarInfoItems,
		(int)(sizeof(kChkCarInfoItems) / sizeof(kChkCarInfoItems[0])));
}
