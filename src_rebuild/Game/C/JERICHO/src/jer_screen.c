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

/* PsyX's widescreen viewport probe, DECLARED rather than included: this file builds
 * into the lean runtime lib, whose TUs have no game headers - and PsyX_public.h
 * drags in math/SDL defines that collide with the game's (the same C4005 reason
 * mp.c declares its debug-key hook instead of including it). The rect is the PSX
 * RECT16 the call fills; only x and w are read. C linkage, so the symbol resolves
 * against PsyX at link time whatever it is named here. */
typedef struct JerWideRect { short x, y, w, h; } JerWideRect;
extern void PsyX_GetPSXWidescreenMappedViewport(JerWideRect* rect);

static JER_SCREEN gScreens[JER_SCREEN_MAX];
static int gScreenCount = 0;

/* index of the visible screen, or -1 */
static int gVisible = -1;

/* See jer_screen.h. Mirrors sky.c's sun remap: the GTE projection lands in the
 * widescreen-MAPPED viewport (a centred 240-unit square-pixel band inside the
 * 320-wide buffer, so [40, 280] at 4:3 and [70, 250] at 16:9), and 2D drawing
 * happens in the 320x240 framebuffer. It is NOT the identity even at 4:3 - it is a
 * small correction there and a large one on a wide aspect, which is why the report
 * is about 16:9. */
int jer_screen_remap_x(int x)
{
#ifndef PSX
	JerWideRect vp;

	PsyX_GetPSXWidescreenMappedViewport(&vp);

	/* The viewport is stable through a level, so log it only when it CHANGES: that
	 * one line answers "what is the mapping" for a run, and shows when a display-mode
	 * switch (frontend, map screen, a fade) moves it. */
	{
		static int lastX = 0x7fffffff, lastW = 0x7fffffff;

		if (vp.x != lastX || vp.w != lastW)
		{
			lastX = vp.x;
			lastW = vp.w;
			jer_log("[jer_screen] widescreen viewport x=%d right=%d (span %d)\n",
				vp.x, vp.w, vp.w - vp.x);
		}
	}

	/* NOTE the encoding: this rect's w is the RIGHT EDGE (PsyX_render does
	 * rect->w += rect->x), not a width, so test the span rather than w > 320.
	 * The body is dr2math's RemapVal(x, vp.x, vp.w, 0, 320), written out because
	 * that header is not reachable from here. */
	if (vp.w > vp.x)
		x = (int)(320.0f * ((float)x - (float)vp.x) / ((float)vp.w - (float)vp.x));
#endif

	return x;
}

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
