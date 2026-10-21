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
 *
 * Backing out of a menu returns to the row the player left the screen BELOW it
 * by, not to the top row: the engine keeps the pressed button on its nav stack
 * and re-highlights it. A row that no longer exists — a live roster that shrank,
 * a menu whose items changed — falls back to the first row, so the highlight is
 * always on a row the screen actually has. Nothing for a module to do; a menu
 * that wants the selection to follow something else sets it in on_enter.
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

// The module-menu registry. Menus append in modlist load order, so every module's
// index depends on what loaded before it - which is why callers must resolve their
// own menus with jer_frontend_find(id) and never hardcode an index. 12 was not
// enough for the shipped set plus cainescrossfire (carhacks 1 + mp 9 + CC 6 = 16);
// and an over-cap register used to be dropped without a word.
#define JER_FE_MAX_MENUS	24
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
	/* Optional heading, drawn in orange above the rows (e.g. "SELECT CAR"). */
	const char* title;
	/* Optional car-icon preview: called on each layout, set *city to a
	 * LevelNames index and *model to a model NUMBER; the engine draws that
	 * city's car icon for it (the stock car-select art). Leave it NULL for no
	 * icon. */
	void (*get_preview)(void* ud, int* city, int* model);
	/* Optional BACK. Triangle is the back button on every stock frontend
	 * screen, so the host offers it to a module menu too. A menu whose back is
	 * not simply "the previous screen" answers here: return 1 to say the press is
	 * handled (the host consumes it and relayouts). Return 0, or leave this NULL,
	 * and the host applies its OWN back - a row with is_back set if the menu has
	 * one, otherwise the engine's ordinary previous-screen pop, exactly as
	 * Triangle behaves on a stock screen. A menu that STANDS IN FOR a screen in a
	 * chain (opened with jer_frontend_open_replace) wants this default: its back
	 * is that screen's back. */
	int (*on_back)(void* ud);		/* Triangle: 1 = handled */

	/* Optional ROW WIDTH, in the frontend's own 640x480 draw space, for a menu whose
	 * rows carry more than a label -- a table, say. 0 = the stock box; either way
	 * jer_frontend_row_rect() reports what is in force, and the engine's box and
	 * highlight grow with it, so a module's own content and the row the engine
	 * highlights stay the same shape. (Trailing on purpose: every existing menu
	 * initialiser leaves it 0.) */
	int row_w;

	/* Optional ROW PITCH (0 = the stock 36), for a long list that wants to show
	 * more rows without the screen growing. */
	int row_step;

	/* Optional "the last N items are CONTROLS": they are laid out upward from the
	 * bottom edge instead of following the list, so they do not move while the
	 * list above them grows, shrinks or is rebuilt. 0 = every item is a list row. */
	int bottom_items;

	/* Optional LOWER EDGE, in the same 640x480 draw space (0 = the stock 462). With
	 * bottom_items, this is what the controls sit ON: a screen whose list is empty
	 * but whose controls should still read as "the bottom of the screen" asks for a
	 * lower value here rather than for the stock one. */
	int row_bottom;

	/* Optional TOP of the LIST, in the same draw space (0 = the stock 190). A
	 * screen with its own heading line wants the list lower: the stock top sits
	 * close enough to the menu TITLE that a heading drawn just above the first row
	 * lands on top of the title, and no gap is left between the heading and the rows. */
	int row_top;

	/* 1 = CENTRE the list rows in the band between row_top and the bottom-pinned
	 * controls, instead of hanging them off the top. A browser whose list is often
	 * shorter than the band wants this: the roster stays the middle of the screen
	 * instead of leaving a void under it. 0 = top-anchored (the stock behaviour).
	 * The ENGINE does the shifting, so the highlight and the rows still agree. */
	int list_align;
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
int  jer_frontend_current_menu(void);		/* index of the module menu on screen, else -1 */

/* Route the frontend's main-menu "Multiplayer" button to a registered menu
 * (by id). NULL clears it. */
void jer_frontend_set_main_entry(const char* id);

/* Module-side: the current menu's items changed — rebuild the screen. */
void jer_frontend_refresh(void);

/* Where the engine will place row `index` of the menu that is ON SCREEN, so a
 * module that draws its own row content (jer_text.h) sits on the row the engine
 * highlights. Read from the current menu -- which is the one being drawn -- so
 * there is no count to pass and a menu with its own row_w is reported at ITS
 * width. The x/w are constant for a menu; y moves with the index and, for a menu
 * too tall for the screen, with the count (the engine slides the block up to
 * fit). Returns 0 -- and writes nothing -- when no module menu is up or the index
 * is off it. */
int  jer_frontend_row_rect(int index, int* x, int* y, int* w, int* h);

/* The y the FIRST LIST row occupies (or would, when the list is empty), in the
 * same draw space. A screen with bottom-pinned controls cannot get this from
 * row_rect(0): with an empty list that index IS the first control, so a heading
 * drawn above it lands in the middle of the screen instead of above the table. */
int  jer_frontend_list_top(void);

/* The index of the row the engine is highlighting (0 when nothing is up). A
 * module that draws its own rows uses it to show something about the row under
 * the cursor. */
int  jer_frontend_selected(void);

/* Module-side: as jer_frontend_refresh(), but the rebuilt menu selects row
 * `index` instead of resetting to the top. A menu whose cursor must stay on a
 * control whose row index moves when its rows are rebuilt (a page row whose
 * page length changed) calls this after changing the page. Out of range falls
 * back to the top row, and an ordinary jer_frontend_refresh() clears it. */
void jer_frontend_refresh_at(int index);

/* Module-side: navigate to a registered menu (by index) or a raw frontend
 * screen index (e.g. for a stock screen a module wants to return to). */
void jer_frontend_open(int menuIndex);
void jer_frontend_goto(int screenIndex);

/* As above, but the target TAKES THE PLACE of the current screen instead of
 * being pushed on top of it: the nav stack is untouched, so Back/Triangle from
 * the new screen return to whatever the replaced screen would have returned to.
 * A menu that STANDS IN FOR a screen in a chain (e.g. the car-select menu
 * replacing the stock car screen) uses this, so backing out does not land on the
 * screen it replaced; without it the replaced screen is left on the back stack. */
void jer_frontend_replace(int screenIndex);
void jer_frontend_open_replace(int menuIndex);

#ifdef __cplusplus
}
#endif

#endif /* JER_FRONTEND_H */
