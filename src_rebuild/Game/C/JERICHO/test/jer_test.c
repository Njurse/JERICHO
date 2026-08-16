/* jer_test.c — JERICHO smoke test (not part of the game build).
 * Links the SDK + generated registry + a module and verifies activation.
 * Usage: jer_test.exe [rootDir]   (default "JERICHO" — the central folder
 * with MODS/ + CONFIG/; pass a sandbox copy to avoid touching the repo).
 */
#include "jericho.h"
#include "jer_pause_menu.h"

#include <stdio.h>
#include <string.h>

static int print_list(const char* what)
{
	JER_MODULE_INFO mods[JER_MAX_MODULES];
	int count = jer_module_list(mods, JER_MAX_MODULES);
	int i;

	printf("--- jer_test: %s (%d module(s)) ---\n", what, count);

	for (i = 0; i < count; i++)
		printf("  %-10s v%-6s [%s]\n", mods[i].id, mods[i].version, mods[i].enabled ? "ON" : "OFF");

	return count;
}

int main(int argc, char** argv)
{
	const char* root = argc > 1 ? argv[1] : "JERICHO";
	JER_MODULE_INFO mods[JER_MAX_MODULES];
	int count;
	int origEnabled = -1;
	int i;

	printf("--- jer_test: initialising (root=%s) ---\n", root);

	/* read JERICHO/CONFIG/modlist.ini (absent -> everything enabled),
	 * activate, fire JER_EVENT_BOOT */
	jer_init(root);

	count = jer_module_list(mods, JER_MAX_MODULES);

	for (i = 0; i < count; i++)
	{
		if (strcmp(mods[i].id, "example") == 0)
			origEnabled = mods[i].enabled;
	}

	print_list("modules after boot");

	/* exercise the manager: toggle example to the OPPOSITE of its current
	 * state, reload, verify the ini round-trip persisted the status */
	if (origEnabled >= 0 && jer_manager_set_enabled(root, "example", !origEnabled) == 0)
	{
		printf("--- jer_test: toggled example to %d, reloading ---\n", !origEnabled);
		jer_manager_reload(root);
	}

	count = jer_module_list(mods, JER_MAX_MODULES);

	for (i = 0; i < count; i++)
	{
		if (strcmp(mods[i].id, "example") == 0)
			printf("--- jer_test: example enabled=%d (expected %d after toggle) ---\n",
				mods[i].enabled, !origEnabled);
	}

	print_list("modules after toggle");

	/* move example UP (-1): it is last in modlist.ini, so this actually
	 * reorders the file (persisted to the ini) */
	if (jer_manager_move(root, "example", -1) == 0)
	{
		printf("--- jer_test: moved example up (persisted to modlist.ini) ---\n");
		/* restore the load order we just changed */
		jer_manager_move(root, "example", 1);
	}

	/* restore the original enabled state so re-runs stay deterministic */
	if (origEnabled >= 0)
		jer_manager_set_enabled(root, "example", origEnabled);

	/* module pause menus: the registry is empty until a module registers;
	 * example registers none, so count must be 0 here */
	printf("--- jer_test: pause menu count=%d (expected 0, example registers none) ---\n", jer_pause_menu_count());

	printf("--- jer_test: firing JER_EVENT_FRAME ---\n");
	jer_fire(JER_EVENT_FRAME, NULL);

	printf("--- jer_test: firing custom event (should be ignored) ---\n");
	jer_fire(JER_EVENT_MODULE_CUSTOM + 1, NULL);

	printf("--- jer_test: done ---\n");
	return 0;
}
