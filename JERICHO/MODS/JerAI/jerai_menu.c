/*
 * jerai_menu.c -- JerAI's settings, as a page of the game's own pause menu.
 *
 * The page is registered with jer_pause_menu.h like every other module page
 * (Continue -> JERICHO Addons -> JerAI), and L2 opens the pause straight on it
 * (jer_pause_menu_open), only while JerAI is enabled. Triangle or Start on the
 * page closes the pause again; the Evade settings are a submenu of it.
 *
 * The pause stops the game, so the car gets no input at all while the page is
 * open. The one thing left to guard is the key that CLOSED it: Cross on Close,
 * Triangle or Start would otherwise reach the car on the first frame back (a
 * Triangle edge leaves the car, a handbrake edge cycles the control mode). Those
 * keys are stripped from the car pad until they are released
 * (JerAiMenuPadMask, applied in jerai.c's CAR_PAD hook).
 *
 * Every row writes its value straight to JerAI.ini through jer_config, so a
 * change survives a reload and the .ini always shows the live state.
 *
 * L2 itself is still the camera's look-left (and Caine's Crossfire's gun): the
 * camera reads it outside the car-pad hook, out of this module's reach.
 */

#include "jerai_internal.h"
#include "jerai_drive.h"
#include "jerai_behavior.h"

#include "jer_pause_menu.h"
#include "jer_config.h"
#include "jer_events.h"
#include "pad.h"
#include "pause.h"	/* pauseflag */

#include <stdio.h>

#define JERAI_MENU_KEY		MPAD_L2

/* the page's stable ids (jer_pause_menu_open, jer_pause_menu_add_items) */
#define JERAI_PAGE_ID		"jerai"
#define JERAI_EVADE_PAGE_ID	"jerai.evade"

/* keys that can close the pause from this page: kept off the car until released */
#define JERAI_MENU_CLOSE_KEYS	(MPAD_CROSS | MPAD_TRIANGLE | MPAD_START | MPAD_L2)

/* steer_trace when the row is switched on with Cross: the rate the stats tool
 * suggests (tools/jerai_steer_stats.py) */
#define JERAI_TRACE_DEFAULT	3
#define JERAI_TRACE_MAX		30

static int sPauseAsked;	/* we opened the pause and have not seen it close yet */
static int sSwallow;	/* close keys still held since the pause closed: kept off the car */

static void JerAiSetInt(const char* key, int* field, int value)
{
	*field = value;
	jer_config_set_int("JerAI", key, value);
}

static const char* JerAiOnOff(int v)
{
	return v ? "On" : "Off";
}

static int JerAiClamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------ */
/* main page rows                                                      */
/* ------------------------------------------------------------------ */

static int JerAiBehaviorIndex(void)
{
	int i, n = JerAiBehaviorCount();

	for (i = 0; i < n; i++)
	{
		if (JerAiBehaviorIdAt(i) == gJerAi.mode)
			return i;
	}

	return 0;
}

static void LabelBehavior(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Behavior: < %s >", JerAiBehaviorName(gJerAi.mode));
}

static int ActBehavior(void* ud, int dir)
{
	int n = JerAiBehaviorCount();
	int i = JerAiBehaviorIndex() + (dir < 0 ? -1 : 1);	/* Cross steps forward */

	(void)ud;

	if (n <= 0)
		return JER_PAUSE_QUIT_NONE;

	i = (i % n + n) % n;
	JerAiSetInt(JERAI_CFG_MODE, &gJerAi.mode, JerAiBehaviorIdAt(i));
	JerAiLogState("menu");

	return JER_PAUSE_QUIT_NONE;
}

#define JERAI_TOGGLE_ROW(fn, text, key, field)					\
	static void Label##fn(void* ud, char* out, int max)			\
	{									\
		(void)ud;							\
		snprintf(out, max, "%s: %s", text, JerAiOnOff(gJerAi.field));	\
	}									\
	static int Act##fn(void* ud, int dir)					\
	{									\
		(void)ud; (void)dir;						\
		JerAiSetInt(key, &gJerAi.field, !gJerAi.field);			\
		return JER_PAUSE_QUIT_NONE;					\
	}

