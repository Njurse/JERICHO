/*
 * jer_prompt.c — a host-owned Yes/No prompt (the state; the FRONTEND draws it).
 *
 * It used to be a jer_screen presentation screen, which was the wrong shape for a
 * question: a presentation screen is a heading plus one live body line, so the two
 * answers ended up as one padded string ("Yes        No") inside the ordinary menu
 * box, and the menu underneath still drew its own rows and labels over the top of
 * it. A question needs the screen for itself, which is what the engine's own quit
 * prompt does -- see JerichoDrawPrompt in Frontend/FEmain.c for the drawing side.
 */
#include <stdio.h>
#include <string.h>

#include "jer_prompt.h"

static int gPromptOpen = 0;
static int gPromptYes = 1;			/* highlight starts on Yes */
static char gPromptQuestion[160] = "";
static JER_PROMPT_ON_YES gPromptOnYes = NULL;

int jer_prompt_begin(const char* question, JER_PROMPT_ON_YES on_yes)
{
	snprintf(gPromptQuestion, sizeof(gPromptQuestion), "%s",
		(question != NULL && question[0] != 0) ? question : "Are you sure?");

	gPromptYes = 1;
	gPromptOnYes = on_yes;
	gPromptOpen = 1;

	return 1;
}

int jer_prompt_active(void)
{
	return gPromptOpen;
}

const char* jer_prompt_question(void)
{
	return gPromptOpen ? gPromptQuestion : NULL;
}

int jer_prompt_selected_yes(void)
{
	return gPromptOpen && gPromptYes;
}

void jer_prompt_close(void)
{
	gPromptOpen = 0;
	gPromptOnYes = NULL;
	gPromptQuestion[0] = 0;
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
