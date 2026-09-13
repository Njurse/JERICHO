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
 */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_HUD_TEXT_MAX	96	/* longest message, terminator included */
#define JER_HUD_MAX		4	/* messages that can be on screen at once */
#define JER_HUD_DEFAULT_FRAMES	90	/* ~3 seconds at the 30 fps sim step */

/* Show `text` on the HUD for `frames` (<= 0 means JER_HUD_DEFAULT_FRAMES).
 * Returns the slot used, or -1 if the text was empty. Longer text is
 * truncated to JER_HUD_TEXT_MAX-1 characters rather than rejected. */
int jer_hud_message(const char* text, int frames);

/* Show `text` as the only message: clears anything already pending first, for
 * a single-line banner that a repeating event would otherwise stack up. */
int jer_hud_message_replace(const char* text, int frames);

/* Drop every pending message. */
void jer_hud_clear(void);

/* How many messages are currently on screen (0 = none). */
int jer_hud_active(void);

/* Engine-internal: draw the active messages and age the queue. Called once per
 * frame from DrawGame, next to the pause menu. Modules do not call this. */
void jer_hud_draw(void);

#ifdef __cplusplus
}
#endif

#endif
