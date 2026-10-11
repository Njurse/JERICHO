/*
 * jer_pause_menu.c — registry of module-provided pause menus (see
 * jer_pause_menu.h). The game's pause screen (pause.c) reads this list
 * and bridges it to its own static menu system; modules just register
 * their menus here at boot.
 */
#include "jer_pause_menu.h"
#include "jericho.h"	/* jer_log: a refused registration must say so (see below) */

#include <string.h>
#include <stdio.h>	/* snprintf */

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

/* CONTRIBUTIONS: rows other modules want in a menu they do not own.
 *
 * Kept as (target string, items, count) and matched when the bridge BUILDS a menu,
 * which is what lets a module contribute before or after the target is registered.
 * Two caps rather than one because they fail differently: too many contributions is
 * a module asking for too much, while too many ITEMS is simply the pause screen
 * getting too big -- and that one is shared with the bridge's own pools, which
 * already refuse out loud. */
#define JER_PAUSE_ADDS_MAX 24		/* separate contributions */
#define JER_PAUSE_ADDS_ITEMS_MAX 64	/* rows across all of them */

typedef struct JER_PAUSE_ADD
{
	char target[48];			/* the id or the title it asked for */
	const JER_PAUSE_MENU_ITEM* items;
	int count;
} JER_PAUSE_ADD;

static JER_PAUSE_ADD gAdds[JER_PAUSE_ADDS_MAX];
static int gAddCount;
static int gAddItems;

int jer_pause_menu_add_items(const char* target, const JER_PAUSE_MENU_ITEM* items, int count)
{
	if (target == NULL || target[0] == 0 || items == NULL || count <= 0)
		return 0;

	if (gAddCount >= JER_PAUSE_ADDS_MAX || gAddItems + count > JER_PAUSE_ADDS_ITEMS_MAX)
	{
		jer_log("[jericho] pause menus: contribution to '%s' NOT recorded - the contribution "
			"pool is full (%d/%d contributions, %d/%d rows) - raise JER_PAUSE_ADDS_* in "
			"jer_pause_menu.c\n",
			target, gAddCount, JER_PAUSE_ADDS_MAX, gAddItems, JER_PAUSE_ADDS_ITEMS_MAX);

		return 0;
	}

	snprintf(gAdds[gAddCount].target, sizeof(gAdds[gAddCount].target), "%s", target);
	gAdds[gAddCount].items = items;
	gAdds[gAddCount].count = count;
	gAddCount++;
	gAddItems += count;

	return 1;
}

int jer_pause_menu_extra_count(const char* target)
{
	int i, n = 0;

	if (target == NULL || target[0] == 0)
		return 0;

	for (i = 0; i < gAddCount; i++)
	{
		if (strcmp(gAdds[i].target, target) == 0)
			n += gAdds[i].count;
	}

	return n;
}

const JER_PAUSE_MENU_ITEM* jer_pause_menu_extra_at(const char* target, int index)
{
	int i;

	if (target == NULL || target[0] == 0 || index < 0)
		return NULL;

	for (i = 0; i < gAddCount; i++)
	{
		if (strcmp(gAdds[i].target, target) != 0)
			continue;

		if (index < gAdds[i].count)
			return &gAdds[i].items[index];

		index -= gAdds[i].count;
	}

	return NULL;
}

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

	/* A reset starts the world again, so it starts the contributions again too:
	 * they are boot-time declarations, not session state. */
	gAddCount = 0;
	gAddItems = 0;
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
