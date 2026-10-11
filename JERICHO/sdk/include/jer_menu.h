#ifndef JER_MENU_H
#define JER_MENU_H

/* jer_menu — the JERICHO frontend-menu library.
 *
 * A tiny reusable list menu (cursor + wrap) for modules that need to take
 * over a frontend decision — e.g. the levelhacks mod's
 * Singleplayer/Multiplayer prompt after the take-a-ride city select. The
 * module owns the rendering (via JER_EVENT_DRAW_OVERLAY or its own draw)
 * and the pad read; this just keeps the cursor state and the label
 * mapping, so every menu looks/behaves the same. The core stays free of
 * the game headers, so the caller passes its own pad-bit VALUES (e.g.
 * MPAD_D_UP from the game's pad.h).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* HOW DEEP A MENU MAY NEST, in levels INCLUDING the top one: 1 = a flat menu,
 * 2 = a menu with submenus, 3 = a sub-submenu, and so on. Levels are a fixed
 * per-menu stack, so this is a ceiling: asking to go deeper is REFUSED with
 * JER_MENU_TOO_DEEP rather than corrupting the stack (a library has no log to
 * shout into, so the return code IS how it says so).
 *
 * Deliberately the same number as JER_PAUSE_MENU_MAX_DEPTH: the pause menus and
 * this library are two ways to put a nested menu in front of a player, and two
 * different ceilings for one idea would just be a thing to get wrong. */
#define JER_MENU_MAX_DEPTH 5

/* One item's child level, declared by the CALLER as a static tree. An item with
 * no child has no entry (or a NULL one) in the table it is read from.
 *
 * `subs` is the child's OWN table, so a tree of any shape is just nested static
 * arrays -- no allocation and nothing for the library to own. */
typedef struct JerMenuSub
{
	const char* const* items;		/* the child level's labels */
	int count;				/* the child level's item count */
	const struct JerMenuSub* subs;		/* the child's own submenus (NULL = none) */
} JerMenuSub;

/* One level of the open menu: what a level needs to be restored when the player
 * backs out of it, cursor included -- so returning to a page puts you back where
 * you were rather than at the top. */
typedef struct JerMenuLevel
{
	const char* const* items;
	int count;
	int cursor;
	const JerMenuSub* subs;
	int subCount;
	const char* label;			/* the parent item this level was opened from */
} JerMenuLevel;

typedef struct JerMenu
{
	const char* const* items;	/* label table of the CURRENT level (may be a static array) */
	int count;			/* number of items in the CURRENT level */
	int cursor;			/* current selection in the CURRENT level */
	int visible;			/* 1 while the menu is active */
	int kind;			/* JER_MENU_KIND_* (drives jer_menu_tick) */
	const char* title;		/* heading (may be NULL) */

	/* NESTING. A flat menu leaves all of this zero and behaves exactly as it did
	 * before: subs == NULL means every item is a leaf. */
	const JerMenuSub* subs;		/* TOP level's table: subs[i] belongs to item i */
	int subCount;			/* entries in subs[] (0 = flat) */
	int depth;			/* 0 = the top level */
	JerMenuLevel stack[JER_MENU_MAX_DEPTH];	/* stack[depth] == the top level */
} JerMenu;

/* The base menu TYPES. A type is only how the cursor and the input behave (and
 * how jer_menu_draw renders it); the state is the same JerMenu, so a caller can
 * switch a menu's type between screens with the matching constructor. */
enum
{
	JER_MENU_KIND_LIST = 0,	/* a vertical list (the frontend list look) */
	JER_MENU_KIND_YESNO,	/* a centred Yes / No prompt (the quit-prompt look) */
	JER_MENU_KIND_CAROUSEL	/* a one-at-a-time picker with left/right (vehicle select) */
};

/* What jer_menu_update / jer_menu_tick report. 1 stays "the player chose the
 * current item", so existing callers keep working unchanged. */
enum
{
	JER_MENU_NOTHING = 0,	/* no input worth acting on */
	JER_MENU_SELECT = 1,	/* chose the current item (a LEAF item) */
	JER_MENU_OPENED = 2,	/* chose an item that HAS a submenu: one level was pushed */
	JER_MENU_TOO_DEEP = 3	/* chose a submenu at JER_MENU_MAX_DEPTH: refused, nothing moved */
};

/* begin/end a menu over a label table (a plain list by default) */
void jer_menu_begin(JerMenu* m, const char* const* items, int count);
void jer_menu_end(JerMenu* m);

/* the typed constructors — each is jer_menu_begin plus a kind and a title */
void jer_menu_list(JerMenu* m, const char* title, const char* const* items, int count);
void jer_menu_yesno(JerMenu* m, const char* title);		/* Yes at 0, No at 1 */
void jer_menu_carousel(JerMenu* m, const char* title, const char* const* items, int count);

/* nav: pass the caller's pad bit VALUES (upBit/downBit/selectBit, e.g.
 * MPAD_D_UP / MPAD_D_DOWN / MPAD_CROSS) + the current pad + the pressed
 * edge. Returns 1 when the current item is confirmed (the select bit's
 * press edge). */
int jer_menu_update(JerMenu* m, int upBit, int downBit, int selectBit, int curPad, int curPadNew);

/* The unified tick for the typed menus: up/down moves a list (or a carousel, as
 * a convenience), left/right moves a YESNO or CAROUSEL, and select confirms.
 * Returns 1 on confirm. Any bit value may be 0 to ignore that direction. */
int jer_menu_tick(JerMenu* m, int upBit, int downBit, int leftBit, int rightBit,
		  int selectBit, int curPad, int curPadNew);

/* Declare the menu's submenu tree: the TOP level's table, whose entries may carry
 * their own tables in turn. Call it after one of the constructors; a menu with no
 * submenus never needs it and is unchanged. `subs` must OUTLIVE the menu -- a
 * static array is the intended shape, since the library owns nothing. */
void jer_menu_submenus(JerMenu* m, const JerMenuSub* subs, int count);

/* Go back one level: returns 1 when a level was popped, 0 at the top level (there
 * is nothing to go back to, so a caller can treat that as "close the menu").
 * WHICH button means back is the caller's business -- this library stays free of
 * the game's pad constants, like the rest of it. */
int jer_menu_back(JerMenu* m);

/* 0 for the top level, 1 inside a submenu, and so on. */
int jer_menu_depth(const JerMenu* m);

/* 1 when the CURRENT item opens a submenu, so a caller can mark it in its own draw
 * (jer_menu_draw already does). */
int jer_menu_has_sub(const JerMenu* m);

/* the current item's label */
const char* jer_menu_label(const JerMenu* m);

/* the menu's heading ("" when none) */
const char* jer_menu_title(const JerMenu* m);

/* Draw a menu in the standard JERICHO look, centred in a 320x240 frame at the
 * given anchor (pass the frame's centre, e.g. 160, 110). Engine-side (it uses
 * the game's own font), called from a module's JER_EVENT_DRAW_OVERLAY handler
 * so the module decides when it is on screen. */
void jer_menu_draw(const JerMenu* m, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* JER_MENU_H */
