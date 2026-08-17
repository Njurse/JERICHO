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
 * Menu depth: root -> Modules -> your menu -> one submenu level.
 * ------------------------------------------------------------------ */

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
} JER_PAUSE_MENU;

/* Register a menu (and its submenus). Call from the module entry; safe to
 * call once per activation (the registry is reset on every reload). */
void jer_pause_menu_register(const JER_PAUSE_MENU* menu);

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
