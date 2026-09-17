/*
 * jer_prompt.c — a host-owned Yes/No prompt, drawn as a presentation screen.
 *
 * Deliberately thin: it owns the question, the highlighted answer, and the
 * action for Yes. The engine pumps the screen and hands in the pad (see
 * jer_prompt.h).
 */
#include <stdio.h>
#include <string.h>

#include "jer_prompt.h"
#include "jer_screen.h"

static int gPromptOpen = 0;
static int gPromptYes = 1;			/* highlight starts on Yes */
static char gPromptQuestion[160] = "";
static JER_PROMPT_ON_YES gPromptOnYes = NULL;

static void jerPromptBody(void* ud, char* out, int max)
{
	(void)ud;

	/* the selection marker carries the state, since a presentation screen is
	 * one heading plus one live body line */
	snprintf(out, max, "%sYes        %sNo",
		(gPromptYes != 0) ? "> " : "  ",
		(gPromptYes != 0) ? "  " : "> ");
}

int jer_prompt_begin(const char* question, JER_PROMPT_ON_YES on_yes)
{
	JER_SCREEN scr;

	snprintf(gPromptQuestion, sizeof(gPromptQuestion), "%s",
		(question != NULL) ? question : "");

	gPromptYes = 1;
	gPromptOnYes = on_yes;
	gPromptOpen = 1;

	/* the title is per-question, so register a screen with it patched in */
	memset(&scr, 0, sizeof(scr));
	scr.id = "jer-prompt";
	scr.title = gPromptQuestion;
	scr.get_body = jerPromptBody;

	if (jer_screen_register(&scr) < 0)
	{
		gPromptOpen = 0;
		return 0;
	}

	return jer_screen_show("jer-prompt");
}

int jer_prompt_active(void)
{
	return gPromptOpen;
}

void jer_prompt_close(void)
{
	gPromptOpen = 0;
	gPromptOnYes = NULL;
	jer_screen_dismiss();
}

int jer_prompt_tick(int prev, int next, int confirm, int cancel)
{
	int answer;

	if (!gPromptOpen)
		return JER_PROMPT_NONE;

	if (prev || next)
		gPromptYes = prev ? 1 : 0;

	if (cancel)
	{
		jer_prompt_close();
		return JER_PROMPT_NO;
	}

	if (!confirm)
		return JER_PROMPT_NONE;

	answer = gPromptYes ? JER_PROMPT_YES : JER_PROMPT_NO;

	/* take the action before closing: the action may raise its own notice */
	{
		JER_PROMPT_ON_YES act = gPromptOnYes;

		jer_prompt_close();

		if (answer == JER_PROMPT_YES && act != NULL)
			act();
	}

	return answer;
}
