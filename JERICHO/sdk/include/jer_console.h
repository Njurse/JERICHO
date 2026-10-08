#ifndef JER_CONSOLE_H
#define JER_CONSOLE_H

/* jer_console — a unified, scrolling STATUS STREAM for JERICHO.
 *
 * The bottom-left status messages used to be scattered: mp drew its own
 * HOST/CLIENT tag and two connection lines, and its notifications lived in a
 * separate ring. jer_console is the one place for the STREAM: a small ring of
 * short lines, drawn half-size at the bottom left, newest at the bottom, so a
 * connection handshake, a join and a chat message read as a single log.
 *
 * A line is KEPT (scrollback), not flashed: the join sequence runs in the
 * frontend, and the same ring is drawn there too, so a "connected" line is
 * still on screen once the level is up. Text longer than a row WRAPS onto the
 * next row rather than running off the edge. The ring is deliberately small —
 * the oldest row falls off rather than the buffer growing without bound.
 *
 * TWO KINDS OF LINE:
 *   - STATUS (jer_console_line / jer_console_log), single colour, dimmed with
 *     age. Hidden while the console is toggled off.
 *   - CHAT (jer_console_chat), drawn from colour RUNS so the speaker's name can
 *     carry their own colour. Chat is a conversation, not debug output, so a
 *     chat line is drawn even while the console is off.
 *
 * The implementation lives in the GAME (jer_console.c) because only the game
 * can draw into the display buffer, exactly like jer_hud.c. Modules just call
 * these.
 *
 * TOGGLE. `~` / backtick flips the console on or off and the choice is saved
 * (jer_config, CONFIG/hud.ini, key `console`). While it is off the status
 * stream is hidden — and so are the mp pieces drawn as part of the same block
 * (its HOST/CLIENT tag and connection lines), which ask jer_console_enabled()
 * before drawing. The chat input line (jer_console_input) always draws, so what
 * you are typing is never invisible.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_CONSOLE_LINE_MAX	96	/* longest ROW; a longer line wraps */
#define JER_CONSOLE_MAX		8	/* rows kept (and drawn) at once */

#define JER_CONSOLE_SEG_MAX	6	/* colour runs in one line */
#define JER_CONSOLE_SEG_TEXT_MAX 64	/* text in one run, terminator included */

/* One colour run of a line. `ambient` (non-zero) draws in the line's own
 * colour — the age-based grey a plain line uses — instead of r/g/b. */
typedef struct JER_CONSOLE_SEG
{
	const char* text;
	unsigned char r, g, b;
	int ambient;
} JER_CONSOLE_SEG;

/* Append one STATUS line. jer_console_log is printf-style (jer_error's /
 * jer_log's convention); jer_console_line takes it ready-made. Text longer than
 * a row wraps. NULL-safe. */
void jer_console_line(const char* text);
void jer_console_log(const char* fmt, ...);

/* Append one CHAT line, built from colour runs (a run with ambient != 0 keeps
 * the plain-line colour). Drawn even while the console is off. NULL-safe; a
 * line with no text at all is dropped. */
void jer_console_chat(const JER_CONSOLE_SEG* segs, int count);

/* The line being typed (the chat prompt), drawn at the bottom of the block
 * REGARDLESS of the console's on/off state. Call it every frame while the
 * prompt is open and jer_console_input(NULL) to clear it. Truncated to one row
 * (an input buffer is short). */
void jer_console_input(const char* text);

/* Drop every line (the stream is empty again). */
void jer_console_clear(void);

/* How many rows are currently in the ring (0 = nothing to show). */
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
 * path (FEPrintStringSized) over the in-game one (PrintStringHiresScaledSpaced);
 * the two states use different fonts. Called once per frame from DrawGame
 * (frontend = 0) and State_FrontEnd (frontend = 1). Modules do not call this. */
void jer_console_draw(int frontend);

#ifdef __cplusplus
}
#endif

#endif /* JER_CONSOLE_H */
