/*
 * jer_pause_menu.c — registry of module-provided pause menus (see
 * jer_pause_menu.h). The game's pause screen (pause.c) reads this list
 * and bridges it to its own static menu system; modules just register
 * their menus here at boot.
 */
#include "jer_pause_menu.h"

#include <string.h>

#define JER_PAUSE_MAX_MENUS 8

static const JER_PAUSE_MENU* gMenus[JER_PAUSE_MAX_MENUS];
static int gMenuCount;

void jer_pause_menu_register(const JER_PAUSE_MENU* menu)
{
	if (menu == NULL || gMenuCount >= JER_PAUSE_MAX_MENUS)
		return;

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
