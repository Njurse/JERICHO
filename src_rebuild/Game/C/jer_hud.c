/* jer_hud.c — see JERICHO/include/jer_hud.h.
 *
 * Engine-side (not in JERICHO/src) because drawing needs the game's display
 * buffer and its 2D text printer, exactly like jer_npc.c needs the ped types.
 */

#include "driver2.h"
#include "pres.h"

#include "jericho.h"
#include "jer_hud.h"

typedef struct JER_HUD_MSG
{
	char text[JER_HUD_TEXT_MAX];
	int frames;		// frames left on screen; <= 0 means the slot is free
} JER_HUD_MSG;

static JER_HUD_MSG sHud[JER_HUD_MAX];

// y of the first line, and the line spacing. The screen is 320x240 and the
// stock 2D font is 10-12px, so four lines fit above the action.
#define JER_HUD_FIRST_Y		22
#define JER_HUD_LINE_H		16

static void jerHudSetText(char* dst, const char* src)
{
	int i = 0;

	if (src != NULL)
	{
		while (src[i] != '\0' && i < JER_HUD_TEXT_MAX - 1)
		{
			dst[i] = src[i];
			i++;
		}
	}

	dst[i] = '\0';
}

int jer_hud_message(const char* text, int frames)
{
	int i, slot = -1;

	if (text == NULL || text[0] == '\0')
		return -1;

	if (frames <= 0)
		frames = JER_HUD_DEFAULT_FRAMES;

	// a free slot, else the one with the least time left (oldest)
	for (i = 0; i < JER_HUD_MAX; i++)
	{
		if (sHud[i].frames <= 0)
		{
			slot = i;
			break;
		}

		if (slot < 0 || sHud[i].frames < sHud[slot].frames)
			slot = i;
	}

	jerHudSetText(sHud[slot].text, text);
	sHud[slot].frames = frames;

	jer_log("[jer_hud] %s (%d frames)\n", sHud[slot].text, frames);

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

void jer_hud_draw(void)
{
	int i;

	for (i = 0; i < JER_HUD_MAX; i++)
	{
		if (sHud[i].frames <= 0)
			continue;

		// PrintStringCentred centres the line itself, so this needs no
		// assumption about the 2D coordinate space.
		PrintStringCentred(sHud[i].text, (short)(JER_HUD_FIRST_Y + i * JER_HUD_LINE_H));

		sHud[i].frames--;
	}
}
