#ifndef JER_PAUSE_MENU_H
#define JER_PAUSE_MENU_H

/* ------------------------------------------------------------------
 * jer_pause_menu.h — module-provided pause menus.
 *
 * Modules register menus (and nested submenus) from their entry point;
 * the game's pause screen collects them under a "Modules" submenu. No
 * game headers are needed — the engine bridges to its own menu system.
 *
 *   static const JER_PAUSE_MENU_ITEM items[] = {
 *       { NULL, MyLabel,  MyToggle,  NULL, NULL, 0 },  // dynamic label + toggle
 *       { "Sub", NULL, NULL, NULL, &submenu, 0 },      // opens a submenu
 *       { "Vol", NULL, MyAdjust, NULL, NULL, 1 },      // left/right adjusts
 *   };
 *   static const JER_PAUSE_MENU menu = { "My Mod", items, 3 };
 *   ...in the module entry...
 *   jer_pause_menu_register(&menu);
 *
 * Menu depth: the root pause menu -> the "JERICHO Addons" container -> your
 * page -> up to JER_PAUSE_MENU_MAX_DEPTH submenu levels (see below).
 * ------------------------------------------------------------------ */

/* HOW DEEP YOUR MENU MAY NEST, measured in submenu levels BELOW your own page.
 *
 *   0 = a page with no submenus at all (most modules)
 *   1 = your page -> a submenu
 *   2 = your page -> a submenu -> a sub-submenu
 *   ...
 *
 * The engine's pause screen owns the levels above your page (the root pause menu
 * and the "JERICHO Addons" container), and its navigation stack is a fixed array,
 * so this is a CEILING rather than a hint: a tree that would nest deeper than this
 * is refused and logged -- the deeper menu is left out, never built half-way.
 *
 * The engine sizes that stack FROM this constant (PAUSE_MENU_LEVELS in pause.c is
 * derived from it), so raising it here is a compile-visible act rather than a
 * secret agreement between two files that can drift apart. */
#define JER_PAUSE_MENU_MAX_DEPTH 5

#ifdef __cplusplus
extern "C" {
#endif

/* What the pause screen should do after an item activates. Values mirror
 * the engine's menu quit codes (see Game/C/pause.h). */
enum
{
	JER_PAUSE_QUIT_NONE = 0,	/* stay in the pause menu */
	JER_PAUSE_QUIT_CONTINUE = 1,	/* leave the pause (unpause) */
	JER_PAUSE_QUIT_QUIT = 2,	/* exit to the frontend */
	JER_PAUSE_QUIT_RESTART = 3,	/* restart the current mission */
	JER_PAUSE_QUIT_DIRECTOR = 4,	/* film director */
	JER_PAUSE_QUIT_QUICKREPLAY = 5,	/* quick replay */
	JER_PAUSE_QUIT_BACKMENU = 6,	/* go back one menu level */
	JER_PAUSE_QUIT_NEXTMISSION = 7	/* go to the next mission */
};

struct JER_PAUSE_MENU;

typedef struct JER_PAUSE_MENU_ITEM
{
	const char* label;	/* static label (ignored when get_label is set) */
	void (*get_label)(void* userdata, char* out, int max);	/* dynamic label */
	int (*on_activate)(void* userdata, int direction);	/* CROSS -> 0; with adjust set, left/right -> -1/1. Return a JER_PAUSE_QUIT_* code. */
	void* userdata;
	const struct JER_PAUSE_MENU* submenu;	/* CROSS opens this submenu (takes priority over on_activate) */
	int adjust;		/* 1 = left/right also activates (on_activate receives -1/1) */
} JER_PAUSE_MENU_ITEM;

typedef struct JER_PAUSE_MENU
{
	const char* title;
	const JER_PAUSE_MENU_ITEM* items;	/* item_count entries (no terminator) */
	int item_count;

	/* A STABLE KEY, so other modules can contribute rows to THIS menu
	 * (jer_pause_menu_add_items). Optional -- appended at the end precisely so every
	 * existing initialiser stays valid and means what it always did.
	 *
	 * Set one whenever the menu is worth extending. A TITLE is display text and gets
	 * reworded ("Change car" became "Car options" in this very tree); contributing
	 * by title still works, but it is a string a rename can quietly break, which is
	 * exactly the failure a stable key exists to prevent. By convention use the
	 * owning module's prefix: "mp.car", "mp.warp". */
	const char* id;
} JER_PAUSE_MENU;

/* Register a menu (and its submenus). Call from the module entry; safe to
 * call once per activation (the registry is reset on every reload). */
void jer_pause_menu_register(const JER_PAUSE_MENU* menu);

/* CONTRIBUTE ROWS TO A MENU YOU DO NOT OWN.
 *
 * `target` is the menu's id (jer_pause_menu.id), or its title when it has no id.
 * The rows land at the END of that menu, before nothing in particular -- the menu's
 * own items keep their order and positions, so a contribution can never displace
 * what the owner put there.
 *
 * A module owns its page and decides nothing else: this is how carhacks puts its
 * car actions into mp's car page without either module including the other, and it
 * is deliberately one-way (there is no remove, and no reordering).
 *
 * Resolved when the menu is BUILT, not when this is called, so a contribution may
 * be made in any order relative to the menu being registered.
 *
 * Returns 1 when recorded. Returns 0 -- AND SAYS WHY IN THE LOG -- when there is no
 * room left, so a contribution that never appears can be found rather than
 * wondered about. A `target` that matches no menu yet is NOT an error: the match
 * happens at build time. */
int jer_pause_menu_add_items(const char* target, const JER_PAUSE_MENU_ITEM* items, int count);

/* Reading the contributions back. THE BRIDGE USES THESE when it builds a menu; they
 * are here (rather than engine-side) because the two copies of this header must be
 * identical, and a module has no less right to see what was contributed to it. */
int jer_pause_menu_extra_count(const char* target);
const JER_PAUSE_MENU_ITEM* jer_pause_menu_extra_at(const char* target, int index);

/* Host-side (engine): clear the registry before each module activation
 * batch so re-registration after a reload doesn't accumulate duplicates. */
void jer_pause_menu_reset(void);

/* Host-side (engine): registered menu count / lookup. */
int jer_pause_menu_count(void);
const JER_PAUSE_MENU* jer_pause_menu_get(int index);

#ifdef __cplusplus
}
#endif

#endif /* JER_PAUSE_MENU_H */
