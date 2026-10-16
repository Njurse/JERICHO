/* jer_text.c — the implementation behind jer_text.h.
 *
 * Engine-side on purpose (it sits beside jer_console.c rather than under
 * JERICHO/src) because it has to call the engine's own printers, which the
 * frontend and pres.c keep file-local and do not export through a header.
 *
 * Both paths are the SAME calls jer_console's feed makes, at the same reference
 * size, so text drawn through here at 100 looks exactly like console text. The
 * only things this file adds are the measure the console never needed (it wraps
 * one string by measured width; it never had to place a column) and the scale,
 * which a table needs to fit five columns where one console line fits one.
 */

#include "driver2.h"
#include "pres.h"
#include "system.h"		// current (the display buffer + OT the text goes into)

#include <string.h>

#include "jericho.h"
#include "jer_text.h"

/* The frontend's printer and its measure (FEmain.c). Declared here rather than
 * in a header because the frontend keeps its printers file-local — the same
 * reason jer_console.c declares FEPrintStringSized. */
extern int FEPrintStringSized(char* string, int x, int y, int scale, int transparent, int r, int g, int b);
extern int FEStringWidth(char* string);

/* pres.c's per-glyph quad (declare it the way jer_console.c and sandbox.c do),
 * and the HQ-font-loaded flag PrintString itself switches on. Read only. */
struct FONT_QUAD
{
	float x0, y0, s0, t0;	// top-left
	float x1, y1, s1, t1;	// bottom-right
};
extern void GetHiresBakedQuadScaled(int char_index, float* xpos, float* ypos, struct FONT_QUAD* q, float scale);
extern unsigned int gHiresFontTexture;

/* The frontend's hires-font-loaded flag, which FEStringWidth/FEPrintStringSized
 * branch on. Read only. */
extern unsigned int gHiresFEFontTexture;

/* --- the 100% reference: what the console draws at -------------------------
 * Half of the FE font's standard 4096 and half of the HQ font's 0.275f, with the
 * console's glyph tracking. A table's percentages are relative to these. */
#define JER_TEXT_FE_SCALE		2048	/* FE scale at 100% */
#define JER_TEXT_FE_UNIT		4096	/* the scale FEStringWidth measures at */
#define JER_TEXT_HQ_SCALE		0.1375f	/* HQ scale at 100% */
#define JER_TEXT_HQ_TRACKING	0.75f	/* the console's glyph spacing, at 100% */
#define JER_TEXT_FALLBACK_CHAR	6		/* px per char when no HQ font is loaded, at 100% */

/* The FE/HQ fonts are drawn with the PSX texture filter on (x2), so a modulation
 * of C lands on screen as 2C and clips. Halve what we pass, exactly as
 * jer_console does, so the colour a caller asks for is the colour seen. */
static int jerTextInk(int v)
{
	if (v <= 0)
		return 0;
	if (v >= 255)
		return 128;		/* 128 * 2 = 256, clipped back to 255 */

	return (v * 128) / 255;
}

/* A percentage a caller may have got wrong (0, negative) falls back to 100 rather
 * than drawing invisible text or dividing by 0. */
static int jerTextPct(int scale_pct)
{
	return (scale_pct > 0) ? scale_pct : JER_TEXT_SCALE_CONSOLE;
}

/* The frontend's scale at a percentage. (Defined before jerTextFeWidth, which
 * needs it.) */
static int jerTextFeScale(int scale_pct)
{
	return (JER_TEXT_FE_SCALE * jerTextPct(scale_pct)) / 100;
}

/* The frontend width, matching FEPrintStringSized's OWN advance rather than a
 * scaled sum of it. That printer advances a SPACE by a fixed 4 UNSCALED and every
 * other glyph by (w * scale) / 4096, so scaling FEStringWidth's total would scale
 * the spaces with it and a centred line would drift 4px per space at any scale
 * but 100. The hires path bakes every glyph including the space, so there the sum
 * scales.
 *
 * (FEStringWidth reports the bitmap sum with that same fixed 4 per space, which is
 * why the spaces come back out before scaling.) */
static int jerTextFeWidth(const char* text, int scale_pct)
{
	const char* p;
	int spaces = 0;
	int w;
	int scale = jerTextFeScale(scale_pct);

	if (gHiresFEFontTexture)
		return (FEStringWidth((char*)text) * scale) / JER_TEXT_FE_UNIT;

	for (p = text; *p != '\0'; p++)
	{
		if (*p == ' ')
			spaces++;
	}

	w = FEStringWidth((char*)text) - 4 * spaces;

	return (w * scale) / JER_TEXT_FE_UNIT + 4 * spaces;
}

static int jerTextHqScale(int scale_pct)
{
	return (JER_TEXT_HQ_SCALE * (float)jerTextPct(scale_pct)) / 100.0f;
}

/* One glyph's advance in the in-game font. Mirrors jerCharWidth in jer_console.c:
 * the HQ font's own width plus the tracking, both scaled. */
static int jerTextCharW(unsigned char ch, int scale_pct)
{
	struct FONT_QUAD q;
	float fx = 0.0f, fy = 0.0f;
	float track = (JER_TEXT_HQ_TRACKING * (float)jerTextPct(scale_pct)) / 100.0f;

	if (!gHiresFontTexture || ch < 32 || ch >= 127)
	{
		int fallback = (JER_TEXT_FALLBACK_CHAR * jerTextPct(scale_pct)) / 100;

		return (fallback > 0) ? fallback : 1;
	}

	GetHiresBakedQuadScaled((int)ch, &fx, &fy, &q, jerTextHqScale(scale_pct));

	return (int)(fx + track + 0.5f);
}

int jer_text_width(int frontend, int scale_pct, const char* text)
{
	int w = 0;

	if (text == NULL || text[0] == '\0')
		return 0;

	if (frontend)
	{
		w = jerTextFeWidth(text, scale_pct);
	}
	else
	{
		const unsigned char* p = (const unsigned char*)text;

		while (*p != '\0')
			w += jerTextCharW(*p++, scale_pct);
	}

	return w;
}

int jer_text_draw_at(int frontend, int scale_pct, int x, int y, const char* text, int r, int g, int b)
{
	int end;

	if (text == NULL || text[0] == '\0' || current == NULL)
		return x;

	r = jerTextInk(r);
	g = jerTextInk(g);
	b = jerTextInk(b);

	SetTextColour((u_char)r, (u_char)g, (u_char)b);

	if (frontend)
	{
		end = FEPrintStringSized((char*)text, x, y, jerTextFeScale(scale_pct), 0, r, g, b);
	}
	else if (gHiresFontTexture)
	{
		float track = (JER_TEXT_HQ_TRACKING * (float)jerTextPct(scale_pct)) / 100.0f;

		end = PrintStringHiresScaledSpaced((char*)text, x, y, jerTextHqScale(scale_pct), track);
	}
	else
	{
		end = PrintString((char*)text, x, y);
	}

	/* A printer that refused (no display buffer) reports -1; keep the caller's x
	 * so a chained column does not run off to nowhere. */
	return (end < 0) ? x : end;
}

int jer_text_draw_centred(int frontend, int scale_pct, int cx, int y, const char* text, int r, int g, int b)
{
	int w = jer_text_width(frontend, scale_pct, text);

	if (w <= 0)
		return 0;

	jer_text_draw_at(frontend, scale_pct, cx - w / 2, y, text, r, g, b);

	return w;
}
