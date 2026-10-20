#ifndef JER_PROMPT_H
#define JER_PROMPT_H

/* ------------------------------------------------------------------
 * jer_prompt.h — a host-owned Yes/No prompt.
 *
 * The question and the two answers are drawn by the FRONTEND, in the same place
 * and the same style as the engine's own "are you sure you want to quit?" prompt:
 * the question centred at y=256, Yes and No side by side at y=288 either side of
 * centre, the highlighted answer in yellow. While it is up the prompt owns the
 * pad and the screen underneath draws nothing but its background, so the prompt
 * reads as a prompt rather than as a line of text over a live menu.
 *
 * JERICHO owns the question, the choices and what "yes" does:
 *
 *     jer_prompt_begin("Compile the deep mods? JERICHO must restart.", jer_compile_request);
 *
 * Left/right move the highlight, Cross confirms the highlighted answer, Circle
 * dismisses — the same shape as the engine's own quit prompt.
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	JER_PROMPT_NONE = 0,	/* still open */
	JER_PROMPT_NO,			/* declined or dismissed */
	JER_PROMPT_YES			/* confirmed */
};

/* Run when the player answers Yes. */
typedef void (*JER_PROMPT_ON_YES)(void);

/* Raise the prompt with the given question and the action for Yes. The caller
 * owns the wording. Returns non-zero when it is now up. */
int jer_prompt_begin(const char* question, JER_PROMPT_ON_YES on_yes);

/* Engine, once per frontend frame: feed the prompt the pad and act on the
 * answer. The four flags are already-decoded booleans, so the engine keeps its
 * own pad makes. Returns the result (JER_PROMPT_NONE while it is still open). */
int jer_prompt_tick(int prev, int next, int confirm, int cancel);

/* Non-zero while the prompt is up — while it is, it owns the pad, so the
 * frontend must not also act on it. */
int jer_prompt_active(void);

/* Dismiss it without an answer. */
void jer_prompt_close(void);

/* The engine's drawing side: the question, and which answer is highlighted.
 * Both are NULL/0 when no prompt is up. */
const char* jer_prompt_question(void);
int jer_prompt_selected_yes(void);

#ifdef __cplusplus
}
#endif

#endif /* JER_PROMPT_H */
