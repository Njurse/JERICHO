#include "driver2.h"
#include "pause.h"
#include "jericho.h"	// JERICHO-HOOK: mod runtime (inert without modules)
#include "jer_events.h"	// JERICHO-HOOK: event argument structs
#include "jer_pause_menu.h"	// JERICHO-HOOK: module pause menus
#include "system.h"
#include "mission.h"
#include "overlay.h"
#include "pres.h"
#include "pad.h"
#include "main.h"
#include "glaunch.h"
#include "scores.h"
#include "sound.h"
#include "cutscene.h"
#include "overmap.h"
#include "handling.h"
#include "platform.h"
#include "loadsave.h"
#include "cutrecorder.h"

#define REPLAY_NAME_LEN		16
#define SCORE_NAME_LEN		5

#define PAUSE_MENU_LEVELS 4	/* root + Modules container + module menu + one submenu */

static int gScoreEntered = 0;
static char EnterNameText[32] = { 0 };		// translated text

int PauseReturnValue;	// JERICHO-HOOK: exported so modules can unpause (e.g. after a map teleport)

int pauseflag = 0;
int gShowMap = 0;
int gDrawPauseMenus = 0;
int gEnteringScore = 0;
static int gScorePosition = 0;
static int allownameentry = 0;

int playerwithcontrol[3] = { 0 };

// message box
struct MENU_MESSAGE
{
	char* header;
	char* text;
	int show;
} gDisplayedMessage = { NULL, NULL, 0 };

typedef void(*pauseFunc)(int dir);

struct MENU_ITEM;
struct MENU_HEADER;

struct MENU_ITEM
{
	char* Text;
	u_char Type;
	u_char Justify;
	pauseFunc func;
	EXIT_VALUE ExitValue;
	MENU_HEADER* SubMenu;
};

struct MENU_HEADER
{
	char* Title;
	XYWH Bound;
	u_char NumItems;
	MENU_ITEM* MenuItems;
};

static MENU_ITEM* ActiveItem[PAUSE_MENU_LEVELS];
static MENU_HEADER* VisibleMenus[PAUSE_MENU_LEVELS];
static MENU_HEADER* ActiveMenu;
static int ActiveMenuItem;
static int VisibleMenu;

static char SfxVolumeText[8];
static char MusicVolumeText[8];

static char ScoreTime[5][16];
static char ScoreItems[5][16];
static char ScoreName[5][7];

static char validchars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-+!\xFF\xFE";

void PauseMap(int direction);
void SfxVolume(int direction);
void MusicVolume(int direction);
void SaveReplay(int direction);
void SaveGame(int direction);
void SkipCutscene(int direction);

void EnterScoreName();

void CreateScoreNames(SCORE_ENTRY *table, PLAYER_SCORE *score, int position);
void DrawHighScoreMenu(int selection); 

char EnterScoreText[32] = { 0 };

enum MenuItemType
{
	//PAUSE_TYPE_ACTIVE		= (1 << 0),		// suggested - USUSED flag
	PAUSE_TYPE_FUNC			= (1 << 1),		// simple function calling when pressing Cross
	PAUSE_TYPE_DIRFUNC		= (1 << 2),		// function calling with direction (left/right)
	PAUSE_TYPE_SFXVOLUME	= (1 << 3),
	PAUSE_TYPE_MUSICVOLUME	= (1 << 4),
	PAUSE_TYPE_SUBMENU		= (1 << 6),
	PAUSE_TYPE_ENDITEMS		= (1 << 7),
};

#if defined(_DEBUG) || defined(DEBUG_OPTIONS)

void SetRightWayUp(int direction)
{
	extern char gRightWayUp;
	gRightWayUp = 1;
	PauseReturnValue = MENU_QUIT_CONTINUE;
}

void SetDisplayPosition(int direction)
{
	extern int gDisplayPosition;
	gDisplayPosition ^= 1;
}

void ToggleInvincibility(int direction)
{
	extern int gInvincibleCar;
	gInvincibleCar ^= 1;
}

void ToggleImmunity(int direction)
{
	extern int gPlayerImmune;
	gPlayerImmune ^= 1;
}

void TogglePlayerGhost(int direction)
{
	extern int playerghost;
	playerghost ^= 1;
}

void ToggleOverlays(int direction)
{
	gDoOverlays ^= 1;
}

// JERICHO-HOOK: module pause menus are registered by the modules themselves
// (jer_pause_menu.h) and bridged by JerPauseBuildModuleMenus() below — no
// per-module shells live in this file anymore.

int lastCar = -1;

void ToggleSecretCarFun(int direction)
{
	extern CAR_COSMETICS car_cosmetics[SPECIAL_CAR_SLOT];
	extern int wantedCar[2];

	int active = (ActiveCheats.cheat10 ^= 1);

	if (active)
	{
		if (lastCar == -1)
			lastCar = wantedCar[0];

		// make our current car the secret car
		wantedCar[0] = 12;
	}
	else
	{
		if (lastCar != -1)
		{
			// restore our initial car
			wantedCar[0] = lastCar;
			lastCar = -1;
		}
	}

	FixCarCos(&car_cosmetics[SPECIAL_CAR_SLOT], 12);
}

void ToggleJerichoMode(int direction)
{
	ActiveCheats.cheat12 ^= 1;
}

void TogglePuppyDogCops(int direction)
{
	gPuppyDogCop ^= 1;
}

extern void LoadSky(void);

void DebugTimeOfDayDay(int direction)
{
	wantedTimeOfDay = TIME_DAY;
	gTimeOfDay = TIME_DAY;
	gWantNight = 0;
	LoadSky();
}

void DebugTimeOfDayNight(int direction)
{
	wantedTimeOfDay = TIME_NIGHT;
	gTimeOfDay = TIME_NIGHT;
	gWantNight = 1;
	LoadSky();
}

void DebugTimeOfDayDusk(int direction)
{
	wantedTimeOfDay = TIME_DUSK;
	gTimeOfDay = TIME_DUSK;
	gWantNight = 0;
	LoadSky();
}

void DebugTimeOfDayDawn(int direction)
{
	wantedTimeOfDay = TIME_DAWN;
	gTimeOfDay = TIME_DAWN;
	gWantNight = 0;
	LoadSky();
}

void DebugTimeOfDayRain(int direction)
{
	//extern int weather;
	//weather ^= weather;
	gWeather ^= 1;
	wantedWeather = gWeather;

	if (gWeather == WEATHER_RAIN)
		wetness = 7000;
	else
		wetness = 0;

	LoadSky();
}


