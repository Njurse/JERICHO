#ifndef JER_NOTIFY_H
#define JER_NOTIFY_H

/* jer_notify — the engine's own on-screen notice.
 *
 * This is the text the game already uses for "You Drowned" / "You wrecked your
 * vehicle" / "Game Over": SetPlayerMessage -> DrawMessage, i.e. golden
 * (128,128,64), centred, near the top of the screen, for a fixed number of
 * seconds, with a priority that lets a more important line override a lesser
 * one. jer_notify wraps exactly that path, so a module's callout lands in the
 * same place, in the same style, and obeys the same priority rules - it is the
 * "in-game notify" a dev reaches for.
 *
 * The other style is jer_hud_message (jer_hud.h): a stacked queue of plain
 * lines drawn in the ambient colour. Use jer_notify for the game's own voice,
 * jer_hud_message for module chatter.
 *
 * The text is COPIED into an engine-side buffer before it is queued (the
 * engine keeps the pointer), so the caller may pass a temporary.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_NOTIFY_TEXT_MAX	128	/* longest notice, terminator included */
#define JER_NOTIFY_PRIORITY_MAX	5	/* engine mission lines use 2..3 */
#define JER_NOTIFY_SECONDS_MAX	30

/* Show `text` as the player's notice for `seconds` (<= 0 = 3 seconds) at
 * `priority` (0..JER_NOTIFY_PRIORITY_MAX; a line already showing with a higher
 * priority is not replaced). Returns 1 if it was queued, 0 if `text` was empty. */
int jer_notify(const char* text, int priority, int seconds);

/* Drop the player's current notice. */
void jer_notify_clear(void);

#ifdef __cplusplus
}
#endif

#endif
