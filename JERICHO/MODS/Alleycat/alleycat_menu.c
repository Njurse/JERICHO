/*
 * alleycat_menu.c -- Alleycat's settings, as a page of the game's own pause menu.
 *
 * The page is registered with jer_pause_menu.h like every other module page
 * (Continue -> JERICHO Addons -> Alleycat), and L2 opens the pause straight on it
 * (jer_pause_menu_open), only while Alleycat is enabled. Triangle or Start on the
 * page closes the pause again; the Evade settings are a submenu of it.
 *
 * The pause stops the game, so the car gets no input at all while the page is
 * open. The one thing left to guard is the key that CLOSED it: Cross on Close,
 * Triangle or Start would otherwise reach the car on the first frame back (a
 * Triangle edge leaves the car, a handbrake edge cycles the control mode). Those
 * keys are stripped from the car pad until they are released
 * (AlleycatMenuPadMask, applied in alleycat.c's CAR_PAD hook).
 *
 * Every row writes its value straight to Alleycat.ini through jer_config, so a
 * change survives a reload and the .ini always shows the live state.
 *
 * L2 itself is still the camera's look-left (and Caine's Crossfire's gun): the
 * camera reads it outside the car-pad hook, out of this module's reach.
 */

#include "alleycat_internal.h"
#include "alleycat_drive.h"
#include "alleycat_behavior.h"

#include "jer_pause_menu.h"
#include "jer_config.h"
#include "jer_events.h"
#include "pad.h"
#include "pause.h"	/* pauseflag */

#include <stdio.h>

#define ALLEYCAT_MENU_KEY		MPAD_L2

/* the page's stable ids (jer_pause_menu_open, jer_pause_menu_add_items) */
#define ALLEYCAT_PAGE_ID		"alleycat"
#define ALLEYCAT_EVADE_PAGE_ID	"alleycat.evade"

/* keys that can close the pause from this page: kept off the car until released */
#define ALLEYCAT_MENU_CLOSE_KEYS	(MPAD_CROSS | MPAD_TRIANGLE | MPAD_START | MPAD_L2)

/* steer_trace when the row is switched on with Cross: the rate the stats tool
 * suggests (tools/alleycat_steer_stats.py) */
#define ALLEYCAT_TRACE_DEFAULT	3
#define ALLEYCAT_TRACE_MAX		30

static int sPauseAsked;	/* we opened the pause and have not seen it close yet */
static int sSwallow;	/* close keys still held since the pause closed: kept off the car */

static void AlleycatSetInt(const char* key, int* field, int value)
{
	*field = value;
	jer_config_set_int("Alleycat", key, value);
}

static const char* AlleycatOnOff(int v)
{
	return v ? "On" : "Off";
}

static int AlleycatClamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------ */
/* main page rows                                                      */
/* ------------------------------------------------------------------ */

static int AlleycatBehaviorIndex(void)
{
	int i, n = AlleycatBehaviorCount();

	for (i = 0; i < n; i++)
	{
		if (AlleycatBehaviorIdAt(i) == gAlleycat.mode)
			return i;
	}

	return 0;
}

static void LabelBehavior(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Behavior: < %s >", AlleycatBehaviorName(gAlleycat.mode));
}

static int ActBehavior(void* ud, int dir)
{
	int n = AlleycatBehaviorCount();
	int i = AlleycatBehaviorIndex() + (dir < 0 ? -1 : 1);	/* Cross steps forward */

	(void)ud;

	if (n <= 0)
		return JER_PAUSE_QUIT_NONE;

	i = (i % n + n) % n;
	AlleycatSetInt(ALLEYCAT_CFG_MODE, &gAlleycat.mode, AlleycatBehaviorIdAt(i));
	AlleycatLogState("menu");

	return JER_PAUSE_QUIT_NONE;
}

#define ALLEYCAT_TOGGLE_ROW(fn, text, key, field)					\
	static void Label##fn(void* ud, char* out, int max)			\
	{									\
		(void)ud;							\
		snprintf(out, max, "%s: %s", text, AlleycatOnOff(gAlleycat.field));	\
	}									\
	static int Act##fn(void* ud, int dir)					\
	{									\
		(void)ud; (void)dir;						\
		AlleycatSetInt(key, &gAlleycat.field, !gAlleycat.field);			\
		return JER_PAUSE_QUIT_NONE;					\
	}

ALLEYCAT_TOGGLE_ROW(Immune, "Immunity", ALLEYCAT_CFG_IMMUNE, immune)
ALLEYCAT_TOGGLE_ROW(Follow, "Follow players", ALLEYCAT_CFG_FOLLOW, followEnabled)
ALLEYCAT_TOGGLE_ROW(Ram, "Pursuit: ram", ALLEYCAT_CFG_PURSUIT_RAM, pursuitRam)
ALLEYCAT_TOGGLE_ROW(Flee, "Flee players", ALLEYCAT_CFG_FLEE, fleeEnabled)
ALLEYCAT_TOGGLE_ROW(Road, "Road view", ALLEYCAT_CFG_VIZ_ROAD, vizRoad)
ALLEYCAT_TOGGLE_ROW(Path, "Path view", ALLEYCAT_CFG_VIZ_PATH, vizPath)
ALLEYCAT_TOGGLE_ROW(Hud, "HUD lines", ALLEYCAT_CFG_VIZ_HUD, vizHud)

