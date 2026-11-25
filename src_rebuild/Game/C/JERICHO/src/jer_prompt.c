/*
 * jer_prompt.c — a host-owned Yes/No prompt, optionally with a third answer
 * (the state; the FRONTEND draws it).
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
static int gPromptSel = 0;			/* 0 = Yes, 1 = No, 2 = the third; starts on Yes */
static int gPromptCount = 2;		/* answers on offer: 2, or 3 with a third label */
static char gPromptQuestion[400] = "";
static char gPromptYesLabel[24] = "Yes";
static char gPromptNoLabel[24] = "No";
static char gPromptThirdLabel[24] = "";
static JER_PROMPT_ON_YES gPromptOnYes = NULL;
static JER_PROMPT_ON_YES gPromptOnNo = NULL;
static JER_PROMPT_ON_YES gPromptOnThird = NULL;

int jer_prompt_begin(const char* question, JER_PROMPT_ON_YES on_yes)
{
	return jer_prompt_begin3(question, NULL, NULL, NULL, on_yes, NULL, NULL);
}

const char* jer_prompt_yes_label(void) { return gPromptYesLabel; }
const char* jer_prompt_no_label(void) { return gPromptNoLabel; }
const char* jer_prompt_third_label(void) { return gPromptThirdLabel; }

int jer_prompt_begin_ex(const char* question, const char* yes_label, const char* no_label,
	JER_PROMPT_ON_YES on_yes, JER_PROMPT_ON_YES on_no)
{
	return jer_prompt_begin3(question, yes_label, no_label, NULL, on_yes, on_no, NULL);
}

int jer_prompt_begin3(const char* question, const char* yes_label, const char* no_label,
	const char* third_label, JER_PROMPT_ON_YES on_yes, JER_PROMPT_ON_YES on_no,
	JER_PROMPT_ON_YES on_third)
{
	snprintf(gPromptYesLabel, sizeof(gPromptYesLabel), "%s", yes_label ? yes_label : "Yes");
	snprintf(gPromptNoLabel, sizeof(gPromptNoLabel), "%s", no_label ? no_label : "No");
	snprintf(gPromptThirdLabel, sizeof(gPromptThirdLabel), "%s", third_label ? third_label : "");
	gPromptCount = gPromptThirdLabel[0] != 0 ? 3 : 2;
	gPromptOnNo = on_no;
	gPromptOnThird = on_third;
	snprintf(gPromptQuestion, sizeof(gPromptQuestion), "%s",
		(question != NULL && question[0] != 0) ? question : "Are you sure?");

	gPromptSel = 0;
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
	return gPromptOpen && gPromptSel == 0;
}

int jer_prompt_selected(void)
{
	return gPromptOpen ? gPromptSel : -1;
}

void jer_prompt_close(void)
{
	gPromptOpen = 0;
	gPromptOnYes = NULL;
	gPromptOnNo = NULL;
	gPromptOnThird = NULL;
	gPromptThirdLabel[0] = 0;
	gPromptCount = 2;
	gPromptQuestion[0] = 0;
}

int jer_prompt_tick(int prev, int next, int confirm, int cancel)
{
	int answer;

	if (!gPromptOpen)
		return JER_PROMPT_NONE;

	/* one step per press, clamped. With two answers this is the old rule
	 * exactly: left = Yes, right = No. */
	if (prev && gPromptSel > 0)
		gPromptSel--;
	else if (next && gPromptSel < gPromptCount - 1)
		gPromptSel++;

	if (cancel)
	{
		JER_PROMPT_ON_YES noAct = gPromptOnNo;

		jer_prompt_close();
		if (noAct != NULL)
			noAct();
		return JER_PROMPT_NO;
	}

	if (!confirm)
		return JER_PROMPT_NONE;

	answer = gPromptSel == 0 ? JER_PROMPT_YES : (gPromptSel == 2 ? JER_PROMPT_THIRD : JER_PROMPT_NO);

	/* take the action before closing: the action may raise its own notice */
	{
		JER_PROMPT_ON_YES act = gPromptOnYes;
		JER_PROMPT_ON_YES noAct = gPromptOnNo;
		JER_PROMPT_ON_YES thirdAct = gPromptOnThird;

		jer_prompt_close();

		if (answer == JER_PROMPT_YES && act != NULL)
			act();
		else if (answer == JER_PROMPT_NO && noAct != NULL)
			noAct();
		else if (answer == JER_PROMPT_THIRD && thirdAct != NULL)
			thirdAct();
	}

	return answer;
}
