#ifndef JER_TEXT_H
#define JER_TEXT_H

/* ------------------------------------------------------------------
 * jer_text.h — measure text, and draw it where YOU want it.
 *
 * Both of the engine's text fonts are PROPORTIONAL: a glyph advances by its own
 * width, not by a fixed cell. That is fine for a sentence and useless for a
 * table, and the frontend's menus cannot help either — a menu row is ONE string,
 * right-aligned into its box, so padded columns drift with whatever happens to be
 * in them.
 *
 * A module that wants a table therefore has to do two things itself: know how
 * wide a string is, and draw at an x of its own choosing. That is all this header
 * is. A row is then N calls, one per column, at N fixed x positions.
 *
 * THE FONT comes first, because the engine has one face per state and they are
 * different typefaces at different native sizes — text meant to sit on a menu has
 * to be the menu's face:
 *
 *     JER_TEXT_FE   the FRONTEND's face. What its own menus, labels and titles
 *                   use; the engine picks its bitmap or hires variant exactly as
 *                   it does for them.
 *     JER_TEXT_HQ   the in-game face (the HQ hires font, else the plain bitmap
 *                   one — again the engine's own choice).
 *
 * jer_text_width returns a value in the SAME units as the x you hand to
 * jer_text_draw_at, so a caller lays out columns without knowing which variant is
 * live. Both are NULL-safe: a NULL or empty string is 0 wide and draws nothing.
 *
 * SCALE is a percentage, and 100 is the size the mp console draws at (an FE scale
 * of 2048, an HQ scale of 0.1375); 200 would be the frontend's own standard size.
 * Smaller values exist for one reason — several columns in one row — so a table
 * can trade size for columns while everything else keeps using 100.
 *
 * The colour is the colour you SEE — it is pre-scaled for the font filter the way
 * jer_console_*'s colours are, so 255,255,255 is white rather than clipped.
 *
 * To draw a list over a module menu's own rows, pair this with
 * jer_frontend_row_rect() (jer_frontend.h) so your text sits on the rows the
 * engine is highlighting. Nothing here draws a background: a frontend screen
 * already has one, and an in-game caller is responsible for its own.
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

/* The engine's faces. See the note above: pick the one belonging to the screen
 * you are drawing on, or the text will not look like the rest of it. */
enum
{
	JER_TEXT_FE = 0,	/* the frontend's face (its menus' typeface) */
	JER_TEXT_HQ		/* the in-game face */
};

/* 100 == the console's small text. The reference the percentages are relative to. */
#define JER_TEXT_SCALE_CONSOLE 100

/* How wide `text` would be drawn at `scale_pct`, in the units the draw advances
 * by. 0 for NULL or an empty string. Use it to centre a line or to check a column
 * fits before drawing it into a fixed slot. */
int jer_text_width(int font, int scale_pct, const char* text);

/* Draw `text` with its LEFT edge at x, and return the x just past its last glyph
 * (so cells can simply be chained). Returns x unchanged when there is nothing to
 * draw. */
int jer_text_draw_at(int font, int scale_pct, int x, int y, const char* text, int r, int g, int b);

/* Draw `text` centred on `cx`. Returns the width drawn. */
int jer_text_draw_centred(int font, int scale_pct, int cx, int y, const char* text, int r, int g, int b);

#ifdef __cplusplus
}
#endif

#endif /* JER_TEXT_H */