JERAI_TOGGLE_ROW(Immune, "Immunity", JERAI_CFG_IMMUNE, immune)
JERAI_TOGGLE_ROW(Follow, "Follow players", JERAI_CFG_FOLLOW, followEnabled)
JERAI_TOGGLE_ROW(Flee, "Flee players", JERAI_CFG_FLEE, fleeEnabled)
JERAI_TOGGLE_ROW(Road, "Road view", JERAI_CFG_VIZ_ROAD, vizRoad)
JERAI_TOGGLE_ROW(Path, "Path view", JERAI_CFG_VIZ_PATH, vizPath)
JERAI_TOGGLE_ROW(Hud, "HUD lines", JERAI_CFG_VIZ_HUD, vizHud)

static void LabelTrace(void* ud, char* out, int max)
{
	(void)ud;

	if (gJerAi.steerTrace > 0)
		snprintf(out, max, "Steer trace: every %d frame%s", gJerAi.steerTrace,
			gJerAi.steerTrace == 1 ? "" : "s");
	else
		snprintf(out, max, "Steer trace: Off");
}

/* Cross switches it on at the stats tool's rate or off; left/right set the rate */
static int ActTrace(void* ud, int dir)
{
	int v;

	(void)ud;

	if (dir == 0)
		v = gJerAi.steerTrace > 0 ? 0 : JERAI_TRACE_DEFAULT;
	else
		v = JerAiClamp(gJerAi.steerTrace + dir, 0, JERAI_TRACE_MAX);

	JerAiSetInt(JERAI_CFG_STEER_TRACE, &gJerAi.steerTrace, v);

	return JER_PAUSE_QUIT_NONE;
}

static void LabelDump(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, gJerAi.dumpRequested ? "Dump road graph: queued" : "Dump road graph");
}

static int ActDump(void* ud, int dir)
{
	(void)ud; (void)dir;
	gJerAi.dumpRequested = 1;	/* serviced by the frame hook once the game runs */
	return JER_PAUSE_QUIT_NONE;
}

static int ActClose(void* ud, int dir)
{
	(void)ud; (void)dir;
	return JER_PAUSE_QUIT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Evade settings rows                                                 */
/* ------------------------------------------------------------------ */

static void LabelPace(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Pace: %+d%% over limit", gJerAi.pacePct);
}

static int ActPace(void* ud, int dir)
{
	(void)ud;
	JerAiSetInt(JERAI_CFG_PACE_PCT, &gJerAi.pacePct,
		JerAiClamp(gJerAi.pacePct + (dir < 0 ? -5 : 5), 0, 100));
	return JER_PAUSE_QUIT_NONE;
}

static void LabelLights(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Red lights: %s", gJerAi.runLights ? "Run" : "Stop");
}

static int ActLights(void* ud, int dir)
{
	(void)ud; (void)dir;
	JerAiSetInt(JERAI_CFG_RUN_LIGHTS, &gJerAi.runLights, !gJerAi.runLights);
	return JER_PAUSE_QUIT_NONE;
}

static void LabelClearance(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Cop clearance: %d", gJerAi.copClearance);
}

static int ActClearance(void* ud, int dir)
{
	(void)ud;
	JerAiSetInt(JERAI_CFG_COP_CLEARANCE, &gJerAi.copClearance,
		JerAiClamp(gJerAi.copClearance + (dir < 0 ? -500 : 500), 1000, 20000));
	return JER_PAUSE_QUIT_NONE;
}

/* 352 and up is the engine's own lock (jerai.h JERAI_CFG_MAX_STEER), i.e. no cap */
static void LabelSteer(void* ud, char* out, int max)
{
	(void)ud;

	if (gJerAi.maxSteer >= 352)
		snprintf(out, max, "Max steer: no cap");
	else
		snprintf(out, max, "Max steer: %d", gJerAi.maxSteer);
}

static int ActSteer(void* ud, int dir)
{
	int v = gJerAi.maxSteer >= 352 ? 352 : gJerAi.maxSteer;

	(void)ud;
	v = JerAiClamp(v + (dir < 0 ? -32 : 32), 64, 352);
	JerAiSetInt(JERAI_CFG_MAX_STEER, &gJerAi.maxSteer, v >= 352 ? 512 : v);	/* 512 = the shipped "no cap" */

	return JER_PAUSE_QUIT_NONE;
}

/* a readout: the limit of the road under the car, and the cruise Evade makes of it
 * (jerai_behavior.c: roadLimit * (100 + pace_pct) / 100) */
static void LabelRoadLimit(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Road limit: %d, cruise %d", gJerAi.roadLimit,
		gJerAi.roadLimit * (100 + gJerAi.pacePct) / 100);
}

