/* jer_console.c — see JERICHO/include/jer_console.h.
 *
 * One small ring of status lines drawn half-size at the bottom left, in the
 * frontend and in-game, as the single place the mp handshake / join / chat
 * messages land. Engine-side (not in JERICHO/src) for the same reason as
 * jer_hud.c: only the game can draw into the display buffer.
 *
 * A ROW holds its text plus the colour RUNS that make it up, so a chat line can
 * tint the speaker's name while the rest stays plain. A line longer than a row
 * is WRAPPED into several rows at push time (breaking at a space where one is
 * close to the limit), because the two views fit a very different number of
 * pixels per row.
 *
 * The two states use DIFFERENT fonts, so the drawer has two paths:
 *   in-game   PrintStringHiresScaledSpaced at 0.138f (half of the 0.275f
 *             default) plus a little tracking, falling back to the plain PSX
 *             printer when no HQ font is loaded;
 *   frontend  FEPrintStringSized at 2048 (half of the 4096 the frontend's own
 *             FEPrintString uses) -- the frontend never sees the in-game font.
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
/* In-game: bottom-left, half size. The newest row sits at BOTTOM; the mp
 * HOST/CLIENT tag block keeps rows 208..232 below it, and the chat input line
 * (jer_console_input) is the row under that. */
#define JER_CONSOLE_X			6
#define JER_CONSOLE_TEXT_SCALE		0.138f	/* half of the 0.275f default */
#define JER_CONSOLE_LINE_H		11
#define JER_CONSOLE_BOTTOM		196

/* Extra pixels between glyphs. At the half-size scale the HQ font's own side
 * bearings collapse to well under a pixel (measured: a ~4.5 px 'M' advance
 * carried a ~0.6 px gap) and the letters read as jammed together -- about half
 * the gap the default 0.275f text has. This nudges it back to roughly that. */
#define JER_CONSOLE_TRACKING		0.75f

/* Frontend: the FE draws in its own (larger) coordinate space and font. */
#define JER_CONSOLE_FE_X		32
#define JER_CONSOLE_FE_SCALE		2048	/* half of the FE's standard 4096 */
#define JER_CONSOLE_FE_LINE_H		18
#define JER_CONSOLE_FE_BOTTOM		456

/* The typed chat line: the row under the whole HUD block, drawn whether or not
 * the console is on. */
#define JER_CONSOLE_INPUT_Y		232
#define JER_CONSOLE_FE_INPUT_Y		496
#define JER_CONSOLE_INPUT_R		150
#define JER_CONSOLE_INPUT_G		255
#define JER_CONSOLE_INPUT_B		150

/* How many characters fit on one row before it wraps. Both views are "half
 * size" in their own space and fit roughly the same number. */
#define JER_CONSOLE_WRAP		72

/* The flattened text of one push, before wrapping (6 runs x 64). */
#define JER_CONSOLE_FLAT_MAX		(JER_CONSOLE_SEG_MAX * JER_CONSOLE_SEG_TEXT_MAX)

/* --- storage ------------------------------------------------------------ */
typedef struct JER_CONSOLE_RUN
{
	unsigned char off;		/* first char of the run within the row text */
	unsigned char len;		/* chars in the run */
	unsigned char r, g, b;
	unsigned char ambient;		/* 1 = draw in the row's own (age) colour */
} JER_CONSOLE_RUN;

typedef struct JER_CONSOLE_ROW
{
	char text[JER_CONSOLE_LINE_MAX];
	unsigned char runCount;		/* >= 1 for a live row */
	unsigned char chat;		/* 1 = a chat row (drawn even when off) */
	JER_CONSOLE_RUN runs[JER_CONSOLE_SEG_MAX];
} JER_CONSOLE_ROW;

static JER_CONSOLE_ROW sRows[JER_CONSOLE_MAX];
static int  sCount = 0;
static int  sEnabled = -1;		/* -1 = not loaded from the config yet */

static char sInput[JER_CONSOLE_LINE_MAX];
static int  sInputSet = 0;

/* Newest row brightest, older ones dimmer so the eye lands on the latest. */
static int jerConsoleAgeColour(int age)
{
	return (age == 0) ? 255 : (age < 3 ? 190 : 150);
}

