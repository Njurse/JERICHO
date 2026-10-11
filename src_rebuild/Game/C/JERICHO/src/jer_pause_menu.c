/*
 * jer_pause_menu.c — registry of module-provided pause menus (see
 * jer_pause_menu.h). The game's pause screen (pause.c) reads this list
 * and bridges it to its own static menu system; modules just register
 * their menus here at boot.
 */
#include "jer_pause_menu.h"
#include "jericho.h"	/* jer_log: a refused registration must say so (see below) */

#include <string.h>

/* How many TOP-LEVEL menus may be registered.
 *
 * This is NOT the same ceiling as the bridge's pools in pause.c, although the two
 * used to share the name JER_PAUSE_MAX_MENUS -- which is exactly the kind of thing
 * that gets "raised" in the wrong file when a mod's menu goes missing. This one
 * counts registrations (seven modules make one each today); the bridge's counts
 * the headers it builds, including every submenu, which is why it is much larger.
 * Headroom here, not a target. */
#define JER_PAUSE_REGISTRY_MAX 32

static const JER_PAUSE_MENU* gMenus[JER_PAUSE_REGISTRY_MAX];
static int gMenuCount;

void jer_pause_menu_register(const JER_PAUSE_MENU* menu)
{
	if (menu == NULL)
		return;

	if (gMenuCount >= JER_PAUSE_REGISTRY_MAX)
	{
		/* The menu is simply absent from the pause screen, so say so rather than
		 * returning quietly: a silently dropped registration looks identical to the
		 * mod not being loaded, and that is the same failure shape as the bridge
		 * running out of pool. */
		jer_log("[jericho] pause menus: '%s' NOT registered - the registry is full "
			"(%d) - raise JER_PAUSE_REGISTRY_MAX in jer_pause_menu.c\n",
			(menu->title != NULL) ? menu->title : "?", gMenuCount);

		return;
	}

	gMenus[gMenuCount++] = menu;
}

/* Called by the runtime before every activation batch so re-registering
 * modules (e.g. after a Mods-menu reload) don't accumulate duplicates. */
void jer_pause_menu_reset(void)
{
	gMenuCount = 0;
}

int jer_pause_menu_count(void)
{
	return gMenuCount;
}

const JER_PAUSE_MENU* jer_pause_menu_get(int index)
{
	if (index < 0 || index >= gMenuCount)
		return NULL;

	return gMenus[index];
}
