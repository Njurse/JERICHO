#ifndef JER_SCREEN_H
#define JER_SCREEN_H

/* ------------------------------------------------------------------
 * jer_screen.h — module-provided PRESENTATION screens.
 *
 * A presentation screen is a full-screen, NON-INTERACTIVE frontend screen: a
 * heading, one live body line and an optional step counter. It is the
 * counterpart of jer_frontend.h for work that has nothing to choose — a boot or
 * progress screen, e.g.
 *
 *      Compiling JERICHO addons...
 *      ZOOMMOD [3/7]
 *
 * The engine only pumps the registry (jer_screen_tick + jer_screen_active +
 * jer_screen_title/jer_screen_body), so JERICHO and modules add screens without
 * adding engine code.
 *
 *   static char gName[32]; static int gStep, gTotal;
 *   static void body(void* ud, char* out, int max) { snprintf(out, max, "%s [%d/%d]", gName, gStep, gTotal); }
 *   static int  step(void* ud) { return ++gStep > gTotal; }        // 1 = finished
 *   static const JER_SCREEN scr = { "compile", "Compiling JERICHO addons...", body, step, NULL, NULL, NULL };
 *   ...
 *   jer_screen_register(&scr);
 *   jer_screen_show("compile");
 *
 * Behaviour:
 *   - get_body  : called on every draw for the live body line (optional).
 *   - on_update : called once per frame while visible, before the body is read;
 *                 return non-zero when the work is done and the screen may be
 *                 dismissed.
 *   - on_enter / on_exit : called once when shown / dismissed.
 *
 * A screen is only drawn while the game is in the frontend, so show() from a
 * gameplay hook takes effect on the next frontend frame.
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_SCREEN_MAX		16
#define JER_SCREEN_BODY_MAX	96

typedef struct JER_SCREEN
{
	const char* id;					/* stable id (jer_screen_show) */
	const char* title;				/* static heading line */

	void (*get_body)(void* ud, char* out, int max);	/* live body line (optional) */
	int  (*on_update)(void* ud);			/* non-zero = finished, dismiss */
	void (*on_enter)(void* ud);			/* became visible */
	void (*on_exit)(void* ud);			/* dismissed */
	void* userdata;
} JER_SCREEN;

/* Register a screen from a module entry (or the JERICHO host). The registry is
 * cleared on every reload, so register once per activation. Returns its index,
 * or -1 when the table is full or the screen is invalid. */
int  jer_screen_register(const JER_SCREEN* screen);

/* Host-side (engine): clear the registry before each activation batch. */
void jer_screen_reset(void);

/* Show a registered screen by id (NULL dismisses). Non-zero if it was found. */
int  jer_screen_show(const char* id);

/* Dismiss the visible screen (idempotent). Runs its on_exit. */
void jer_screen_dismiss(void);

/* Host-side (engine): advance the visible screen one frame. Runs on_update and
 * auto-dismisses when it reports finished. Safe to call with nothing visible. */
void jer_screen_tick(void);

/* Host-side (engine): non-zero while a screen should be drawn. */
int  jer_screen_active(void);

/* Host-side (engine): the visible screen, its heading, and its live body line
 * (the body is rebuilt from get_body each call). All NULL-safe. */
const JER_SCREEN* jer_screen_current(void);
const char* jer_screen_title(void);
void jer_screen_body(char* out, int max);

#ifdef __cplusplus
}
#endif

#endif /* JER_SCREEN_H */
