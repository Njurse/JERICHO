/* jer_hud.c — see JERICHO/include/jer_hud.h.
 *
 * Engine-side (not in JERICHO/src) because drawing needs the game's display
 * buffer and its 2D text printer, exactly like jer_npc.c needs the ped types.
 *
 * A message is a LINE made of one or more COLOUR RUNS. jer_hud_message is the
 * single-run case and draws in the ambient text colour, exactly as this file
 * always did; jer_hud_message_segs is for a line that has to name something in
 * its own colour (cainescrossfire's faction names in the kill banner), and a run can
 * still ask for the ambient colour so the surrounding wording keeps matching
 * every other HUD line.
 *
 * PrintString stamps the ambient gFontColour into every glyph and SetTextColour
 * is global state, so this drawer SAVES the colour on entry and puts it back on
 * exit: JER_EVENT_DRAW_OVERLAY fires immediately after jer_hud_draw, and the
 * module overlays it draws (cainescrossfire's AI readout, jer_error's notices) expect
 * the colour they left behind.
 */

#include "driver2.h"
#include "pres.h"
#include "system.h"	// DB* current (the display buffer + OT the HUD draws into)

#include "jericho.h"
#include "jer_hud.h"

typedef struct JER_HUD_RUN
{
	char text[JER_HUD_SEG_TEXT_MAX];
	unsigned char r, g, b;
	int ambient;				// 1 = draw in the ambient colour
} JER_HUD_RUN;

typedef struct JER_HUD_MSG
{
	char text[JER_HUD_TEXT_MAX];		// the whole line, flattened (logs)
	JER_HUD_RUN runs[JER_HUD_SEG_MAX];
	int runCount;				// >= 1 for a live message
	int frames;				// frames left on screen; <= 0 = free
} JER_HUD_MSG;

static JER_HUD_MSG sHud[JER_HUD_MAX];

// Anchored persistent lines (jer_hud_panel). Unlike sHud these do not age: they
// are drawn every frame until cleared.
typedef struct JER_HUD_PANEL
{
	char text[JER_HUD_TEXT_MAX];
	int anchor;			// JER_HUD_ANCHOR_*
	int r, g, b;
	int used;
	int bar;			// 1 = draw a meter instead of a line of text
	int barValue, barMax;		// the meter's fraction (barValue / barMax)
} JER_HUD_PANEL;

static JER_HUD_PANEL sPanel[JER_HUD_PANEL_MAX];

// y of the first line, and the line spacing. The screen is 320x240 and the
// stock 2D font is 10-12px, so four lines fit above the action.
#define JER_HUD_FIRST_Y		22
#define JER_HUD_LINE_H		16

// Panel bars (jer_hud_panel_bar). Deliberately SMALL: a thin meter that sits
// on the line under a lock-on name, not a full-width stock percentage bar.
#define JER_HUD_BAR_W		56
#define JER_HUD_BAR_H		4

// Screen width, for centring. PrintStringCentred uses the same 320.
#define JER_HUD_SCREEN_W	320

static void jerHudSetText(char* dst, const char* src, int max)
{
	int i = 0;

	if (src != NULL)
	{
		while (src[i] != '\0' && i < max - 1)
		{
			dst[i] = src[i];
			i++;
		}
	}

	dst[i] = '\0';
}

// A free slot, else the one with the least time left (oldest).
static int jerHudPickSlot(void)
{
	int i, slot = 0;

	for (i = 0; i < JER_HUD_MAX; i++)
	{
		if (sHud[i].frames <= 0)
			return i;

		if (sHud[i].frames < sHud[slot].frames)
			slot = i;
	}

	return slot;
}

static int jerHudResolveFrames(int frames)
{
	return (frames <= 0) ? JER_HUD_DEFAULT_FRAMES : frames;
}

int jer_hud_message(const char* text, int frames)
{
	int slot;

	if (text == NULL || text[0] == '\0')
		return -1;

	slot = jerHudPickSlot();

	jerHudSetText(sHud[slot].text, text, JER_HUD_TEXT_MAX);

	// one run, in whatever colour is ambient at draw time: the behaviour this
	// API has always had
	jerHudSetText(sHud[slot].runs[0].text, text, JER_HUD_SEG_TEXT_MAX);
	sHud[slot].runs[0].r = 0;
	sHud[slot].runs[0].g = 0;
	sHud[slot].runs[0].b = 0;
	sHud[slot].runs[0].ambient = 1;
	sHud[slot].runCount = 1;
	sHud[slot].frames = jerHudResolveFrames(frames);

	jer_log("[jer_hud] %s (%d frames)\n", sHud[slot].text, sHud[slot].frames);

	return slot;
}

