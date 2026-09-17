/*
 * jer_screen.c — the JERICHO presentation-screen registry.
 *
 * Pure registry + state; the engine draws it (see Frontend/FEmain.c). Keeping
 * the data here and only the drawing in the engine matches the rest of JERICHO
 * (jer_frontend.h / jer_pause_menu.h) and means a module needs no engine code
 * to raise a screen.
 */
#include <string.h>

#include "jer_screen.h"

static JER_SCREEN gScreens[JER_SCREEN_MAX];
static int gScreenCount = 0;

/* index of the visible screen, or -1 */
static int gVisible = -1;

void jer_screen_reset(void)
{
	memset(gScreens, 0, sizeof(gScreens));
	gScreenCount = 0;
	gVisible = -1;
}

int jer_screen_register(const JER_SCREEN* screen)
{
	if (screen == NULL || screen->id == NULL || screen->id[0] == 0)
		return -1;

	if (gScreenCount >= JER_SCREEN_MAX)
		return -1;

	/* re-registering the same id replaces it, so a module can refresh a screen
	 * it already declared without growing the table */
	{
		int i;

		for (i = 0; i < gScreenCount; i++)
		{
			if (strcmp(gScreens[i].id, screen->id) == 0)
			{
				gScreens[i] = *screen;
				return i;
			}
		}
	}

	gScreens[gScreenCount] = *screen;

	return gScreenCount++;
}

static int jer_screen_find(const char* id)
{
	int i;

	if (id == NULL)
		return -1;

	for (i = 0; i < gScreenCount; i++)
	{
		if (gScreens[i].id != NULL && strcmp(gScreens[i].id, id) == 0)
			return i;
	}

	return -1;
}

int jer_screen_show(const char* id)
{
	int index = jer_screen_find(id);

	if (index < 0)
		return 0;

	if (gVisible != index)
	{
		jer_screen_dismiss();

		gVisible = index;

		if (gScreens[gVisible].on_enter != NULL)
			gScreens[gVisible].on_enter(gScreens[gVisible].userdata);
	}

	return 1;
}

void jer_screen_dismiss(void)
{
	if (gVisible < 0)
		return;

	if (gScreens[gVisible].on_exit != NULL)
		gScreens[gVisible].on_exit(gScreens[gVisible].userdata);

	gVisible = -1;
}

void jer_screen_tick(void)
{
	if (gVisible < 0)
		return;

	if (gScreens[gVisible].on_update != NULL && gScreens[gVisible].on_update(gScreens[gVisible].userdata))
		jer_screen_dismiss();
}

int jer_screen_active(void)
{
	return gVisible >= 0 && gScreens[gVisible].title != NULL;
}

const JER_SCREEN* jer_screen_current(void)
{
	return gVisible >= 0 ? &gScreens[gVisible] : NULL;
}

const char* jer_screen_title(void)
{
	return jer_screen_active() ? gScreens[gVisible].title : NULL;
}

void jer_screen_body(char* out, int max)
{
	if (out == NULL || max <= 0)
		return;

	out[0] = 0;

	if (gVisible < 0)
		return;

	if (gScreens[gVisible].get_body != NULL)
		gScreens[gVisible].get_body(gScreens[gVisible].userdata, out, max);
	else if (gScreens[gVisible].title != NULL)
		/* no live body: nothing to add under the heading */
		out[0] = 0;
}
