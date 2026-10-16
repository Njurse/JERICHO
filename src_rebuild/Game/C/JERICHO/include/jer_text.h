#ifndef JER_TEXT_H
#define JER_TEXT_H

/* ------------------------------------------------------------------
 * jer_text.h — measure text, and draw it where YOU want it.
 *
 * Both of the engine's text fonts are PROPORTIONAL: a glyph advances by its own
 * width, not by a fixed cell. That is fine for a sentence and useless for a
 * table, and the frontend's menus cannot help either — a menu row is ONE string,
 * right-aligned into a fixed 256px box and truncated at 32 characters, so padded
 * columns drift with whatever happens to be in them.
 *
 * A module that wants a table therefore has to do two things itself: know how
 * wide a string is, and draw at an x of its own choosing. That is all this header
 * is. A row is then N calls, one per column, at N fixed x positions.
 *
 * The two states have different fonts, and this follows the state exactly as the
 * console's feed does:
 *
 *     frontend   FEPrintStringSized            the FE font
 *     in-game    PrintStringHiresScaledSpaced  the HQ font, or PrintString
 *
 * jer_text_width returns a value in the SAME units as the x you hand to
 * jer_text_draw_at, so a caller lays out columns without knowing which font is in
 * use. Both are NULL-safe: a NULL or empty string is 0 wide and draws nothing.
 *
 * SCALE is a percentage, and 100 is the size the mp console draws at (the
 * familiar small text: an FE scale of 2048, an HQ scale of 0.1375). Smaller
 * values exist for one reason — a five-column row does not fit at 100 — so a
 * table can trade size for columns while everything else keeps using 100.
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

/* 100 == the console's small text. The reference the percentages are relative to. */
#define JER_TEXT_SCALE_CONSOLE 100

/* How wide `text` would be drawn at `scale_pct`, in the units the draw advances
 * by. 0 for NULL or an empty string. Use it to centre a line or to check a column
 * fits before drawing it into a fixed slot. */
int jer_text_width(int frontend, int scale_pct, const char* text);

/* Draw `text` with its LEFT edge at x, and return the x just past its last glyph
 * (so cells can simply be chained). Returns x unchanged when there is nothing to
 * draw. `frontend` selects the font, as above. */
int jer_text_draw_at(int frontend, int scale_pct, int x, int y, const char* text, int r, int g, int b);

/* Draw `text` centred on `cx`. Returns the width drawn. */
int jer_text_draw_centred(int frontend, int scale_pct, int cx, int y, const char* text, int r, int g, int b);

#ifdef __cplusplus
}
#endif

#endif /* JER_TEXT_H */