MENU_ITEM DebugTimeOfDayItems[] =
{
	{ "Day",	PAUSE_TYPE_FUNC, 	2,	DebugTimeOfDayDay,	MENU_QUIT_NONE,		NULL },
	{ "Night", 	PAUSE_TYPE_FUNC,  2,  DebugTimeOfDayNight,MENU_QUIT_NONE,		NULL },
	{ "Dusk", 	PAUSE_TYPE_FUNC, 	2,  DebugTimeOfDayDusk, MENU_QUIT_NONE,		NULL },
	{ "Dawn", 	PAUSE_TYPE_FUNC, 	2,  DebugTimeOfDayDawn,	MENU_QUIT_NONE,		NULL },
	{ "Rain", 	PAUSE_TYPE_FUNC, 	2,  DebugTimeOfDayRain,	MENU_QUIT_NONE,		NULL },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_HEADER DebugTimeOfDayHeader =
{ "Time Of Day", { 0, 0, 0, 0 }, 0u, DebugTimeOfDayItems };

MENU_ITEM DebugJustForFunItems[] =
{
	{ "Secret Car Fun", PAUSE_TYPE_FUNC,	2,  ToggleSecretCarFun, MENU_QUIT_RESTART,	NULL },
	{ "Jericho Mode",	PAUSE_TYPE_FUNC,	2,  ToggleJerichoMode,	MENU_QUIT_NONE,		NULL },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_HEADER DebugJustForFunHeader =
{ "Just for fun", { 0, 0, 0, 0 }, 0u, DebugJustForFunItems };

MENU_ITEM DebugOptionsItems[] =
{
#ifdef CUTSCENE_RECORDER
	{ gCurrentChasePauseText, 5u, 2u, (pauseFunc)&CutRec_NextChase, MENU_QUIT_NONE, NULL },
#endif
	{ "Display position", PAUSE_TYPE_FUNC, 	2,	SetDisplayPosition,		MENU_QUIT_NONE,		NULL},
	{ "Back on Wheels",	PAUSE_TYPE_FUNC, 	2,	SetRightWayUp,		MENU_QUIT_NONE,		NULL},
	{ "Time of Day", 	PAUSE_TYPE_SUBMENU, 2,  NULL,		  		MENU_QUIT_NONE,		&DebugTimeOfDayHeader },
	{ "Fun Cheats", 	PAUSE_TYPE_SUBMENU, 2,  NULL,		  		MENU_QUIT_NONE,		&DebugJustForFunHeader },
	{ "Invincibility", 	PAUSE_TYPE_FUNC, 	2,  ToggleInvincibility,MENU_QUIT_NONE,		NULL},
	{ "Immunity", 		PAUSE_TYPE_FUNC, 	2,  ToggleImmunity,		MENU_QUIT_NONE,		NULL},
	{ "Puppy Dog Cops",	PAUSE_TYPE_FUNC,	2,  TogglePuppyDogCops,	MENU_QUIT_NONE,		NULL },
	{ "Toggle Overlay",	PAUSE_TYPE_FUNC,	2,  ToggleOverlays,		MENU_QUIT_NONE,		NULL },
	{ "Player Ghost", 	PAUSE_TYPE_FUNC, 	2,  TogglePlayerGhost,	MENU_QUIT_NONE,		NULL },
	{ "Next Mission",	0, 					2,  NULL,				MENU_QUIT_NEXTMISSION, 	NULL },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_HEADER DebugOptionsHeader =
{ "Debug Options", { 0, 0, 0, 0 }, 0u, DebugOptionsItems };
#endif

MENU_ITEM YesNoRestartItems[] =
{
	{ G_LTXT_ID(GTXT_NO), 1u, 2u, NULL, MENU_QUIT_BACKMENU, NULL },
	{ G_LTXT_ID(GTXT_YES), 1u, 2u, NULL, MENU_QUIT_RESTART, NULL },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM YesNoQuitItems[] =
{
	{ G_LTXT_ID(GTXT_NO), 1u, 2u, NULL, MENU_QUIT_BACKMENU, NULL },
	{ G_LTXT_ID(GTXT_YES), 1u, 2u, NULL, MENU_QUIT_QUIT, NULL },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_HEADER YesNoRestartHeader =
{ G_LTXT_ID(GTXT_AreYouSure), { 0, 0, 0, 0 }, 0u, YesNoRestartItems };

MENU_HEADER YesNoQuitHeader =
{ G_LTXT_ID(GTXT_AreYouSure), { 0, 0, 0, 0 }, 0u, YesNoQuitItems };

MENU_ITEM MainPauseItems[] =
{
	{ G_LTXT_ID(GTXT_Continue), 1u, 2u, NULL, MENU_QUIT_CONTINUE, NULL },
	{ G_LTXT_ID(GTXT_ShowMap), PAUSE_TYPE_FUNC, 2u, (pauseFunc)&PauseMap, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_Restart), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoRestartHeader },
	{ G_LTXT_ID(GTXT_SfxVolume), PAUSE_TYPE_SFXVOLUME | PAUSE_TYPE_DIRFUNC, 2u, (pauseFunc)&SfxVolume, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_MusicVolume), PAUSE_TYPE_MUSICVOLUME | PAUSE_TYPE_DIRFUNC, 2u, (pauseFunc)&MusicVolume, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_FilmDirector), 1u, 2u, NULL, MENU_QUIT_DIRECTOR, NULL},
	{ G_LTXT_ID(GTXT_QuickReplay),1u,2u,NULL,MENU_QUIT_QUICKREPLAY,NULL},
#if defined(_DEBUG) || defined(DEBUG_OPTIONS)
	{ "Debug Options", PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &DebugOptionsHeader },
#endif
	{ G_LTXT_ID(GTXT_Exit), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoQuitHeader },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM MultiplayerPauseItems[] =
{
	{ G_LTXT_ID(GTXT_Continue), 1u, 2u, NULL, MENU_QUIT_CONTINUE, NULL },
	{ G_LTXT_ID(GTXT_Restart), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoRestartHeader },
	{ G_LTXT_ID(GTXT_SfxVolume), PAUSE_TYPE_SFXVOLUME | PAUSE_TYPE_DIRFUNC, 2u, (pauseFunc)&SfxVolume, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_MusicVolume), PAUSE_TYPE_MUSICVOLUME | PAUSE_TYPE_DIRFUNC, 2u, (pauseFunc)&MusicVolume, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_QuickReplay),1u,2u,NULL,MENU_QUIT_QUICKREPLAY,NULL},
	{ G_LTXT_ID(GTXT_Exit), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoQuitHeader },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM CutscenePauseItems[] =
{
	{ G_LTXT_ID(GTXT_Continue), 1u, 2u, NULL, MENU_QUIT_CONTINUE, NULL },
	{ G_LTXT_ID(GTXT_SkipCutscene), PAUSE_TYPE_FUNC, 2u, (pauseFunc)&SkipCutscene, MENU_QUIT_CONTINUE, NULL },
	{ G_LTXT_ID(GTXT_Restart), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoRestartHeader },
	{ G_LTXT_ID(GTXT_SfxVolume), PAUSE_TYPE_SFXVOLUME | PAUSE_TYPE_DIRFUNC, 2u, (pauseFunc)&SfxVolume, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_MusicVolume), PAUSE_TYPE_MUSICVOLUME | PAUSE_TYPE_DIRFUNC, 2u, (pauseFunc)&MusicVolume, MENU_QUIT_NONE, NULL },
#if defined(_DEBUG) || defined(DEBUG_OPTIONS)
	{ "Debug Options", PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &DebugOptionsHeader },
#endif
	{ G_LTXT_ID(GTXT_Exit), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoQuitHeader },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM MissionCompleteItems[] =
{
#ifdef PSX
	{ G_LTXT_ID(GTXT_SaveGame), 3u, 2u, (pauseFunc)&SaveGame, MENU_QUIT_NONE, NULL },
#endif
	{ G_LTXT_ID(GTXT_Continue), 1u, 2u, NULL, MENU_QUIT_NEXTMISSION, NULL },
	{ G_LTXT_ID(GTXT_FilmDirector),1u,2u,NULL,MENU_QUIT_DIRECTOR,NULL},
	{ G_LTXT_ID(GTXT_QuickReplay),1u,2u,NULL,MENU_QUIT_QUICKREPLAY,NULL},
	{ G_LTXT_ID(GTXT_SaveReplay), PAUSE_TYPE_FUNC, 2u, (pauseFunc)&SaveReplay, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_Restart), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoRestartHeader },
	{ G_LTXT_ID(GTXT_Exit), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoQuitHeader },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM MissionFailedItems[6] =
{
	{ G_LTXT_ID(GTXT_FilmDirector),1u,2u,NULL,MENU_QUIT_DIRECTOR,NULL},
	{ G_LTXT_ID(GTXT_QuickReplay),1u,2u,NULL,MENU_QUIT_QUICKREPLAY,NULL},
	{ G_LTXT_ID(GTXT_Exit), 3u, 2u, (pauseFunc)&SaveReplay, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_RetryMission),PAUSE_TYPE_SUBMENU,2u,NULL,MENU_QUIT_NONE,&YesNoRestartHeader },
	{ G_LTXT_ID(GTXT_Exit), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoQuitHeader },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM TakeARideFinishedItems[] =
{
	{ G_LTXT_ID(GTXT_Restart), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoRestartHeader },
	{ G_LTXT_ID(GTXT_FilmDirector),1u,2u,NULL,MENU_QUIT_DIRECTOR,NULL},
	{ G_LTXT_ID(GTXT_QuickReplay),1u,2u,NULL,MENU_QUIT_QUICKREPLAY,NULL},
	{ G_LTXT_ID(GTXT_SaveReplay), PAUSE_TYPE_FUNC, 2u, (pauseFunc)&SaveReplay, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_Exit), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoQuitHeader },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM DrivingGameFinishedItems[] =
{
	{ G_LTXT_ID(GTXT_PlayAgain), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoRestartHeader },
	{ EnterScoreText, 3u, 2u, (pauseFunc)&EnterScoreName, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_FilmDirector),1u,2u,NULL,MENU_QUIT_DIRECTOR,NULL},
	{ G_LTXT_ID(GTXT_QuickReplay),1u,2u,NULL,MENU_QUIT_QUICKREPLAY,NULL},
	{ G_LTXT_ID(GTXT_SaveReplay), PAUSE_TYPE_FUNC, 2u, (pauseFunc)&SaveReplay, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_Exit), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoQuitHeader },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM MultiplayerFinishedItems[] =
{
	{ G_LTXT_ID(GTXT_PlayAgain), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoRestartHeader },
	{ G_LTXT_ID(GTXT_QuickReplay),1u,2u,NULL,MENU_QUIT_QUICKREPLAY,NULL},
	{ G_LTXT_ID(GTXT_SaveReplay), PAUSE_TYPE_FUNC, 2u, (pauseFunc)&SaveReplay, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_Exit), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoQuitHeader },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM ChaseGameFinishedItems[] =
{
	{ G_LTXT_ID(GTXT_PlayAgain), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoRestartHeader },
	{ G_LTXT_ID(GTXT_FilmDirector),1u,2u,NULL,MENU_QUIT_DIRECTOR,NULL},
	{ G_LTXT_ID(GTXT_QuickReplay),1u,2u,NULL,MENU_QUIT_QUICKREPLAY,NULL},
	{ G_LTXT_ID(GTXT_SaveReplay), PAUSE_TYPE_FUNC, 2u, (pauseFunc)&SaveReplay, MENU_QUIT_NONE, NULL },
	{ G_LTXT_ID(GTXT_Exit), PAUSE_TYPE_SUBMENU, 2u, NULL, MENU_QUIT_NONE, &YesNoQuitHeader },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM NoPadItems[] =
{
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL}
};

MENU_ITEM NoMultiPadItems[] =
{
	{ "Exit", 1u, 2u, NULL, MENU_QUIT_QUIT, NULL },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};

MENU_ITEM InvalidPadItems[] =
{
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL}
};