/* --- ring --------------------------------------------------------------- */
static void jerConsoleAddRow(const char* text, const unsigned char* r,
	const unsigned char* g, const unsigned char* b, const unsigned char* amb,
	int len, int chat)
{
	JER_CONSOLE_ROW* row;
	int i;

	if (len > JER_CONSOLE_LINE_MAX - 1)
		len = JER_CONSOLE_LINE_MAX - 1;

	if (sCount < JER_CONSOLE_MAX)
	{
		row = &sRows[sCount++];
	}
	else
	{
		/* full: drop the oldest row and make room at the end */
		memmove(&sRows[0], &sRows[1], sizeof(sRows[0]) * (JER_CONSOLE_MAX - 1));
		row = &sRows[JER_CONSOLE_MAX - 1];
	}

	memcpy(row->text, text, (size_t)len);
	row->text[len] = '\0';
	row->chat = (unsigned char)(chat ? 1 : 0);
	row->runCount = 0;

	/* Group consecutive same-colour characters into runs. */
	for (i = 0; i < len; i++)
	{
		if (row->runCount > 0)
		{
			JER_CONSOLE_RUN* p = &row->runs[row->runCount - 1];

			if (p->r == r[i] && p->g == g[i] && p->b == b[i] && p->ambient == (amb[i] ? 1 : 0))
			{
				p->len++;
				continue;
			}
		}

		if (row->runCount >= JER_CONSOLE_SEG_MAX)
		{
			row->runs[row->runCount - 1].len++;	/* no room: extend the last */
			continue;
		}

		{
			JER_CONSOLE_RUN* p = &row->runs[row->runCount++];

			p->off = (unsigned char)i;
			p->len = 1;
			p->r = r[i];
			p->g = g[i];
			p->b = b[i];
			p->ambient = (unsigned char)(amb[i] ? 1 : 0);
		}
	}

	/* Mirror to the log, deliberately: the on-screen stream is then greppable,
	 * exactly like MpConnEvent's lines ("read me what you see" -> one search). */
	jer_log("[console] %s\n", row->text);
}

/* Flatten the runs, then WRAP them into rows at a space near the limit. */
static void jerConsolePush(const JER_CONSOLE_SEG* segs, int count, int chat)
{
	char flat[JER_CONSOLE_FLAT_MAX + 1];
	unsigned char cr[JER_CONSOLE_FLAT_MAX];
	unsigned char cg[JER_CONSOLE_FLAT_MAX];
	unsigned char cb[JER_CONSOLE_FLAT_MAX];
	unsigned char camb[JER_CONSOLE_FLAT_MAX];
	int len = 0;
	int pos, i;

	if (segs == NULL || count <= 0)
		return;

	if (count > JER_CONSOLE_SEG_MAX)
		count = JER_CONSOLE_SEG_MAX;

	for (i = 0; i < count && len < JER_CONSOLE_FLAT_MAX; i++)
	{
		const char* t = segs[i].text;

		if (t == NULL)
			continue;

		for (; *t != '\0' && len < JER_CONSOLE_FLAT_MAX; t++, len++)
		{
			flat[len] = *t;
			cr[len] = segs[i].r;
			cg[len] = segs[i].g;
			cb[len] = segs[i].b;
			camb[len] = (unsigned char)(segs[i].ambient ? 1 : 0);
		}
	}

	if (len == 0)
		return;

	flat[len] = '\0';

	for (pos = 0; pos < len; )
	{
		int take = len - pos;

		if (take > JER_CONSOLE_WRAP)
		{
			int cut = -1;

			take = JER_CONSOLE_WRAP;

			/* prefer breaking at a space in the back half of the row */
			for (i = take; i > JER_CONSOLE_WRAP / 2; i--)
			{
				if (flat[pos + i] == ' ')
				{
					cut = i;
					break;
				}
			}

			if (cut > 0)
				take = cut;
		}

		jerConsoleAddRow(flat + pos, cr + pos, cg + pos, cb + pos, camb + pos, take, chat);

		pos += take;

		while (pos < len && flat[pos] == ' ')	/* swallow the break space */
			pos++;
	}
}

