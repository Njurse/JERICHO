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

typedef struct JerMenu
{
	const char* const* items;	/* label table (may be a static array) */
	int count;			/* number of items */
	int cursor;			/* current selection */
	int visible;			/* 1 while the menu is active */
	int kind;			/* JER_MENU_KIND_* (drives jer_menu_tick) */
	const char* title;		/* heading (may be NULL) */
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
