#ifndef JER_CONSOLE_H
#define JER_CONSOLE_H

/* jer_console — a unified, scrolling STATUS STREAM for JERICHO.
 *
 * The bottom-left status messages used to be scattered: mp drew its own
 * HOST/CLIENT tag and two connection lines, carhacks printed imports to the
 * log, and there was no one place to read "what is going on". jer_console is
 * that one place: a small ring of short lines, drawn half-size at the bottom
 * left, newest at the bottom, so a connection handshake and a level-load
 * message read as a single stream.
 *
 * A line is KEPT (scrollback), not flashed: the join sequence runs in the
 * frontend, and the same ring is drawn there too, so a "connected" line is
 * still on screen once the level is up. The ring is deliberately small — the
 * oldest line falls off rather than the buffer growing without bound.
 *
 * The implementation lives in the GAME (jer_console.c) because only the game
 * can draw into the display buffer, exactly like jer_hud.c. Modules just call
 * these.
 *
 * TOGGLE. `~` / backtick flips the console on or off and the choice is saved
 * (jer_config, CONFIG/hud.ini, key `console`). While it is off the whole
 * bottom HUD block is hidden — the stream AND the mp HOST/CLIENT tag and its
 * connection lines — so `~` is a single "hide the debug HUD" switch. Modules
 * that own part of that block ask jer_console_enabled() before drawing.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_CONSOLE_LINE_MAX	96	/* longest line, terminator included */
#define JER_CONSOLE_MAX		8	/* lines kept (and drawn) at once */

/* Append one line to the stream. jer_console_log is printf-style
 * (jer_error's / jer_log's convention); jer_console_line takes it ready-made.
 * Text longer than JER_CONSOLE_LINE_MAX-1 is truncated. NULL-safe. */
void jer_console_line(const char* text);
void jer_console_log(const char* fmt, ...);

/* Drop every line (the stream is empty again). */
void jer_console_clear(void);

/* How many lines are currently in the ring (0 = nothing to show). */
int  jer_console_count(void);

/* Is the console currently shown? Loads the saved value on first call
 * (default: ON). */
int  jer_console_enabled(void);

/* Set the shown/hidden state explicitly and SAVE it. Modules normally do not
 * call this — it is the `~` handler's job — but a lever/test may. */
void jer_console_set_enabled(int on);

/* Flip the shown/hidden state, save it, and return the NEW state. This is what
 * the `~` key handler calls. */
int  jer_console_toggle(void);

/* Engine-internal: draw the stream. `frontend` selects the frontend's font
 * path (FEPrintStringSized) over the in-game one (PrintStringHiresScaled);
 * the two states use different fonts. Called once per frame from DrawGame
 * (frontend = 0) and State_FrontEnd (frontend = 1). Modules do not call this. */
void jer_console_draw(int frontend);

#ifdef __cplusplus
}
#endif

#endif /* JER_CONSOLE_H */
