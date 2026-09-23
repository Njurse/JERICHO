#ifndef JER_HUD_H
#define JER_HUD_H

/* jer_hud — on-screen HUD messages for JERICHO modules.
 *
 * A tiny queue of short plaintext lines drawn over the game world from the
 * engine's overlay pass (JER_EVENT_DRAW_OVERLAY fires immediately after), so a
 * module can report something to the player without owning a draw hook of its
 * own. Each message expires on its own after `frames` - the sim steps at 30 fps
 * (FilterFrameTime, 2 vblanks), so 90 frames is about 3 seconds.
 *
 * The implementation lives in the GAME (jer_hud.c) because only the game can
 * draw into the display buffer; modules just call these.
 *
 * Lines are centred and stacked from the top of the screen, oldest first. The
 * queue is deliberately small: a burst of messages recycles the slot with the
 * least time left rather than growing without bound.
 *
 * COLOUR. A message is a line made of one or more colour RUNS. jer_hud_message
 * is the single-run case and draws in whatever text colour is ambient at draw
 * time, exactly as it always has. jer_hud_message_segs lets a line name
 * something in its own colour - "You killed VASQUEZ" with only the name tinted
 * - by concatenating its runs into one centred line and stamping each run in
 * its own colour; a run marked `ambient` keeps the surrounding wording in the
 * ambient colour so a partly-coloured line still matches the plain ones.
 *
 * Colouring is ambient global state in the engine (SetTextColour/gFontColour),
 * so the drawer saves and restores it: a coloured message must not tint
 * whatever draws next.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_HUD_TEXT_MAX	96	/* longest message, terminator included */
#define JER_HUD_MAX		4	/* messages that can be on screen at once */
#define JER_HUD_DEFAULT_FRAMES	90	/* ~3 seconds at the 30 fps sim step */

#define JER_HUD_SEG_MAX		6	/* colour runs in one message */
#define JER_HUD_SEG_TEXT_MAX	64	/* longest run, terminator included */

/* One colour run of a message. `text` may be NULL (treated as empty and
 * skipped). With `ambient` 0 the run is drawn in r/g/b (0..255 each); with
 * `ambient` 1 it is drawn in the ambient text colour at draw time and r/g/b are
 * ignored - use that for the connective words in a partly-coloured line. */
typedef struct JER_HUD_SEG
{
	const char* text;
	unsigned char r, g, b;
	int ambient;
} JER_HUD_SEG;

/* Show `text` on the HUD for `frames` (<= 0 means JER_HUD_DEFAULT_FRAMES).
 * Returns the slot used, or -1 if the text was empty. Longer text is
 * truncated to JER_HUD_TEXT_MAX-1 characters rather than rejected. */
int jer_hud_message(const char* text, int frames);

/* Show a multi-colour message: the runs are concatenated into one centred line
 * and each is drawn in its own colour, so a name can carry its team's colour
 * inside a sentence. At most JER_HUD_SEG_MAX runs are used and each is
 * truncated to JER_HUD_SEG_TEXT_MAX-1 characters; empty runs are skipped.
 * Returns the slot used, or -1 when there is nothing to draw (count <= 0, or
 * every run empty). */
int jer_hud_message_segs(const JER_HUD_SEG* segs, int count, int frames);

/* Show `text` as the only message: clears anything already pending first, for
 * a single-line banner that a repeating event would otherwise stack up. */
int jer_hud_message_replace(const char* text, int frames);

/* Drop every pending message. */
void jer_hud_clear(void);

/* How many messages are currently on screen (0 = none). */
int jer_hud_active(void);

/* ---------------------------------------------------------------------------
 * Panels: anchored, PERSISTENT lines.
 *
 * The queue above is centred, stacked from the top and expires on its own -
 * right for an event, wrong for a READOUT, where a lock-on name belongs in a
 * corner for exactly as long as the lock lasts and then not at all. A panel is
 * re-read every frame at its anchor until it is cleared or set again, so a
 * caller simply sets it while its subject exists and clears it when it does not.
 *
 * Panels sharing an anchor stack downward in slot order. Returns the slot used,
 * or -1 for a bad slot or empty text. */
#define JER_HUD_PANEL_MAX	4

#define JER_HUD_ANCHOR_TOP_LEFT		0
#define JER_HUD_ANCHOR_TOP_CENTRE	1
#define JER_HUD_ANCHOR_TOP_RIGHT	2

int jer_hud_panel(int slot, int anchor, const char* text, int r, int g, int b);
void jer_hud_panel_clear(int slot);

/* Draw `slot` as a small filled METER instead of a line of text - `value/max`
 * is the fraction shown (e.g. a health bar under a lock-on name). Same
 * anchoring and stacking as jer_hud_panel; the empty part is a dark
 * semi-transparent track so it reads over any scenery. Call it every frame
 * while the subject lives, and clear the slot (jer_hud_panel_clear, or max
 * <= 0) when it does not. Returns the slot used, or -1 for a bad slot or an
 * empty meter. */
int jer_hud_panel_bar(int slot, int anchor, int value, int max, int r, int g, int b);

/* Engine-internal: draw the active messages and age the queue. Called once per
 * frame from DrawGame, next to the pause menu. Modules do not call this. */
void jer_hud_draw(void);

#ifdef __cplusplus
}
#endif

#endif
