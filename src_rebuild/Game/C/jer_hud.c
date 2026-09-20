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
} JER_HUD_PANEL;

static JER_HUD_PANEL sPanel[JER_HUD_PANEL_MAX];

// y of the first line, and the line spacing. The screen is 320x240 and the
// stock 2D font is 10-12px, so four lines fit above the action.
#define JER_HUD_FIRST_Y		22
#define JER_HUD_LINE_H		16

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

	return slot;
}

void jer_hud_panel_clear(int slot)
{
	if (slot < 0 || slot >= JER_HUD_PANEL_MAX)
		return;

	sPanel[slot].used = 0;
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

			w = StringWidth(p->text);

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

			SetTextColour(p->r, p->g, p->b);
			PrintString(p->text, x, y);
		}
	}

	// leave the text colour exactly as it was found (see the file header)
	gFontColour = ambient;
}
