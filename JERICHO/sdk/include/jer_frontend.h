#ifndef JER_FRONTEND_H
#define JER_FRONTEND_H

/* ------------------------------------------------------------------
 * jer_frontend.h — module-provided FRONTEND menus.
 *
 * The frontend counterpart of jer_pause_menu.h: a module registers menus
 * (and submenus) from its entry point and the engine renders them as REAL
 * frontend screens — native PSXSCREEN buttons with the standard look and
 * navigation — not a floating overlay. A module can also route the
 * main-menu "Multiplayer" entry to one of its menus.
 *
 *   static const JER_FE_ITEM items[] = {
 *       { "Host Game", NULL, NULL, HostAct, NULL, -1, 0 },   // callback
 *       { "Join Game", NULL, NULL, JoinAct, NULL,  2, 0 },   // opens submenu 2
 *       { "Back",      NULL, NULL, NULL,    NULL, -1, 1 },   // previous screen
 *   };
 *   static const JER_FE_MENU menu = { "mp.root", items, 3, NULL, NULL };
 *   ...in the module entry...
 *   jer_frontend_register_menu(&menu);
 *   jer_frontend_set_main_entry("mp.root");   // Multiplayer button opens it
 *
 * Item behaviour:
 *   - submenu >= 0      : Cross opens that registered menu (engine nav).
 *   - is_back != 0      : Cross returns to the previous screen (engine nav).
 *   - on_activate != NULL: Cross calls it; return 1 if you handled it.
 *   - on_adjust != NULL : Left/Right call it with -1 / +1.
 *   - get_label != NULL : the label is rebuilt every layout (live values).
 *
 * The engine calls on_enter before each layout; a module whose item list
 * changes calls jer_frontend_refresh() to rebuild the screen.
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_FE_MAX_MENUS	8
#define JER_FE_MAX_ITEMS	12

typedef struct JER_FE_ITEM
{
	const char* label;			/* static label, or NULL with get_label */
	void (*get_label)(void* ud, char* out, int max);	/* dynamic label */
	void* userdata;
	int (*on_activate)(void* ud);		/* Cross: 1 = handled */
	int (*on_adjust)(void* ud, int dir);	/* Left/Right: 1 = handled */
	int  submenu;				/* >= 0: open this menu index */
	int  is_back;				/* != 0: return to the previous screen */
} JER_FE_ITEM;

typedef struct JER_FE_MENU
{
	const char* id;				/* stable id (jer_frontend_set_main_entry) */
	const JER_FE_ITEM* items;		/* item_count entries (no terminator) */
	int item_count;
	void (*on_enter)(void* ud);		/* refresh before layout (optional) */
	void* userdata;
} JER_FE_MENU;

/* Register a menu (and submenus). Call from the module entry; the registry
 * is cleared on every reload, so register once per activation. */
void jer_frontend_register_menu(const JER_FE_MENU* menu);

/* Host-side (engine): clear the registry before each activation batch. */
void jer_frontend_reset(void);

/* Host-side (engine): lookup. */
int  jer_frontend_menu_count(void);
const JER_FE_MENU* jer_frontend_menu_get(int index);
int  jer_frontend_find(const char* id);		/* -1 if absent */

/* Route the frontend's main-menu "Multiplayer" button to a registered menu
 * (by id). NULL clears it. */
void jer_frontend_set_main_entry(const char* id);

/* Module-side: the current menu's items changed — rebuild the screen. */
void jer_frontend_refresh(void);

/* Module-side: navigate to a registered menu (by index) or a raw frontend
 * screen index (e.g. for a stock screen a module wants to return to). */
void jer_frontend_open(int menuIndex);
void jer_frontend_goto(int screenIndex);

#ifdef __cplusplus
}
#endif

#endif /* JER_FRONTEND_H */