void jer_console_line(const char* text)
{
	JER_CONSOLE_SEG seg;

	if (text == NULL || text[0] == '\0')
		return;

	seg.text = text;
	seg.r = seg.g = seg.b = 0;
	seg.ambient = 1;

	jerConsolePush(&seg, 1, 0);
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

	jer_console_line(buf);
}

void jer_console_chat(const JER_CONSOLE_SEG* segs, int count)
{
	jerConsolePush(segs, count, 1);
}

void jer_console_input(const char* text)
{
	if (text == NULL || text[0] == '\0')
	{
		sInputSet = 0;
		sInput[0] = '\0';
		return;
	}

	strncpy(sInput, text, sizeof(sInput) - 1);
	sInput[sizeof(sInput) - 1] = '\0';
	sInputSet = 1;
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
static void jerConsoleDrawRow(const JER_CONSOLE_ROW* row, int frontend, int y, int age)
{
	char tmp[JER_CONSOLE_LINE_MAX];
	int base = jerConsoleAgeColour(age);
	int x = frontend ? JER_CONSOLE_FE_X : JER_CONSOLE_X;
	int r;

	for (r = 0; r < row->runCount; r++)
	{
		const JER_CONSOLE_RUN* run = &row->runs[r];
		int len = run->len;

		if (len > JER_CONSOLE_LINE_MAX - 1)
			len = JER_CONSOLE_LINE_MAX - 1;

		memcpy(tmp, row->text + run->off, (size_t)len);
		tmp[len] = '\0';

		if (run->ambient)
			SetTextColour((u_char)base, (u_char)base, (u_char)base);
		else
			SetTextColour(run->r, run->g, run->b);

		if (frontend)
			x = FEPrintStringSized(tmp, x, y, JER_CONSOLE_FE_SCALE, 0,
				gFontColour.r, gFontColour.g, gFontColour.b);
		else if (gHiresFontTexture)
			x = PrintStringHiresScaledSpaced(tmp, x, y, JER_CONSOLE_TEXT_SCALE, JER_CONSOLE_TRACKING);
		else
			x = PrintString(tmp, x, y);
	}
}

static void jerConsoleDrawInput(int frontend)
{
	SetTextColour(JER_CONSOLE_INPUT_R, JER_CONSOLE_INPUT_G, JER_CONSOLE_INPUT_B);

	if (frontend)
		FEPrintStringSized(sInput, JER_CONSOLE_FE_X, JER_CONSOLE_FE_INPUT_Y, JER_CONSOLE_FE_SCALE, 0,
			JER_CONSOLE_INPUT_R, JER_CONSOLE_INPUT_G, JER_CONSOLE_INPUT_B);
	else if (gHiresFontTexture)
		PrintStringHiresScaledSpaced(sInput, JER_CONSOLE_X, JER_CONSOLE_INPUT_Y, JER_CONSOLE_TEXT_SCALE, JER_CONSOLE_TRACKING);
	else
		PrintString(sInput, JER_CONSOLE_X, JER_CONSOLE_INPUT_Y);
}

void jer_console_draw(int frontend)
{
	CVECTOR ambient;
	int enabled;
	int placed = 0;
	int i;

	if (current == NULL)
		return;

	enabled = jer_console_enabled();

	ambient = gFontColour;

	/* Newest first, bottom-up. A chat row draws even while the console is off:
	 * it is a conversation, not debug output. */
	for (i = sCount - 1; i >= 0; i--)
	{
		int y;

		if (!enabled && !sRows[i].chat)
			continue;

		if (frontend)
			y = JER_CONSOLE_FE_BOTTOM - placed * JER_CONSOLE_FE_LINE_H;
		else
			y = JER_CONSOLE_BOTTOM - placed * JER_CONSOLE_LINE_H;

		jerConsoleDrawRow(&sRows[i], frontend, y, placed);
		placed++;
	}

	if (sInputSet)
		jerConsoleDrawInput(frontend);

	/* leave the text colour exactly as it was found (see the file header) */
	gFontColour = ambient;
}
