/* jer_console.c — see JERICHO/include/jer_console.h.
 *
 * One small ring of status lines drawn half-size at the bottom left, in the
 * frontend and in-game, as the single place the mp handshake / level-load
 * messages land. Engine-side (not in JERICHO/src) for the same reason as
 * jer_hud.c: only the game can draw into the display buffer.
 *
 * The two states use DIFFERENT fonts, so the drawer has two paths:
 *   in-game   PrintStringHiresScaled at 0.138f (half of the 0.275f default),
 *             falling back to the plain PSX printer when no HQ font is loaded;
 *   frontend  FEPrintStringSized at 2048 (half of the 4096 the frontend's own
 *             FEPrintString uses) — the frontend never sees the in-game font.
 *
 * Like jer_hud.c this SAVES gFontColour on entry and restores it on exit:
 * PrintString stamps the ambient colour into every glyph, and whatever draws
 * next (the module overlays, jer_error's notices) expects the colour it left.
 */

#include "driver2.h"
#include "pres.h"
#include "system.h"		// DB* current (the display buffer + OT the console draws into)

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "jericho.h"
#include "jer_console.h"

#include "JERICHO/include/jer_config.h"

/* Reaches into pres.c for the HQ-font-loaded flag, the same way `PrintString`
 * decides which path to take; only read, never written. */
extern unsigned int gHiresFontTexture;

/* The frontend's scaled text printer (FEmain.c). Declared here rather than in a
 * header because the frontend keeps its printers file-local. */
extern int FEPrintStringSized(char* string, int x, int y, int scale, int transparent, int r, int g, int b);

/* Where the console's saved state lives (jer_config -> CONFIG/hud.ini). */
#define JER_CONSOLE_MOD		"hud"
#define JER_CONSOLE_KEY		"console"

/* --- layout ------------------------------------------------------------- */
/* In-game: bottom-left, half size. The newest line sits at BOTTOM; the mp
 * HOST/CLIENT tag block keeps rows 208..232 below it. */
#define JER_CONSOLE_X			8
#define JER_CONSOLE_TEXT_SCALE		0.138f	/* half of the 0.275f default */
#define JER_CONSOLE_LINE_H		11
#define JER_CONSOLE_BOTTOM		200

/* Frontend: the FE draws in its own (larger) coordinate space and font. */
#define JER_CONSOLE_FE_X		32
#define JER_CONSOLE_FE_SCALE		2048	/* half of the FE's standard 4096 */
#define JER_CONSOLE_FE_LINE_H		18
#define JER_CONSOLE_FE_BOTTOM		456

static char sLines[JER_CONSOLE_MAX][JER_CONSOLE_LINE_MAX];
static int  sCount = 0;
static int  sEnabled = -1;		/* -1 = not loaded from the config yet */

/* --- ring --------------------------------------------------------------- */
static void jerConsolePush(const char* text)
{
	int i;

	if (text == NULL || text[0] == '\0')
		return;

	if (sCount < JER_CONSOLE_MAX)
	{
		i = sCount++;
	}
	else
	{
		/* full: drop the oldest line and make room at the end */
		memmove(sLines[0], sLines[1], sizeof(sLines[0]) * (JER_CONSOLE_MAX - 1));
		i = JER_CONSOLE_MAX - 1;
	}

	strncpy(sLines[i], text, JER_CONSOLE_LINE_MAX - 1);
	sLines[i][JER_CONSOLE_LINE_MAX - 1] = '\0';

	/* Mirror to the log, deliberately: the on-screen stream is then greppable,
	 * exactly like MpConnEvent's lines ("read me what you see" -> one search). */
	jer_log("[console] %s\n", sLines[i]);
}

void jer_console_line(const char* text)
{
	jerConsolePush(text);
}

void jer_console_log(const char* fmt, ...)
{
	char buf[JER_CONSOLE_LINE_MAX];
	va_list ap;

	if (fmt == NULL)
		return;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	jerConsolePush(buf);
}

void jer_console_clear(void)
{
	sCount = 0;
}

int jer_console_count(void)
{
	return sCount;
}

/* --- on/off + persistence ---------------------------------------------- */
int jer_console_enabled(void)
{
	if (sEnabled < 0)
	{
		sEnabled = jer_config_get_bool(JER_CONSOLE_MOD, JER_CONSOLE_KEY, 1) ? 1 : 0;

		/* once, on first use: the persisted state, for the log (a `~` key change
		 * is logged by jer_console_toggle). */
		jer_log("[jer_console] console %s (from CONFIG/hud.ini)\n", sEnabled ? "ON" : "OFF");
	}

	return sEnabled;
}

void jer_console_set_enabled(int on)
{
	sEnabled = on ? 1 : 0;

	jer_config_set_bool(JER_CONSOLE_MOD, JER_CONSOLE_KEY, sEnabled);
}

int jer_console_toggle(void)
{
	jer_console_set_enabled(!jer_console_enabled());

	jer_log("[jer_console] console %s\n", sEnabled ? "ON" : "OFF");

	return sEnabled;
}

/* --- draw --------------------------------------------------------------- */
void jer_console_draw(int frontend)
{
	CVECTOR ambient;
	int i;

	if (current == NULL)
		return;

	if (!jer_console_enabled() || sCount <= 0)
		return;

	ambient = gFontColour;

	for (i = 0; i < sCount; i++)
	{
		int age = (sCount - 1) - i;	/* 0 = newest */
		int y, c;

		if (frontend)
			y = JER_CONSOLE_FE_BOTTOM - age * JER_CONSOLE_FE_LINE_H;
		else
			y = JER_CONSOLE_BOTTOM - age * JER_CONSOLE_LINE_H;

		/* newest brightest, older ones dimmer so the eye lands on the latest */
		c = (age == 0) ? 255 : (age < 3 ? 190 : 150);

		SetTextColour(c, c, c);

		if (frontend)
		{
			FEPrintStringSized(sLines[i], JER_CONSOLE_FE_X, y, JER_CONSOLE_FE_SCALE, 0, c, c, c);
		}
		else if (gHiresFontTexture)
		{
			PrintStringHiresScaled(sLines[i], JER_CONSOLE_X, y, JER_CONSOLE_TEXT_SCALE);
		}
		else
		{
			PrintString(sLines[i], JER_CONSOLE_X, y);
		}
	}

	/* leave the text colour exactly as it was found (see the file header) */
	gFontColour = ambient;
}