static int ActBack(void* ud, int dir)
{
	(void)ud; (void)dir;
	return JER_PAUSE_QUIT_BACKMENU;
}

/* ------------------------------------------------------------------ */
/* the pages                                                           */
/* ------------------------------------------------------------------ */

static const JER_PAUSE_MENU_ITEM sEvadeItems[] =
{
	{ NULL,   LabelPace,      ActPace,      NULL, NULL, 1 },
	{ NULL,   LabelLights,    ActLights,    NULL, NULL, 0 },
	{ NULL,   LabelClearance, ActClearance, NULL, NULL, 1 },
	{ NULL,   LabelSteer,     ActSteer,     NULL, NULL, 1 },
	{ NULL,   LabelRoadLimit, NULL,         NULL, NULL, 0 },
	{ "Back", NULL,           ActBack,      NULL, NULL, 0 },
};

static const JER_PAUSE_MENU sEvadeMenu =
{
	"Evade settings", sEvadeItems, (int)(sizeof(sEvadeItems) / sizeof(sEvadeItems[0])), JERAI_EVADE_PAGE_ID
};

static const JER_PAUSE_MENU_ITEM sMainItems[] =
{
	{ NULL,             LabelBehavior, ActBehavior, NULL, NULL,        1 },
	{ NULL,             LabelImmune,   ActImmune,   NULL, NULL,        0 },
	{ NULL,             LabelFollow,   ActFollow,   NULL, NULL,        0 },
	{ NULL,             LabelFlee,     ActFlee,     NULL, NULL,        0 },
	{ "Evade settings", NULL,          NULL,        NULL, &sEvadeMenu, 0 },
	{ NULL,             LabelRoad,     ActRoad,     NULL, NULL,        0 },
	{ NULL,             LabelPath,     ActPath,     NULL, NULL,        0 },
	{ NULL,             LabelHud,      ActHud,      NULL, NULL,        0 },
	{ NULL,             LabelTrace,    ActTrace,    NULL, NULL,        1 },
	{ NULL,             LabelDump,     ActDump,     NULL, NULL,        0 },
	{ "Close",          NULL,          ActClose,    NULL, NULL,        0 },
};

static const JER_PAUSE_MENU sMainMenu =
{
	"JerAI", sMainItems, (int)(sizeof(sMainItems) / sizeof(sMainItems[0])), JERAI_PAGE_ID
};

void JerAiMenuRegister(void)
{
	jer_pause_menu_register(&sMainMenu);
}

/* ------------------------------------------------------------------ */
/* L2, and the keys that closed the pause                              */
/* ------------------------------------------------------------------ */

/* the pause we opened has closed: whatever close key is still down stays off the car */
static void JerAiMenuSyncPause(void)
{
	if (sPauseAsked && !pauseflag)
	{
		sPauseAsked = 0;
		sSwallow = Pads[0].mapped & JERAI_MENU_CLOSE_KEYS;
	}
}

void JerAiMenuForceOpen(void)
{
	if (jer_pause_menu_open(JERAI_PAGE_ID))
		sPauseAsked = 1;
}

void JerAiMenuTick(void)
{
	JerAiMenuSyncPause();

	/* a released key is no longer swallowed */
	sSwallow &= Pads[0].mapped;

	/* the page belongs to an enabled JerAI: L2 is left alone otherwise */
	if (!gJerAi.enabled)
		return;

	if ((Pads[0].mapnew & JERAI_MENU_KEY) && !(sSwallow & JERAI_MENU_KEY))
		JerAiMenuForceOpen();
}

unsigned int JerAiMenuPadMask(void)
{
	JerAiMenuSyncPause();
	return (unsigned int)sSwallow;
}
