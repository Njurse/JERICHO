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
 * Opened/closed with L2 (the look-left shoulder); the car ignores injected
 * shoulder buttons, so it does not interfere with driving.
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
#define JERAI_MENU_MAX_ROWS	16
#define JERAI_MENU_LABEL	48

static JerMenu sMenu;
static char sLabels[JERAI_MENU_MAX_ROWS][JERAI_MENU_LABEL];
static const char* sItems[JERAI_MENU_MAX_ROWS];
static int sRows;
static int sOpen;

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

	snprintf(sLabels[sRows], JERAI_MENU_LABEL, "Dump road graph now");
	sItems[sRows++] = sLabels[sRows - 1];

	snprintf(sLabels[sRows], JERAI_MENU_LABEL, "Close");
	sItems[sRows++] = sLabels[sRows - 1];
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
		gJerAi.dumpRequested = 1;
	}
	else
	{
		sOpen = 0;
		jer_menu_end(&sMenu);
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

	if (newp & JERAI_MENU_KEY)
	{
		if (sOpen)
		{
			sOpen = 0;
			jer_menu_end(&sMenu);
		}
		else
		{
			JerAiMenuOpenNow();
		}

		return;
	}

	if (!sOpen)
		return;

	if (newp & MPAD_TRIANGLE)
	{
		sOpen = 0;
		jer_menu_end(&sMenu);
		return;
	}

	if (jer_menu_tick(&sMenu, MPAD_D_UP, MPAD_D_DOWN, 0, 0, MPAD_CROSS, cur, newp))
		JerAiMenuConfirm(sMenu.cursor);
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
