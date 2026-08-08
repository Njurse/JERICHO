/* jer_test.c — JERICHO smoke test (not part of the game build).
 * Links the SDK + generated registry + a module and verifies activation.
 */
#include "jericho.h"

#include <stdio.h>

int main(void)
{
	JER_MODULE_INFO mods[JER_MAX_MODULES];
	int count;
	int i;

	printf("--- jer_test: initialising ---\n");

	/* read mods/modlist.json (absent -> everything enabled), activate,
	 * fire JER_EVENT_BOOT */
	jer_init("mods");

	count = jer_module_list(mods, JER_MAX_MODULES);
	printf("--- jer_test: %d module(s) in the registry ---\n", count);

	for (i = 0; i < count; i++)
		printf("  %-10s v%-6s [%s]\n", mods[i].id, mods[i].version, mods[i].enabled ? "ON" : "OFF");

	printf("--- jer_test: firing JER_EVENT_FRAME ---\n");
	jer_fire(JER_EVENT_FRAME, NULL);

	printf("--- jer_test: firing custom event (should be ignored) ---\n");
	jer_fire(JER_EVENT_MODULE_CUSTOM + 1, NULL);

	printf("--- jer_test: done ---\n");
	return 0;
}
