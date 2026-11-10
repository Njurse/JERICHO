/*
 * jerai_menu.c -- the in-game control menu.
 *
 * A jer_menu list drawn over the game (JER_EVENT_DRAW_OVERLAY) and driven by the
 * PHYSICAL pad read in the frame hook - deliberately not the injected pad, so the
 * player can work the menu while JerAI is driving the car: the car gets the
 * synthetic pad, the menu gets the real one, and they never fight.
 *
 * Rows: the behaviors (select one), the four visualization layers (toggle), a
 * one-shot "dump the road graph", and Close. Every change is written straight to
 * the config, so it survives a reload and the .ini always reflects the state.
 *
 * Opened/closed with L2 (the look-left shoulder), and only while JerAI is
 * enabled. While it is open the keys it reads (D-pad up/down, Cross, Triangle)
 * are stripped from the live car pad (JerAiMenuPadMask, applied in jerai.c's
 * CAR_PAD hook), so working the menu in Manual does not also steer, accelerate,
 * handbrake or leave the car. A key that closed the menu stays stripped until it
 * is released, so the closing Triangle never reaches the car as a handbrake edge.
 * L2 itself is still the camera's look-left: that is read by the camera, not
 * the car pad, and is out of this module's reach.
 */

#include "jerai_internal.h"
#include "jerai_drive.h"
#include "jerai_behavior.h"

#include "jer_menu.h"
#include "jer_config.h"
#include "jer_events.h"
#include "pad.h"

#include <stdio.h>

#define JERAI_MENU_KEY		MPAD_L2

/* every key the open menu reads: kept off the car while it is open */
#define JERAI_MENU_PAD_KEYS	(MPAD_D_UP | MPAD_D_DOWN | MPAD_CROSS | MPAD_TRIANGLE)
#define JERAI_MENU_MAX_ROWS	16
#define JERAI_MENU_LABEL	48

static JerMenu sMenu;
static char sLabels[JERAI_MENU_MAX_ROWS][JERAI_MENU_LABEL];
static const char* sItems[JERAI_MENU_MAX_ROWS];
static int sRows;
static int sOpen;
static int sSwallow;	/* menu keys still held since the menu closed: kept off the car */

/* the four layers, in row order after the behaviors */
static const char* const sLayerKey[4] =
{
	JERAI_CFG_VIZ_ROAD, JERAI_CFG_VIZ_GRID, JERAI_CFG_VIZ_PATH, JERAI_CFG_VIZ_HUD
};
static const char* const sLayerName[4] = { "Road graph", "Grid", "Path", "HUD" };

static int* JerAiLayerField(int i)
{
	switch (i)
	{
	case 0: return &gJerAi.vizRoad;
	case 1: return &gJerAi.vizGrid;
	case 2: return &gJerAi.vizPath;
	default: return &gJerAi.vizHud;
	}
}

static void JerAiMenuRebuild(void)
{
	int i;
	int n = JerAiBehaviorCount();

	sRows = 0;

	for (i = 0; i < n && sRows < JERAI_MENU_MAX_ROWS; i++)
	{
		snprintf(sLabels[sRows], JERAI_MENU_LABEL, "%s %s",
			JerAiBehaviorIdAt(i) == gJerAi.mode ? ">" : " ", JerAiBehaviorNameAt(i));
		sItems[sRows] = sLabels[sRows];
		sRows++;
	}

	for (i = 0; i < 4 && sRows < JERAI_MENU_MAX_ROWS; i++)
	{
		snprintf(sLabels[sRows], JERAI_MENU_LABEL, "%s: %s",
			sLayerName[i], *JerAiLayerField(i) ? "ON" : "off");
		sItems[sRows] = sLabels[sRows];
		sRows++;
	}

	snprintf(sLabels[sRows], JERAI_MENU_LABEL, "Immunity: %s", gJerAi.immune ? "ON" : "off");
	sItems[sRows] = sLabels[sRows];
	sRows++;

	/* one statement per step: `sItems[sRows++] = sLabels[sRows - 1]` is
	 * unsequenced, and the build's compilers evaluated it shifted by a row */
	snprintf(sLabels[sRows], JERAI_MENU_LABEL, "Dump road graph now");
	sItems[sRows] = sLabels[sRows];
	sRows++;

	snprintf(sLabels[sRows], JERAI_MENU_LABEL, "Close");
	sItems[sRows] = sLabels[sRows];
	sRows++;
}

/* Close, and keep whatever menu key is still down off the car until it is let go. */
static void JerAiMenuClose(void)
{
	if (!sOpen)
		return;

	sOpen = 0;
	sSwallow = Pads[0].mapped & JERAI_MENU_PAD_KEYS;
	jer_menu_end(&sMenu);
}

static void JerAiMenuConfirm(int row)
{
	int n = JerAiBehaviorCount();

	if (row < 0 || row >= sRows)
		return;

	if (row < n)
	{
		gJerAi.mode = JerAiBehaviorIdAt(row);
		jer_config_set_int("JerAI", JERAI_CFG_MODE, gJerAi.mode);
	}
	else if (row < n + 4)
	{
		int i = row - n;
		int* field = JerAiLayerField(i);

		*field = !*field;
		jer_config_set_int("JerAI", sLayerKey[i], *field);
	}
	else if (row == n + 4)
	{
		gJerAi.immune = !gJerAi.immune;
		jer_config_set_int("JerAI", JERAI_CFG_IMMUNE, gJerAi.immune);
	}
	else if (row == n + 5)
	{
		gJerAi.dumpRequested = 1;
	}
	else
	{
		JerAiMenuClose();
		return;
	}

	JerAiMenuRebuild();
}

static void JerAiMenuOpenNow(void)
{
	JerAiMenuRebuild();
	jer_menu_list(&sMenu, "JerAI", (const char* const*)sItems, sRows);
	sOpen = 1;

	gJerAi.ctx->jer_log(gJerAi.ctx, "[JerAI] menu open: %d rows (mode=%d road=%d grid=%d path=%d hud=%d)\n",
		sRows, gJerAi.mode, gJerAi.vizRoad, gJerAi.vizGrid, gJerAi.vizPath, gJerAi.vizHud);
}

void JerAiMenuForceOpen(void)
{
	JerAiMenuOpenNow();
}

void JerAiMenuTick(void)
{
	int cur = Pads[0].mapped;
	int newp = Pads[0].mapnew;

	/* a released key is no longer swallowed */
	sSwallow &= cur;

	/* the menu belongs to an enabled JerAI: closed (and unopenable) otherwise */
	if (!gJerAi.enabled)
	{
		JerAiMenuClose();
		return;
	}

	if (newp & JERAI_MENU_KEY)
	{
		if (sOpen)
			JerAiMenuClose();
		else
			JerAiMenuOpenNow();

		return;
	}

	if (!sOpen)
		return;

	if (newp & MPAD_TRIANGLE)
	{
		JerAiMenuClose();
		return;
	}

	if (jer_menu_tick(&sMenu, MPAD_D_UP, MPAD_D_DOWN, 0, 0, MPAD_CROSS, cur, newp))
		JerAiMenuConfirm(sMenu.cursor);
}

unsigned int JerAiMenuPadMask(void)
{
	return sOpen ? (unsigned int)JERAI_MENU_PAD_KEYS : (unsigned int)sSwallow;
}

int JerAiOnDrawOverlay(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	if (!sOpen)
		return JER_RESULT_CONTINUE;

	jer_menu_draw(&sMenu, 160, 90);

	return JER_RESULT_CONTINUE;
}