int jer_hud_message_segs(const JER_HUD_SEG* segs, int count, int frames)
{
	JER_HUD_MSG* m;
	int i, n = 0, len = 0, slot;

	if (segs == NULL || count <= 0)
		return -1;

	slot = jerHudPickSlot();
	m = &sHud[slot];
	m->text[0] = '\0';
	m->runCount = 0;

	for (i = 0; i < count && n < JER_HUD_SEG_MAX; i++)
	{
		const char* src = (segs[i].text != NULL) ? segs[i].text : "";
		JER_HUD_RUN* run = &m->runs[n];
		int j;

		if (src[0] == '\0')
			continue;		// an empty run has nothing to draw

		jerHudSetText(run->text, src, JER_HUD_SEG_TEXT_MAX);
		run->r = segs[i].r;
		run->g = segs[i].g;
		run->b = segs[i].b;
		run->ambient = segs[i].ambient ? 1 : 0;
		n++;

		// the flattened copy: the log line, and the whole message's length
		for (j = 0; run->text[j] != '\0' && len < JER_HUD_TEXT_MAX - 1; j++)
			m->text[len++] = run->text[j];
	}

	m->text[len] = '\0';

	if (n == 0)
	{
		m->frames = 0;			// nothing to draw
		return -1;
	}

	m->runCount = n;
	m->frames = jerHudResolveFrames(frames);

	jer_log("[jer_hud] %s (%d runs, %d frames)\n", m->text, n, m->frames);

	return slot;
}

int jer_hud_message_replace(const char* text, int frames)
{
	jer_hud_clear();

	return jer_hud_message(text, frames);
}

void jer_hud_clear(void)
{
	int i;

	for (i = 0; i < JER_HUD_MAX; i++)
	{
		sHud[i].frames = 0;
		sHud[i].runCount = 0;
		sHud[i].text[0] = '\0';
	}
}

int jer_hud_active(void)
{
	int i, n = 0;

	for (i = 0; i < JER_HUD_MAX; i++)
	{
		if (sHud[i].frames > 0)
			n++;
	}

	return n;
}

// Centre the assembled line (the runs concatenated) exactly as
// PrintStringCentred would, then stamp each run in its own colour.
static void jerHudDrawRuns(JER_HUD_MSG* m, short y, const CVECTOR* ambient)
{
	int i, x, total = 0;

	for (i = 0; i < m->runCount; i++)
		total += StringWidth(m->runs[i].text);

	x = (JER_HUD_SCREEN_W - total) / 2;

	for (i = 0; i < m->runCount; i++)
	{
		JER_HUD_RUN* run = &m->runs[i];

		if (run->ambient)
			// the same ambient colour the plain single-run path uses
			SetTextColour(ambient->r, ambient->g, ambient->b);
		else
			SetTextColour(run->r, run->g, run->b);

		PrintString(run->text, x, y);
		x += StringWidth(run->text);
	}
}

// A small 2D filled meter: a dark track plus a coloured fill of value/max.
// Added to the same OT bucket the stock percentage bars use (current->ot + 1),
// one bucket behind the text so a name drawn over it still wins.
static void jerHudDrawBar(short x, short y, int value, int max, int r, int g, int b)
{
	POLY_G4* poly;
	int fillW, x1, y1;

	x1 = (short)(x + JER_HUD_BAR_W);
	y1 = (short)(y + JER_HUD_BAR_H);

	poly = (POLY_G4*)current->primptr;
	setPolyG4(poly);
	setSemiTrans(poly, 1);
	poly->r0 = poly->r1 = poly->r2 = poly->r3 = 0;
	poly->g0 = poly->g1 = poly->g2 = poly->g3 = 0;
	poly->b0 = poly->b1 = poly->b2 = poly->b3 = 0;
	poly->x0 = poly->x2 = x;
	poly->x1 = poly->x3 = x1;
	poly->y0 = poly->y1 = y;
	poly->y2 = poly->y3 = y1;
	addPrim(current->ot + 1, poly);
	current->primptr += sizeof(POLY_G4);

	if (max <= 0 || value <= 0)
		return;

	if (value > max)
		value = max;

	fillW = (JER_HUD_BAR_W * value) / max;

	if (fillW <= 0)
		return;

	poly = (POLY_G4*)current->primptr;
	setPolyG4(poly);
	poly->r0 = poly->r1 = poly->r2 = poly->r3 = r;
	poly->g0 = poly->g1 = poly->g2 = poly->g3 = g;
	poly->b0 = poly->b1 = poly->b2 = poly->b3 = b;
	poly->x0 = poly->x2 = x;
	poly->x1 = poly->x3 = (short)(x + fillW);
	poly->y0 = poly->y1 = y;
	poly->y2 = poly->y3 = y1;
	addPrim(current->ot + 1, poly);
	current->primptr += sizeof(POLY_G4);
}