MENU_ITEM InvalidMultiPadItems[] =
{
	{ G_LTXT_ID(GTXT_Exit), 1u, 2u, NULL, MENU_QUIT_QUIT, NULL },
	{ NULL, PAUSE_TYPE_ENDITEMS, 0u, NULL, MENU_QUIT_NONE, NULL }
};


MENU_HEADER PauseMenuHeader =
{ G_LTXT_ID(GTXT_Paused), { 0, 0, 0, 0 }, 0u, MainPauseItems };

MENU_HEADER MultiplayerPauseHeader =
{ G_LTXT_ID(GTXT_Paused), { 0, 0, 0, 0 }, 0u, MultiplayerPauseItems };

MENU_HEADER CutscenePauseMenuHeader =
{ G_LTXT_ID(GTXT_Paused), { 0, 0, 0, 0 }, 0u, CutscenePauseItems };

MENU_HEADER MissionCompleteHeader =
{ G_LTXT_ID(GTXT_MissionSuccessful), { 0, 0, 0, 0 }, 0u, MissionCompleteItems };

MENU_HEADER MissionFailedHeader =
{ G_LTXT_ID(GTXT_MissionFailed), { 0, 0, 0, 0 }, 0u, MissionFailedItems };

MENU_HEADER TakeARideFinishedHeader =
{ G_LTXT_ID(GTXT_GameOver), { 0, 0, 0, 0 }, 0u, TakeARideFinishedItems };

MENU_HEADER DrivingGameFinishedHeader =
{ G_LTXT_ID(GTXT_GameOver), { 0, 0, 0, 0 }, 0u, DrivingGameFinishedItems };

MENU_HEADER MultiplayerFinishedHeader =
{ G_LTXT_ID(GTXT_GameOver), { 0, 0, 0, 0 }, 0u, MultiplayerFinishedItems };

MENU_HEADER ChaseGameFinishedHeader =
{ G_LTXT_ID(GTXT_GameOver), { 0, 0, 0, 0 }, 0u, ChaseGameFinishedItems };

MENU_HEADER NoPadHeader =
{
	G_LTXT_ID(GTXT_InsertController1),
	{ 0, 0, 0, 0 },
	0u,
	NoPadItems
};

MENU_HEADER NoMultiPadHeader =
{
	G_LTXT_ID(GTXT_InsertController2),
	{ 0, 0, 0, 0 },
	0u,
	NoMultiPadItems
};

MENU_HEADER InvalidPadHeader =
{
	G_LTXT_ID(GTXT_IncorrectController1),
	{ 0, 0, 0, 0 },
	0u,
	InvalidPadItems
};

MENU_HEADER InvalidMultiPadHeader =
{
	G_LTXT_ID(GTXT_IncorrectController2),
	{ 0, 0, 0, 0 },
	0u,
	InvalidMultiPadItems
};


u_char gCurrentTextChar = 0;
typedef void(*OnEntryComplete)(void* data, char* text);

void ScoreNameInputHandler(const char* input)
{
	if (!input)
	{
		gCurrentTextChar = 255;
		return;
	}

	gCurrentTextChar = *input;
}

#define USE_PAD_INPUT defined(PSX)

