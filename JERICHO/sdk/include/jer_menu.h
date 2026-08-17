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
} JerMenu;

/* begin/end a menu over a label table */
void jer_menu_begin(JerMenu* m, const char* const* items, int count);
void jer_menu_end(JerMenu* m);

/* nav: pass the caller's pad bit VALUES (upBit/downBit/selectBit, e.g.
 * MPAD_D_UP / MPAD_D_DOWN / MPAD_CROSS) + the current pad + the pressed
 * edge. Returns 1 when the current item is confirmed (the select bit's
 * press edge). */
int jer_menu_update(JerMenu* m, int upBit, int downBit, int selectBit, int curPad, int curPadNew);

/* the current item's label */
const char* jer_menu_label(const JerMenu* m);

#ifdef __cplusplus
}
#endif

#endif /* JER_MENU_H */