// ---------------------------------------------------------------------------
// panels
// ---------------------------------------------------------------------------
int jer_hud_panel(int slot, int anchor, const char* text, int r, int g, int b)
{
	JER_HUD_PANEL* p;

	if (slot < 0 || slot >= JER_HUD_PANEL_MAX)
		return -1;

	if (text == NULL || text[0] == '\0')
	{
		jer_hud_panel_clear(slot);
		return -1;
	}

	p = &sPanel[slot];

	jerHudSetText(p->text, text, JER_HUD_TEXT_MAX);

	p->anchor = anchor;
	p->r = r < 0 ? 0 : (r > 255 ? 255 : r);
	p->g = g < 0 ? 0 : (g > 255 ? 255 : g);
	p->b = b < 0 ? 0 : (b > 255 ? 255 : b);
	p->used = 1;
	p->bar = 0;

	return slot;
}

// A panel slot drawn as a small filled METER rather than a line of text - the
// health bar under a lock-on name. `value/barMax` is the fraction shown; the
// empty part is a dark semi-transparent track so it reads over any scenery.
// Call it every frame while the subject exists (like jer_hud_panel); clear the
// slot (jer_hud_panel_clear, or barMax <= 0) when it does not.
int jer_hud_panel_bar(int slot, int anchor, int value, int max, int r, int g, int b)
{
	JER_HUD_PANEL* p;

	if (slot < 0 || slot >= JER_HUD_PANEL_MAX)
		return -1;

	if (max <= 0)
	{
		jer_hud_panel_clear(slot);
		return -1;
	}

	p = &sPanel[slot];

	p->text[0] = '\0';
	p->anchor = anchor;
	p->r = r < 0 ? 0 : (r > 255 ? 255 : r);
	p->g = g < 0 ? 0 : (g > 255 ? 255 : g);
	p->b = b < 0 ? 0 : (b > 255 ? 255 : b);
	p->used = 1;
	p->bar = 1;
	p->barValue = value < 0 ? 0 : value;
	p->barMax = max;

	return slot;
}

void jer_hud_panel_clear(int slot)
{
	if (slot < 0 || slot >= JER_HUD_PANEL_MAX)
		return;

	sPanel[slot].used = 0;
	sPanel[slot].bar = 0;
	sPanel[slot].text[0] = '\0';
}

void jer_hud_draw(void)
{
	CVECTOR ambient = gFontColour;
	int i;

	for (i = 0; i < JER_HUD_MAX; i++)
	{
		if (sHud[i].frames <= 0)
			continue;

		jerHudDrawRuns(&sHud[i], (short)(JER_HUD_FIRST_Y + i * JER_HUD_LINE_H), &ambient);

		sHud[i].frames--;
	}

	// panels last, so a readout sits over the messages rather than under them.
	// They are stacked per anchor in slot order, and the text colour is saved
	// and restored with the messages' (see the file header).
	{
		short stacked[3];

		stacked[0] = stacked[1] = stacked[2] = 0;

		for (i = 0; i < JER_HUD_PANEL_MAX; i++)
		{
			JER_HUD_PANEL* p = &sPanel[i];
			short x, y;
			int anchor, w;

			if (!p->used)
				continue;

			anchor = p->anchor;

			if (anchor < 0 || anchor >= 3)
				anchor = JER_HUD_ANCHOR_TOP_LEFT;

			w = p->bar ? JER_HUD_BAR_W : StringWidth(p->text);

			switch (anchor)
			{
				case JER_HUD_ANCHOR_TOP_RIGHT:
					x = (short)(JER_HUD_SCREEN_W - 8 - w);
					break;

				case JER_HUD_ANCHOR_TOP_CENTRE:
					x = (short)((JER_HUD_SCREEN_W - w) / 2);
					break;

				default:
					x = 8;
					break;
			}

			if (x < 0)
				x = 0;

			y = (short)(JER_HUD_FIRST_Y + stacked[anchor] * JER_HUD_LINE_H);
			stacked[anchor]++;

			if (p->bar)
			{
				jerHudDrawBar(x, y, p->barValue, p->barMax, p->r, p->g, p->b);
			}
			else
			{
				SetTextColour(p->r, p->g, p->b);
				PrintString(p->text, x, y);
			}
		}
	}

	// leave the text colour exactly as it was found (see the file header)
	gFontColour = ambient;
}