// [A] Enter the replay name to save
char* WaitForTextEntry(char* textBufPtr, int maxLength)
{
	u_char chr;
	int so, co;
	int delay, toggle;
	char* username;
	unsigned short npad, dpad;

	username = textBufPtr ? textBufPtr : (char*)_overlay_buffer;
	delay = 0;
	toggle = 0;
	co = 1;
	so = strlen(username);

#if !USE_PAD_INPUT
	// PsyX input handler
	g_cfg_gameOnTextInput = ScoreNameInputHandler;
	gCurrentTextChar = 0;
#endif

	do {

		if (!FilterFrameTime())
			continue;
		
		ReadControllers();

		npad = Pads[0].dirnew;
		dpad = Pads[0].direct;

		// cancel
		if (npad & 0x10)
			return NULL;

#if USE_PAD_INPUT
		if (dpad & 0x20)
		{
			// switch to end
			delay = 1;
			toggle = 0;
			co = 67;
		}
		else if (dpad & 0x8000)
		{
			// move left/right to switch chars
			if (delay-- == 0)
			{
				delay = 20;
				toggle = 0;
				co--;
			}
			else if (delay < 1)
			{
				delay = 2;
				toggle = 0;
				co--;
			}

			if (co < 0)
				co = 67;
		}
		else if (dpad & 0x2000)
		{
			if (delay-- == 0)
			{
				delay = 20;
				toggle = 0;
				co++;
			}
			else if (delay < 1)
			{
				delay = 2;
				toggle = 0;
				co++;
			}

			if (co > 67)
				co = 0;
		}
		else
		{
			delay = 0;
		}

		if (so == maxLength)
			chr = 254;
		else
			chr = validchars[co];
#else
		if (gCurrentTextChar > 0)
		{
			if (gCurrentTextChar == 255)
			{
				// do backspace
				chr = 255;
			}
			else
			{
				// Find a valid character
				for (co = 0; co < 69; co++)
				{
					if (validchars[co] == gCurrentTextChar)
						break;
				}

				if (so == maxLength)
				{
					chr = 254;
					gCurrentTextChar = 0;
				}
				else
					chr = validchars[co];
			}
		}
		else
		{
			if (so == maxLength)
				chr = 254;
			else
				chr = 'I';
		}

		if (npad & 0x40)
		{
			gCurrentTextChar = 254;
			chr = 254;
		}
#endif
		toggle++;

		if (toggle & 4)
			username[so] = ' ';
		else if (chr == ' ')
			username[so] = '.';
		else
			username[so] = chr;

#if USE_PAD_INPUT
		if (npad & 0x80)
		{
			if (so > 0)
				so--;

			username[so] = 0;
			username[so + 1] = 0;
		}

		if (npad & 0x40)
		{
#else
		if (gCurrentTextChar > 0)
		{
			gCurrentTextChar = 0;
#endif
			// complete
			if (chr == 254)
			{
				username[so] = 0;
				break;
			}

			// delete chars
			if (chr == 255)
			{
				if (so > 0)
					so--;

				username[so] = 0;
				username[so + 1] = 0;
			}
			else if (so < maxLength)
			{
				username[so] = chr;
				username[so + 1] = 0;
				so++;
			}
		}

		DrawGame();

#ifdef __EMSCRIPTEN__
		emscripten_sleep(0);
#endif
	} while (true);

#if !USE_PAD_INPUT
	g_cfg_gameOnTextInput = NULL;
#endif

	return username;
}

void SkipCutscene(int direction)
{
	gSkipInGameCutscene = 1;
}

// [D] [T]
void SaveReplay(int direction)
{
	char* result;
	char filename[64];

#ifdef PSX
	UNIMPLEMENTED();
	// CallMemoryCard(0x10, 1);
#else

#ifdef CUTSCENE_RECORDER
	if(_CutRec_IsOn())
	{
		if (CutRec_SaveChase())
		{
			gDisplayedMessage.header = G_LTXT(GTXT_SaveReplay);
			gDisplayedMessage.text = G_LTXT(GTXT_OK);
			gDisplayedMessage.show = 25;
		}
		else
		{
			gDisplayedMessage.header = G_LTXT(GTXT_SaveReplay);
			gDisplayedMessage.text = G_LTXT(GTXT_SavingError);
			gDisplayedMessage.show = 15;
		}
	}
	else
#endif // CUTSCENE_RECORDER
	{
		int cnt;
		FILE* temp;
		_mkdir("Replays");

		ClearMem(EnterNameText, REPLAY_NAME_LEN);
		
		// detect the best file name
		// TODO: if replay is loaded - set the loaded replay filename
		if (gLoadedReplay)
		{
			strcpy(EnterNameText, gCurrentReplayFilename);
		}
		else
		{
			cnt = 1;
			while (cnt < 1000)
			{
				sprintf(EnterNameText, "Chase%d", cnt);
				sprintf(filename, "Replays/%s.D2RP", EnterNameText);

				if ((temp = fopen(filename, "r")) != NULL)
				{
					fclose(temp);
					cnt++;
				}
				else
					break;
			}
		}

		gDisplayedMessage.header = G_LTXT(GTXT_EnterName);
		gDisplayedMessage.text = EnterNameText;
		gDisplayedMessage.show = -1;

		// wait for user input the replay name
		result = WaitForTextEntry(EnterNameText, REPLAY_NAME_LEN);

		gDisplayedMessage.show = 0;

		if (result)
		{
			sprintf(filename, "Replays/%s.D2RP", result);

			if (SaveReplayToFile(filename))
			{
				gDisplayedMessage.header = G_LTXT(GTXT_SaveReplay);
				gDisplayedMessage.text = G_LTXT(GTXT_OK);
				gDisplayedMessage.show = 15;
			}
			else
			{
				gDisplayedMessage.header = G_LTXT(GTXT_SaveReplay);
				gDisplayedMessage.text = G_LTXT(GTXT_SavingError);
				gDisplayedMessage.show = 15;
			}
		}
	}
#endif // PSX
}

// [D] [T]
void SaveGame(int direction)
{
#ifndef PSX
	SaveCurrentGame();
#endif
}

// [D] [T]
int MaxMenuStringLength(MENU_HEADER *pMenu)
{
	int max;
	int temp;
	MENU_ITEM *pItems;

	pItems = pMenu->MenuItems;
	max = StringWidth(GET_GAME_TXT(pMenu->Title));

	while ((pItems->Type & PAUSE_TYPE_ENDITEMS) == 0)
	{
		temp = StringWidth(GET_GAME_TXT(pItems->Text));

		if (pItems->Type & (PAUSE_TYPE_SFXVOLUME | PAUSE_TYPE_MUSICVOLUME)) 
			temp = temp + StringWidth(" 100");

		if (temp > max)
			max = temp;

		pItems++;
	}

	return max;
}


// [D] [T]
void SetupMenu(MENU_HEADER *menu, int back)
{
	int len;
	MENU_ITEM *pItem;
	int numItems;

	numItems = 0;

	ActiveMenuItem = 0;

	pItem = menu->MenuItems;
	while (pItem->Type != PAUSE_TYPE_ENDITEMS)
	{
		if (back && pItem == ActiveItem[VisibleMenu])
			ActiveMenuItem = numItems;

		numItems++;
		pItem++;
	}

	ActiveMenu = menu;
	ActiveMenu->NumItems = numItems;

	len = MaxMenuStringLength(ActiveMenu);

	ActiveMenu->Bound.x = ((304 - len) / 2) - 4;
	ActiveMenu->Bound.w = len + 24;
	ActiveMenu->Bound.y = MAX(48, (SCREEN_H - (numItems + 1) * 15) / 2);
	ActiveMenu->Bound.h = (numItems + 1) * 15 + 10;

	ActiveItem[VisibleMenu] = &ActiveMenu->MenuItems[ActiveMenuItem];
}

/* ------------------------------------------------------------------ */
/* JERICHO-HOOK: bridge module-provided pause menus (jer_pause_menu.h) */
/*                                                                     */
/* Modules register JER_PAUSE_MENU trees; we translate them into the   */
/* engine's static MENU_ITEM/MENU_HEADER form each time the pause      */
/* opens and collect them under a "Modules" submenu spliced into the   */
/* root menu. No per-module code lives here.                           */
/* ------------------------------------------------------------------ */

#define JER_PAUSE_MAX_MENUS 8		/* registered menus (incl. submenus) */
#define JER_PAUSE_MAX_ITEMS 64		/* bridge items (incl. terminators) */
#define JER_PAUSE_ITEM_TEXT 48		/* label buffer per bridge item */
#define JER_PAUSE_ROOT_ITEMS 24		/* dynamic root copy capacity */

static MENU_ITEM gJerModuleItems[JER_PAUSE_MAX_ITEMS];
static char gJerModuleText[JER_PAUSE_MAX_ITEMS][JER_PAUSE_ITEM_TEXT];
static MENU_HEADER gJerModuleHeaders[JER_PAUSE_MAX_MENUS];
static MENU_ITEM gJerModulesItems[JER_PAUSE_MAX_MENUS + 1];	/* "Modules" container */
static char gJerModulesText[JER_PAUSE_MAX_MENUS][JER_PAUSE_ITEM_TEXT];
static MENU_HEADER gJerModulesHeader;				/* "Modules" container */
static MENU_ITEM gJerPauseRootItems[JER_PAUSE_ROOT_ITEMS];	/* root copy + Modules */
static MENU_HEADER gJerPauseRoot;				/* dynamic root when modules exist */

/* bridge slot -> owning (menu, item index): set for activatable items */
typedef struct JER_PAUSE_BRIDGE { const JER_PAUSE_MENU* menu; int item; } JER_PAUSE_BRIDGE;
static JER_PAUSE_BRIDGE gJerBridge[JER_PAUSE_MAX_ITEMS];
static int gJerBridgeCount;		/* items used (incl. terminators) */
static int gJerModuleHeaderCount;
static MENU_HEADER* gJerTopHeaders[JER_PAUSE_MAX_MENUS];	/* registered top-level menus */
static int gJerTopCount;

static EXIT_VALUE JerPauseMapQuit(int q)
{
	switch (q)
	{
		case JER_PAUSE_QUIT_CONTINUE:	return MENU_QUIT_CONTINUE;
		case JER_PAUSE_QUIT_QUIT:		return MENU_QUIT_QUIT;
		case JER_PAUSE_QUIT_RESTART:	return MENU_QUIT_RESTART;
		case JER_PAUSE_QUIT_DIRECTOR:	return MENU_QUIT_DIRECTOR;
		case JER_PAUSE_QUIT_QUICKREPLAY:return MENU_QUIT_QUICKREPLAY;
		case JER_PAUSE_QUIT_BACKMENU:	return MENU_QUIT_BACKMENU;
		case JER_PAUSE_QUIT_NEXTMISSION:return MENU_QUIT_NEXTMISSION;
		default:						return MENU_QUIT_NONE;
	}
}

/* generic engine pauseFunc for module items: dispatch to the module's
 * on_activate, apply the returned quit code, refresh every module label */
static void JerPauseBridgeFunc(int direction)
{
	MENU_ITEM* item = ActiveItem[VisibleMenu];
	int idx;
	const JER_PAUSE_MENU* menu;
	const JER_PAUSE_MENU_ITEM* mi;
	int quit;
	int i;

	if (item == NULL)
		return;

	idx = (int)(item - gJerModuleItems);

	if (idx < 0 || idx >= gJerBridgeCount || gJerBridge[idx].menu == NULL)
		return;

	menu = gJerBridge[idx].menu;

	if (gJerBridge[idx].item < 0 || gJerBridge[idx].item >= menu->item_count)
		return;

	mi = &menu->items[gJerBridge[idx].item];

	quit = mi->on_activate != NULL ? mi->on_activate(mi->userdata, direction) : JER_PAUSE_QUIT_NONE;

	item->ExitValue = JerPauseMapQuit(quit);

	/* toggles/adjustments change labels: refresh all module labels */
	for (i = 0; i < gJerBridgeCount; i++)
	{
		const JER_PAUSE_MENU* m = gJerBridge[i].menu;

		if (m != NULL)
		{
			const JER_PAUSE_MENU_ITEM* mit = &m->items[gJerBridge[i].item];

			if (mit->get_label != NULL)
				mit->get_label(mit->userdata, gJerModuleText[i], JER_PAUSE_ITEM_TEXT);
		}
	}
}

/* build the bridge header for one module menu (recursively its submenus).
 * Items + terminators come from the single shared pool. depth counts from
 * the module menu (0); submenus may only nest one level (the engine's
 * pause stack is PAUSE_MENU_LEVELS = root/Modules/menu/submenu). Returns
 * NULL when nothing fits — callers must treat that as "no submenu". */
static MENU_HEADER* JerPauseBuildMenuRec(const JER_PAUSE_MENU* menu, int depth)
{
	MENU_HEADER* hdr;
	MENU_ITEM* items;
	int built = 0;
	int j;

	if (menu == NULL || depth > 1)
		return NULL;

	/* no room for another header, or for its items + terminator */
	if (gJerModuleHeaderCount >= JER_PAUSE_MAX_MENUS || gJerBridgeCount >= JER_PAUSE_MAX_ITEMS)
		return NULL;

	hdr = &gJerModuleHeaders[gJerModuleHeaderCount];
	items = &gJerModuleItems[gJerBridgeCount];

	for (j = 0; j < menu->item_count; j++)
	{
		const JER_PAUSE_MENU_ITEM* mi = &menu->items[j];
		MENU_ITEM* it;
		int idx;

		if (gJerBridgeCount >= JER_PAUSE_MAX_ITEMS - 1)
			break;

		idx = gJerBridgeCount;
		it = &gJerModuleItems[gJerBridgeCount++];

		memset(it, 0, sizeof(*it));
		it->Text = gJerModuleText[idx];
		it->Justify = 2;
		it->ExitValue = MENU_QUIT_NONE;
		gJerBridge[idx].menu = NULL;
		gJerBridge[idx].item = 0;

		if (mi->submenu != NULL)
		{
			MENU_HEADER* sub = JerPauseBuildMenuRec(mi->submenu, depth + 1);

			if (sub != NULL)
			{
				it->Type = PAUSE_TYPE_SUBMENU;
				it->SubMenu = sub;
			}
			else
			{
				/* submenu couldn't be built (pool/depth): dead entry, so the
				 * engine never dereferences a NULL SubMenu on CROSS */
				it->Type = 0;
				it->ExitValue = MENU_QUIT_NONE;
			}
		}
		else
		{
			it->Type = (u_char)(PAUSE_TYPE_FUNC | (mi->adjust ? PAUSE_TYPE_DIRFUNC : 0));
			it->func = JerPauseBridgeFunc;
			gJerBridge[idx].menu = menu;
			gJerBridge[idx].item = j;
		}

		if (mi->get_label != NULL)
			mi->get_label(mi->userdata, gJerModuleText[idx], JER_PAUSE_ITEM_TEXT);
		else if (mi->label != NULL)
			snprintf(gJerModuleText[idx], JER_PAUSE_ITEM_TEXT, "%s", mi->label);
		else
			gJerModuleText[idx][0] = 0;

		built++;
	}

	if (built == 0)
	{
		/* nothing fit: don't claim a header slot — the engine must never
		 * see an empty submenu (its nav would wrap to MenuItems[-1]) */
		return NULL;
	}

	/* commit the header now that it has real items */
	hdr->Title = (char*)menu->title;
	hdr->Bound.x = hdr->Bound.y = hdr->Bound.w = hdr->Bound.h = 0;
	hdr->NumItems = 0;		/* SetupMenu computes */
	hdr->MenuItems = items;
	gJerModuleHeaderCount++;

	/* ENDITEMS terminator */
	memset(&gJerModuleItems[gJerBridgeCount], 0, sizeof(MENU_ITEM));
	gJerModuleItems[gJerBridgeCount].Type = PAUSE_TYPE_ENDITEMS;
	gJerBridgeCount++;

	return hdr;
}

/* rebuild all module bridges (and refresh labels); returns 1 when any
 * module menu is registered */
static int JerPauseBuildModuleMenus(void)
{
	int i;
	int count = jer_pause_menu_count();

	gJerBridgeCount = 0;
	gJerModuleHeaderCount = 0;
	gJerTopCount = 0;

	for (i = 0; i < count && gJerTopCount < JER_PAUSE_MAX_MENUS; i++)
	{
		MENU_HEADER* h = JerPauseBuildMenuRec(jer_pause_menu_get(i), 0);

		if (h != NULL)
			gJerTopHeaders[gJerTopCount++] = h;
	}

	return gJerTopCount;
}

/* splice a "Modules" submenu into a copy of the root pause menu; returns
 * the dynamic root (or the original when no module menus are registered) */
static MENU_HEADER* JerPauseRootOr(MENU_HEADER* original)
{
	MENU_ITEM* src;
	int n = 0;
	int i;

	if (gJerTopCount == 0)
		return original;

	gJerPauseRoot = *original;
	gJerPauseRoot.MenuItems = gJerPauseRootItems;

	src = original->MenuItems;
	n = 0;

	/* copy the first item (Continue), then splice "Modules" right after it
	 * (where the old per-module shells used to live), then the rest */
	if (src->Type != PAUSE_TYPE_ENDITEMS && n < JER_PAUSE_ROOT_ITEMS - 2)
	{
		gJerPauseRootItems[n++] = *src++;
	}

	/* "Modules" container: one SUBMENU entry per registered menu */
	gJerModulesHeader.Title = "JERICHO Addons";
	gJerModulesHeader.Bound.x = gJerModulesHeader.Bound.y = 0;
	gJerModulesHeader.Bound.w = gJerModulesHeader.Bound.h = 0;
	gJerModulesHeader.NumItems = 0;
	gJerModulesHeader.MenuItems = gJerModulesItems;

	for (i = 0; i < gJerTopCount; i++)
	{
		MENU_ITEM* it = &gJerModulesItems[i];

		memset(it, 0, sizeof(*it));
		snprintf(gJerModulesText[i], JER_PAUSE_ITEM_TEXT, "%s", gJerTopHeaders[i]->Title);
		it->Text = gJerModulesText[i];
		it->Type = PAUSE_TYPE_SUBMENU;
		it->Justify = 2;
		it->ExitValue = MENU_QUIT_NONE;
		it->SubMenu = gJerTopHeaders[i];
	}

	memset(&gJerModulesItems[gJerTopCount], 0, sizeof(MENU_ITEM));
	gJerModulesItems[gJerTopCount].Type = PAUSE_TYPE_ENDITEMS;

	/* the Modules entry inside the root copy (right after the first item) */
	if (n < JER_PAUSE_ROOT_ITEMS - 2)
	{
		memset(&gJerPauseRootItems[n], 0, sizeof(MENU_ITEM));
		gJerPauseRootItems[n].Text = "Modules";
		gJerPauseRootItems[n].Type = PAUSE_TYPE_SUBMENU;
		gJerPauseRootItems[n].Justify = 2;
		gJerPauseRootItems[n].SubMenu = &gJerModulesHeader;
		n++;
	}

	/* remaining engine items */
	while (src->Type != PAUSE_TYPE_ENDITEMS && n < JER_PAUSE_ROOT_ITEMS - 1)
	{
		gJerPauseRootItems[n++] = *src++;
	}

	memset(&gJerPauseRootItems[n], 0, sizeof(MENU_ITEM));
	gJerPauseRootItems[n].Type = PAUSE_TYPE_ENDITEMS;

	return &gJerPauseRoot;
}

// [D] [T]
void InitaliseMenu(PAUSEMODE mode)
{
	MENU_HEADER* pNewMenu;
	int i;

	for (i = 0; i < PAUSE_MENU_LEVELS; i++)
	{
		ActiveItem[i] = NULL;
		VisibleMenus[i] = NULL;
	}

	// JERICHO-HOOK: (re)build the module-provided pause menus whenever the
	// pause opens (this also refreshes their dynamic labels).
	JerPauseBuildModuleMenus();

	pNewMenu = NULL;

	allownameentry = 0;

	switch (mode) 
	{
		case PAUSEMODE_PAUSE:
		case PAUSEMODE_PAUSEP1:
		case PAUSEMODE_PAUSEP2:
			if (NumPlayers == 1 && gMultiplayerLevels == 0) 
			{
				if (gInGameCutsceneActive == 0)
					pNewMenu = JerPauseRootOr(&PauseMenuHeader);
				else 
					pNewMenu = &CutscenePauseMenuHeader;
			}
			else 
				pNewMenu = &MultiplayerPauseHeader;

			break;
		case PAUSEMODE_GAMEOVER:
			switch (GameType) 
			{
				case GAME_PURSUIT:
					pNewMenu = &ChaseGameFinishedHeader;
					gMissionCompletionState = mode;
					break;
				case GAME_GETAWAY:
				case GAME_CHECKPOINT:
					if (NumPlayers == 1)
					{
						pNewMenu = &DrivingGameFinishedHeader;
						gMissionCompletionState = mode;
					}
					else
					{
						pNewMenu = &MultiplayerFinishedHeader;
						gMissionCompletionState = mode;
					}
					break;
				case GAME_GATERACE:
				case GAME_TRAILBLAZER:
				case GAME_SURVIVAL:
					if (NumPlayers == 1)
					{
						pNewMenu = &DrivingGameFinishedHeader;
						gMissionCompletionState = mode;
						allownameentry = 1;
					}
					else
					{
						pNewMenu = &MultiplayerFinishedHeader;
						gMissionCompletionState = mode;
					}
					break;
				default:
					if (NumPlayers == 1)
					{
						pNewMenu = &TakeARideFinishedHeader;
						gMissionCompletionState = mode;
					}
					else
					{
						pNewMenu = &MultiplayerFinishedHeader;
						gMissionCompletionState = mode;
					}
					break;
			}
			break;
		case PAUSEMODE_COMPLETE:

			switch (GameType) 
			{
				case GAME_MISSION:
					pNewMenu = &MissionCompleteHeader;
					gMissionCompletionState = mode;
					break;
				case GAME_GETAWAY:
				case GAME_GATERACE:
				case GAME_CHECKPOINT:
				case GAME_TRAILBLAZER:
				case GAME_SURVIVAL:
				case GAME_COPSANDROBBERS:
				case GAME_CAPTURETHEFLAG:
					if (NumPlayers == 1) 
					{
						pNewMenu = &DrivingGameFinishedHeader;
						gMissionCompletionState = mode;
						allownameentry = 1;
					}
					else
					{
						pNewMenu = &MultiplayerFinishedHeader;
						gMissionCompletionState = mode;
					}
					break;
				case GAME_PURSUIT:
					pNewMenu = &ChaseGameFinishedHeader;
					gMissionCompletionState = mode;
					break;
				default:
					if (NumPlayers == 1)
					{
						pNewMenu = &TakeARideFinishedHeader;
						gMissionCompletionState = mode;
					}
					else
					{
						pNewMenu = &MultiplayerFinishedHeader;
						gMissionCompletionState = mode;
					}
					break;
			}
			break;
		case PAUSEMODE_PADERROR:
			if (pad_connected < 0) 
			{
				if (NumPlayers == 1) 
					pNewMenu = &InvalidPadHeader;
				else
					pNewMenu = &InvalidMultiPadHeader;

				if (Pads[0].type == 1)
					pNewMenu->Title = G_LTXT(GTXT_IncorrectController1);
				else
					pNewMenu->Title = G_LTXT(GTXT_IncorrectController2);
			}
			else 
			{
				if (NumPlayers == 1) 
					pNewMenu = &NoPadHeader;
				else
					pNewMenu = &NoMultiPadHeader;

				if (Pads[0].type == 0)
					pNewMenu->Title = G_LTXT(GTXT_InsertController1);
				else
					pNewMenu->Title = G_LTXT(GTXT_InsertController2);
			}
			break;
	}

	// [A]
	if(pNewMenu)
	{
		VisibleMenu = 0;
		VisibleMenus[VisibleMenu] = pNewMenu;

		if (NoPlayerControl == 0 && OnScoreTable(NULL) != -1 && allownameentry)
		{
			gScoreEntered = 0;

			sprintf(EnterScoreText, G_LTXT(GTXT_EnterScore));
			sprintf(EnterNameText, G_LTXT(GTXT_EnterName));
		}
		else
		{
			gScoreEntered = 1;

			sprintf(EnterScoreText, G_LTXT(GTXT_ViewTable));
			sprintf(EnterNameText, G_LTXT(GTXT_HighScores));
		}

		SetupMenu(pNewMenu, 0);
	}
	else
	{
		printError("pNewMenu is NULL!\n");
	}
}

// [D] [T]
void DrawVisibleMenus(void)
{
	int width;
	int itemXpos;
	POLY_FT3 *null;
	POLY_F4 *poly;
	MENU_ITEM *pItem;
	MENU_HEADER *pActive;
	int r, b;
	int i;
	int xpos, ypos;
	int menuWidth;
	
	int x1, x2, y1, y2;
	static int maxPercentageWidth = StringWidth(" 100");

	if (NumPlayers > 1)
	{
		SetFullscreenDrawing(2);
	}

	pActive = VisibleMenus[VisibleMenu];

	xpos = pActive->Bound.x;
	ypos = pActive->Bound.y;
	menuWidth = pActive->Bound.w;
	
	if (pActive->Title)
	{
		OutputString(GET_GAME_TXT(pActive->Title), 2, xpos, ypos, menuWidth, 128, 32, 32);

		ypos += 15;
	}

	// show all menu items
	for (i = 0; i < pActive->NumItems; i++)
	{
		pItem = &pActive->MenuItems[i];

		if (!pItem->Text)
			continue;
	
		if (pItem == ActiveItem[VisibleMenu])
		{
			r = 0;
			b = 0;
		}
		else
		{
			b = 128;
			r = 128;
		}

		if(pItem->Type & (PAUSE_TYPE_SFXVOLUME | PAUSE_TYPE_MUSICVOLUME))
		{
			width = StringWidth(GET_GAME_TXT(pItem->Text));

			itemXpos = xpos + ((menuWidth - width) - maxPercentageWidth) / 2;

			OutputString(GET_GAME_TXT(pItem->Text), 1, itemXpos, ypos, menuWidth, r, 128, b);
			
			if (pItem->Type & PAUSE_TYPE_SFXVOLUME)
				OutputString(SfxVolumeText, 1, itemXpos + width + 10, ypos, menuWidth, r, 128, b);
			else if (pItem->Type & PAUSE_TYPE_MUSICVOLUME)
				OutputString(MusicVolumeText, 1, itemXpos + width + 10, ypos, menuWidth, r, 128, b);
		}
		else
		{
			OutputString(GET_GAME_TXT(pItem->Text), pItem->Justify, xpos, ypos, menuWidth, r, 128, b);
		}

		ypos += 15;
	}

	poly = (POLY_F4*)current->primptr;

	ypos = pActive->Bound.y;
	
	setPolyF4(poly);
	x1 = xpos - 5;
	x2 = xpos + menuWidth + 5;

	y1 = ypos - 5;
	y2 = ypos + pActive->Bound.h;

	poly->x0 = x1;
	poly->x1 = x2;
	poly->y0 = y1;
	poly->x2 = x1;

	poly->y1 = y1;
	poly->y2 = y2;

	poly->x3 = x2;
	poly->y3 = y2;

	poly->r0 = 16;
	poly->g0 = 16;
	poly->b0 = 16;

	setSemiTrans(poly, 1);

	addPrim(current->ot, poly);
	current->primptr += sizeof(POLY_F4);

	null = (POLY_FT3 *)current->primptr;
	setPolyFT3(null);
	null->x0 = -1;
	null->y0 = -1;
	null->x1 = -1;
	null->y1 = -1;
	null->x2 = -1;
	null->y2 = -1;
	null->tpage = 0;

	addPrim(current->ot, null);

	current->primptr += sizeof(POLY_FT3);
}

// [D] [T]
void ControlMenu(void)
{
	static int controlmenu_debounce = 0;
	MENU_ITEM* pItem;
	int i;
	ushort paddata;
	MENU_HEADER* menu;
	ushort paddirect;
	int doit;

	if (playerwithcontrol[2] == 0)
	{
		paddata = Pads[1].dirnew;
		paddirect = Pads[1].direct;

		if (playerwithcontrol[0])
		{
			paddata = Pads[0].dirnew;
			paddirect = Pads[0].direct;
		}
	}
	else
	{
		paddata = Pads[0].dirnew;
		paddirect = Pads[0].direct;
	
		if (NumPlayers == 2)
		{
			paddata = Pads[1].dirnew | Pads[0].dirnew;
			paddirect = Pads[1].direct | Pads[0].direct;
		}
	}

	// toggle map off
	if (gShowMap)
	{
		// JERICHO-HOOK: modules may claim the map input (e.g. a teleport
		// cursor) — the engine then leaves the map open to them.
		{
			JER_ARGS_MAP jerArgs;

			jerArgs.action = JER_MAP_ACTION_INPUT;
			jerArgs.value = paddata;
			jerArgs.result = NULL;

			if (jer_fire(JER_EVENT_MAP, &jerArgs) == JER_RESULT_STOP)
				return;
		}

		if (paddata & (MPAD_CROSS | MPAD_TRIANGLE))
			PauseMap(0);

		return;
	}

	pItem = ActiveItem[VisibleMenu];

	if ((paddirect & 0xA000) && (pItem->Type & PAUSE_TYPE_DIRFUNC))
	{
		doit = 0;

		if (controlmenu_debounce == 0)
		{
			doit = 1;
			controlmenu_debounce = 10;
		}
		else
		{
			controlmenu_debounce--;

			if (controlmenu_debounce == 0)
			{
				doit = 1;
				controlmenu_debounce = 2;
			}
		}

		if (doit)
		{
			// left/right
			(*pItem->func)((paddirect & 0x8000) ? -1 : 1);
		}

		return;
	}

	controlmenu_debounce = 0;

#ifndef PSX
	// Pause fix for PC mapping
	if ((paddata & MPAD_TRIANGLE) && paddata & (MPAD_D_UP | MPAD_D_DOWN))
	{
		paddata = 0;
	}
#endif

	if (paddata & MPAD_D_UP)
	{
		// go up
		ActiveMenuItem--;
		
		if (ActiveMenuItem < 0)
			ActiveMenuItem = ActiveMenu->NumItems - 1;

		ActiveItem[VisibleMenu] = &ActiveMenu->MenuItems[ActiveMenuItem];
	}
	else if (paddata & MPAD_D_DOWN)
	{
		// go down
		ActiveMenuItem++;
		
		if (ActiveMenuItem > ActiveMenu->NumItems - 1)
			ActiveMenuItem = 0;

		ActiveItem[VisibleMenu] = &ActiveMenu->MenuItems[ActiveMenuItem];
	}
	else if (paddata & MPAD_CROSS)
	{
		// Enter submenu
		if (pItem->Type & PAUSE_TYPE_SUBMENU)
		{
			menu = pItem->SubMenu;
	
			VisibleMenu++;
			VisibleMenus[VisibleMenu] = menu;
	
			SetupMenu(menu, 0);
			return;
		}

		// function flag
		if (pItem->Type & PAUSE_TYPE_FUNC)
			(*pItem->func)(0);
			
		if (pItem->ExitValue == MENU_QUIT_NONE)
			return;

		if (pItem->ExitValue == MENU_QUIT_BACKMENU)
		{
			VisibleMenu--;		// go back in menu stack
			SetupMenu(VisibleMenus[VisibleMenu], 1);
		}
		else
			PauseReturnValue = pItem->ExitValue;
	}
	else if ((paddata & MPAD_TRIANGLE) || (paddata & MPAD_START)) // Triangle or Start
	{
		// continue game if needed

		if (VisibleMenu == 0)
		{
#ifndef PSX
			// hack for keyboard swap
			if(!(paddata & MPAD_START))
				return;
#endif
			for (i = 0; i < ActiveMenu->NumItems; i++)
			{
				pItem = &ActiveMenu->MenuItems[i];

				if (pItem->ExitValue == MENU_QUIT_CONTINUE)
				{
					PauseReturnValue = pItem->ExitValue;
					return;
				}
			}
		}
		else
		{
			// only triangle
			if(paddata & 0x10)
			{
				VisibleMenu--;
				SetupMenu(VisibleMenus[VisibleMenu], 1);
			}
		}
	}
}

// [D] [T]
void PauseMap(int direction)
{
	gShowMap ^= 1;

	ReadControllers();

	map_x_shift = 0;
	map_z_shift = 0;

	if (gShowMap == 0)
		InitOverheadMap();
}

// [D] [T]
void SfxVolume(int direction)
{
	if (direction < 0) 
		gMasterVolume = gMasterVolume + -100;
	else if (0 < direction)
		gMasterVolume = gMasterVolume + 100;

	if (gMasterVolume < -10000) 
		gMasterVolume = -10000;

	if (gMasterVolume > 0) 
		gMasterVolume = 0;

	sprintf(SfxVolumeText, "%d", (10000 + gMasterVolume) / 100);

	SetMasterVolume(gMasterVolume);
}

// [D] [T]
void MusicVolume(int direction)
{
	if (direction < 0)
		gMusicVolume = gMusicVolume + -100;
	else if (0 < direction) 
		gMusicVolume = gMusicVolume + 100;

	if (gMusicVolume < -10000)
		gMusicVolume = -10000;

	if (gMusicVolume > 0) 
		gMusicVolume = 0;

	sprintf(MusicVolumeText, "%d", (10000 + gMusicVolume) / 100);

	SetXMVolume(gMusicVolume);
}

// [D] [T]
void EnterScoreName(void)
{
	u_char chr;
	char* username;
	char* enteredName;

	SCORE_ENTRY* table;
	unsigned short npad, dpad;

	username = NULL;

	if (!gScoreEntered) 
	{
		gScorePosition = OnScoreTable(&table);

		if (gScorePosition != -1) 
			username = ScoreName[gScorePosition];

		CreateScoreNames(table, &gPlayerScore, gScorePosition);
	}
	else
	{
		gScorePosition = -1;
	}

	gEnteringScore = 1;

	enteredName = WaitForTextEntry(username, SCORE_NAME_LEN);

	if (enteredName && username)
	{
		strcpy(gPlayerScore.name, enteredName);

		AddScoreToTable(table, gScorePosition);

		sprintf(EnterScoreText, G_LTXT(GTXT_ViewTable));
		sprintf(EnterNameText, G_LTXT(GTXT_HighScores));

		gScoreEntered = 1;
	}

	gEnteringScore = 0;
}

void DisplayMenuMessage(char* header, char* text)
{
	POLY_FT3* null;
	POLY_F4* prim;
	int i;
	int ypos;

	OutputString(header, 2, 160, 105, 0, 128, 32, 32);
	ypos = 125;
	OutputString(text, 2, 160, ypos, 0, 128, 128, 128);

	// TODO: Multiline support
	/*
	i = 0;
	do {
		OutputString(text, 2, 160, ypos, 0, 128, 128, 128);

		ypos += 15;

		i++;
	} while (i < 5);
	*/
	prim = (POLY_F4*)current->primptr;
	setPolyF4(prim);
	setSemiTrans(prim, 1);
	setShadeTex(prim, 1);

	prim->y0 = 100;
	prim->y1 = 100;
	prim->y2 = 160;
	prim->y3 = 160;
	prim->x0 = 80;
	prim->x1 = 240;
	prim->x2 = 80;
	prim->x3 = 240;

	prim->r0 = 16;
	prim->g0 = 16;
	prim->b0 = 16;

	addPrim(current->ot, prim);
	current->primptr += sizeof(POLY_F4);

	null = (POLY_FT3*)current->primptr;
	setPolyFT3(null);

	null->x0 = -1;
	null->y0 = -1;
	null->x1 = -1;
	null->y1 = -1;
	null->x2 = -1;
	null->y2 = -1;
	null->tpage = 0;

	addPrim(current->ot, null);
	current->primptr += sizeof(POLY_FT3);
}

// [D] [T]
void DrawHighScoreMenu(int selection)
{
	POLY_FT3* null;
	POLY_F4* prim;
	int i;
	int r,b;
	int ypos;
	char text[8];

	OutputString(EnterNameText, 2, 160, 70, 0, 128, 32, 32);
	OutputString(G_LTXT(GTXT_Name), 1, 40, 90, 0, 128, 128, 32);
	OutputString(G_LTXT(GTXT_Time), 4, 280, 90, 0, 128, 128, 32);

	ypos = 110;

	i = 0;
	do {
		
		if (i == selection) 
		{
			r = 0;
			b = 0;
		}
		else
		{
			r = 128;
			b = 128;
		}

		sprintf(text, "%d", i+1);

		OutputString(text, 1, 40, ypos, 0, r, 128, b);

		OutputString(ScoreName[i], 1, 60, ypos, 0, r, 128, b);
		OutputString(ScoreItems[i], 4, 220, ypos, 0, r, 128, b);
		OutputString(ScoreTime[i], 4, 280, ypos, 0, r, 128, b);

		ypos += 15;

		i++;
	} while (i < 5);

	prim = (POLY_F4*)current->primptr;
	setPolyF4(prim);
	setSemiTrans(prim, 1);
	setShadeTex(prim, 1);

	prim->y0 = 65;
	prim->y1 = 65;
	prim->y2 = 197;
	prim->y3 = 197;
	prim->x0 = 26;
	prim->x1 = 294;
	prim->x2 = 26;
	prim->x3 = 294;

	prim->r0 = 16;
	prim->g0 = 16;
	prim->b0 = 16;

	addPrim(current->ot, prim);
	current->primptr += sizeof(POLY_F4);

	null = (POLY_FT3*)current->primptr;
	setPolyFT3(null);

	null->x0 = -1;
	null->y0 = -1;
	null->x1 = -1;
	null->y1 = -1;
	null->x2 = -1;
	null->y2 = -1;
	null->tpage = 0;

	addPrim(current->ot, null);
	current->primptr += sizeof(POLY_FT3);
}

int mytolower(int ch)
{
	if (ch >= 'A' && ch <= 'Z')
		return ('a' + ch - 'A');
	else
		return ch;
}
	
void strlower(char* str)
{
	while (*str != '\0')
	{
		*str = mytolower(*str);
		str++;
	}
}

// [D] [T]
void CreateScoreNames(SCORE_ENTRY* table, PLAYER_SCORE* score, int position)
{
	int time;
	char* text;
	int i;

	switch (GameType) 
	{
		case GAME_PURSUIT:
		case GAME_GETAWAY:
		case GAME_CHECKPOINT:
		case GAME_SURVIVAL:
			text = NULL;
			break;
		case GAME_GATERACE:
			text = G_LTXT(GTXT_Gates);
			break;
		case GAME_TRAILBLAZER:
			text = G_LTXT(GTXT_Cones);
			break;
		default:
			printError("CreateScoreNames: Invalid game type\n");
			return;
	}

	i = 0;
	do {
		if (i == position)
		{
			time = score->time;
			if (time == -1)
				sprintf(ScoreTime[i], "-:--.--");
			else
				sprintf(ScoreTime[i], "%d:%02d.%02d", time / 180000, time / 3000 + (time / 180000) * -0x3c, (time % 3000) / 0x1e);

			ScoreItems[i][0] = '\0';

			if (text != NULL && score->items != -1)
			{
				sprintf(ScoreItems[i], "%d %s", score->items, text);
				strlower(ScoreItems[i]);
			}

			ClearMem(ScoreName[i], 7);
		}
		else 
		{
			time = table->time;

			if (time == -1)
				sprintf(ScoreTime[i], "-:--.--");
			else
				sprintf(ScoreTime[i], "%d:%02d.%02d", time / 180000, time / 3000 + (time / 180000) * -0x3c, (time % 3000) / 0x1e);

			ScoreItems[i][0] = '\0';

			if (text != NULL && table->items != -1)
			{
				sprintf(ScoreItems[i], "%d %s", table->items, text);
				strlower(ScoreItems[i]);
			}

			sprintf(ScoreName[i], "%s", table->name);			
			table++;
		}

		i++;
	} while (i < 5);

}

// [A]
int UpdatePauseMenu(PAUSEMODE mode)
{
	PAUSEMODE passed_mode;
	passed_mode = mode;

	if (mode == PAUSEMODE_PADERROR)
		mode = PAUSEMODE_PAUSE;
	
	if (passed_mode == PAUSEMODE_PADERROR)
	{
		if (pad_connected == 1)
			InitaliseMenu(mode);
		else
			InitaliseMenu(PAUSEMODE_PADERROR);
	}
	else
	{
		if (pad_connected != 1)
			InitaliseMenu(PAUSEMODE_PADERROR);
	}

	if (pad_connected < 1)
		playerwithcontrol[2] = 1;

	ControlMenu();

	if (PauseReturnValue != 0)
		gDrawPauseMenus = 0;

	return PauseReturnValue;
}

// [D] [T]
void ShowPauseMenu(PAUSEMODE mode)
{
	ReadControllers();

	if (mode == PAUSEMODE_PAUSEP1)
	{
		playerwithcontrol[0] = 1;
		playerwithcontrol[1] = 0;
		playerwithcontrol[2] = 0;
	}
	else if (mode == PAUSEMODE_PAUSEP2)
	{
		playerwithcontrol[0] = 0;
		playerwithcontrol[1] = 1;
		playerwithcontrol[2] = 0;
	}
	else
	{
		playerwithcontrol[0] = 0;
		playerwithcontrol[1] = 0;
		playerwithcontrol[2] = 1;
	}

	SetDispMask(1);

	SfxVolume(0);
	MusicVolume(0);

	StopPadVibration(0);
	StopPadVibration(1);

	InitaliseMenu(mode);
	gDrawPauseMenus = 1;
		
	PauseReturnValue = 0;
}

// [D] [T]
void DrawPauseMenus(void)
{
	int displayMessage;

#if !defined(PSX) && defined(DEBUG_OPTIONS)
	extern int g_FreeCameraEnabled;

	if (g_FreeCameraEnabled)
		return;
#endif

	displayMessage = gDisplayedMessage.show == -1 || gDisplayedMessage.show > 0;

	if ((gDrawPauseMenus || displayMessage) && gShowMap == 0)
	{
		if (displayMessage)
		{
			DisplayMenuMessage(gDisplayedMessage.header, gDisplayedMessage.text);

			if(gDisplayedMessage.show > 0)
				gDisplayedMessage.show--;
		}
		else if (gEnteringScore)
			DrawHighScoreMenu(gScorePosition);
		else
			DrawVisibleMenus();
	}
}