static void LabelTrace(void* ud, char* out, int max)
{
	(void)ud;

	if (gAlleycat.steerTrace > 0)
		snprintf(out, max, "Steer trace: every %d frame%s", gAlleycat.steerTrace,
			gAlleycat.steerTrace == 1 ? "" : "s");
	else
		snprintf(out, max, "Steer trace: Off");
}

/* Cross switches it on at the stats tool's rate or off; left/right set the rate */
static int ActTrace(void* ud, int dir)
{
	int v;

	(void)ud;

	if (dir == 0)
		v = gAlleycat.steerTrace > 0 ? 0 : ALLEYCAT_TRACE_DEFAULT;
	else
		v = AlleycatClamp(gAlleycat.steerTrace + dir, 0, ALLEYCAT_TRACE_MAX);

	AlleycatSetInt(ALLEYCAT_CFG_STEER_TRACE, &gAlleycat.steerTrace, v);

	return JER_PAUSE_QUIT_NONE;
}

static void LabelDump(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, gAlleycat.dumpRequested ? "Dump road graph: queued" : "Dump road graph");
}

static int ActDump(void* ud, int dir)
{
	(void)ud; (void)dir;
	gAlleycat.dumpRequested = 1;	/* serviced by the frame hook once the game runs */
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
	snprintf(out, max, "Pace: %+d%% over limit", gAlleycat.pacePct);
}

static int ActPace(void* ud, int dir)
{
	(void)ud;
	AlleycatSetInt(ALLEYCAT_CFG_PACE_PCT, &gAlleycat.pacePct,
		AlleycatClamp(gAlleycat.pacePct + (dir < 0 ? -5 : 5), 0, 100));
	return JER_PAUSE_QUIT_NONE;
}

static void LabelLights(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Red lights: %s", gAlleycat.runLights ? "Run" : "Stop");
}

static int ActLights(void* ud, int dir)
{
	(void)ud; (void)dir;
	AlleycatSetInt(ALLEYCAT_CFG_RUN_LIGHTS, &gAlleycat.runLights, !gAlleycat.runLights);
	return JER_PAUSE_QUIT_NONE;
}

static void LabelClearance(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Cop clearance: %d", gAlleycat.copClearance);
}

static int ActClearance(void* ud, int dir)
{
	(void)ud;
	AlleycatSetInt(ALLEYCAT_CFG_COP_CLEARANCE, &gAlleycat.copClearance,
		AlleycatClamp(gAlleycat.copClearance + (dir < 0 ? -500 : 500), 1000, 20000));
	return JER_PAUSE_QUIT_NONE;
}

/* 352 and up is the engine's own lock (alleycat.h ALLEYCAT_CFG_MAX_STEER), i.e. no cap */
static void LabelSteer(void* ud, char* out, int max)
{
	(void)ud;

	if (gAlleycat.maxSteer >= 352)
		snprintf(out, max, "Max steer: no cap");
	else
		snprintf(out, max, "Max steer: %d", gAlleycat.maxSteer);
}

static int ActSteer(void* ud, int dir)
{
	int v = gAlleycat.maxSteer >= 352 ? 352 : gAlleycat.maxSteer;

	(void)ud;
	v = AlleycatClamp(v + (dir < 0 ? -32 : 32), 64, 352);
	AlleycatSetInt(ALLEYCAT_CFG_MAX_STEER, &gAlleycat.maxSteer, v >= 352 ? 512 : v);	/* 512 = the shipped "no cap" */

	return JER_PAUSE_QUIT_NONE;
}

/* a readout: the limit of the road under the car, and the cruise Evade makes of it
 * (alleycat_behavior.c: roadLimit * (100 + pace_pct) / 100) */
static void LabelRoadLimit(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Road limit: %d, cruise %d", gAlleycat.roadLimit,
		gAlleycat.roadLimit * (100 + gAlleycat.pacePct) / 100);
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
	"Evade settings", sEvadeItems, (int)(sizeof(sEvadeItems) / sizeof(sEvadeItems[0])), ALLEYCAT_EVADE_PAGE_ID
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
	"Alleycat", sMainItems, (int)(sizeof(sMainItems) / sizeof(sMainItems[0])), ALLEYCAT_PAGE_ID
};

void AlleycatMenuRegister(void)
{
	jer_pause_menu_register(&sMainMenu);
}

/* ------------------------------------------------------------------ */
/* L2, and the keys that closed the pause                              */
/* ------------------------------------------------------------------ */

/* the pause we opened has closed: whatever close key is still down stays off the car */
static void AlleycatMenuSyncPause(void)
{
	if (sPauseAsked && !pauseflag)
	{
		sPauseAsked = 0;
		sSwallow = Pads[0].mapped & ALLEYCAT_MENU_CLOSE_KEYS;
	}
}

void AlleycatMenuForceOpen(void)
{
	if (jer_pause_menu_open(ALLEYCAT_PAGE_ID))
		sPauseAsked = 1;
}

void AlleycatMenuTick(void)
{
	AlleycatMenuSyncPause();

	/* a released key is no longer swallowed */
	sSwallow &= Pads[0].mapped;

	/* the page belongs to an enabled Alleycat: L2 is left alone otherwise */
	if (!gAlleycat.enabled)
		return;

	if ((Pads[0].mapnew & ALLEYCAT_MENU_KEY) && !(sSwallow & ALLEYCAT_MENU_KEY))
		AlleycatMenuForceOpen();
}

unsigned int AlleycatMenuPadMask(void)
{
	AlleycatMenuSyncPause();
	return (unsigned int)sSwallow;
}
