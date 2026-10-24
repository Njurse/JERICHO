#include "driver2.h"

#include "jericho.h"		/* jer_fire */
#include "jer_notify.h"	/* car select: the palette notice */
#include "../C/jer_events.h"	/* JER_EVENT_CAR_AVAILABILITY */
#include "../C/JERICHO/include/jer_texture.h"	/* custom menu background */
#include "../C/JERICHO/include/jer_console.h"	/* unified status console stream */
#include "../C/JERICHO/include/jer_text.h"	/* measuring and placing text (the prompt) */
#include "../C/jer_menu_bg.h"	/* the background itself */

// JERICHO: -shotfront <frame> (set and counted in main.c) and PsyCross's live-window
// screenshot. PsyX_TakeScreenshot is NOT in PsyCross's extern "C" block, so it is
// declared without C linkage - same reasoning as in main.c.
extern int gScreenshotFrameFront;
extern int gFrontendFrames;
void PsyX_TakeScreenshot(void);

#include "FEmain.h"

#include "C/cd_icon.h"

#include "C/system.h"
#include "C/glaunch.h"
#include "C/mission.h"
#include "C/time.h"
#include "C/loadsave.h"
#include "C/pad.h"
#include "C/loadview.h"
#include "C/pres.h"
#include "C/sound.h"
#include "C/debris.h"
#include "C/E3stuff.h"
#include "C/fmvplay.h"
#include "C/gamesnd.h"
#include "C/scores.h"
#include "C/loadsave.h"
#include "C/spool.h"
#include "C/state.h"

#include <stdlib.h>	// JERICHO-HOOK: getenv/atoi — the frontend press test lever only

#include "jericho.h"	// JERICHO-HOOK: mod runtime (mods manager frontend)
#include "../C/jer_events.h"	// JERICHO-HOOK: event argument structs
#include "../C/JERICHO/include/jer_frontend.h"	// JERICHO-HOOK: module frontend menus
#include "../C/JERICHO/include/jer_screen.h"	// JERICHO-HOOK: presentation screens
#include "../C/JERICHO/include/jer_menu.h"	// JERICHO-HOOK: jer_menu_draw (shared menu look)
#include "../C/JERICHO/include/jer_prompt.h"	// JERICHO-HOOK: host yes/no prompt
#include "../C/JERICHO/include/jer_text.h"	// JERICHO: the credits page text

#ifndef PSX

#include "PsyX/PsyX_render.h"
#include "../utils/targa.h"
#include "../utils/hqfont.h"

#define HIRES_FONTS

struct FEFONT_QUAD
{
	float x0, y0, s0, t0; // top-left
	float x1, y1, s1, t1; // bottom-right
};

TextureID gHiresFEFontTexture = 0;
OUT_FN2RANGE gHiresFEFontRanges[4];
OUT_FN2INFO gHiresFEFontCharData[4][224];
int gHiresFEFontRangeCount = 0;


void InitHiresFEFont()
{
	char namebuffer[64];
	u_char* data;

	// init font2
	if (!gHiresFEFontTexture)
	{
		gHiresFEFontRangeCount = 0;

		int width, height, bpp;
		int x, y;
		int size;
		FILE* fp;
		sprintf(namebuffer, "%s%s", gDataFolder, "GFX\\HQ\\fefont.fn2");
		FS_FixPathSlashes(namebuffer);

		fp = fopen(namebuffer, "rb");
		if (fp)
		{
			int i;

			// read fn2 step by step
			OUT_FN2HEADER fn2hdr;
			fread(&fn2hdr, sizeof(fn2hdr), 1, fp);

			gHiresFEFontRangeCount = fn2hdr.range_count;
			for (i = 0; i < fn2hdr.range_count; ++i)
			{
				fread(&gHiresFEFontRanges[i], sizeof(gHiresFEFontRanges[i]), 1, fp);
				fread(gHiresFEFontCharData[i], sizeof(OUT_FN2INFO), gHiresFEFontRanges[i].count, fp);
			}

			fclose(fp);
		}

		// load TGA file
		sprintf(namebuffer, "%s%s", gDataFolder, "GFX\\HQ\\fefont.tga");
		FS_FixPathSlashes(namebuffer);
		
		if (LoadTGAImage(namebuffer, &data, width, height, bpp))
		{
			if (bpp == 32)
			{
				gHiresFEFontTexture = GR_CreateRGBATexture(HIRES_FONT_SIZE_W, HIRES_FONT_SIZE_H, data);
			}
			free(data);
		}
	}
}

int FEGetHiresBakedQuad(int char_index, float scale, float* xpos, float* ypos, FEFONT_QUAD* q)
{
	float ipw = 1.0f / (float)HIRES_FONT_SIZE_W;
	float iph = 1.0f / (float)HIRES_FONT_SIZE_H;

	const int idx = char_index - gHiresFEFontRanges[0].start;
	if (idx < 0)
		return 0;
	const OUT_FN2INFO* b = gHiresFEFontCharData[0] + idx;

	float fscale = 0.5f * scale;

	float s_x = b->x1 - b->x0;
	float s_y = b->y1 - b->y0;

	q->x0 = *xpos + b->xoff * fscale;
	q->y0 = *ypos + b->yoff * fscale;
	q->x1 = (b->xoff2 - b->xoff) * fscale;
	q->y1 = (b->yoff2 - b->yoff) * fscale;

	q->s0 = b->x0 * 255.0f * ipw;
	q->t0 = b->y0 * 255.0f * iph;
	q->s1 = s_x * 255.0f * ipw;
	q->t1 = s_y * 255.0f * iph;

	q->y0 += 32.0f;

	*xpos += b->xadvance * fscale;

	return 1;
}

void SetHiresFEFontTexture(int enabled)
{
	if (gHiresFEFontTexture == 0)
	{
		return;
	}

	DR_PSYX_TEX* tex = (DR_PSYX_TEX*)current->primptr;
	if (enabled)
		SetPsyXTexture(tex, gHiresFEFontTexture, 255, 255);
	else
		SetPsyXTexture(tex, 0, 0, 0);

	addPrim(current->ot + 1, tex);
	current->primptr += sizeof(DR_PSYX_TEX);
}

int FEStringWidthHires(char* string)
{
	char* pString = string;
	u_char c = 0;

	int w = 0;

	while ((c = *pString++) != 0)
	{
		float fx, fy;
		fx = 0;
		fy = 0;
		FEFONT_QUAD q;
		if (!FEGetHiresBakedQuad(c, 1.0f, &fx, &fy, &q))
			continue;

		w += fx;
	}

	return w;
}

int FEPrintStringSizedHires(char* string, int x, int y, int scale, int transparent, int r, int g, int b)
{
	if (current == NULL || string == NULL)
		return -1;

	POLY_FT4* shadow;
	POLY_FT4* font;
	u_char let;
	int w;
	int h;

	SetHiresFEFontTexture(0);
	font = (POLY_FT4*)current->primptr;

	while ((let = *string++) != 0)
	{
		if (let == '\n')
			continue;

		float fx, fy;
		fx = x;
		fy = y;
		FEFONT_QUAD q;
		if (!FEGetHiresBakedQuad(let, scale / 4096.0f, &fx, &fy, &q))
			continue;

		setPolyFT4(font);
		setSemiTrans(font, 1);

		setRGB0(font, r, g, b);
		setUVWH(font, q.s0, q.t0, q.s1, q.t1);
		setXYWH(font, q.x0, q.y0, q.x1, q.y1);

		font->clut = 0;
		font->tpage = 0;

		addPrim(current->ot + 1, font);
		shadow = font + 1;

		// add shadow poly
		memcpy(shadow, font, sizeof(POLY_FT4));
		setRGB0(shadow, 10, 10, 10);
		setXYWH(shadow, q.x0 + 1.0f, q.y0 + 1.0f, q.x1, q.y1);

		addPrim(current->ot + 1, shadow);
		font += 2;

		// make room for next character
		x += fx - x;
	}

	// set tail
	current->primptr = (char*)font;
	SetHiresFEFontTexture(1);

	return x;
}

int FEPrintStringHires(char* string, float x, float y, int justification, int r, int g, int b)
{
	POLY_FT4* shadow;
	POLY_FT4* font;
	u_char let;

	if (justification & 4)
	{
		x -= FEStringWidthHires(string);
	}

	SetHiresFEFontTexture(0);
	font = (POLY_FT4*)current->primptr;

	int counter = 0;

	while ((let = *string++) != 0)
	{
		float fx, fy;
		fx = x;
		fy = y;
		FEFONT_QUAD q;
		FEGetHiresBakedQuad(let, 1.0f, &fx, &fy, &q);

		setPolyFT4(font);
		setSemiTrans(font, 1);

		setRGB0(font, r, g, b);
		setUVWH(font, q.s0, q.t0, q.s1, q.t1);
		setXYWH(font, q.x0, q.y0, q.x1, q.y1);

		font->clut = 0;
		font->tpage = 0;

		addPrim(current->ot + 1, font);
		shadow = font + 1;

		// add shadow poly
		memcpy(shadow, font, sizeof(POLY_FT4));
		setRGB0(shadow, 10, 10, 10);
		setXYWH(shadow, q.x0 + 1.0f, q.y0 + 1.0f, q.x1, q.y1);

		addPrim(current->ot + 1, shadow);
		font += 2;

		// add space for next character
		x += fx - x;

		if (++counter >= 32)
			break;
	}

	// set tail
	current->primptr = (char *)font;

	SetHiresFEFontTexture(1);

	return x;
}

#endif // PSX

struct PSXBUTTON
{
	short x, y, w, h;
	u_char l, r;		// left, right next item id
	u_char u, d;		// up, down next item id
	int userDrawFunctionNum;
	short s_x, s_y;
	int action, var;
	char Name[32];
};

struct PSXSCREEN
{
	u_char index;
	u_char numButtons;
	u_char userFunctionNum;
	PSXBUTTON buttons[12];	/* JERICHO-HOOK: was 8 — module menus (jer_frontend.h) need more rows */
};

// #define USE_EMBEDDED_FRONTEND_SCREENS

enum FEButtonAction
{
	BTN_NEXT_SCREEN = 1,
	BTN_START_GAME = 2,
	BTN_DISABLED = 3,
	BTN_PREVIOUS_SCREEN = 4,
	BTN_HIDDEN = 5,
	BTN_MODULE = 6		/* handled by a module's frontend menu (jer_frontend.h) */
};

#define FE_MAKEVAR(code, value)		((code & 0xffff) << 8 | (value & 0xff))

#ifdef USE_EMBEDDED_FRONTEND_SCREENS
#include "FEscreens.inc"
#else
PSXSCREEN PsxScreens[42 + JER_FE_MAX_MENUS];
#endif

#define FE_OTSIZE 16

struct FE_CHARDATA
{
	u_char u;
	u_char v;
	u_char w;
	u_char h;
};

struct FE_FONT
{
	int NumFonts;
	FE_CHARDATA CharInfo[256];
};

struct SCREEN_LIMITS
{
	short minx, miny, maxx, maxy;
};

struct BOTCH
{
	int missNum;
	char** name;
};

char* NullStr = "\0";

typedef int(*screenFunc)(int bSetup);

extern ACTIVE_CHEATS AvailableCheats;
extern ACTIVE_CHEATS ActiveCheats;

int CentreScreen(int bSetup); // 0x001C31FC
int CarSelectScreen(int bSetup); // 0x001C3430
int CopDiffLevelScreen(int bSetup); // 0x001C6404
int VibroOnOffScreen(int bSetup); // 0x001C3A60
int MissionSelectScreen(int bSetup); // 0x001C3B1C
int MissionCityScreen(int bSetup); // 0x001C4338
int CutSceneSelectScreen(int bSetup); // 0x001C4600
int CutSceneCitySelectScreen(int bSetup); // 0x001C4B30
int SetVolumeScreen(int bSetup); // 0x001C50B0
int ScoreScreen(int bSetup); // 0x001C59A0
int SubtitlesOnOffScreen(int bSetup); // 0x001C64B0
int CityCutOffScreen(int bSetup); // 0x001C64FC
int ControllerScreen(int bSetup); // 0x001C6548
int MainScreen(int bSetup); // 0x001C6614
int CheatScreen(int bSetup); // 0x001C5B4C
int ImmunityOnOffScreen(int bSetup); // 0x001C6654
int InvincibleOnOffScreen(int bSetup); // 0x001C66A0
int GamePlayScreen(int bSetup); // 0x001C66EC
int GameNameScreen(int bSetup); // 0x001C60A0
int CheatNumlayerSelect(int bSetup); // 0x001C6724
int UserReplaySelectScreen(int bSetup);
int TimeOfDaySelectScreen(int bSetup);
int DemoScreen(int bSetup);
int MiniCarsOnOffScreen(int bSetup);
int JerichoModsScreen(int bSetup);
int JerFrontendMenuScreen(int bSetup);	// JERICHO-HOOK: module frontend menus

// JERICHO-HOOK: the runtime frontend screens a Take-a-Ride passes through
// (DATA\SCRS.BIN): 0 main menu, 1 the city list, 3 time of day, 14 car select.
// Screen 1 is the one whose confirm asks Singleplayer vs Multiplayer.
#define JERICHO_TAKEARIDE_CITY_SCREEN 1

// JERICHO-HOOK: frontend Mods manager screen (built into a spare screen slot)
#define JERICHO_MODS_SCREEN 41
// JERICHO-HOOK: module-provided frontend menus live in slots 42..(42+MAX-1)
#define JERICHO_FE_SCREEN_BASE 42


// JERICHO-HOOK: safety cap on the boot presentation-screen loop (jer_screen.h):
// a screen whose on_update never reports finished must not hang the frontend.
#define JER_SCREEN_BOOT_MAX_FRAMES 100000

static int gJerichoOptionsButtonAdded;
static int gJerichoModsNeedSetup;	// a page change needs the buttons rebuilt
static int gJerichoCreditsOpen;	// the mods screen is showing its Credits page
static int gJerichoCreditsPending;	// ...opened by the Mods menu, so entry must not clear it
static void JerichoDrawCredits(void);

// JERICHO: the update check lives in the runtime (jer_update.c), which is built
// without the exe's JERICHO_RELEASE_MODS, so the exe answers the flavour for it.
#ifdef __cplusplus
extern "C" {
#endif
void jer_update_frontend_tick(int onMainMenu);
void jer_rebake_frontend_tick(int onMainMenu);	/* jer_rebake.c: ask to re-bake a stale D1 bake */
int jer_update_screen_active(void);		/* jer_update.c: the update notes own the screen */
int jer_build_is_dev(void);
void jer_modstore_frontend_tick(int onMainMenu);	/* jer_modstore.c: polls the store's helper */
void jer_modstore_open_menu(void);	/* jer_modstore.c: open the Mods table menu */
/* The three host-side actions the Mods menu calls back into. Declared HERE, with
 * C linkage, so the definitions further down inherit it: jer_modstore.c is
 * compiled against their plain names. */
int  jer_mods_compile_available(void);
void jer_mods_request_compile(void);
void jer_mods_open_credits(void);
/* Open the Mods screen the way the Options button does (a dev/test entry). */
void jer_mods_open_screen(void);
/* Quit the game from a host action (the rebuild prompt's Yes): the engine's own
 * quit path unwinds the frontend, closes the log and releases the audio device
 * before the detached build replaces the exe. */
void jer_frontend_quit(void);
void jer_frontend_dim_rect(int x, int y, int w, int h, int shade);
#ifdef __cplusplus
}
#endif
/* Darken a rectangle of the frontend (draw space) by `shade` (0..255), for text
 * that has to read over the busy background. Subtractive, at ot+10: after the
 * background (ot+11), before the highlight (ot+6) and the font (ot+1). One per
 * frame -- the statics stay in the OT until EndFrame. */
void jer_frontend_dim_rect(int x, int y, int w, int h, int shade)
{
	static POLY_F4 dim;
	static DR_TPAGE mode;

	if (current == NULL)
		return;
	setPolyF4(&dim);
	setSemiTrans(&dim, 1);
	setRGB0(&dim, shade, shade, shade);
	setXYWH(&dim, x, y, w, h);
	setDrawTPage(&mode, 1, 0, getTPage(0, 2, 0, 0));	/* abr 2: background - poly */
	addPrim(current->ot + 10, &dim);
	addPrim(current->ot + 10, &mode);	/* head of the slot: applied first */
}

int jer_build_is_dev(void)
{
	return GAME_BUILD_SUFFIX[0] != '\0';	/* version.h: "" on the shipping Release */
}

// JERICHO-HOOK: module-provided frontend menus (jer_frontend.h)
static const JER_FE_MENU* gFeMenus[JER_FE_MAX_MENUS];
static int gFeMenuCount;
static int gFeMainEntry = -1;		// menu index the Multiplayer entry opens
static int gFeNeedSetup;		// rebuild the current module menu screen
static int gFePendingSelect = -1;	// row a refresh should land on (see jer_frontend_refresh_at)
static int gFeMenuEnterFrame;		// suppress module-menu input on the frame it appears
					// (the pad state handed to that first frame can be stale,
					//  which auto-confirmed a row)

screenFunc fpUserFunctions[] = {
	CentreScreen,
	CarSelectScreen,
	CopDiffLevelScreen,
	VibroOnOffScreen,
	MissionSelectScreen,
	MissionCityScreen,
	CutSceneCitySelectScreen,
	CutSceneSelectScreen,
	SetVolumeScreen,
	ScoreScreen,
	SubtitlesOnOffScreen,
	CityCutOffScreen,
	ControllerScreen,
	MainScreen,
	CheatScreen,
	ImmunityOnOffScreen,
	InvincibleOnOffScreen,
	GamePlayScreen,
	GameNameScreen,
	CheatNumlayerSelect,
	UserReplaySelectScreen,
	TimeOfDaySelectScreen,
	DemoScreen,
	MiniCarsOnOffScreen,
	JerichoModsScreen,
	JerFrontendMenuScreen
};

char* gfxNames[4] = {
	"DATA\\CARS\\CCARS.RAW",
	"DATA\\CARS\\HCARS.RAW",
	"DATA\\CARS\\VCARS.RAW",
	"DATA\\CARS\\RCARS.RAW",
};

char* cutGfxNames[4] = {
	"DATA\\CUTS\\CCUTS.RAW",
	"DATA\\CUTS\\HCUTS.RAW",
	"DATA\\CUTS\\VCUTS.RAW",
	"DATA\\CUTS\\RCUTS.RAW",
};

int CutAmounts[5] = {
	0, 8, 7, 5, 8
};

int CutAmountsTotal[5] = {
	0, 8, 15, 20, 40
};

char* CutSceneNames[28] =
{
	M_LTXT_ID(MTXT_RedRiver),
	M_LTXT_ID(MTXT_Themorgue),
	M_LTXT_ID(MTXT_TheWitness),
	M_LTXT_ID(MTXT_Lennysapartment),
	M_LTXT_ID(MTXT_TheCubaConnection),
	M_LTXT_ID(MTXT_TheIntruder),
	M_LTXT_ID(MTXT_MeetingCaine),
	M_LTXT_ID(MTXT_LeavingTown),

	M_LTXT_ID(MTXT_Lookingforalead),
	M_LTXT_ID(MTXT_Movingout),
	M_LTXT_ID(MTXT_Watchingthetruck),
	M_LTXT_ID(MTXT_RosannaSotoClue),
	M_LTXT_ID(MTXT_TheDockyard),
	M_LTXT_ID(MTXT_TheHit),
	M_LTXT_ID(MTXT_SeizingJericho),

	M_LTXT_ID(MTXT_VasquezinVegas),
	M_LTXT_ID(MTXT_TradingJericho),
	M_LTXT_ID(MTXT_CBankjob),
	M_LTXT_ID(MTXT_ThePoolHall),
	M_LTXT_ID(MTXT_CainesWarpath),

	M_LTXT_ID(MTXT_CaineinRio),
	M_LTXT_ID(MTXT_WarningJones),
	M_LTXT_ID(MTXT_TheShootout),
	M_LTXT_ID(MTXT_Lennysgetaway),
	M_LTXT_ID(MTXT_Lennygetsit),
	M_LTXT_ID(MTXT_BackinChicago),
	M_LTXT_ID(MTXT_VasquezmeetsCaine),
	M_LTXT_ID(MTXT_Credits),
};

char *areaNames[4][8] = {
	{
		M_LTXT_ID(MTXT_Downtown),
		M_LTXT_ID(MTXT_Wrigleyville),
		M_LTXT_ID(MTXT_Greektown),
		M_LTXT_ID(MTXT_GrantPark),
		M_LTXT_ID(MTXT_MeigsField),
		M_LTXT_ID(MTXT_UkrainianVillage),
		M_LTXT_ID(MTXT_RiverNorth),
		M_LTXT_ID(MTXT_CabriniGreen),
	},
	{
		M_LTXT_ID(MTXT_NecropolisdeColon),
		M_LTXT_ID(MTXT_Capitolio),
		M_LTXT_ID(MTXT_OldHavana),
		M_LTXT_ID(MTXT_TheDocks),
		M_LTXT_ID(MTXT_Vedado),
		M_LTXT_ID(MTXT_Plaza),
		M_LTXT_ID(MTXT_PlazadelaRevolucion),
		NULL,
	},
	{
		M_LTXT_ID(MTXT_Downtown),
		M_LTXT_ID(MTXT_UpperStrip),
		M_LTXT_ID(MTXT_Lakeside),
		M_LTXT_ID(MTXT_MidStrip),
		M_LTXT_ID(MTXT_NorthVegas),
		M_LTXT_ID(MTXT_Lakes),
		M_LTXT_ID(MTXT_GhostTown),
		NULL,
	},
	{
		M_LTXT_ID(MTXT_Centro),
		M_LTXT_ID(MTXT_Copacabana),
		M_LTXT_ID(MTXT_SantaTereza),
		M_LTXT_ID(MTXT_LagoaRodrigodeFreitas),
		M_LTXT_ID(MTXT_PracadaBandeira),
		M_LTXT_ID(MTXT_Leblon),
		M_LTXT_ID(MTXT_Flamengo),
		NULL,
	},
};

static char gameAreas[64] = {
	// Getaway (0-6)
	0, 1, // Downtown, Wrigleyville
	0, 1, // Necropolis De Colon, Capitolio
	0, 1, // Downtown, Upper Strip
	0, 1, // Centro, Copacabana

	// Gate race (8-14)
	2, 3, // Greektown, Grant Park
	0, 2, // Necropolis de Colon, Old Havana
	2, 3, // Lakeside, Mid Strip
	1, 2, // Copacabana, Santa Tereza

	// Checkpoint (16-22)
	0, 4, // Downtown, Meigs Field
	3, 2, // The Docks, Old Havana
	4, 2, // North Vegas, Lakeside
	3, 4, // Lagoa Rodrigo de Freitas, Praca da Bandeira

	// Trailblazer (24-30)
	3, 0, // Grant Park, Downtown
	2, 4, // Old Havana, Vedado
	0, 1, // Downtown, Upper Strip
	5, 4, // Leblon, Praca da Bandeira

	// [MP] Cops 'n Robbers (32-38)
	5, 0, // Ukrainian Village, Downtown
	4, 0, // Vedado, Necropolis de Colon
	3, 0, // Mid Strip, Downtown
	1, 0, // Copacabana, Centro

	// [MP] Capture the Flag (40-46)
	7, 6, // Cabrini Green, River North
	2, 5, // Old Havana, Plaza
	5, 6, // Lakes, Ghost Town
	6, 0, // Flamengo, Centro

	// [MP] Take a Ride (48-54)
	6, 7, // River North, Cabrini Green
	5, 2, // Plaza, Old Havana
	0, 5, // Downtown, Lakes
	0, 6, // Centro, Flamengo

	// [MP] Checkpoint (56-62)
	6, 7, // River North, Cabrini Green
	2, 6, // Old Havana, Plaza de la Revolucion
	6, 4, // Ghost Town, North Vegas
	0, 6, // Centro, Flamengo
};

#define AREA_NAME(level, index) GET_MISSION_TXT(areaNames[level][index])

#define GAMEMODE_AREA(level, offset, index) gameAreas[offset + level * 2 + index]
#define GAMEMODE_AREA_NAME(level, offset, index) AREA_NAME(level, GAMEMODE_AREA(level, offset, index))

char cutUnlock[] = {
	2, 2, 2, 4, 5,
	6, 7, 7, 7, 9,
	11, 11, 11, 11, 12,
	12, 13, 13, 14, 15,
	17, 17, 17, 17, 18,
	18, 19, 19, 19, 19,
	21, 21, 21, 21, 22,
	22, 22, 23, 23, 23,
	28
};

int CarAvailability[CITY_COUNT][10] = {
	{1,1,1,1,0,0,0,0,0,0},
	{1,1,1,1,0,0,0,0,0,0},
	{1,1,1,1,0,0,0,0,0,0},
	{1,1,1,1,0,0,0,0,0,0},
};

char carNumLookup[CITY_COUNT][10] = {
	{1, 2, 3, 4, 0, 8, 9, 10, 11, 12},
	{1, 2, 3, 4, 0, 8, 9, 10, 11, 12},
	{1, 2, 3, 4, 0, 8, 9, 10, 11, 12},
	{1, 2, 3, 4, 0, 8, 9, 10, 11, 12},
};

int minmaxSelections[CITY_COUNT][2] = {

	{ 0, 8 },
	{ 8, 18 },
	{ 18, 28 },
	{ 28, 37 }
};

BOTCH botch[38] = {
	{ 1, &MissionName[0]},
	{ 2, &MissionName[1]},
	{ 3, &MissionName[2]},
	{ 4, &MissionName[3]},
	{ 5, &MissionName[4]},
	{ 6, &MissionName[5]},
	{ 7, &MissionName[6]},
	{ 9, &MissionName[7]},
	{ 10, &MissionName[8]},
	{ 11, &MissionName[9]},
	{ 13, &MissionName[10]},
	{ 14, &MissionName[11]},
	{ 15, &MissionName[12]},
	{ 16, &MissionName[13]},
	{ 17, &MissionName[14]},
	{ 18, &MissionName[15]},
	{ 19, &MissionName[16]},
	{ 20, &MissionName[17]},
	{ 21, &MissionName[18]},
	{ 22, &MissionName[19]},
	{ 23, &MissionName[20]},
	{ 24, &MissionName[21]},
	{ 25, &MissionName[22]},
	{ 26, &MissionName[23]},
	{ 27, &MissionName[24]},
	{ 28, &MissionName[25]},
	{ 29, &MissionName[26]},
	{ 30, &MissionName[27]},
	{ 31, &MissionName[28]},
	{ 32, &MissionName[29]},
	{ 33, &MissionName[30]},
	{ 34, &MissionName[31]},
	{ 35, &MissionName[32]},
	{ 37, &MissionName[33]},
	{ 38, &MissionName[34]},
	{ 39, &MissionName[35]},
	{ 40, &MissionName[36]},
	{ 0,NULL}
};

char ScreenTitle[128];
char ScoreName[128];

int cutSelection = 0;
int currCity = 0;

enum MenuSelectScreens
{
	SCREEN_NONE = 0,

	SCREEN_MISSION,
	SCREEN_CAR,
	SCREEN_CUTCITY,		// really not used, but keep it here
	SCREEN_CUTSCENE,
	SCREEN_SCORES,
	SCREEN_TIMEOFDAY,
};

MenuSelectScreens iScreenSelect = SCREEN_NONE;

int bRedrawFrontend = 1;
int bReturnToMain = 1;

int idle_timer = 0;
int currPlayer = 1;
int feNewPad = 0;
int fePad = 0;
int ScreenDepth = 0;

int gIdleReplay = 0;

int padsConnected[2] = { 1, 0 };
int feVariableSave[4] = { -1 };

int bDrawExtra = 0;
int mainScreenLoaded = 1;
int bDoneAllready = 0;
int allowVibration = 1;
int bQuitToSystem = 0;
int bQuitToSystemSel = 0;

int carSelection = 0;
int currSelIndex = 0;
int lastCutCity = -1;
int lastCity = -1;
int currMission = -1;
int missionSetup = 0;

char* ScreenNames[12] = { 0 };

char loaded[3] = {
	-1,	// City
	-1, // Cars
	-1  // Cuts
};

PSXSCREEN* pCurrScreen = NULL;
PSXSCREEN* pNewScreen = NULL;

PSXBUTTON* pNewButton = NULL;
PSXBUTTON* pCurrButton = NULL;

PSXSCREEN* pScreenStack[10] = { 0 };
PSXBUTTON* pButtonStack[10] = { 0 };

POLY_FT4 BackgroundPolys[6];
POLY_F4 BackgroundBlack;
FE_FONT feFont;

RECT16 extraRect = { 896, 256, 64, 219 };

SPRT extraSprt;
POLY_FT3 extraDummy;

int BuildButtonsVertical(int count, int xStart, int yStart);

int FEStringWidth(char* string);
int FEPrintStringSized(char* string, int x, int y, int scale, int transparent, int r, int g, int b);
int FEPrintString(char* string, int x, int y, int justification, int r, int g, int b);

void LoadFrontendScreens(int full);
void NewSelection(short dir);
void EndFrame(void);

/* JERICHO-HOOK: presentation screens (jer_screen.h) */
void SetFEDrawMode(void);			/* defined below; used by the boot screen loop */
int  JerichoDrawScreen(void);
static void JerichoDrawPrompt(void);	/* a host prompt (jer_prompt.h) */
void JerichoRunBootScreens(void);

// [D] [T]
void SetVariable(int var)
{
	int code = (var >> 8);
	int value = (var & 0xff);

	switch (code)
	{
		case 1:
			GameLevel = value;
			break;
		case 2:
			GameType = (GAMETYPE)value;

			if (value != GAME_COPSANDROBBERS)
				gWantNight = 0;

			break;
		case 3:
			NumPlayers = (value + 1);
			break;
		case 4:
			gWantNight = value > 0;

			break;
		case 5:
			gCurrentMissionNumber = 1;
			GameType = GAME_MISSION;
			break;
		case 6:
			pScreenStack[ScreenDepth] = pCurrScreen;
			pButtonStack[ScreenDepth] = pCurrButton;

			if (value == 1) 
			{
#ifdef PSX
#else
				// [A] temporary
				// TODO: Do menu with the replays
				if(LoadReplayFromFile("Replays/CHASE.D2RP"))
				{
					StoredGameType = GameType;
					GameType = GAME_LOADEDREPLAY;
					SetState(STATE_GAMESTART);
				}
				else
#endif
				{
					SetState(STATE_INITFRONTEND);
				}
			}
			else
			{
				// [A] load configuration
				LoadCurrentProfile(0);

				//SetState(STATE_INITFRONTEND);
				SetMasterVolume(gMasterVolume);
				SetXMVolume(gMusicVolume);
			}

			break;
		case 7:
			pScreenStack[ScreenDepth] = pCurrScreen;
			pButtonStack[ScreenDepth] = pCurrButton;

#ifndef PSX
			if (value == 0) 
			{
				// [A] save configuration
				SaveCurrentProfile(1);
			}
			else 
			{
				// [A] load progress
				if(LoadCurrentGame())
				{
					GameType = GAME_CONTINUEMISSION;
					SetState(STATE_GAMESTART);
				}
			}
#endif

			break;
		case 8:
			gSubGameNumber = value;
			break;
		case 9:
			gSubtitles = (value == 0) ? 0 : 1;
			break;
		case 10:
			gInvincibleCar = value;
			ActiveCheats.cheat3 = value;
			break;
		case 11:
			gPlayerImmune = value;
			ActiveCheats.cheat4 = value;
			break;
		case 12:
			GameLevel = 3;
			gSubGameNumber = (value == 0) ? 2 : 0;
			break;
		case 13:
			GameType = GAME_IDLEDEMO;
			gCurrentMissionNumber = (value + 400);
			break;
#if ENABLE_BONUS_CONTENT
		case 14: // [A]
		{
			ShowBonusGallery();
			LoadFrontendScreens(0);
		}
		case 15: // [A] mini cars cheat
		{
			ActiveCheats.cheat13 = value;
		}
#endif
	}
}

// [D] [T]
void SetupBackgroundPolys(void)
{
	POLY_FT4 *poly;
	POLY_F4* bg;
	int i;

	for (i = 0; i < 6; i++)
	{
		poly = &BackgroundPolys[i];

		setPolyFT4(poly);
		setRGB0(poly, 128, 128, 128);
	}

	poly = &BackgroundPolys[0];
	setXYWH(poly, 0, 0, 256, 256);
	setUVWH(poly, 0, 0, 255, 255);
	setClut(poly, 960, 256);
	setTPage(poly, 0, 0, 640, 0);

	poly = &BackgroundPolys[1];
	setXYWH(poly, 256, 0, 256, 256);
	setUVWH(poly, 0, 1, 255, 254);
	setClut(poly, 960, 256);
	setTPage(poly, 0, 0, 704, 0);

	poly = &BackgroundPolys[2];
	setXYWH(poly, 512, 0, 128, 256);
	setUVWH(poly, 0, 1, 128, 254);
	setClut(poly, 960, 256);
	setTPage(poly, 0, 0, 768, 0);

	poly = &BackgroundPolys[3];
	setXYWH(poly, 0, 256, 256, 256);
	setUVWH(poly, 0, 0, 255, 255);
	setClut(poly, 960, 256);
	setTPage(poly, 0, 0, 832, 0);

	poly = &BackgroundPolys[4];
	setXYWH(poly, 256, 256, 256, 256);
	setUVWH(poly, 0, 0, 255, 255);
	setClut(poly, 960, 256);
	setTPage(poly, 0, 0, 896, 0);

	poly = &BackgroundPolys[5];
	setXYWH(poly, 512, 256, 128, 256);
	setUVWH(poly, 0, 0, 128, 255);
	setClut(poly, 960, 256);
	setTPage(poly, 0, 0, 960, 0);

	bg = &BackgroundBlack;
	setPolyF4(bg);
	setXYWH(bg, -1000, 0, 2000, 512);
	setRGB0(bg, 0, 0, 0);
}

SPRT HighlightSprt;
POLY_FT4 HighlightDummy;

// [D] [T]
void SetupScreenSprts(PSXSCREEN *pScr)
{
	setSprt(&HighlightSprt);
	setRGB0(&HighlightSprt, 128, 128, 128);
	setXY0(&HighlightSprt, 364, 198);
	setUV0(&HighlightSprt, 0, 0);
	setWH(&HighlightSprt, 256, 36);
	setClut(&HighlightSprt, 960, 258);

	setPolyFT4(&HighlightDummy);
	setXY3(&HighlightDummy, -1, -1, -1, -1, -1, -1);
	setTPage(&HighlightDummy, 0, 0, 704, 256);

	pNewScreen = NULL;
	pCurrScreen = pScr;

	if (pScr->userFunctionNum == 0 || pScr->userFunctionNum == 128)
	{
		pCurrButton = pNewButton;	/* the row we came FROM, or NULL for the top */
	}
	else if ((fpUserFunctions[pScr->userFunctionNum - 1])(1) == 0)
	{
		/* The screen function may ask for a row itself (it writes pNewButton and
		 * returns 0), so read it back AFTER the call. A function that returns
		 * non-zero sets pCurrButton itself - it is showing its own current
		 * setting - and is left alone. */
		pCurrButton = pNewButton;
	}

	/* The row a Back press carried here belongs to THIS entry only. It points
	 * into the button array of the screen we came from, so a later entry that
	 * does not set one of its own must not inherit it: it used to survive when
	 * the screen function returned non-zero, and the next screen to be entered
	 * then highlighted a row computed from another screen's array - i.e. nothing
	 * sensible. */
	pNewButton = NULL;

	/* And the row must still EXIST. A screen that rebuilt its rows - a module
	 * menu whose list shrank, the car list - may have fewer than the one we left
	 * it on, and a stale pointer highlights nothing at all. Anything out of range
	 * falls back to the top row. */
	if (pCurrButton == NULL || pCurrButton < pScr->buttons ||
		pCurrButton >= pScr->buttons + pScr->numButtons)
	{
		pCurrButton = pScr->buttons;
	}
}

DR_MOVE In;
DR_MOVE Out;
RECT16 storeRect = { 768, 475, 255, 36 };



// [D] [T]
void DisplayOnScreenText(void)
{
	char* text;

	/* The breadcrumb is BUILT below, so start from empty rather than sprintf-ing a
	 * variable format string (NullStr) into it. */
	ScreenTitle[0] = 0;

	if (padsConnected[0] == 0) {
		int transparent = 0;

		if (Pads[0].type == 0) {
			text = "Please insert controller into Port 1";
			transparent = 1;
		}
		else {
			if (Pads[0].type != 1)
				return;

			text = "Incompatible controller in Port 1";
		}

		FEPrintStringSized(text, 40, 400, 3072, transparent, 64, 64, 64);
	}
	else
	{
		if (iScreenSelect != SCREEN_SCORES && iScreenSelect != SCREEN_CAR)
		{
			/* ScreenTitle is 128 bytes; ScreenNames[i] is a button NAME (up to 31
			 * chars) and ScreenDepth can be 12 deep, with a module free to push
			 * levels of its own -- so the old strcat chain could run off the end,
			 * which aborts outright under glibc's _FORTIFY_SOURCE. Append only what
			 * still fits, and skip a name that is still NULL (the array is zeroed at
			 * boot and only filled as screens are opened). */
			int used = 0;

			for (int i = 0; i < ScreenDepth; i++)
			{
				int room = (int)sizeof(ScreenTitle) - used;
				int wrote;

				if (ScreenNames[i] == NULL)
					continue;

				wrote = snprintf(ScreenTitle + used, (size_t)room, "%s%s",
					(used > 0) ? " - " : "", ScreenNames[i]);

				if (wrote < 0 || wrote >= room)
					break;		/* the rest does not fit; stop here */

				used += wrote;
			}

			FEPrintStringSized(ScreenTitle, 40, 400, 3072, 1, 64, 64, 64);
		}

		if (iScreenSelect == SCREEN_CUTSCENE)
		{
			text = GET_MISSION_TXT(CutSceneNames[cutSelection + CutAmountsTotal[currCity]]);

			FEPrintStringSized(text, 100, 226, 3072, 1, 64, 64, 64);
		}
	}
}

void DrawButton(PSXBUTTON* pBtn, int i)
{
	int status = pBtn->action >> 8;

	int draw = (status != BTN_HIDDEN);

#ifndef PSX
	if (pBtn == pCurrButton)
	{
		SPRT* hghltSprt;
		POLY_FT4* hghltDummy;
		RECT16 rect;

		rect.x = pCurrButton->s_x;
		rect.y = pCurrButton->s_y;

		hghltSprt = (SPRT*)current->primptr;
		current->primptr += sizeof(SPRT);
		*hghltSprt = HighlightSprt;

		hghltDummy = (POLY_FT4*)current->primptr;
		current->primptr += sizeof(POLY_FT4);
		*hghltDummy = HighlightDummy;

		setXY0(hghltSprt, rect.x, rect.y);

		addPrim(current->ot + 6, hghltSprt);
		addPrim(current->ot + 6, hghltDummy);

		draw = 1;
	}
#endif

	if (draw)
	{
		if (status == BTN_DISABLED)
		{
			FEPrintString(pBtn->Name, pBtn->x * 2 + pBtn->w, pBtn->y, 4, 32, 32, 32);
		}
		else
		{
			if (iScreenSelect == SCREEN_MISSION && (i == 0 || i == 5) ||
				iScreenSelect == SCREEN_CAR && (i == 0 || i == 2) ||
				iScreenSelect == SCREEN_CUTSCENE && (i == 0 || i == 2) ||
				iScreenSelect == SCREEN_TIMEOFDAY && i == 2)				// [A]
			{
				FEPrintString(pBtn->Name, pBtn->x * 2 + pBtn->w, pBtn->y, 4, 124, 108, 40);
			}
			else
			{
				FEPrintString(pBtn->Name, pBtn->x * 2 + pBtn->w, pBtn->y, 4, 128, 128, 128);
			}
		}
	}
}

// [D] [T]
void DrawScreen(PSXSCREEN *pScr)
{
	int numBtnsToDraw;
	int i;

	// JERICHO: the custom menu background. On by default; CONFIG/jericho.ini
	// custom_menu_background = 0 restores the stock DATA/GFX.RAW art. It is loaded once,
	// lazily, on the first frame that draws a screen (by then the GL context exists).
	jer_menu_bg_tick();

	if (jer_menu_bg_ready())
	{
		// jer_texture draws only inside a render pass; here we are drawing straight into
		// the display buffer, so say so. 640x512 is the area the stock background covers
		// in the frontend's own coordinate space (six polys tiled to 640x512), and the
		// blit scales whatever the image is to fill it. Bucket 11 is the one the stock
		// background used, i.e. behind the menu text.
		gJerTextureRenderPass = 1;
		jer_texture_draw_screen(jer_menu_bg_texture(), 0, 0, 640, 512, 11);
		gJerTextureRenderPass = 0;
	}
	else
	{
		for (i = 0; i < 6; i++)
			addPrim(current->ot + 11, &BackgroundPolys[i]);

		addPrim(current->ot + 11, &BackgroundBlack);
	}
	
	if (pScr)
	{
		if (iScreenSelect == SCREEN_CUTCITY && (gFurthestMission < 39))
		{
			// hide the Director's Cut button
			numBtnsToDraw = pScr->numButtons - 1;
		}
		else 
		{
			numBtnsToDraw = pScr->numButtons;
		}

#ifdef PSX
		for (i = 0; i < numBtnsToDraw; i++)
			DrawButton(&pScr->buttons[i], i);
#else
		if(bQuitToSystem)
		{
			int len;
			int quitLen;

			quitLen = FEStringWidth(G_LTXT(GTXT_QuitToSystem));
			FEPrintString(G_LTXT(GTXT_QuitToSystem), 320 - quitLen / 2, 256, 0, 128, 128, 128);

			if (quitLen < 100)
				quitLen = 100;
			
			len = FEStringWidth(G_LTXT(GTXT_YES));
			FEPrintString(G_LTXT(GTXT_YES), 320 - quitLen / 4 - len / 2, 288, 0, 128, 128, bQuitToSystemSel ? 0 : 128);

			len = FEStringWidth(G_LTXT(GTXT_NO));
			FEPrintString(G_LTXT(GTXT_NO), 320 + quitLen / 4 - len/2, 288, 0, 128, 128, bQuitToSystemSel ? 128 : 0);
		}
		else if (jer_prompt_active())
		{
			/* a host prompt takes the screen: the screen's own buttons are NOT drawn.
			 * The prompt itself is painted from the per-frame hook below, not here --
			 * a module menu draws on its own path and would otherwise leave an active
			 * prompt invisible while it still ate the pad. */
		}
		else
		{
			NewSelection(0);
			
			for (i = 0; i < numBtnsToDraw; i++)
				DrawButton(&pScr->buttons[i], i);
		}
#endif

		// debug version text, and NOT on a release screen. A release still records
		// the build in its log (the boot banner) and reports it in Options ->
		// JERICHO, so stamping it across every menu is noise rather than
		// information. Same rule as the Debug Options submenu in pause.c. The
		// buffer lives in here too, so a release does not even build the string.
#if (defined(_DEBUG) || defined(DEBUG_OPTIONS)) && !defined(JERICHO_RELEASE_MODS)
		{
			char version_info[32];
			GetTimeStamp(version_info);
			FEPrintString(version_info, 40, 16, 0, 128, 128, 0);
			FEPrintString("--- " GAME_VERSION " ---", 320, 16, 0, 128, 128, 0);
		}
#endif

		SetTextColour(128, 0, 0);

		DisplayOnScreenText();
		
		if (bDrawExtra) 
		{
#ifdef PSX
			addPrim(current->ot + 2, &extraSprt);
			addPrim(current->ot + 2, &extraDummy);
#else
			SPRT* extrSprt;
			POLY_FT3* extrDummy;
			
			extrSprt = (SPRT*)current->primptr;
			current->primptr += sizeof(SPRT);
			*extrSprt = extraSprt;

			extrDummy = (POLY_FT3*)current->primptr;
			current->primptr += sizeof(POLY_FT3);
			*extrDummy = extraDummy;

			addPrim(current->ot + 2, extrSprt);
			addPrim(current->ot + 2, extrDummy);
#endif
		}
	}

#ifdef PSX
	else 
	{
		EndFrame();
	}
#else

	EndFrame();

#endif
}

extern POLY_FT4 cd_sprite;

// [D] [T]
void FEDrawCDicon(void)
{
	ushort* palette;
	int i;
	RECT16 dest;

	if(!bCdIconSetup)
		return;

	cd_icon[23] = cd_icon[11];

	palette = &cd_icon[10];

	for (i = 0; i < 12; i++)
	{
		palette[1] = palette[2];
		palette++;
	}

	dest.x = 960;
	dest.y = 433;
	dest.w = 16;
	dest.h = 1;

	LoadImage(&dest, (u_long*)(cd_icon + 10));

	DrawPrim(&cd_sprite);
	DrawSync(0);
}

// [D] [T]
void SetupExtraPoly(char *fileName, int offset, int offset2)
{
	int i;
	RECT16 rect = extraRect;

	FEDrawCDicon();
	Loadfile(fileName, (char*)_frontend_buffer + offset2);

	rect.x = 896;
	rect.y = 256;
	rect.w = 64;
	rect.h = 219;
	
	LoadImage(&rect, (u_long*)(_frontend_buffer + offset2 + offset * 0x8000));
	
	DrawSync(0);
	VSync(0);

	if(bDrawExtra == 0)
	{
		setSprt(&extraSprt);
		setPolyFT3(&extraDummy);

		setXY0(&extraSprt, 100, 226);
		setRGB0(&extraSprt, 128, 128, 128);
		setUV0(&extraSprt, 0, 0);
		setWH(&extraSprt, 255, 219);
		setClut(&extraSprt, 960, 256);

		setXY3(&extraDummy, -1, -1, -1, -1, -1, -1);
		setTPage(&extraDummy, 0, 0, 896, 256);
	}

	bDrawExtra = 1;

	if (strcmp(fileName, "DATA\\CITY.RAW") == 0) 
	{
		loaded[0] = 1;
		loaded[1] = -1;
	}

	for (i = 0; i < 4; i++)
	{
		if (strcmp(fileName, gfxNames[i]) == 0)
		{
			loaded[0] = -1;
			loaded[1] = i;
			loaded[2] = -1;
		}
	}

	for (i = 0; i < 4; i++)
	{
		if (strcmp(fileName, cutGfxNames[i]) == 0) 
		{
			loaded[1] = -1;
			loaded[2] = i;
		}
	}
}

// [D] [T]
void LoadBackgroundFile(char* name)
{
	int iTpage;
	RECT16 rect;
	int i;

	iTpage = 11;

	mainScreenLoaded = (strcmp(name, "DATA\\GFX.RAW") == 0);

	rect.w = 64;
	rect.h = 256;

	for (i = 0; i < 6; i++)
	{
		FEDrawCDicon();

		LoadfileSeg(name, (char*)_other_buffer, i * 0x8000, 0x8000);
		FEDrawCDicon();

		rect.y = (i / 6);
		rect.x = (i - rect.y * 6) * 64 + 640;
		rect.y *= 256;

		LoadImage(&rect, (u_long*)_other_buffer);
		FEDrawCDicon();
	}

	LoadfileSeg(name, (char*)_other_buffer, iTpage * 0x8000, 512);
	FEDrawCDicon();

	rect.h = 1;
	rect.y = (iTpage / 6);
	rect.x = (iTpage - rect.y * 6) * 64 + 640;
	rect.y *= 256;

	LoadImage(&rect, (u_long*)_other_buffer);
	DrawSync(0);

	SetupBackgroundPolys();

	bRedrawFrontend = 1;
}

/* ------------------------------------------------------------------ *
 * JERICHO-HOOK: presentation screens (JERICHO/include/jer_screen.h).
 *
 * A presentation screen is a full-screen, non-interactive frontend frame: a
 * heading plus one live body line ("Compiling JERICHO addons..." /
 * "ZOOMMOD [3/7]"). It lets long work show progress instead of a frozen
 * screen. The engine only pumps the registry and draws it — what a screen says
 * is owned by JERICHO and its modules.
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 * A host prompt (JERICHO/include/jer_prompt.h).
 *
 * The engine's own quit prompt is the reference for the colours and the place --
 * a grey question above, the highlighted answer in yellow -- but NOT for the
 * spacing: it puts its two answers at +-questionWidth/4, which is fine for its own
 * six-word question and throws them to the edges of a longer one. Here Yes and No
 * are measured and centred as ONE pair with a fixed gap, so the layout does not
 * depend on the length of the question.
 *
 * The question itself is wrapped with jer_text_width, because a single centred
 * line of a long question runs off the screen (and the frontend's own drawing
 * clips it rather than wrapping it). The block is anchored at the BOTTOM, so a
 * two- or three-line question grows upwards and never runs into the answers.
 * ------------------------------------------------------------------ */
#define JER_PROMPT_WRAP_MAX	4
#define JER_PROMPT_LINE_LEN	192
#define JER_PROMPT_WIDTH	544	/* 640 less a 48px margin each side */
#define JER_PROMPT_SCALE	150
#define JER_PROMPT_LINE_H	26
#define JER_PROMPT_LEFT_X	48	/* question: left-aligned, like the other FE pages */
#define JER_PROMPT_TEXT_Y	200	/* first question line; wrapped lines grow downwards */
#define JER_PROMPT_RIGHT_X	592	/* answers end here: bottom-right, where BACK sits */
#define JER_PROMPT_ANSWERS_Y	440
#define JER_PROMPT_GAP		48

static int JerichoWrapText(const char* text, int maxWidth, char lines[JER_PROMPT_WRAP_MAX][JER_PROMPT_LINE_LEN])
{
	const char* p = text;
	int count = 0;

	while (p != NULL && *p != 0 && count < JER_PROMPT_WRAP_MAX)
	{
		size_t len = strlen(p);
		size_t take;

		if (len > JER_PROMPT_LINE_LEN - 1)
			len = JER_PROMPT_LINE_LEN - 1;

		memcpy(lines[count], p, len);
		lines[count][len] = 0;

		/* the longest prefix of what is left that fits the line */
		take = len;
		while (take > 0 && jer_text_width(JER_TEXT_FE, JER_PROMPT_SCALE, lines[count]) > maxWidth)
		{
			take--;
			lines[count][take] = 0;
		}

		/* cut mid-word? back up to the space before it */
		if (take > 0 && p[take] != 0 && p[take] != ' ')
		{
			size_t back = take;

			while (back > 0 && p[back] != ' ')
				back--;

			if (back > 0)
			{
				take = back;
				lines[count][take] = 0;
			}
		}

		while (take > 0 && lines[count][take - 1] == ' ')
			lines[count][--take] = 0;

		count++;

		p += take;
		while (*p == ' ')
			p++;
	}

	return count;
}

static void JerichoDrawPrompt(void)
{
	static char lines[JER_PROMPT_WRAP_MAX][JER_PROMPT_LINE_LEN];
	const char* question = jer_prompt_question();
	int yesSel = jer_prompt_selected_yes();
	int count;
	int i;
	int y;
	int wy;
	int wn;
	int x;

	if (question == NULL)
		question = "Are you sure?";

	count = JerichoWrapText(question, JER_PROMPT_WIDTH, lines);

	/* Jaret's FE rule: the text on the left, wrapped; the options bottom-right */
	y = JER_PROMPT_TEXT_Y;

	for (i = 0; i < count; i++)
	{
		jer_text_draw_at(JER_TEXT_FE, JER_PROMPT_SCALE, JER_PROMPT_LEFT_X, y + i * JER_PROMPT_LINE_H,
			lines[i], 235, 235, 235);
	}

	wy = jer_text_width(JER_TEXT_FE, JER_PROMPT_SCALE, "Yes");
	wn = jer_text_width(JER_TEXT_FE, JER_PROMPT_SCALE, "No");

	x = JER_PROMPT_RIGHT_X - (wy + JER_PROMPT_GAP + wn);

	/* one line per prompt, the first frame it is drawn: where the layout ended
	 * up is exactly what is wrong when a prompt looks wrong, and guessing it from
	 * a screenshot is guesswork */
	{
		static char loggedQuestion[160] = "";

		if (strncmp(loggedQuestion, question, sizeof(loggedQuestion) - 1) != 0)
		{
			snprintf(loggedQuestion, sizeof(loggedQuestion), "%s", question);
			printInfo("[prompt] \"%s\" -> %d line(s) at y=%d, Yes x=%d w=%d, No x=%d w=%d\n",
				question, count, y, x, wy, x + wy + JER_PROMPT_GAP, wn);

			for (i = 0; i < count; i++)
			{
				printInfo("[prompt]   line %d w=%d: \"%s\"\n",
					i, jer_text_width(JER_TEXT_FE, JER_PROMPT_SCALE, lines[i]), lines[i]);
			}
		}
	}

	jer_text_draw_at(JER_TEXT_FE, JER_PROMPT_SCALE, x, JER_PROMPT_ANSWERS_Y, "Yes",
		yesSel ? 255 : 130, yesSel ? 215 : 130, yesSel ? 60 : 130);

	jer_text_draw_at(JER_TEXT_FE, JER_PROMPT_SCALE, x + wy + JER_PROMPT_GAP, JER_PROMPT_ANSWERS_Y, "No",
		yesSel ? 130 : 255, yesSel ? 130 : 215, yesSel ? 130 : 60);
}

/* One frame of the active screen. Returns 1 if a screen was drawn. */
int JerichoDrawScreen(void)
{
	static POLY_F4 jerBack;		/* static: the OT holds the pointer until EndFrame */
	char body[JER_SCREEN_BODY_MAX];
	const char* title = jer_screen_title();
	int x;

	/* a host prompt owns the screen while it is up: two screens on one frame is
	 * how a prompt ends up with a progress line bleeding through it */
	if (jer_prompt_active())
		return 0;

	if (title == NULL || current == NULL)
		return 0;

	jer_screen_body(body, sizeof(body));

	/* a black backdrop, so it reads as a screen rather than an overlay. It sits
	 * just above the frontend's own screen content (indices run high = furthest
	 * back to 0 = nearest) and just below the frontend font text at ot+1, so a
	 * screen raised over the frontend covers it properly. */
	setPolyF4(&jerBack);
	setRGB0(&jerBack, 0, 0, 0);
	setXYWH(&jerBack, 0, 0, 640, 512);
	addPrim(current->ot + 2, &jerBack);

	x = 320 - FEStringWidth((char*)title) / 2;
	FEPrintString((char*)title, x, 236, 0, 230, 230, 230);

	if (body[0] != 0)
	{
		x = 320 - FEStringWidth(body) / 2;
		FEPrintString(body, x, 262, 0, 150, 205, 150);
	}

	return 1;
}

/* jer_menu_draw — the shared JERICHO menu look (see JERICHO/include/jer_menu.h).
 * Engine-side so every module's menu is drawn the same way with the frontend
 * font: call it from a module's JER_EVENT_DRAW_OVERLAY handler with the frame
 * centre. Renders the three base types (list / centred yes-no / carousel). */
void jer_menu_draw(const JerMenu* m, int x, int y)
{
	const char* title;
	int i;

	if (m == NULL || !m->visible)
		return;

	title = jer_menu_title(m);

	/* BREADCRUMB. Inside a submenu the title alone is ambiguous -- with two or
	 * three levels open "Vehicles" does not say which page you came through --
	 * so the path down to here is shown after it. Flat menus (depth 0) are drawn
	 * exactly as they always were. */
	{
		static char tbuf[160];

		if (m->depth > 0)
		{
			int d;
			int used = snprintf(tbuf, sizeof(tbuf), "%s", title);

			/* snprintf reports the length it WOULD have written, so a clipped
			 * title leaves `used` past the end of the buffer. */
			if (used < 0)
				used = 0;
			else if (used > (int)sizeof(tbuf) - 1)
				used = (int)sizeof(tbuf) - 1;

			for (d = 1; d <= m->depth && d < JER_MENU_MAX_DEPTH; d++)
			{
				const char* lbl = m->stack[d].label;
				int room, wrote;

				if (lbl == NULL)
					continue;

				room = (int)sizeof(tbuf) - used;

				/* Bound by the space actually LEFT, not by a fixed threshold: a
				 * submenu label comes from the module and may be any length, and
				 * appending one unchecked is what used to run off the end of this
				 * buffer -- an outright abort under glibc's _FORTIFY_SOURCE. */
				if (room <= 4)		/* not even room for " > x" + NUL */
					break;

				wrote = snprintf(tbuf + used, (size_t)room, " > %s", lbl);

				if (wrote < 0 || wrote >= room)
					break;		/* did not fit; tbuf is still terminated */

				used += wrote;
			}

			title = tbuf;
		}
	}

	if (title != NULL && title[0] != 0)
		FEPrintString((char*)title, x - FEStringWidth((char*)title) / 2, y - 18, 0, 235, 235, 235);

	if (m->kind == JER_MENU_KIND_YESNO)
	{
		char line[48];
		const char* vals[2];
		vals[0] = "Yes";
		vals[1] = "No";

		snprintf(line, sizeof(line), "%s%s        %s%s",
			(m->cursor == 0) ? "> " : "  ", vals[0],
			(m->cursor == 1) ? "> " : "  ", vals[1]);

		FEPrintString(line, x - FEStringWidth(line) / 2, y, 0, 255, 255, 200);
		return;
	}

	if (m->kind == JER_MENU_KIND_CAROUSEL)
	{
		const char* lbl = jer_menu_label(m);
		char line[80];

		/* A carousel label is the module's own string and may be any length, so it is
		 * CLIPPED into the row rather than sprintf-ed past it (which would abort under
		 * glibc's _FORTIFY_SOURCE). */
		snprintf(line, sizeof(line), "<  %s  >", (lbl != NULL && lbl[0] != 0) ? lbl : "-");
		FEPrintString(line, x - FEStringWidth(line) / 2, y, 0, 255, 235, 130);
		return;
	}

	/* LIST */
	for (i = 0; i < m->count; i++)
	{
		char line[80];
		const char* lbl = (m->items != NULL && m->items[i] != NULL) ? m->items[i] : "";
		int sel = (i == m->cursor);
		int opens = 0;

		/* An item that opens a submenu is marked, so "this goes deeper" is visible
		 * before the player commits. Read straight off the table rather than through
		 * jer_menu_has_sub (which answers for the CURSOR only). */
		if (m->subs != NULL && i < m->subCount &&
		    m->subs[i].items != NULL && m->subs[i].count > 0)
			opens = 1;

		/* As the carousel above: a module's item label is clipped, not trusted. */
		snprintf(line, sizeof(line), "%s%s%s", sel ? "> " : "  ", lbl, opens ? "  >" : "");
		FEPrintString(line, x - 70, y + i * 14, 0,
			sel ? 255 : 190, 255, sel ? 140 : 190);
	}
}

/* Boot variant: draw + advance while a screen is pending. Runs before the rest
 * of the frontend is built, so a compile-style screen is visible first. A
 * no-op when no screen is pending.
 *
 * NOTE: this deliberately does NOT use EndFrame(). EndFrame VSyncs before it
 * presents, which deadlocks while swap-interval blocking is on — and at this
 * point in the boot the display has just been brought up. The sequence below
 * mirrors ShowLoadingScreen (loadview.c), which is what runs immediately
 * before it: draw -> DrawSync -> VSync (skipped when fast-loading) ->
 * PsyX_EndScene -> re-install the env. */
void JerichoRunBootScreens(void)
{
	int guard;

	jer_screen_tick();

	if (!jer_screen_active())
		return;

	/* the frontend menu is not built yet: set up the draw env here (this also
	 * gives us a valid `current` to add prims to) and bring the display up */
	SetFEDrawMode();
	SetDispMask(1);

#ifndef PSX
	PsyX_EnableSwapInterval((gFastLoadingScreens == 0));
#endif

	/* EndFrame() normally clears the OT it has just drawn and swapped in; this
	 * loop does not use EndFrame, so clear it here before the first draw. */
	ClearOTagR((u_long*)current->ot, FE_OTSIZE);

	/* guard: a screen with no on_update would otherwise spin forever */
	for (guard = 0; jer_screen_active() && guard < JER_SCREEN_BOOT_MAX_FRAMES; guard++)
	{
		/* EndFrame() rewinds the primitive buffer as part of its buffer swap;
		 * this loop does not use EndFrame, so rewind it here — otherwise the
		 * font prims walk off the end of the buffer after a few frames. */
		current->primptr = current->primtab;

		JerichoDrawScreen();

		DrawOTag((u_long*)(current->ot + FE_OTSIZE - 1));

		DrawSync(0);

		if (gFastLoadingScreens == 0)
			VSync(0);

#ifndef PSX
		PsyX_EndScene();

#endif

		PutDispEnv(&current->disp);
		PutDrawEnv(&current->draw);

		ClearOTagR((u_long*)current->ot, FE_OTSIZE);

		jer_screen_tick();
	}
}

// [D] [T]
void LoadFrontendScreens(int full)
{
	int size;
	int iNumScreens;
	char* ptr;
	RECT16 rect;

	ShowLoadingScreen("GFX\\FELOAD.TIM", 1, 12);
	ShowLoading();

	// JERICHO-HOOK: presentation screens run here — before the rest of the
	// frontend is built — so work that must happen before the menu is visible.
	// A deep-mod compile raises its own screen (it can only happen at boot,
	// because it relinks the exe the game is running from).
	jer_build_begin();
	JerichoRunBootScreens();
	jer_build_finish();

#ifndef USE_EMBEDDED_FRONTEND_SCREENS
	if (full)
	{
		Loadfile("DATA\\SCRS.BIN", (char*)_other_buffer);

		ptr = (char*)_other_buffer + 20; // skip header and number of screens
		iNumScreens = (int)_other_buffer[16];

		for (int i = 0; i < iNumScreens; i++)
		{
			PsxScreens[i].numButtons = *ptr++;
			PsxScreens[i].userFunctionNum = *ptr++;

			for (int j = 0; j < PsxScreens[i].numButtons; j++)
			{
				memcpy((u_char*)&PsxScreens[i].buttons[j], (u_char*)ptr, sizeof(PSXBUTTON));
				ptr += sizeof(PSXBUTTON);
			}
		}

#ifndef PSX
		// [A] SCREEN HACKS
		// Time of day extended screen
		PsxScreens[3].userFunctionNum = 22;			// TimeOfDaySelectScreen
#endif
		// for web demo content (or empty SCRS.BIN)
		if (PsxScreens[0].userFunctionNum == 128)
			PsxScreens[0].userFunctionNum = 23;		// DemoScreen

#ifndef PSX
		// replay theater
		PsxScreens[4].buttons[0].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 39);
		PsxScreens[4].buttons[0].var = -1;
		PsxScreens[39].userFunctionNum = 21;		// UserReplaySelectScreen
#endif // PSX

#if ENABLE_BONUS_CONTENT
		 // make mini cars cheat screen
		PsxScreens[40] = PsxScreens[31];
		PsxScreens[40].userFunctionNum = 24;		// MiniCarsOnOffScreen
		PsxScreens[40].buttons[0].var = FE_MAKEVAR(15, 1);
		PsxScreens[40].buttons[1].var = FE_MAKEVAR(15, 0);
#endif

		// JERICHO-HOOK: the frontend Mods manager screen (slot 41). Reuses a
		// spare screen's graphics; JerichoModsScreen lays out its own buttons.
		// The screens were just (re)loaded, so the Options-screen injection
		// must be re-done (this also covers exit-to-menu, which reloads them).
		PsxScreens[JERICHO_MODS_SCREEN] = PsxScreens[31];
		PsxScreens[JERICHO_MODS_SCREEN].userFunctionNum = 25;	// JerichoModsScreen
		gJerichoOptionsButtonAdded = 0;

		// JERICHO-HOOK: module frontend menu slots (all share one handler)
		{
			int fei;

			for (fei = 0; fei < JER_FE_MAX_MENUS; fei++)
			{
				PsxScreens[JERICHO_FE_SCREEN_BASE + fei] = PsxScreens[31];
				PsxScreens[JERICHO_FE_SCREEN_BASE + fei].userFunctionNum = 26;	// JerFrontendMenuScreen
			}
		}

		gFeNeedSetup = 1;
	}
#endif

	rect.w = 64;
	rect.h = 256;

	ShowLoading();
	LoadBackgroundFile("DATA\\GFX.RAW");

	for (int i = 0; i < 2; i++)
	{
		// [A] optimized. Before it was to wasteful to load 16 sectors
		int loadSize = i == 0 ? 0x8000 : (36 * 128);
		rect.h = i == 0 ? 256 : 36;
		
		ShowLoading();
		LoadfileSeg("DATA\\GFX.RAW", (char*)_other_buffer, 0x30000 + (i * 0x8000), loadSize);

		rect.x = 640 + (i * 64);
		rect.y = 256;

		LoadImage(&rect, (u_long*)_other_buffer);
		DrawSync(0);
	}

	// Load clut
	ShowLoading();
	LoadfileSeg("DATA\\GFX.RAW", (char*)_other_buffer, 0x58000, /*0x8000*/ 512);

	rect.x = 960;
	rect.y = 256;
	rect.h = 4;

	LoadImage(&rect, (u_long*)_other_buffer);
	DrawSync(0);

	if (full)
	{
		Loadfile("DATA\\FEFONT.BNK", (char*)_other_buffer);
		memcpy((u_char*)&feFont, (u_char*)_other_buffer, sizeof(feFont));
	}
}

// [D] [T]
void ReInitScreens(int returnToMain)
{
#ifndef PSX
	bReturnToMain = returnToMain;

	if(bReturnToMain)
	{
		bCdIconSetup = 0;
		iScreenSelect = SCREEN_NONE;

		cutSelection = 0;
		currCity = 0;

		bRedrawFrontend = 0;

		idle_timer = 0;
		currPlayer = 1;
		feNewPad = 0;
		fePad = 0;
		ScreenDepth = 0;

		loaded[0] = -1;
		loaded[1] = -1;
		loaded[2] = -1;
	
		padsConnected[0] = 1;
		padsConnected[1] = 0;

		bDrawExtra = 0;
		mainScreenLoaded = 1;
		bDoneAllready = 0;
		allowVibration = 1;

		ScreenNames[0] = 0;
		gInFrontend = 1;
		currMission = -1;
		missionSetup = 0;
	}

	extern void SwitchMappings(int menu);
	SwitchMappings(1);
#endif // !PSX

	bCdIconSetup = 0;
	InitCdIcon();

	if (bReturnToMain)
	{
		ScreenDepth = 0;
		NumPlayers = 1;
		gWantNight = 0;
		gNight = 0;
		gTimeOfDay = TIME_DAY;
		gSubGameNumber = 0;

		feVariableSave[3] = -1;
		feVariableSave[2] = -1;
		feVariableSave[1] = -1;
		feVariableSave[0] = -1;

		if (!mainScreenLoaded)
		{
			bDrawExtra = 0;
			LoadBackgroundFile("DATA\\GFX.RAW");
		}
	}
	else 
	{
		bReturnToMain = 1;
	}

	pCurrScreen = pScreenStack[ScreenDepth];
	pNewButton = pButtonStack[ScreenDepth];

	// [A] state hack for demo
	if (pCurrScreen == NULL)
		pCurrScreen = &PsxScreens[0];

	SetupScreenSprts(pCurrScreen);
	SetupBackgroundPolys();

	idle_timer = VSync(-1);
}

// [D] [T]
void NewSelection(short dir)
{
	PSXBUTTON *pNewB;
	RECT16 rect;

	if (pCurrScreen->numButtons == 0)
		return;

	pNewB = pCurrButton;
	int btn = 0;

	// any buttons pressed?
	if (dir != 0)
	{
#ifdef PSX
		SetDrawMove(&In, &storeRect, pCurrButton->s_x, pCurrButton->s_y);
		addPrim(current->ot + 9, &In);
#endif

		if ((dir & MPAD_D_UP) != 0)
		{
			btn = pCurrButton->u;
		}
		else if ((dir & MPAD_D_DOWN) != 0)
		{
			btn = pCurrButton->d;
		}
		else if ((dir & MPAD_D_LEFT) != 0)
		{
			btn = pCurrButton->l;
		}
		else if ((dir & MPAD_D_RIGHT) != 0)
		{
			btn = pCurrButton->r;
		}

		if (btn != 0)
		{
			FESound(3);
			pNewB = &pCurrScreen->buttons[btn - 1];
		}
	}

	pCurrButton = pNewB;

	// show who has control
	if (NumPlayers == 2 && currPlayer != 1)
		FEPrintStringSized(G_LTXT(GTXT_Player2), 400, 260, 3072, 0, 128, 128, 128);

#ifdef PSX
	rect.x = pCurrButton->s_x;
	rect.y = pCurrButton->s_y;
	rect.w = 255;
	rect.h = 36;

	SetDrawMove(&Out, &rect, storeRect.x, storeRect.y);
	addPrim(current->ot + 8, &Out);

	setXY0(&HighlightSprt, rect.x, rect.y);

	addPrim(current->ot + 6, &HighlightSprt);
	addPrim(current->ot + 6, &HighlightDummy);

	DrawButton(pNewB, pNewB - pCurrScreen->buttons);

	EndFrame();
#endif
}

// JERICHO-HOOK: the frontend take-a-ride CITY CONFIRM. Fires JER_EVENT_FRONTEND so a
// module may rewrite the pending level/gametype/player count or DEFER (run its own
// start over the frozen frontend). Returns the module's `defer` flag: the caller must
// not continue the stock flow when it is set. Two city screens can reach a Take-a-Ride
// -- the city list itself (screen 1) and the mission/cutscene city list
// (CutSceneCitySelectScreen) -- so both fire the event from here rather than each
// carrying its own copy.
static int JerichoFrontendCityConfirm(void)
{
	JER_ARGS_FRONTEND jerFe;

	jerFe.gameLevel = GameLevel;
	jerFe.gameType = GameType;
	jerFe.numPlayers = NumPlayers;
	jerFe.defer = 0;

	jer_fire(JER_EVENT_FRONTEND, &jerFe);

	GameLevel = jerFe.gameLevel;
	GameType = (GAMETYPE)jerFe.gameType;
	NumPlayers = jerFe.numPlayers;

	return jerFe.defer;
}

// [D] [T]
int HandleKeyPress(void)
{
	if (pCurrScreen == NULL || pCurrButton == NULL)
		return 0;

	if (pCurrScreen->userFunctionNum != 0)
	{
		// notify the user function first
		if ((fpUserFunctions[pCurrScreen->userFunctionNum - 1])(0) != 0)
		{
			// user function handled the key press
			feNewPad = 0;
		}
	}

	if (feNewPad & MPAD_CROSS)
	{	
		int action = pCurrButton->action >> 8;

		if (action != BTN_DISABLED)
		{
			int defer = 0;

			FESound(2);

			if (pCurrButton->var != -1)
				SetVariable(pCurrButton->var);

			/* JERICHO-HOOK: the Take-a-Ride city confirm. Screen 1 navigates
			 * through the button actions below rather than through a screen
			 * function, so the module hook is fired HERE -- after SetVariable has
			 * committed the picked city to GameLevel -- and a module that DEFERS
			 * owns the start (the switch below is skipped), exactly as on the
			 * mission/cutscene city screen, which fires the same hook. */
			if (pCurrScreen == &PsxScreens[JERICHO_TAKEARIDE_CITY_SCREEN])
				defer = JerichoFrontendCityConfirm();

			if (!defer) switch (action)
			{
				case BTN_NEXT_SCREEN:
					pScreenStack[ScreenDepth] = pCurrScreen;
					pButtonStack[ScreenDepth] = pCurrButton;

					ScreenNames[ScreenDepth] = pCurrButton->Name;

					pNewScreen = &PsxScreens[pCurrButton->action & 0xFF];

					if (ScreenDepth < 10)
						ScreenDepth++;

					break;
				case BTN_START_GAME:
				{
					/* JERICHO-HOOK: a multiplayer module may own the start --
					 * it broadcasts the level the stock screens just picked
					 * and launches locally instead of the single-player
					 * state change. */
					JER_ARGS_MP_FRONTEND jerMp;
					jerMp.action = JER_MP_FE_START;
					jerMp.query = 0;
					jerMp.claimed = 0;
					jerMp.passthrough = 0;
					jer_fire(JER_EVENT_MP_FRONTEND, &jerMp);

					if (jerMp.claimed && !jerMp.passthrough)
						break;

					if (NumPlayers == 2 && iScreenSelect == SCREEN_CAR && (currPlayer == 2))
					{
						(fpUserFunctions[pCurrScreen->userFunctionNum - 1])(1);
						bRedrawFrontend = 1;
					}
					else
					{
						pScreenStack[ScreenDepth] = pCurrScreen;
						pButtonStack[ScreenDepth] = pCurrButton;

						ScreenNames[ScreenDepth] = pCurrButton->Name;

						SetState(STATE_GAMESTART);
					}
					break;
				}
				case BTN_PREVIOUS_SCREEN:
					if (ScreenDepth > 0)
					{
						if (--ScreenDepth == 0)
							NumPlayers = 1;

						pNewScreen = pScreenStack[ScreenDepth];
						pNewButton = pButtonStack[ScreenDepth];
					}
					break;
			}
		}
	}
	else if (feNewPad & MPAD_TRIANGLE)
	{
		if (ScreenDepth > 0)
		{
			if (!bDoneAllready)
				FESound(0);
			else 
				bDoneAllready = 0;

			if (--ScreenDepth == 0)
			{
				gWantNight = 0;
				gSubGameNumber = 0;
				NumPlayers = 1;
			}

			pNewScreen = pScreenStack[ScreenDepth];
			pNewButton = pButtonStack[ScreenDepth];
		}
	}
	else
	{
		// any d-pad buttons pressed?
		if ((feNewPad & (MPAD_D_UP | MPAD_D_RIGHT | MPAD_D_DOWN | MPAD_D_LEFT)) != 0)
		{
			NewSelection(feNewPad);
		}
		else
		{
			// button pressed, but we didn't handle it
			return 1;
		}
	}

	idle_timer = VSync(-1);

	return 1;
}


// [D] [T]
void PadChecks(void)
{
	int i;
	// TODO: null check for pCurrScreen

	int oldnum = numPadsConnected;

	ReadControllers();
	
	numPadsConnected = 0;
	feNewPad = Pads[0].mapnew;
	fePad = Pads[0].mapped;

	/* JERICHO TEST LEVER (JERICHO_TEST_FEPRESS=<screen>[:<row>[<action>]],...):
	 * press a pad button on the named frontend screen with the cursor on row <row>
	 * (0 = the top row, reached by pressing Down), and log every frontend screen
	 * change so what the press caused is visible. Steps run in order: only the
	 * FIRST screen is navigated to; every later step's screen has to be the one the
	 * previous press produced, or the chain the press should have caused did not
	 * happen.
	 *
	 * <action> is optional and defaults to a single Cross:
	 *     (nothing)   press Cross
	 *     R / L       press RIGHT / LEFT, for an ADJUST row
	 *     R<n> / L<n> press it n times
	 *
	 * The action exists because Cross alone cannot get past an ADJUST row: a menu
	 * whose rows are left/right values (mp's Host Game session settings, for one)
	 * stops a Cross-only walk dead, and the step after it never runs -- which is
	 * exactly what "the harness needs a human to finish that menu" looks like from
	 * the outside. Nothing else had to change: Cross stays the default, so every
	 * existing step list means what it always did.
	 *
	 * e.g. JERICHO_TEST_FEPRESS=1:0    answers "take-a-ride city, first city";
	 *      JERICHO_TEST_FEPRESS=1:0,3:0 then goes on to time of day;
	 *      JERICHO_TEST_FEPRESS=3:0R2   sets row 0 of the Host Game page two steps
	 *                                   to the right, then...
	 *      JERICHO_TEST_FEPRESS=3:0R2,3:4 carries on to row 4 (Take a Ride) there.
	 *
	 * The frontend reads Pads[0] straight out of ReadControllers() above, and a
	 * module cannot inject a pad: its FRAME hook runs before this function, so
	 * anything it wrote to Pads[0] is overwritten here. A screen that is only left
	 * by a real press is therefore unreachable in a padless run, and the hooks that
	 * press fires -- the Take-a-Ride city confirm above all -- cannot be asserted
	 * any other way. Inert unless set. */
	{
		#define JERICHO_FE_PRESS_MAX 12
		static int feStepScreens[JERICHO_FE_PRESS_MAX];
		static int feStepRows[JERICHO_FE_PRESS_MAX];
		static int feStepPad[JERICHO_FE_PRESS_MAX];	/* MPAD_* for the step's press */
		static int feStepPresses[JERICHO_FE_PRESS_MAX];	/* how many times, 1 for Cross */
		static int feStepLeft;				/* presses still owed this step */
		static int feStepCount = -1;	/* -1 = not parsed yet */
		static int feStepIdx;
		static int feStepRow;
		static int feStepFrames;
		static int feStepLast = -2;
		const char* fep = getenv("JERICHO_TEST_FEPRESS");

		if (fep != NULL)
		{
			int screenCount = (int)(sizeof(PsxScreens) / sizeof(PsxScreens[0]));
			int cur = (pCurrScreen != NULL && pCurrScreen >= PsxScreens && pCurrScreen < PsxScreens + screenCount)
				? (int)(pCurrScreen - PsxScreens) : -1;

			if (feStepCount < 0)
			{
				const char* s = fep;

				feStepCount = 0;

				while (*s != '\0' && feStepCount < JERICHO_FE_PRESS_MAX)
				{
					feStepScreens[feStepCount] = atoi(s);

					while (*s != '\0' && *s != ',' && *s != ':')
						s++;

					feStepRows[feStepCount] = (*s == ':') ? atoi(s + 1) : 0;

					feStepPad[feStepCount] = MPAD_CROSS;
					feStepPresses[feStepCount] = 1;

					if (*s == ':')
					{
						const char* t = s + 1;

						while (*t >= '0' && *t <= '9')
							t++;

						if (*t == 'L' || *t == 'l' || *t == 'R' || *t == 'r')
						{
							feStepPad[feStepCount] = (*t == 'L' || *t == 'l') ? MPAD_D_LEFT : MPAD_D_RIGHT;
							feStepPresses[feStepCount] = (t[1] >= '0' && t[1] <= '9') ? atoi(t + 1) : 1;

							if (feStepPresses[feStepCount] < 1)
								feStepPresses[feStepCount] = 1;
						}

						s = t;	/* past the row digits and any action */
					}

					while (*s != '\0' && *s != ',')
						s++;

					if (*s == ',')
						s++;

					feStepCount++;
				}

				jer_log("[jericho] test: JERICHO_TEST_FEPRESS driving %d frontend step(s)\n", feStepCount);
			}

			if (cur != feStepLast)
			{
				feStepLast = cur;
				jer_log("[jericho] test: frontend screen -> %d (JERICHO_TEST_FEPRESS)\n", cur);
			}

			if (feStepIdx < feStepCount)
			{
				int scr = feStepScreens[feStepIdx];
				int row = feStepRows[feStepIdx];

				if (scr < 0 || scr >= screenCount)
				{
					feStepIdx = feStepCount;
				}
				else if (cur != scr)
				{
					/* Only the FIRST step is navigated to: after that, the screen
					 * the previous press produced has to BE the next step's screen. */
					if (feStepIdx == 0)
					{
						pNewScreen = &PsxScreens[scr];
						bRedrawFrontend = 1;
					}

					feStepRow = 0;
					feStepFrames = 0;
					feStepLeft = (feStepIdx < feStepCount) ? feStepPresses[feStepIdx] : 0;
				}
				else if (++feStepFrames >= 2)
				{
					feStepFrames = 0;

					if (feStepRow < row)
					{
						feNewPad |= MPAD_D_DOWN;
						feStepRow++;
					}
					else if (feStepLeft > 0)
					{
						feNewPad |= feStepPad[feStepIdx];
						feStepLeft--;

						jer_log("[jericho] test: JERICHO_TEST_FEPRESS pressed %s on screen %d row %d%s\n",
							(feStepPad[feStepIdx] == MPAD_D_LEFT) ? "LEFT" :
							(feStepPad[feStepIdx] == MPAD_D_RIGHT) ? "RIGHT" : "Cross",
							scr, row, (feStepLeft > 0) ? " (more)" : "");

						if (feStepLeft == 0)
						{
							feStepIdx++;
							feStepRow = 0;

							/* Arm the NEXT step here as well as on entry: when the
							 * following step's screen is already the current one the
							 * mismatch branch never runs, so this is the only place
							 * its press count would ever be set. */
							feStepLeft = (feStepIdx < feStepCount) ? feStepPresses[feStepIdx] : 0;
						}
					}
				}
			}
		}
		#undef JERICHO_FE_PRESS_MAX
	}

	for (int i = 0; i < 2; i++) 
	{
		if (Pads[i].type < 2) 
		{
			padsConnected[i] = 0;
		}
		else 
		{
			padsConnected[i] = 1;
			numPadsConnected++;
		}
	}

#ifndef PSX
	// [A] quit to system
	if(feNewPad & MPAD_TRIANGLE)
	{
		if(ScreenDepth == 0)
		{
			bQuitToSystem ^= 1;
			FESound(0);
		}
	}

	if(bQuitToSystem)
	{
		if (feNewPad & MPAD_CROSS)
		{
			if(bQuitToSystemSel == 1)
				bQuitToSystem = 2;
			else
				bQuitToSystem = 0;

			FESound(2);
		}
		else if ((feNewPad & MPAD_D_LEFT) || (feNewPad & MPAD_D_RIGHT))
		{
			bQuitToSystemSel += 1;
			bQuitToSystemSel &= 1;

			FESound(3);
		}

		feNewPad = 0;
	}
#endif

	if (oldnum != numPadsConnected && (oldnum == 2 || numPadsConnected == 2) && ScreenDepth == 0) 
	{
		bRedrawFrontend = 1;
		MainScreen(1);
	}

	if ((Pads[0].dualshock == 0 || padsConnected[0] == 0) &&
		(Pads[1].dualshock == 0 || padsConnected[1] == 0)) 
	{
		if(allowVibration == 1)
		{
			allowVibration = 0;

			if (pCurrScreen->userFunctionNum == 18)
			{
				bRedrawFrontend = 1;
				GamePlayScreen(1);
			}
		}
	}
	else 
	{
		if (allowVibration == 0)
		{
			allowVibration = 1;

			if (pCurrScreen->userFunctionNum == 18)
			{
				bRedrawFrontend = 1;
				GamePlayScreen(1);
			}
		}
	}

	if (oldnum != numPadsConnected && NumPlayers == 2 && numPadsConnected != NumPlayers || 
		numPadsConnected == 0 || 
		padsConnected[0] == 0) 
	{
		bReturnToMain = 1;
		bRedrawFrontend = 1;
		feNewPad = 0x10;

		if (pCurrScreen->userFunctionNum != 0) 
		{
			(fpUserFunctions[pCurrScreen->userFunctionNum - 1])(0);
		}

		feNewPad = 0;

		if (ScreenDepth != 0) 
		{
			ReInitScreens(bReturnToMain);
		}
	}
	if (bRedrawFrontend == 0 && numPadsConnected != oldnum &&
		(gInFrontend != 0 && (pCurrScreen != NULL && pCurrScreen->userFunctionNum != 0)))
	{
		(fpUserFunctions[pCurrScreen->userFunctionNum - 1])(1);
		bRedrawFrontend = 1;
	}
}

// [D] [T]
void SetFEDrawMode(void)
{
	SetVideoMode(video_mode);

	SetDefDrawEnv(&MPBuff[0][0].draw, draw_mode.x1, draw_mode.y1, 640, 512);
	SetDefDispEnv(&MPBuff[0][0].disp, draw_mode.x1, draw_mode.y1, 640, 512);
	SetDefDrawEnv(&MPBuff[0][1].draw, draw_mode.x1, draw_mode.y1, 640, 512);
	SetDefDispEnv(&MPBuff[0][1].disp, draw_mode.x1, draw_mode.y1, 640, 512);

	MPBuff[0][0].draw.isbg = 0;
#ifdef PSX
	MPBuff[0][0].disp.isinter = 1;
#endif
	MPBuff[0][0].draw.dfe = 1;
	MPBuff[0][0].disp.screen.h = 256;
	MPBuff[0][0].primtab = (char*)_primTab1;
	MPBuff[0][0].primptr = (char*)_primTab1;
	MPBuff[0][0].ot = (OTTYPE*)_OT1;

	MPBuff[0][1].draw.isbg = 0;
#ifdef PSX
	MPBuff[0][1].disp.isinter = 1;
#endif
	MPBuff[0][1].draw.dfe = 1;
	MPBuff[0][1].disp.screen.h = 256;
	MPBuff[0][1].primtab = (char*)_primTab2;
	MPBuff[0][1].primptr = (char*)_primTab2;
	MPBuff[0][1].ot = (OTTYPE*)_OT2;

	last = &MPBuff[0][1];
	current = &MPBuff[0][0];

	MPBuff[0][0].disp.screen.x = draw_mode.framex * 2;
	MPBuff[0][0].disp.screen.y = draw_mode.framey;
	MPBuff[0][1].disp.screen.y = draw_mode.framey;
	MPBuff[0][1].disp.screen.x = MPBuff[0][0].disp.screen.x;

	PutDispEnv(&MPBuff[0][0].disp);
	PutDrawEnv(&current->draw);
}

// [A] - was inlined in State_FrontEnd
void InitFrontend(void)
{
	InitCdIcon();
#ifdef HIRES_FONTS
	InitHiresFEFont();
#endif

	ResetGraph(1);
	SetDispMask(0);

	bRedrawFrontend = 0;
	gInFrontend = 1;

	idle_timer = VSync(-1);

	LoadFrontendScreens(1);

	SetupBackgroundPolys();
	SetupScreenSprts(&PsxScreens[0]);

	// load frontend bank
	LoadBankFromLump(SOUND_BANK_SFX, SBK_ID_MENU);
}

// [A] - was inlined in State_FrontEnd
void InitFrontendDisplay(void)
{
	SetDispMask(0);
	ResetGraph(0);

	SetFEDrawMode();

	EnableDisplay();
#ifdef PSX
	DrawScreen(pCurrScreen);
	EndFrame();

	NewSelection(0);

	// REALLY make sure the screen is cleared
	EndFrame();
	EndFrame();
	EndFrame();
	EndFrame();
	EndFrame();
	EndFrame();
#endif

	SetDispMask(1);
}

/*
 * JERICHO-HOOK: restore the frontend's primitive buffer + ordering table to the
 * state EndFrame() expects.
 *
 * EndFrame() rewinds current->primptr as part of its buffer swap and clears the
 * OT it has drawn. Anything that paints a frame WITHOUT going through EndFrame
 * must do the same, or the OT keeps pointing at the frame that was in flight and
 * the next DrawOTag walks prims that were overwritten - the access violation
 * seen in ParsePrimitivesLinkedList -> ProcessFlatPoly after Save Settings.
 */
void JerichoResetFEPrimState(void)
{
	if (current == NULL || current->primtab == NULL)
		return;

	current->primptr = current->primtab;
	ClearOTagR((u_long*)current->ot, FE_OTSIZE);
}

/*
 * JERICHO-HOOK: frontend primitive-buffer probe.
 *
 * The frontend bumps current->primptr by hand with no bounds check, and
 * ShowSavingWaitMessage (vanilla "Save Settings") opens a scene of its own and
 * draws text WITHOUT rewinding it - so PsyX_EndScene then walks an ordering
 * table that still references the previous frame's primitives. This reports how
 * full the buffer is, plus the notice count (the mod-conflict toast was raised
 * again on every modlist reload, which is exactly when this case came up).
 *
 * JERICHO_FE_TRACE=1 -> one line every ~30 FE frames.
 * Always -> one line whenever free space drops under 8 KB (should be never).
 */
static void JerichoPrimProbe(const DB* db, int notices)
{
	static int sTick;
	int used, freeb;

	if (db == NULL || db->primptr == NULL || db->primtab == NULL)
		return;

	used = (int)(db->primptr - db->primtab);
	freeb = PRIMTAB_SIZE - used;

	if (getenv("JERICHO_FE_TRACE") != NULL && (sTick++ % 30) == 0)
		jer_log("[fe-prim] used=%d free=%d notices=%d\n", used, freeb, notices);

	if (freeb < 8192 && (sTick % 30) == 0)
		jer_log("[fe-prim] LOW free=%d used=%d notices=%d PRIMTAB_SIZE=%d\n",
			freeb, used, notices, PRIMTAB_SIZE);
}

/* ------------------------------------------------------------------ *
 * JERICHO-HOOK: notice toasts (jer_error - see JERICHO/include/jericho.h).
 *
 * The "something needs to be told, now" channel: a block of gentle-red text
 * down the LEFT of the frontend frame, for a few seconds. One notice is one
 * line -- a longer message is wrapped by jer_error into several single-line
 * notices, which then stack down the screen.
 *
 * The step below is the FRONTEND font's own line pitch, NOT the in-game one.
 * A FEFONT.BNK glyph cell is 36px tall and the frontend lays its own rows out
 * at 36 as well (see the module-menu and mods-screen button layouts in this
 * file), so the 18 that used to sit here stacked two rows on top of each
 * other. The in-game notices keep 12, because the in-game font is a third of
 * the size (Game/C/main.c).
 * ------------------------------------------------------------------ */
#define JER_FE_NOTICE_X		32	/* left margin */
#define JER_FE_NOTICE_Y		140	/* first line, from the top */
#define JER_FE_NOTICE_LINE	36	/* FEFONT.BNK cell height = frontend row pitch */

static void JerichoDrawNotices(void)
{
	int n = jer_error_count(), k;

	for (k = 0; k < n; k++)
	{
		const char* msg = jer_error_at(k);

		if (msg != NULL)
			FEPrintString((char*)msg, JER_FE_NOTICE_X, JER_FE_NOTICE_Y + k * JER_FE_NOTICE_LINE, 0, 215, 70, 70);
	}
}

// [D] [T]
void State_FrontEnd(void* param)
{
	// JERICHO-HOOK: -frames applies to frontend runs too, so they self-terminate and
	// leave a complete log rather than needing a kill. See JerichoFrameTick in main.c.
	extern void JerichoFrameTick(void);

	JerichoFrameTick();

	// JERICHO-HOOK: the frontend ticks the game-frame event every frame so
	// module menus (e.g. levelhacks' SP/MP prompt) can read input + draw
	// while the frontend is frozen behind them
	jer_fire(JER_EVENT_FRAME, NULL);

	// JERICHO-HOOK: advance any presentation screen (jer_screen.h) raised at
	// runtime; it is drawn with the notices just below.
	jer_screen_tick();

	/* presentation screens sit under the notices */
	JerichoDrawScreen();

	// JERICHO-HOOK: error notices (engine + modules) -- gentle red, left of
	// the screen, ~5 s (e.g. "invalid command line argument")
	JerichoDrawNotices();

	// JERICHO-HOOK: the status console (jer_console.h) -- the unified bottom-left
	// stream, frontend half of the two draw sites (the other is in-game, DrawGame).
	// Drawn here because the connection handshake lines it shows happen while the
	// frontend is up; the ring is shared, so they are still there in-game too.
	jer_console_draw(1);

	PadChecks();

	if (currPlayer == 2)
	{
		if (Pads[1].type < 2)
		{
			feNewPad = ((feNewPad & MPAD_TRIANGLE) != 0) ? MPAD_TRIANGLE : 0;
		}
		else 
		{
			feNewPad = Pads[1].mapnew;
		}
	}

	// JERICHO-HOOK: a host prompt (jer_prompt.h) owns the pad while it is up.
	// Fed here — after the pads are read, before the screen gets a turn — and
	// consumed (feNewPad = 0) so a Cross that answers the prompt cannot also
	// press the button underneath it.
	if (jer_prompt_active())
	{
		jer_prompt_tick((feNewPad & MPAD_D_LEFT) != 0, (feNewPad & MPAD_D_RIGHT) != 0,
			(feNewPad & MPAD_CROSS) != 0, (feNewPad & MPAD_CIRCLE) != 0);

		feNewPad = 0;
	}

	if (HandleKeyPress())
	{
		if (pNewScreen != NULL)
		{
			SetupScreenSprts(pNewScreen);
			bRedrawFrontend = 1;
		}
	}

#ifndef PSX
	DrawScreen(pCurrScreen);
#else
	if (bRedrawFrontend)
	{
		DrawSync(0);
		EndFrame();

		DrawScreen(pCurrScreen);

		EndFrame();

		NewSelection(0);

		bRedrawFrontend = 0;
	}
#endif

	/* JERICHO-HOOK: the screen has just been drawn, so this is where a module
	 * puts text ON a menu row. NOT JER_EVENT_FRAME: that fires earlier in this
	 * function and the menu box drawn below it would cover the text.
	 *
	 * ...unless a host prompt (jer_prompt.h) is up: it owns the screen, and a
	 * question with a module's table painted through it is not a prompt. */
	if (!jer_prompt_active())
	{
		JER_ARGS_FRONTEND_DRAW drawArgs;

		drawArgs.menu = jer_frontend_current_menu();
		jer_fire(JER_EVENT_FRONTEND_DRAW, &drawArgs);
	}

	/* JERICHO: the update check (JERICHO/src/jer_update.c) -- decides once, on
	 * the main menu, and draws the changelog on its own screen. */
	jer_update_frontend_tick(pCurrScreen == &PsxScreens[0] && ScreenDepth == 0);
	jer_rebake_frontend_tick(pCurrScreen == &PsxScreens[0] && ScreenDepth == 0);

	/* JERICHO: the mod store (JERICHO/src/jer_modstore.c) -- polls a running
	 * install/remove/index job and draws its own tables. Runs every frontend
	 * frame, like the updater above: a job is a child process, never a block. */
	/* ...and it is told whether the MAIN MENU is up, for the same reason the updater
	 * is: the store's prompt must not be raised over the changelog screen. */
	jer_modstore_frontend_tick(pCurrScreen == &PsxScreens[0] && ScreenDepth == 0);

	/* A host prompt is drawn here rather than on the screen path: it must be
	 * visible whatever is behind it (a stock screen OR a module menu), and it has
	 * already suppressed the screen's own buttons. */
	if (jer_prompt_active())
		JerichoDrawPrompt();

	/* JERICHO: the Credits page draws its lines over the screen, like a module menu */
	if (gJerichoCreditsOpen && pCurrScreen == &PsxScreens[JERICHO_MODS_SCREEN])
		JerichoDrawCredits();

	/* JERICHO: the build, small and bottom-left, on the Options screen only (the
	 * screen the main menu's Options button opens) -- the window title says the
	 * same thing (GAME_VERSION_FULL, version.h). */
	if ((PsxScreens[0].buttons[4].action >> 8) == BTN_NEXT_SCREEN &&
	    pCurrScreen == &PsxScreens[PsxScreens[0].buttons[4].action & 0xFF])
		jer_text_draw_at(JER_TEXT_FE, 100, 24, 448, GAME_VERSION_FULL, 255, 255, 255);

	/* Ask the modules before the attract demo starts. A multiplayer lobby is
	 * idle by definition, so without this the demo would launch a level nobody
	 * asked for -- and that load blocks the main thread for its whole
	 * duration, dropping every player who was joining. */
	{
		JER_ARGS_FRONTEND_IDLE idleArgs;

		idleArgs.suppress = 0;
		jer_fire(JER_EVENT_FRONTEND_IDLE, &idleArgs);

		/* JERICHO: the update screen is not idle time -- hold the clock so the
		 * attract demo cannot start a level from under it. */
		if (idleArgs.suppress || jer_update_screen_active())
			idle_timer = VSync(-1);
	}

	if ((VSync(-1) - idle_timer) > 1800)
	{
		if (ScreenDepth == 0)
		{
			GameType = GAME_IDLEDEMO;

			gCurrentMissionNumber = gIdleReplay + 400;
			
			if (++gIdleReplay == 4)
				gIdleReplay = 0;

			pScreenStack[0] = pCurrScreen;
			pButtonStack[0] = pCurrButton;
			ScreenNames[0] = pCurrButton->Name;

			SetState(STATE_GAMESTART);

			pCurrScreen = pScreenStack[0];

			bRedrawFrontend = 1;
			ScreenDepth = 0; // fail-safe?
		}

		idle_timer = VSync(-1);
	}

	if(bQuitToSystem == 2)
		SetState(STATE_NONE);
}

// [D] [T]
void EndFrame(void)
{
	DB *db_hold;

	DrawSync(0);
	VSync(0);

	PutDispEnv(&current->disp);
	PutDrawEnv(&current->draw);

	db_hold = current;
	current = last;

	JerichoPrimProbe(last, jer_error_count());

	last->primptr = last->primtab;
	last = db_hold;

	DrawOTag((u_long*)(db_hold->ot + FE_OTSIZE - 1));

	ClearOTagR((u_long*)current->ot, FE_OTSIZE);
	VSync(0);

#ifndef PSX
	PsyX_EndScene();

	// JERICHO: -shotfront <frame> -- capture one presented FRONTEND frame. It has to be
	// here, at the end of EndFrame: -shot counts gameplay frames (gRunFrames), which do
	// not advance while the menus are up, and a screenshot is only meaningful once the
	// frame has been presented.
	if (gScreenshotFrameFront >= 0 && ++gFrontendFrames >= gScreenshotFrameFront)
	{
		gScreenshotFrameFront = -1;
		PsyX_TakeScreenshot();
		printInfo("JERICHO-SHOT: wrote SCREENSHOT.BMP on frontend frame %d\n", gFrontendFrames);
	}
#endif
}

// [A]
int FEStringWidth(char* string)
{
	char* pString = string;
	u_char c = 0;
	int w = 0;

#ifdef HIRES_FONTS
	if (gHiresFEFontTexture)
	{
		return FEStringWidthHires(string);
	}
#endif

	while ((c = *pString++) != 0)
	{
		if (c == ' ')
			w += 4;
		else
			w += feFont.CharInfo[c].w;
	}

	return w;
}

// [D] [T]
int FEPrintString(char *string, int x, int y, int justification, int r, int g, int b)
{
	if (current == NULL || string == NULL)
		return -1;

#ifdef HIRES_FONTS
	if (gHiresFEFontTexture)
	{
		return FEPrintStringHires(string, x, y, justification, r, g, b);
	}
#endif

	FE_CHARDATA *pFontInfo;
	SPRT *font;
	u_char let;

	if (justification & 4)
	{
		x -= FEStringWidth(string);
	}

	font = (SPRT*)current->primptr;

	int counter = 0;

	while ((let = *string++) != 0)
	{
		if (let == ' ')
		{
			x += 4;
		}
		else
		{
			pFontInfo = &feFont.CharInfo[let];

			setSprt(font);
			setSemiTrans(font, 1);

			setRGB0(font, r, g, b);
			setXY0(font, x, y);
			setUV0(font, pFontInfo->u, pFontInfo->v);
			setWH(font, pFontInfo->w, pFontInfo->h);
			// setTPage(font, 0, 0, 640, 256); // [A]
			setClut(font, 960, 257); // [A] seems clut has a transparency bit; width is 256

			addPrim(current->ot + 1, font);
			font++;

			// add space for next character
			x += pFontInfo->w;
		}

		if (++counter >= 32)
			break;
	}

	// set tail
	current->primptr = (char *)font;

	POLY_FT3 *null = (POLY_FT3 *)current->primptr;
	setPolyFT3(null);
	setSemiTrans(null, 1);
	setXY3(null, -1, -1, -1, -1, -1, -1);
	setTPage(null, 0, 0, 640, 256);

	addPrim(current->ot + 1, null);
	null++;

	current->primptr = (char *)null;

	return x;
}


// [D] [T]
int FEPrintStringSized(char *string, int x, int y, int scale, int transparent, int r, int g, int b)
{
	if (current == NULL || string == NULL)
		return -1;

#ifdef HIRES_FONTS
	if (gHiresFEFontTexture)
	{
		return FEPrintStringSizedHires(string, x, y, scale, transparent, r, g, b);
	}
#endif

	POLY_FT4 *font;
	FE_CHARDATA *pFontInfo;
	u_char let;
	int w;
	int h;

	font = (POLY_FT4 *)current->primptr;

	while ((let = *string++) != 0)
	{
		if (let == '\n')
			continue;

		if (let == ' ')
		{
			// add space
			x += 4;
		}
		else
		{
			pFontInfo = &feFont.CharInfo[let];

			w = (pFontInfo->w * scale) / 4096;
			h = (pFontInfo->h * scale) / 4096;

			setPolyFT4(font);
			setSemiTrans(font, transparent);

			setRGB0(font, 128, 128, 128);
			setUVWH(font, pFontInfo->u, pFontInfo->v, pFontInfo->w - 1, pFontInfo->h - 1);
			setXYWH(font, x, y, w, h);
			setRGB0(font, r, g, b);
			setTPage(font, 0, 0, 640, 256);
			setClut(font, 960, 257); // [A] seems clut has a transparency bit; width is 256

			addPrim(current->ot + 1, font);
			font++;

			// make room for next character
			x += w;
		}
	}

	// set tail
	current->primptr = (char *)font;

	return x;
}


SCREEN_LIMITS screen_limits[2] = {
	// min(x/y), max(x/y)
	{ -27,   5, 35, 37 },
	{ -32, -11, 32, 21 },
};

int bScreenSetup = 0;

// [D] [T]
int CentreScreen(int bSetup)
{
	if (bSetup)
		return 0;

#if defined(DEBUG) && !defined(PSX)
	char text[32];

	sprintf(text, "X1: %d, Y1: %d", current->disp.screen.x, current->disp.screen.y);
	FEPrintStringSized(text, 25, 50, 3072, 0, 128, 0, 0);

	sprintf(text, "X2: %d, Y2: %d", last->disp.screen.x, last->disp.screen.y);
	FEPrintStringSized(text, 25, 75, 3072, 0, 128, 0, 0);
#endif

	if (feNewPad & MPAD_CROSS)
	{
		draw_mode_pal.framex = current->disp.screen.x / 2;
		draw_mode_pal.framey = current->disp.screen.y;
		draw_mode_ntsc.framex = current->disp.screen.x / 2;
		draw_mode_ntsc.framey = current->disp.screen.y;
	}
	else if (feNewPad & MPAD_TRIANGLE)
	{
		current->disp.screen.x = draw_mode.framex * 2;
		current->disp.screen.y = draw_mode.framey;
		last->disp.screen.x = draw_mode.framex * 2;
		last->disp.screen.y = draw_mode.framey;
	}
	else
	{
		bool done = false;

		if (feNewPad & MPAD_D_UP)
		{
			if (current->disp.screen.y >= screen_limits[video_mode].miny)
			{
				current->disp.screen.y--;
				last->disp.screen.y--;
				done = true;
			}
		}
		else if (feNewPad & MPAD_D_DOWN)
		{
			if (current->disp.screen.y <= screen_limits[video_mode].maxy)
			{
				current->disp.screen.y++;
				last->disp.screen.y++;
				done = true;
			}
		}
		else if (feNewPad & MPAD_D_LEFT)
		{
			if (current->disp.screen.x >= screen_limits[video_mode].minx)
			{
				current->disp.screen.x--;
				last->disp.screen.x--;
				done = true;
			}
		}
		else if (feNewPad & MPAD_D_RIGHT)
		{
			if (current->disp.screen.x <= screen_limits[video_mode].maxx)
			{
				current->disp.screen.x++;
				last->disp.screen.x++;
				done = true;
			}
		}
		else
		{
			// unhandled button press
			return 0;
		}

		FESound((done) ? 3 : 1);
	}

	return 0;
}


#ifndef PSX
/* Put the chosen CITY back as the frontend's "extra" image (the city picture
 * drawn over the CITYBACK.RAW backdrop). The car-select screen swaps that extra
 * to a CAR icon while it is up, so a screen entered from it must put the city
 * back - the Day/Night screen is the one that shows the city after a car is
 * picked (see TimeOfDaySelectScreen). The city image is reused from
 * _frontend_buffer when it is still loaded (loaded[0] != -1), else reloaded. */
static void FrontendShowCityExtra(void)
{
	RECT16 rect = extraRect;

	LoadBackgroundFile("DATA\\CITYBACK.RAW");

	if (loaded[0] == -1)
	{
		SetupExtraPoly("DATA\\CITY.RAW", currCity, 0);
	}
	else
	{
		bDrawExtra = 1;
		LoadImage(&rect, (u_long*)(_frontend_buffer + currCity * 0x8000));
		DrawSync(0);
	}
}
#endif

// [D] [T]
int CarSelectScreen(int bSetup)
{
	int newSel;
	RECT16 rect;

	newSel = carSelection;
	rect = extraRect;

	if (bSetup)
	{
		iScreenSelect = SCREEN_CAR;

		// setup secret cars
		if (NumPlayers == 1)
		{
			CarAvailability[0][9] = AvailableCheats.cheat5;
			CarAvailability[1][9] = AvailableCheats.cheat6;
			CarAvailability[2][9] = AvailableCheats.cheat7;
			CarAvailability[3][9] = AvailableCheats.cheat8;
		}

		// setup unlockable cars
		// JERICHO-HOOK: a module may lift the progression + single-player gate on
		// the extra vehicles for this level (JER_EVENT_CAR_AVAILABILITY). The
		// per-model data checks below still apply, so a model with no data stays
		// unavailable (forcing one crashed the game in load).
		int unlockExtra = 0;
		int ownList = 0;
		{
			JER_ARGS_CAR_AVAILABILITY jerAvail;

			/* JERICHO: re-scan for runtime city banks first, so the list a module
			 * answers about includes - or has stopped including - a bank whose files
			 * were installed or removed since this screen was last built. */
			JerCityRegistryRefresh();

			jerAvail.level = GameLevel;
			jerAvail.result = 0;
			jerAvail.own_list = 0;
			jer_fire(JER_EVENT_CAR_AVAILABILITY, &jerAvail);
			unlockExtra = jerAvail.result;
			ownList = jerAvail.own_list;
		}

		if (ownList)
		{
			// JERICHO-HOOK: a module wrote the WHOLE table (every city, every slot) and
			// owns the list, so re-gating here would only throw its work away. These gates
			// describe one row - CarAvailability[GameLevel][...] - because that row is all
			// the stock screen ever reads, which is why a player could not pick the rest of
			// the cars the game has: the table starts as {1,1,1,1,0,...} for every city and
			// is lifted for the extras only by game completion, by a cheat, or by the hook
			// above - and the cheat slot is cleared outright in multiplayer (NumPlayers == 1
			// gates it), so it can never appear in a session.
		}
		else if ((gFurthestMission == 40 && NumPlayers == 1) || unlockExtra)
		{
			for (int i = 4; i < 9; i++)
			{
#if USE_PC_FILESYSTEM
				// [A] minimal requirement for this car to be selectable
				extern int gContentOverride;
				if (i == 8)
					CarAvailability[0][i] = gContentOverride && FileExists("LEVELS\\CHICAGO\\CARMODEL_11_clean.MDL");	// remove truck
				else
					CarAvailability[0][i] = 1;
#else
				if (i != 8)
					CarAvailability[0][i] = 1;	// remove truck
#endif
				CarAvailability[1][i] = 1;
				CarAvailability[2][i] = 1;
				CarAvailability[3][i] = 1;
			}
		}
		else
		{
			for (int i = 4; i < 9; i++)
			{
				// unlock the cop car only
				int unlocked = (i == 4) && gFurthestMission == 40;

				CarAvailability[0][i] = unlocked; 
				CarAvailability[1][i] = unlocked;
				CarAvailability[2][i] = unlocked;
				CarAvailability[3][i] = unlocked;
			}
		}

#ifdef PSX
		if (currPlayer != 1) 
			return 0;
#endif

		LoadBackgroundFile("DATA\\CARS\\CARBACK.RAW");

		if (feVariableSave[0] != -1)
		{
			carSelection = feVariableSave[0];
			SetupExtraPoly(gfxNames[GameLevel], carSelection, 0);
		}
		else
		{
			carSelection = 0;

			if (loaded[1] == GameLevel)
			{
				bDrawExtra = 1;

				LoadImage(&rect, (u_long*)_frontend_buffer);
				DrawSync(0);
			}
			else
			{
				SetupExtraPoly(gfxNames[GameLevel], carSelection, 0);
				lastCity = GameLevel;
			}
		}

		feVariableSave[0] = -1;
		feVariableSave[1] = -1;
		feVariableSave[2] = -1;
		feVariableSave[3] = -1;

		lastCutCity = -1;
		currSelIndex = 1;

		pCurrButton = &pCurrScreen->buttons[1];

		return 1;
	}

	if (feNewPad & MPAD_TRIANGLE)
	{
		FESound(0);
		bDoneAllready = 1;
#ifdef PSX
		LoadBackgroundFile("DATA\\GFX.RAW");
		bDrawExtra = 0;
#else
		/* back to the Day/Night screen: show the CITY again, not the car */
		FrontendShowCityExtra();
#endif
		currPlayer = 1;
		iScreenSelect = SCREEN_NONE;
	}

	else if (feNewPad & MPAD_CROSS)
	{
		if (currSelIndex == 0)
		{
			// find best-fit for previous vehicle
			do
			{
				newSel--;
				if (newSel < 0)
					newSel = 9;

			} while (CarAvailability[GameLevel][newSel] == 0);

			carSelection = newSel;
		}
		else if (currSelIndex == 2)
		{
			// find best-fit for next vehicle
			do
			{
				newSel++;
				if (newSel > 9)
					newSel = 0;

			} while (CarAvailability[GameLevel][newSel] == 0);

			carSelection = newSel;
		}
		else
		{
			// select the vehicle
			if (currPlayer == 1)
			{
				feVariableSave[0] = carSelection;
				wantedCar[0] = carNumLookup[GameLevel][carSelection];
			}
			else {
				wantedCar[1] = carNumLookup[GameLevel][carSelection];
			}

			// time for player 2 to select their vehicle?
			if (NumPlayers == 2)
				currPlayer++;

			return 0;
		}
		
		rect = extraRect;
		LoadImage(&rect, (u_long*)(_frontend_buffer + carSelection * 0x8000));
		DrawSync(0);

#ifdef PSX
		DisplayOnScreenText();

		addPrim(current->ot + 2, &extraSprt);
		addPrim(current->ot + 2, &extraDummy);

		EndFrame();
#endif
	}
	else if (feNewPad & MPAD_D_UP)
	{
		currSelIndex = pCurrButton->u - 1;
	}
	else if (feNewPad & MPAD_D_DOWN)
	{
		currSelIndex = pCurrButton->d - 1;
	}
#ifndef PSX
	else if (feNewPad & MPAD_SQUARE)
	{
		/* JERICHO: SQUARE cycles the player's car palette (the six colour variants;
		 * the preview picture cannot show it, so a notice names it). The spawn reads
		 * defaultPlayerPalette; mp saves it to mp.ini [mp] palette.
		 *
		 * The six steps are the engine's six civ_clut slots: 0 is the page's own
		 * CLUT and 1..5 are variants. Stock D2 fills 1..5; a Driver 1 car fills
		 * only 1..4, so its last step clamps to the fourth and the two highest
		 * steps repeat. Kept global on purpose: this point in the frontend does
		 * not know the selected car's source city, and the same duplicate already
		 * exists for a stock D2 car whose row has fewer than five variants. */
		extern u_char defaultPlayerPalette;
		char msg[48];

		defaultPlayerPalette = (u_char)((defaultPlayerPalette + 1) % 6);
		snprintf(msg, sizeof(msg), "Palette %d/6", defaultPlayerPalette + 1);
		jer_notify(msg, 0, 2);
		FESound(3);
	}
#endif

	return 0;
}

// [D] [T]
int CopDiffLevelScreen(int bSetup)
{
	if (bSetup) 
	{
		pCurrButton = &pCurrScreen->buttons[gCopDifficultyLevel];
		return 1;
	}

	if (feNewPad & MPAD_CROSS)
	{
		gCopDifficultyLevel = currSelIndex;
	}
	else if (feNewPad & MPAD_D_UP)
	{
		currSelIndex = pCurrButton->u - 1;
	}
	else if (feNewPad & MPAD_D_DOWN)
	{
		currSelIndex = pCurrButton->d - 1;
	}

	return 0;
}

// [D] [T]
int VibroOnOffScreen(int bSetup)
{
	if (bSetup) 
	{
		currSelIndex = (gVibration ^ 1);
		pCurrButton = &pCurrScreen->buttons[currSelIndex];
		return 1;
	}

	if (feNewPad & MPAD_CROSS)
	{
		gVibration = (currSelIndex ^ 1);
	}
	else if (feNewPad & MPAD_D_UP)
	{
		currSelIndex = pCurrButton->u - 1;
	}
	else if (feNewPad & MPAD_D_DOWN)
	{
		currSelIndex = pCurrButton->d - 1;
	}

	return 0;
}


// [D] [T]
int MissionSelectScreen(int bSetup)
{
	int i;

	if (bSetup) 
	{
		iScreenSelect = SCREEN_MISSION;

		if (!missionSetup)
		{
			currMission = minmaxSelections[currCity][0];
			currSelIndex = 0;

			if (GameType == GAME_REPLAYMISSION)
				LoadBackgroundFile("DATA\\CITYBACK.RAW");
		}

		if (feVariableSave[0] != -1) 
		{
			currMission = feVariableSave[0];
			currSelIndex = feVariableSave[1];
			currCity = feVariableSave[2];
		}

		int usedB = 0;

		bool done = false;
		bool bP = false; // 'Previous' button is visible?
		bool bN = false; // 'Next' button is visible?

		for (i = 0; (i < 4) && !done; i++)
		{
			if (botch[currMission + i].missNum > gFurthestMission ||
				currMission + i >= minmaxSelections[currCity][1] ||
				currMission + i > 36) 
			{
				done = true;
			}
			else
			{
				usedB++;
			}
		}

		switch (usedB)
		{
		case 1:
			pCurrScreen->buttons[1].u = 2;
			pCurrScreen->buttons[1].d = 2;
			sprintf(pCurrScreen->buttons[1].Name, GET_MISSION_TXT(MissionName[currMission]));
			sprintf(pCurrScreen->buttons[2].Name, NullStr);
			sprintf(pCurrScreen->buttons[3].Name, NullStr);
			sprintf(pCurrScreen->buttons[4].Name, NullStr);
			break;
		case 2:
			pCurrScreen->buttons[1].u = 3;
			pCurrScreen->buttons[1].d = 3;
			pCurrScreen->buttons[2].u = 2;
			pCurrScreen->buttons[2].d = 2;
			sprintf(pCurrScreen->buttons[1].Name, GET_MISSION_TXT(MissionName[currMission]));
			sprintf(pCurrScreen->buttons[2].Name, GET_MISSION_TXT(MissionName[currMission + 1]));
			sprintf(pCurrScreen->buttons[3].Name, NullStr);
			sprintf(pCurrScreen->buttons[4].Name, NullStr);
			break;
		case 3:
			pCurrScreen->buttons[1].u = 4;
			pCurrScreen->buttons[1].d = 3;
			pCurrScreen->buttons[2].u = 2;
			pCurrScreen->buttons[2].d = 4;
			pCurrScreen->buttons[3].u = 3;
			pCurrScreen->buttons[3].d = 2;
			sprintf(pCurrScreen->buttons[1].Name, GET_MISSION_TXT(MissionName[currMission]));
			sprintf(pCurrScreen->buttons[2].Name, GET_MISSION_TXT(MissionName[currMission + 1]));
			sprintf(pCurrScreen->buttons[3].Name, GET_MISSION_TXT(MissionName[currMission + 2]));
			sprintf(pCurrScreen->buttons[4].Name, NullStr);
			break;
		case 4:
			pCurrScreen->buttons[1].u = 5;
			pCurrScreen->buttons[1].d = 3;
			pCurrScreen->buttons[2].u = 2;
			pCurrScreen->buttons[2].d = 4;
			pCurrScreen->buttons[3].u = 3;
			pCurrScreen->buttons[3].d = 5;
			pCurrScreen->buttons[4].u = 4;
			pCurrScreen->buttons[4].d = 2;
			sprintf(pCurrScreen->buttons[1].Name, GET_MISSION_TXT(MissionName[currMission]));
			sprintf(pCurrScreen->buttons[2].Name, GET_MISSION_TXT(MissionName[currMission + 1]));
			sprintf(pCurrScreen->buttons[3].Name, GET_MISSION_TXT(MissionName[currMission + 2]));
			sprintf(pCurrScreen->buttons[4].Name, GET_MISSION_TXT(MissionName[currMission + 3]));
			break;
		}

		if (usedB == 4 &&
			botch[currMission + 4].missNum <= gFurthestMission &&
			currMission + 4 != minmaxSelections[currCity][1])
		{
			bN = true;
		}

		if (bN)
		{
			if (currMission != minmaxSelections[currCity][0])
			{
				bP = true;

				pCurrScreen->buttons[0].u = 6;
				pCurrScreen->buttons[0].d = 2;
				pCurrScreen->buttons[5].u = 5;
				pCurrScreen->buttons[5].d = 1;
				pCurrScreen->buttons[1].u = 1;
				pCurrScreen->buttons[4].d = 6;
				pCurrScreen->buttons[0].action = 0;
				pCurrScreen->buttons[5].action = 0;
			}
			else
			{
				pCurrScreen->buttons[1].u = 6;
				pCurrScreen->buttons[4].d = 6;
				pCurrScreen->buttons[5].u = 5;
				pCurrScreen->buttons[5].d = 2;
				pCurrScreen->buttons[5].action = 0;
				pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_HIDDEN, 0);
			}
		}
		else
		{
			if (currMission != minmaxSelections[currCity][0])
			{
				bP = true;

				pCurrScreen->buttons[0].u = usedB + 1;
				pCurrScreen->buttons[0].d = 2;
				pCurrScreen->buttons[1].u = 1;
				pCurrScreen->buttons[usedB].d = 1;
				pCurrScreen->buttons[0].action = 0;
				pCurrScreen->buttons[5].action = FE_MAKEVAR(BTN_HIDDEN, 0);
			}
			else
			{
				// list missions only
				pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_HIDDEN, 0);
				pCurrScreen->buttons[5].action = FE_MAKEVAR(BTN_HIDDEN, 0);
			}
		}

		if (bP)
		{
			pCurrButton = pCurrScreen->buttons;
			currSelIndex = 0;
		}
		else
		{
			pCurrButton = pCurrScreen->buttons + 1;
			currSelIndex = 1;
		}

		if (loaded[0] == -1)
		{
			SetupExtraPoly("DATA\\CITY.RAW", currCity, 0);
		}
		else
		{
			bDrawExtra = 1;
		}

		feVariableSave[0] = -1;
		feVariableSave[1] = -1;
		feVariableSave[2] = -1;
		feVariableSave[3] = -1;

		missionSetup = 1;

		return 1;
	}

	if (feNewPad & MPAD_CROSS)
	{
		i = currMission;

		if (currSelIndex == 0)
		{
			i -= 4;

			if (i < minmaxSelections[currCity][0])
				return 0;
		}
		else if (currSelIndex == 5)
		{
			i += 4;

			if (i > minmaxSelections[currCity][1] || i > gFurthestMission)
				return 0;
		}
		else
		{
			bReturnToMain = 0;
			GameType = GAME_REPLAYMISSION;
			feVariableSave[0] = currMission;
			feVariableSave[1] = currSelIndex;
			feVariableSave[2] = currCity;
			gCurrentMissionNumber = botch[currMission + (currSelIndex - 1)].missNum;

			return 0;
		}

		currMission = i;
		FESound(3);
		fpUserFunctions[pCurrScreen->userFunctionNum - 1](1);
		bRedrawFrontend = 1;

		return 1;
	}
	else if (feNewPad & MPAD_TRIANGLE)
	{
		missionSetup = 0;
		iScreenSelect = SCREEN_NONE;
	}
	else if (feNewPad & MPAD_D_UP)
	{
		currSelIndex = pCurrButton->u - 1;
	}
	else if (feNewPad & MPAD_D_DOWN)
	{
		currSelIndex = pCurrButton->d - 1;
	}

	return 0;
}

// [D] [T]
int MissionCityScreen(int bSetup)
{
	RECT16 rect;

	if (bSetup)
	{
		GameType = GAME_MISSION;

		if (gFurthestMission == 0)
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);
		}
		else if (gFurthestMission < 10)
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);
		}
		else if (gFurthestMission < 21)
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);
		}
		else if (gFurthestMission < 31)
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);
		}
		else
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 19);
		}

		LoadBackgroundFile("DATA\\CITYBACK.RAW");

		if (loaded[0] == -1)
		{
			SetupExtraPoly("DATA\\CITY.RAW", 0, 0);
		}
		else
		{
			bDrawExtra = 1;
		}

		return 0;
	}

	if (feNewPad & MPAD_TRIANGLE)
	{
		// BUGFIX: unload city image
		loaded[0] = -1;

		bDrawExtra = 0;
		FESound(0);
		bDoneAllready = 1;
		LoadBackgroundFile("DATA\\GFX.RAW");
	}
	else
	{
		if (feNewPad & MPAD_D_UP)
		{
			currCity = pCurrButton->u - 1;
		}
		else if (feNewPad & MPAD_D_DOWN)
		{
			currCity = pCurrButton->d - 1;
		}
		else
		{
			currCity = pCurrButton->u & 3;
			return 0;
		}

		rect = extraRect;
		LoadImage(&rect, (u_long*)(_frontend_buffer + currCity * 0x8000));
		DrawSync(0);

#ifdef PSX
		DisplayOnScreenText();

		addPrim(current->ot + 2, &extraSprt);
		addPrim(current->ot + 2, &extraDummy);

		EndFrame();
#endif
	}
	
	return 0;
}

// [D] [T]
int CutSceneSelectScreen(int bSetup)
{
	RECT16 rect;

	rect = extraRect;

	if (bSetup)
	{
		LoadBackgroundFile("DATA\\CUTS\\CUTBACK.RAW");
		currCity = feVariableSave[1];

		if (feVariableSave[0] == -1)
		{
			cutSelection = 0;
			currCity = GameLevel;
		}
		else
		{
			cutSelection = feVariableSave[0];
			feVariableSave[3] = -1;
			feVariableSave[2] = -1;
			feVariableSave[1] = -1;
			feVariableSave[0] = -1;
		}

		if (loaded[2] == GameLevel)
		{
			bDrawExtra = 1;
			LoadImage(&rect, (u_long*)(_frontend_buffer + 0x20000 + cutSelection * 0x8000));
			DrawSync(0);
		}
		else
		{
			SetupExtraPoly(cutGfxNames[GameLevel], cutSelection, 0x20000);
		}

		currSelIndex = 1;

		iScreenSelect = SCREEN_CUTSCENE;

		pCurrButton = pCurrScreen->buttons + 1;

		return 1;
	}

	if (feNewPad & MPAD_TRIANGLE)
	{
		bDrawExtra = 0;
		iScreenSelect = SCREEN_NONE;
	}
	else if (feNewPad & MPAD_CROSS)
	{
		if (currSelIndex == 0)
		{
			if (cutSelection == 0)
			{
				cutSelection = CutAmounts[currCity + 1] - 1;

				if (cutUnlock[gFurthestMission] < CutAmounts[currCity + 1] - 1 + CutAmountsTotal[currCity] + 1)
					cutSelection = cutUnlock[gFurthestMission];
			}
			else
			{
				cutSelection--;
			}
		}
		else
		{
			if (currSelIndex != 2)
			{
				bReturnToMain = 0;
				pCurrButton = pButtonStack[ScreenDepth];
				pScreenStack[ScreenDepth] = pCurrScreen;

				feVariableSave[1] = currCity;
				feVariableSave[0] = cutSelection;

				ScreenNames[ScreenDepth] = pCurrButton->Name;
				SetState(STATE_FMVPLAY, (void*)(feVariableSave[0] + CutAmountsTotal[feVariableSave[1]] + 1));

				return 1;
			}

			if (cutSelection == CutAmounts[currCity + 1] - 1 || cutUnlock[gFurthestMission] <= cutSelection + CutAmountsTotal[currCity] + 1)
			{
				cutSelection = 0;
			}
			else
			{
				cutSelection++;
			}
		}

		rect = extraRect;
		LoadImage(&rect, (u_long*)(_frontend_buffer + 0x20000 + cutSelection * 0x8000));
		DrawSync(0);
		DisplayOnScreenText();

#ifdef PSX
		DisplayOnScreenText();

		addPrim(current->ot + 2, &extraSprt);
		addPrim(current->ot + 2, &extraDummy);

		EndFrame();
#endif

		return 0;
	}
	else if (feNewPad & MPAD_D_UP)
	{
		currSelIndex = pCurrButton->u - 1;
	}
	else if (feNewPad & MPAD_D_DOWN)
	{
		currSelIndex = pCurrButton->d - 1;
	}

	return 0;
}



// [D] [T]
int CutSceneCitySelectScreen(int bSetup)
{
	RECT16 rect;

	rect = extraRect;

	if (bSetup)
	{
		LoadBackgroundFile("DATA\\CITYBACK.RAW");

		if (feVariableSave[0] == -1)
		{
			if (iScreenSelect == SCREEN_NONE)
				iScreenSelect = SCREEN_CUTCITY;
		}
		else
		{
			currCity = feVariableSave[0];

			feVariableSave[3] = -1;
			feVariableSave[2] = -1;
			feVariableSave[1] = -1;
			feVariableSave[0] = -1;
		}

		pCurrScreen->buttons[0].u = 4;
		pCurrScreen->buttons[3].d = 1;

		if (gFurthestMission == 0) 
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);
		}
		else if (gFurthestMission < 10)
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);
		}
		else if (gFurthestMission < 21)
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_DISABLED, 0);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);
		}
		else if (gFurthestMission < 31) 
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);
		}
		else 
		{
			pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 22);

			if (gFurthestMission == 40)
			{
				pCurrScreen->buttons[0].u = 5;
				pCurrScreen->buttons[3].d = 5;
			}
		}

		if (loaded[0] == -1)
		{
			SetupExtraPoly("DATA\\CITY.RAW", (currCity != 4) ? currCity : 0, 0);
		}
		else
		{
			bDrawExtra = 1;
			
			if (currCity == 4) 
				LoadImage(&rect, (u_long*)_frontend_buffer);
			else 
				LoadImage(&rect, (u_long*)(_frontend_buffer + currCity * 0x8000));

			DrawSync(0);
		}

		return 0;
	}

	if (feNewPad & MPAD_CROSS)
	{
		// JERICHO-HOOK: the take-a-ride city confirm — modules may defer the
		// start (their own menu runs over the frozen frontend) or rewrite
		// the pending level/gametype/player count
		if (JerichoFrontendCityConfirm())
			return 0;	/* the module starts the game itself */

		lastCity = -1;
		lastCutCity = GameLevel;

		if (GameLevel == 4)
		{
			bReturnToMain = 0;

			pScreenStack[ScreenDepth] = pCurrScreen;
			pButtonStack[ScreenDepth] = pCurrButton;

			ScreenNames[ScreenDepth] = pCurrButton->Name;

			feVariableSave[0] = currCity;

			SetState(STATE_FMVPLAY, (void*)96);
			return 1;
		}
		else
		{
			lastCity = -1;
		}

		return 0;
	}
	else if (feNewPad & MPAD_TRIANGLE)
	{
		FESound(0);
		bDoneAllready = 1;
		LoadBackgroundFile("DATA\\GFX.RAW");
		iScreenSelect = SCREEN_NONE;
		bDrawExtra = 0;

		return 0;
	}
	else if (feNewPad & MPAD_D_UP)
	{
		currCity = pCurrButton->u - 1;
	}
	else if (feNewPad & MPAD_D_DOWN)
	{
		currCity = pCurrButton->d - 1;
	}

	GameLevel = currCity;

	rect = extraRect;

	if (GameLevel != 4)
		LoadImage(&rect, (u_long*)(_frontend_buffer + GameLevel * 0x8000));
	else
		LoadImage(&rect, (u_long*)_frontend_buffer);

	DrawSync(0);

#ifdef PSX
	DisplayOnScreenText();

	addPrim(current->ot + 2, &extraSprt);
	addPrim(current->ot + 2, &extraDummy);

	EndFrame();
#endif

	return 0;
}



// [D] [T]
int SetVolumeScreen(int bSetup)
{
	int last;
	int ypos[2] = { 292, 338 };
	char text[4];
	RECT16 rect;

	static int masterVolumeHold = 0;
	static int musicVolumeHold = 0;

	if (bSetup) 
	{
		currSelIndex = 0;

		masterVolumeHold = gMasterVolume;
		musicVolumeHold = gMusicVolume;

		LoadBackgroundFile("DATA\\VOL.RAW");

#ifdef PSX
		sprintf(text, "%d", (int)(10000 + gMasterVolume) / 100);
		FEPrintString(text, 152, ypos[0], 2, 128, 128, 128);

		sprintf(text, "%d", (int)(10000 + gMusicVolume) / 100);
		FEPrintString(text, 152, ypos[1], 2, 128, 128, 128);
#endif

		return 0;
	}

#ifndef PSX
	sprintf(text, "%d", (int)(10000 + gMasterVolume) / 100);
	FEPrintString(text, 152, ypos[0], 2, 128, 128, 128);

	sprintf(text, "%d", (int)(10000 + gMusicVolume) / 100);
	FEPrintString(text, 152, ypos[1], 2, 128, 128, 128);
#endif

	// did we make any changes?
	bool update = false;

	currSelIndex = (pCurrButton->u & 3);

	if (feNewPad & MPAD_TRIANGLE)
	{
		FESound(0);
		bDoneAllready = 1;

		gMasterVolume = masterVolumeHold;
		gMusicVolume = musicVolumeHold;

		LoadBackgroundFile("DATA\\GFX.RAW");

		SetMasterVolume(gMasterVolume);
		SetXMVolume(gMusicVolume);

		return 0;
	}
	else if (feNewPad & MPAD_CROSS)
	{
		if (currSelIndex == 2)
			LoadBackgroundFile("DATA\\GFX.RAW");

		return 0;
	}
	else
	{
		int dir = -1; // -1: no action, 0: limit reached, 1: OK

		if (fePad & MPAD_D_LEFT)
		{
			switch (currSelIndex)
			{
			case 0:
				gMasterVolume -= 100;

				if (gMasterVolume < -10000)
				{
					gMasterVolume = -10000;
					dir = 0;
				}
				else
				{
					dir = 1;
				}

				last = gMasterVolume;
				break;
			case 1:
				gMusicVolume -= 100;

				if (gMusicVolume < -10000)
				{
					gMusicVolume = -10000;
					dir = 0;
				}
				else
				{
					dir = 1;
				}

				last = gMusicVolume;
				break;
			}
		}
		else if (fePad & MPAD_D_RIGHT)
		{
			switch (currSelIndex)
			{
			case 0:
				gMasterVolume += 100;

				if (gMasterVolume > 0)
				{
					gMasterVolume = 0;
					dir = 0;
				}
				else
				{
					dir = 1;
				}

				last = gMasterVolume;
				break;
			case 1:
				gMusicVolume += 100;

				if (gMusicVolume > 0)
				{
					gMusicVolume = 0;
					dir = 0;
				}
				else
				{
					dir = 1;
				}

				last = gMusicVolume;
				break;
			}
		}

		if (dir != -1)
		{
			FESound((dir == 1) ? 3 : 1);
			update = true;
		}
	}

	if (update)
	{
		SetMasterVolume(gMasterVolume);
		SetXMVolume(gMusicVolume);

#ifdef PSX
		sprintf(text, "%d", (int)(10000 + last) / 100);
		FEPrintString(text, 152, ypos[currSelIndex], 2, 128, 128, 128);

		sprintf(text, "%d", (int)(10000 + last) / 100);
		FEPrintString(text, 152, ypos[currSelIndex], 2, 0, 0, 0);

		EndFrame(); // [A] inlined
#else
		// don't flush the screen
		return 1;
#endif
	}

	return 0;
}

int GameNum = 0;
CVECTOR scoreCol = { 120, 120, 120 };
CVECTOR otherCol = {125, 115, 34};

// [D] [T]
void DisplayScoreTable(void)
{
	static char* CityNames[4] = {
		M_LTXT(MTXT_Chicago),
		M_LTXT(MTXT_Havana),
		M_LTXT(MTXT_LasVegas),
		M_LTXT(MTXT_RioDeJaneiro)
	};

	int time;
	int i;
	int offset;
	char text[32];
	SCORE_ENTRY *pSE;

	gSubGameNumber = GameNum;

	OnScoreTable(&pSE);

	sprintf(text, ScreenNames[ScreenDepth - 1]);
	FEPrintString(text, 20, 200, 2, otherCol.r, otherCol.g, otherCol.b);

	sprintf(text, CityNames[GameLevel]);
	FEPrintStringSized(text, 280, 206, 3072, 2, otherCol.r, otherCol.g, otherCol.b);

	if (GameType == GAME_TAKEADRIVE && NumPlayers == 2)
		offset = 48;
	else if (GameType == GAME_COPSANDROBBERS)
		offset = 32;
	else if (GameType == GAME_CAPTURETHEFLAG)
		offset = 40;
	else if (GameType == GAME_CHECKPOINT && NumPlayers == 2)
		offset = 56;
	else
		offset = (GameType - 4U) * 8;

	if (GameType != GAME_PURSUIT && GameType != GAME_SURVIVAL) 
	{
		sprintf(text, "%s", GAMEMODE_AREA_NAME(GameLevel, offset, GameNum));
		FEPrintStringSized(text, 420, 206, 3072, 2, otherCol.r, otherCol.g, otherCol.b);
	}

	offset = 240;
	
	for (i = 0; i < 5; i++)
	{
		sprintf(text, pSE[i].name);
		FEPrintString(text, 20, offset, 2, scoreCol.r, scoreCol.g, scoreCol.b);

		if (GameType == GAME_GATERACE || GameType == GAME_TRAILBLAZER) 
		{
			if (pSE[i].items != -1) 
			{
				sprintf(text, "%d", pSE[i].items);
				FEPrintString(text, 140, offset, 2, scoreCol.r, scoreCol.g, scoreCol.b);
			}
		}

		time = pSE[i].time;

		if (time != -1) 
		{
			sprintf(text, "%d:%02d.%02d", time / 180000, time / 3000 - (time / 180000) * 60, (time % 3000) / 30);
			
			FEPrintString(text, 280, offset, 2, scoreCol.r, scoreCol.g, scoreCol.b);
		}

		offset += 36;
	}
}


// [D] [T]
int ScoreScreen(int bSetup)
{
	if (bSetup)
	{
		GameLevel = 0;

		DisplayScoreTable();

		iScreenSelect = SCREEN_SCORES;
		currSelIndex = 0;

		return 0;
	}

	if (feNewPad & MPAD_CROSS)
	{
		if (currSelIndex == 0)
		{
			if (GameType == GAME_SURVIVAL || GameType == GAME_PURSUIT)
			{
				if (--GameLevel < 0)
					GameLevel = 3;

				GameNum = 0;
			}
			else
			{
				GameNum ^= 1;

				if (GameNum == 1)
				{
					if (--GameLevel < 0)
						GameLevel = 3;
				}
			}
		}
		else
		{
			if (GameType == GAME_SURVIVAL || GameType == GAME_PURSUIT)
			{
				if (++GameLevel > 3)
					GameLevel = 0;

				GameNum = 0;
			}
			else
			{
				GameNum ^= 1;

				if (GameNum == 0)
				{
					if (++GameLevel > 3)
						GameLevel = 0;
				}
			}
		}

#ifdef PSX
		DisplayScoreTable();
		bRedrawFrontend = 1;
#endif
	}
	else if (feNewPad & MPAD_TRIANGLE)
	{
		iScreenSelect = SCREEN_NONE;
		return 0;
	}
	else if ((feNewPad & MPAD_D_UP) || (feNewPad & MPAD_D_DOWN))
	{
		currSelIndex ^= 1;
	}

#ifndef PSX
	DisplayScoreTable();
#endif

	return 0;
}

// [D] [T]
int SubtitlesOnOffScreen(int bSetup)
{
	if (bSetup)
	{
		if (gSubtitles == 0)
		{
			pCurrButton = pCurrScreen->buttons + 1;
		}
		else 
		{
			pCurrButton = pCurrScreen->buttons;
		}

		return 1;
	}
	return 0;
}


// [D] [T]
int CityCutOffScreen(int bSetup)
{
	if (bSetup)
	{
		if (gFurthestMission < 20)
			pCurrScreen->buttons[2].action = FE_MAKEVAR(BTN_DISABLED, 0);

		if (gFurthestMission < 29)
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);

#ifndef PSX
		LoadBackgroundFile("DATA\\CITYBACK.RAW");

		if (lastCity == -1)
			currCity = 0;
		else
			currCity = lastCity;

		if (loaded[0] == -1)
		{
			SetupExtraPoly("DATA\\CITY.RAW", currCity, 0);
		}
		else
		{
			bDrawExtra = 1;

			RECT16 rect = extraRect;
			LoadImage(&rect, (u_long*)(_frontend_buffer + currCity * 0x8000));
			DrawSync(0);
		}
#endif
		return 0;
	}

#ifndef PSX
	/*
	if ((fePad & MPAD_CROSS) != 0)
	{
		lastCity = currCity;

		bDrawExtra = 0;

		LoadBackgroundFile("DATA\\GFX.RAW");
		loaded[0] = -1;

		return 0;
	}*/

	if (feNewPad & MPAD_TRIANGLE)
	{
		lastCity = -1;

		bDrawExtra = 0;
		FESound(0);
		bDoneAllready = 1;

		LoadBackgroundFile("DATA\\GFX.RAW");
		loaded[0] = -1;

		return 0;
	}
	else if (feNewPad & MPAD_D_UP)
	{
		currCity = pCurrButton->u - 1;
	}
	else if (feNewPad & MPAD_D_DOWN)
	{
		currCity = pCurrButton->d - 1;
	}
	else
	{
		currCity = pCurrButton->u & 3;
		return 0;
	}

	RECT16 rect = extraRect;
	LoadImage(&rect, (u_long*)(_frontend_buffer + currCity * 0x8000));
	DrawSync(0);
#endif
	return 0;
}


char* contNames[2] = {
	"DATA\\CARCONT.RAW",
	"DATA\\TANCONT.RAW"
};

// [D] [T]
int ControllerScreen(int bSetup)
{
	if (bSetup ) 
	{
		iScreenSelect = SCREEN_SCORES;
		currSelIndex = 0;
		pCurrScreen->numButtons = 0;

		LoadBackgroundFile(contNames[currSelIndex]);
	}
	else
	{
		if (feNewPad & MPAD_CROSS)
		{
			currSelIndex = currSelIndex ^ 1;
			LoadBackgroundFile(contNames[currSelIndex]);

			bRedrawFrontend = 1;
		}
		else if (feNewPad & MPAD_TRIANGLE)
		{
			iScreenSelect = SCREEN_SCORES;
			LoadBackgroundFile("DATA\\GFX.RAW");
		}
	}

	return 0;
}
// To do: specify toggle, and car/level/gamemode arguments
void ForceStartLevel()
{
	GameType = GAME_SURVIVAL;

	// Set the city to Havana (index 1, as seen in FEscreens.inc)
	GameLevel = 3; // 0=Chicago, 1=Havana, 2=Vegas, 3=Rio
	carSelection = 5;

	// Ensure other necessary variables are reset
	gCurrentMissionNumber = 0; // Not a mission
	gSubGameNumber = 0;        // No sub-game
	// Trigger the game to start
	SetState(STATE_GAMESTART);
}
/* ------------------------------------------------------------------ */
/* JERICHO-HOOK: module-provided frontend menus (jer_frontend.h)       */
/* ------------------------------------------------------------------ */

void jer_frontend_reset(void)
{
	int i;

	for (i = 0; i < JER_FE_MAX_MENUS; i++)
		gFeMenus[i] = NULL;

	gFeMenuCount = 0;
	gFeMainEntry = -1;
}

void jer_frontend_register_menu(const JER_FE_MENU* menu)
{
	if (menu == NULL)
		return;

	if (gFeMenuCount >= JER_FE_MAX_MENUS)
	{
		/* Refusing is right; refusing SILENTLY is how a module's menu went missing
		 * with no trace. The caller then has no index and the frontend opens
		 * whatever sits at the number it assumed - name the casualty instead. */
		jer_log("[jericho] frontend menu registry FULL (%d): dropped menu '%s'\n",
			JER_FE_MAX_MENUS, (menu->id != NULL) ? menu->id : "?");

		return;
	}

	gFeMenus[gFeMenuCount++] = menu;
}

int jer_frontend_menu_count(void)
{
	return gFeMenuCount;
}

const JER_FE_MENU* jer_frontend_menu_get(int index)
{
	return (index >= 0 && index < gFeMenuCount) ? gFeMenus[index] : NULL;
}

int jer_frontend_find(const char* id)
{
	int i;

	if (id == NULL)
		return -1;

	for (i = 0; i < gFeMenuCount; i++)
	{
		if (gFeMenus[i]->id != NULL && strcmp(gFeMenus[i]->id, id) == 0)
			return i;
	}

	return -1;
}

void jer_frontend_set_main_entry(const char* id)
{
	gFeMainEntry = jer_frontend_find(id);
}

void jer_frontend_refresh(void)
{
	gFePendingSelect = -1;
	gFeNeedSetup = 1;
	bRedrawFrontend = 1;
}

/* Refresh and land the rebuilt menu on `index`. Used by a control whose row
 * index moves when the menu rebuilds: jer_frontend_refresh() forces the setup
 * path, which otherwise puts the cursor back on the top row. */
void jer_frontend_refresh_at(int index)
{
	gFePendingSelect = index;
	gFeNeedSetup = 1;
	bRedrawFrontend = 1;
}

/* --- module-menu row geometry -------------------------------------------
 * The engine lays out a module menu's rows; a module that draws its OWN row
 * content (jer_text.h) has to put that content on those rows, so both sides read
 * the metrics from here.
 *
 * They are in the DRAW space (the frontend's 640x480). The BUTTON's own x/w are in
 * HALF that: the engine draws a row's label at 2*x + w, right-aligned, which is
 * the box's right edge -- so a box of DX..DX+DW becomes x = DX/2, w = DW. Getting
 * this wrong puts a module's text beside the menu instead of on it, which is
 * exactly what happened the first time. */
#define JER_FE_ROW_DX		334	/* the stock box's left edge, in draw space */
#define JER_FE_ROW_DW		256	/* and its width */
#define JER_FE_SCREEN_W		640	/* the frontend's draw width */
#define JER_FE_ROW_STEP		36
#define JER_FE_ROW_TOP		190
#define JER_FE_ROW_BOTTOM	462
#define JER_FE_ROW_MIN_TOP	40

/* The box and pitch the menu that is currently laid out actually uses. Written by
 * the layout below (the only place that knows the menu), read by
 * jer_frontend_row_rect. */
static int gFeRowX = JER_FE_ROW_DX;
static int gFeRowW = JER_FE_ROW_DW;
static int gFeRowStep = JER_FE_ROW_STEP;
static int gFeRowCount;
static int gFeRowListN;		/* how many of them are list rows (the rest are the
				 * bottom-pinned controls) */
static int gFeRowListTop;	/* the LIST AREA's top (a heading measures from here)... */
static int gFeRowFirst;		/* ...and where the first list ROW is, after any centring */
static int gFeRowBottomTop;	/* ...and the bottom group's */

/* Rows start at JER_FE_ROW_TOP and step by `step`; only a block too tall for the
 * space slides up. */
static int jerFrontendRowTop(int count, int step, int limit, int wanted)
{
	int top = wanted;

	if (top + (count - 1) * step > limit)
		top = limit - (count - 1) * step;

	if (top < JER_FE_ROW_MIN_TOP)
		top = JER_FE_ROW_MIN_TOP;

	return top;
}

/* Which row the engine is highlighting right now. A module that draws its own
 * row content needs this to show something ABOUT the row under the cursor
 * (a description, say) rather than about row 0. */
int jer_frontend_selected(void)
{
	return currSelIndex;
}

int jer_frontend_list_top(void)
{
	if (gFeRowCount <= 0)
		return 0;

	return gFeRowListTop;
}

int jer_frontend_row_rect(int index, int* x, int* y, int* w, int* h)
{
	if (gFeRowCount <= 0 || index < 0 || index >= gFeRowCount)
		return 0;

	/* the statics outlive the menu they were set for, so make sure one of ours is
	 * still the screen being drawn */
	if (jer_frontend_current_menu() < 0)
		return 0;

	if (x != NULL) *x = gFeRowX;
	if (w != NULL) *w = gFeRowW;
	if (h != NULL) *h = gFeRowStep;

	if (y != NULL)
	{
		*y = (index < gFeRowListN) ? (gFeRowFirst + index * gFeRowStep)
					   : (gFeRowBottomTop + (index - gFeRowListN) * gFeRowStep);
	}

	return 1;
}

void jer_frontend_goto(int screenIndex)
{
	if (screenIndex < 0 || screenIndex >= (int)(sizeof(PsxScreens) / sizeof(PsxScreens[0])))
		return;

	if (pCurrScreen != NULL)
	{
		pScreenStack[ScreenDepth] = pCurrScreen;
		pButtonStack[ScreenDepth] = pCurrButton;
		ScreenNames[ScreenDepth] = (char*)"";
		if (ScreenDepth < 10)
			ScreenDepth++;
	}

	pNewScreen = &PsxScreens[screenIndex];
	bRedrawFrontend = 1;
}

/* Like jer_frontend_goto, but the target screen TAKES THE PLACE of the current
 * one instead of being pushed on top of it: the nav stack is left untouched, so
 * the screen below the replaced one stays below it. A module menu that replaces
 * a screen in a chain uses this so Back/Triangle return to whatever the replaced
 * screen would have returned to -- see jer_frontend_open_replace. */
void jer_frontend_replace(int screenIndex)
{
	if (screenIndex < 0 || screenIndex >= (int)(sizeof(PsxScreens) / sizeof(PsxScreens[0])))
		return;

	pNewScreen = &PsxScreens[screenIndex];
	bRedrawFrontend = 1;
}

void jer_frontend_open(int menuIndex)
{
	if (menuIndex < 0 || menuIndex >= gFeMenuCount)
		return;

	jer_frontend_goto(JERICHO_FE_SCREEN_BASE + menuIndex);
}

/* Open a registered menu as a REPLACEMENT for the current screen (see
 * jer_frontend_replace): the menu takes the screen's slot on the nav stack, so
 * Back/Triangle from it return to whatever that screen would have returned to -
 * the behaviour a menu that stands in for a screen in a chain wants. */
void jer_frontend_open_replace(int menuIndex)
{
	if (menuIndex < 0 || menuIndex >= gFeMenuCount)
		return;

	jer_frontend_replace(JERICHO_FE_SCREEN_BASE + menuIndex);
}

/* Which registered module menu is on screen? -1 while the frontend shows
 * anything else (a stock screen, the mods screen) or during gameplay. A
 * module uses this to refresh only the screen it owns. */
int jer_frontend_current_menu(void)
{
	int idx;

	if (pCurrScreen == NULL)
		return -1;

	idx = (int)(pCurrScreen - PsxScreens) - JERICHO_FE_SCREEN_BASE;

	if (idx < 0 || idx >= gFeMenuCount)
		return -1;

	return idx;
}

/* The single handler every module menu slot uses; the slot index selects the
 * registered menu. Lays the items out as native buttons and reports input. */
int JerFrontendMenuScreen(int bSetup)
{
	int menuIdx = (int)(pCurrScreen - PsxScreens) - JERICHO_FE_SCREEN_BASE;
	const JER_FE_MENU* menu = jer_frontend_menu_get(menuIdx);
	int n;

	if (menu == NULL || menu->items == NULL)
		return 0;

	/* the menu heading (JER_FE_MENU.title), in orange, above the rows */
	if (menu->title != NULL)
		FEPrintStringSized((char*)menu->title, 100, 150, 3072, 1, 255, 150, 0);

	if (bSetup || gFeNeedSetup)
	{
		int i;
		int rowTop, rowStep, bottom, listN, bottomTop, rowBottom, rowTopWanted, areaTop;

		gFeNeedSetup = 0;

		if (menu->on_enter != NULL)
			menu->on_enter(menu->userdata);

		/* the optional car icon (JER_FE_MENU.get_preview): resolve the model
		 * to its car-select image slot and show it. bDrawExtra is cleared
		 * first so a menu with no preview leaves no icon behind. */
		bDrawExtra = 0;

		if (menu->get_preview != NULL)
		{
			int pvCity = -1, pvModel = -1;
			int sel;

			menu->get_preview(menu->userdata, &pvCity, &pvModel);

			if (pvCity >= 0 && pvCity < 4)
			{
				for (sel = 0; sel < 10; sel++)
				{
					if (carNumLookup[pvCity][sel] == (char)pvModel)
						break;
				}

				if (sel < 10)
				{
					/* The portrait is a PAIR: the texture, and the palette its
					 * colour indices were authored against. That palette is NOT
					 * in the city raw - it lives in the car-select background
					 * file, which LoadBackgroundFile drops into VRAM at
					 * (960,256), the very CLUT this sprite is drawn with. The
					 * stock car-select loads it immediately before the icon for
					 * exactly that reason; a module menu that skips it draws the
					 * right texture through whatever palette the PREVIOUS screen
					 * left in that slot, which reads as the right colours landing
					 * on the wrong parts of the picture. */
					LoadBackgroundFile("DATA\\CARS\\CARBACK.RAW");

					/* the stock car-select icon, at its usual place */
					SetupExtraPoly(gfxNames[pvCity], sel, 0);
				}
			}
		}

		n = menu->item_count;
		if (n > JER_FE_MAX_ITEMS)
			n = JER_FE_MAX_ITEMS;
		if (n <= 0)
		{
			/* An EMPTY menu owns no rows. Leaving numButtons stale would keep a
			 * highlight on a row this menu no longer has. */
			pCurrScreen->numButtons = 0;
			return bSetup ? 1 : 0;
		}

		pCurrScreen->numButtons = n;

		/* stock frontend row metrics (jerFrontendRowTop/jer_frontend_row_rect are
		 * the one definition of them). Only shift a tall menu up (never a small
		 * one) so it still fits. */
		/* ROWS. A menu may ask for a TIGHTER pitch (a long list) and for its last few
		 * items to be CONTROLS on the bottom edge (so they do not move while the list
		 * above them is rebuilt) -- both are the menu's to say, since only it knows. */
		/* <= n: a menu that is ALL controls (the update screen) pins every row too */
		bottom = (menu->bottom_items > 0 && menu->bottom_items <= n) ? menu->bottom_items : 0;
		listN = n - bottom;
		rowStep = (menu->row_step > 0) ? menu->row_step : JER_FE_ROW_STEP;
		rowBottom = (menu->row_bottom > 0) ? menu->row_bottom : JER_FE_ROW_BOTTOM;
		rowTopWanted = (menu->row_top > 0) ? menu->row_top : JER_FE_ROW_TOP;

		/* the list stops above the controls, and slides up only if it must */
		bottomTop = rowBottom - bottom * rowStep;
		rowTop = jerFrontendRowTop(listN, rowStep,
			bottom ? (bottomTop - rowStep) : rowBottom, rowTopWanted);

		/* CENTRED: slide the whole list down by half of what it does not use. Done
		 * here rather than by the caller so the row rects -- and therefore the
		 * highlight the player sees move -- stay exactly where the rows are drawn. */
		areaTop = rowTop;

		if (menu->list_align == 1 && bottom && listN > 0)
		{
			int slack = (bottomTop - rowTop) - listN * rowStep;

			if (slack > 0)
				rowTop += slack / 2;      /* the AREA stays put; the rows move inside it */
		}

		/* A menu can ask for WIDER rows (a table needs them). The stock box stays
		 * exactly where it has always been, so only a menu that asks moves. */
		gFeRowW = (menu->row_w > 0) ? menu->row_w : JER_FE_ROW_DW;
		gFeRowX = (menu->row_w > 0) ? (JER_FE_SCREEN_W - gFeRowW) / 2 : JER_FE_ROW_DX;
		gFeRowStep = rowStep;
		gFeRowCount = n;
		gFeRowListN = listN;
		gFeRowListTop = areaTop;
		gFeRowFirst = rowTop;
		gFeRowBottomTop = bottomTop;

		for (i = 0; i < n; i++)
		{
			const JER_FE_ITEM* it = &menu->items[i];
			PSXBUTTON* btn = &pCurrScreen->buttons[i];

			btn->x = gFeRowX / 2;
			btn->y = (i < listN) ? (rowTop + i * rowStep)
					     : (bottomTop + (i - listN) * rowStep);
			btn->w = gFeRowW;
			btn->h = rowStep;
			btn->s_x = 370;
			/* the SPRITE follows the row, not the list position. With bottom-pinned
			 * controls the two differ, and art left at the list's y made a control look
			 * as though it had never moved -- which is exactly how the pinned Back and
			 * Manual rows first read. In the stock path y and s_y are equal anyway. */
			btn->s_y = btn->y;

			if (it->get_label != NULL)
				it->get_label(it->userdata, btn->Name, sizeof(btn->Name));
			else
				snprintf(btn->Name, sizeof(btn->Name), "%s", it->label != NULL ? it->label : "");

			btn->u = (u_char)(i == 0 ? n : i);
			btn->d = (u_char)(i == n - 1 ? 1 : i + 2);
			btn->l = 0;
			btn->r = 0;
			btn->var = -1;

			if (it->is_back)
				btn->action = FE_MAKEVAR(BTN_PREVIOUS_SCREEN, 0);
			else if (it->submenu >= 0 && it->submenu < JER_FE_MAX_MENUS)
				btn->action = FE_MAKEVAR(BTN_NEXT_SCREEN, JERICHO_FE_SCREEN_BASE + it->submenu);
			else if (it->on_activate != NULL || it->on_adjust != NULL)
				btn->action = FE_MAKEVAR(BTN_MODULE, 0);	/* module handles the press */
			else
				btn->action = FE_MAKEVAR(BTN_DISABLED, 0);
		}

		/* keep the cursor where the player left it when a LIVE refresh
		 * rebuilds the screen (bSetup marks a fresh entry, which starts at
		 * the top); otherwise a refresh resets the selection to the first
		 * row every frame and the cursor gets stuck there */
		if (bSetup)
		{
			/* A refresh may name the row it should land on: a page row's index
			 * moves when a page is shorter, and an ordinary refresh would put
			 * the cursor back on the top row. Backing out is the other case:
			 * pNewButton carries the row off the nav stack. Anything else
			 * starts at the top. */
			int cur = 0;

			if (gFePendingSelect >= 0)
			{
				if (gFePendingSelect < n)
					cur = gFePendingSelect;

				gFePendingSelect = -1;
			}
			else if (pNewButton != NULL && pNewButton >= pCurrScreen->buttons &&
				pNewButton < pCurrScreen->buttons + n)
			{
				cur = (int)(pNewButton - pCurrScreen->buttons);
			}

			pNewButton = NULL;
			pCurrButton = &pCurrScreen->buttons[cur];
			currSelIndex = cur;
		}
		else
		{
			int cur = (pCurrButton != NULL) ? (int)(pCurrButton - pCurrScreen->buttons) : 0;

			if (cur < 0 || cur >= n)
				cur = 0;

			pCurrButton = &pCurrScreen->buttons[cur];
		}

		if (bSetup)
		{
			gFeMenuEnterFrame = 1;
			return 1;
		}
	}
	else
	{
		n = menu->item_count;
		if (n > JER_FE_MAX_ITEMS)
			n = JER_FE_MAX_ITEMS;
	}

	if (gFeMenuEnterFrame)
	{
		/* the frame the menu appeared: the pad read may still be the previous
		 * screen's (or the "all pressed" boot state) - never act on it */
		gFeMenuEnterFrame = 0;
		return 0;
	}

	if (numPadsConnected == 0)
	{
		/* no pad: the frontend synthesises a Cross (feNewPad = 0x10) to kick
		 * the screen when the controller is unplugged - that is not a real
		 * press, so a module menu must not act on it.
		 *
		 * This is also why Triangle cannot be driven headlessly: a padless run never
		 * reaches a module menu at all. It is a pad-only path, verified by running
		 * the game rather than by a harness lever. */
		return 0;
	}

	/* JERICHO: TRIANGLE. It is the back button on every stock frontend screen, so a
	 * module menu is offered it too - and it is offered FIRST, before the row logic,
	 * because back is a property of the SCREEN, not of whichever row happens to be
	 * selected.
	 *
	 * The module answers for itself (`on_back`). The engine must not assume "the
	 * previous screen": carhacks' car-select Back row returns to the Day/Night
	 * screen on purpose, NOT to the stock car screen it replaced, because going back
	 * there would re-run that screen's setup and re-arm the menu - the loop the
	 * stock return would create.
	 *
	 * With no on_back, a menu that declares a row with is_back set gets that row
	 * pressed, which is what the row is for. With neither, the press is left
	 * unclaimed: a menu with nowhere to go back to behaves like a stock screen with
	 * nowhere to go back to, and the caller keeps the input. */
	if ((feNewPad & MPAD_TRIANGLE) != 0)
	{
		if (menu->on_back != NULL)
		{
			if (menu->on_back(menu->userdata))
			{
				FESound(2);
				feNewPad = 0;
				jer_frontend_refresh();
				return 1;
			}
		}
		else
		{
			int i;

			for (i = 0; i < n; i++)
			{
				if (menu->items[i].is_back != 0 && menu->items[i].on_activate != NULL)
				{
					FESound(2);
					feNewPad = 0;
					menu->items[i].on_activate(menu->items[i].userdata);
					jer_frontend_refresh();
					return 1;
				}
			}
		}
	}

	if (pCurrButton != NULL)
	{
		int idx = (int)(pCurrButton - pCurrScreen->buttons);

		if (idx >= 0 && idx < n)
		{
			const JER_FE_ITEM* it = &menu->items[idx];

			if (it->on_activate != NULL && (feNewPad & MPAD_CROSS))
			{
				int handled = it->on_activate(it->userdata);

				FESound(2);

				if (handled)
				{
					feNewPad = 0;
					jer_frontend_refresh();
					return 1;
				}

				if (it->submenu < 0)
				{
					feNewPad = 0;
					return 1;	/* callback consumed the press */
				}

				/* else: fall through to the engine's submenu navigation */
			}
			else if (it->on_adjust != NULL && (feNewPad & (MPAD_D_LEFT | MPAD_D_RIGHT)))
			{
				int dir = (feNewPad & MPAD_D_RIGHT) ? 1 : -1;

				FESound(2);
				feNewPad = 0;
				it->on_adjust(it->userdata, dir);
				jer_frontend_refresh();

				return 1;
			}
		}
	}

	return 0;
}

// [D] [T]
int MainScreen(int bSetup)
{

	if (bSetup) 
	{
		// JERICHO-HOOK: give a module first say over the title screen. Fired
		// once per row (jer_frontend.h / JER_EVENT_FRONTEND_MAIN_MENU) so a
		// module can rename a row, redirect it to its own menu, disable or hide
		// it - before the stock Multiplayer routing below.
		{
			int bi;

			for (bi = 0; bi < pCurrScreen->numButtons && bi < 12; bi++)
			{
				JER_ARGS_FRONTEND_ENTRY e;
				PSXBUTTON* b = &pCurrScreen->buttons[bi];

				memset(&e, 0, sizeof(e));
				e.index = bi;
				snprintf(e.label, sizeof(e.label), "%s", b->Name);
				e.openMenu = -1;

				jer_fire(JER_EVENT_FRONTEND_MAIN_MENU, &e);

				/* the engine always applies the row (the return value only
				 * says whether a handler ran; a module that changes nothing
				 * leaves the struct as it came in) */
				snprintf(b->Name, sizeof(b->Name), "%s", e.label);

				if (e.hidden)
					b->action = FE_MAKEVAR(BTN_HIDDEN, 0);
				else if (e.openMenu >= 0 && e.openMenu < JER_FE_MAX_MENUS)
					b->action = FE_MAKEVAR(BTN_NEXT_SCREEN, JERICHO_FE_SCREEN_BASE + e.openMenu);
				else if (e.disabled)
					b->action = FE_MAKEVAR(BTN_DISABLED, 0);
			}
		}

		// JERICHO-HOOK: route the Multiplayer entry to a module's frontend
		// menu (jer_frontend.h) so online play is reachable without a second
		// pad; otherwise fall back to the stock 2-pad split-screen screen.
		if (gFeMainEntry >= 0)
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_NEXT_SCREEN, JERICHO_FE_SCREEN_BASE + gFeMainEntry);
		else if (numPadsConnected == 2)
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 6);
		else
			pCurrScreen->buttons[3].action = FE_MAKEVAR(BTN_DISABLED, 0);

		// JERICHO-HOOK: hang the Mods manager off the Options screen (once)
		if (!gJerichoOptionsButtonAdded)
		{
			int optionsIdx = PsxScreens[0].buttons[4].action & 0xFF;
			PSXSCREEN* options;
			PSXBUTTON* last;

			/* only wire in when the main screen's button 4 really navigates */
			if ((PsxScreens[0].buttons[4].action >> 8) != BTN_NEXT_SCREEN)
				return 0;

			options = &PsxScreens[optionsIdx];

			if (options->numButtons == 0)
				return 0;		/* odd screen; retry next entry */

			if (options->numButtons < 8)
			{
				PSXBUTTON* b = &options->buttons[options->numButtons];

				last = &options->buttons[options->numButtons - 1];

				memset(b, 0, sizeof(PSXBUTTON));

				b->x = last->x;
				b->y = last->y + 41;
				b->w = last->w;
				b->h = last->h;
				b->s_x = last->s_x;
				b->s_y = b->y;

				sprintf(b->Name, "JERICHO Config Menu");

				options->numButtons++;
				last->d = options->numButtons;			// last button -> JERICHO
				b->u = options->numButtons - 1;			// JERICHO -> last button
				b->d = 1;								// wrap to the first

				/* first button wraps up to the new last entry too */
				options->buttons[0].u = options->numButtons;

				b->action = FE_MAKEVAR(BTN_NEXT_SCREEN, JERICHO_MODS_SCREEN);
				b->var = -1;
			}

			/* injected (or the screen is full): never add a duplicate */
			gJerichoOptionsButtonAdded = 1;
		}
	}
	//ForceStartLevel();
	return 0;
}

static char* cheatText[] =
{
	G_LTXT_ID(GTXT_NoSecrets),
	G_LTXT_ID(GTXT_MountainTrack),
	G_LTXT_ID(GTXT_Circuit),
	G_LTXT_ID(GTXT_Invincibility),
	G_LTXT_ID(GTXT_Immunity),
	G_LTXT_ID(GTXT_MiniCars),		// [A]
	G_LTXT_ID(GTXT_BonusGallery)	// [A]
};

// [D] [T]
int CheatScreen(int bSetup)
{
	int i;
	int numOpen;
	int k;
	unsigned char cheatOn[12];
	int evilRuss[6];

	int hackLookup1[6] = {
		FE_MAKEVAR(BTN_NEXT_SCREEN, 33),
		FE_MAKEVAR(BTN_NEXT_SCREEN, 33),
		FE_MAKEVAR(BTN_NEXT_SCREEN, 30),
		FE_MAKEVAR(BTN_NEXT_SCREEN, 31),
		FE_MAKEVAR(BTN_NEXT_SCREEN, 40),
		0,
	};

	int hackLookup2[6] = {
		FE_MAKEVAR(12, 1),
		FE_MAKEVAR(12, 0),
		-1,
		-1,
		-1,
		FE_MAKEVAR(14, 0),
	};

	if (bSetup == 0)
	{
		return 0;
	}

	if (gFurthestMission == 40) 
		numOpen = 4 + ENABLE_BONUS_CONTENT * 2;	// [A] now 5 elements as "Bonus gallery" is open
	else 
		numOpen = AvailableCheats.cheat1 + AvailableCheats.cheat2 + AvailableCheats.cheat3 + AvailableCheats.cheat4;
	
	// "Sorry no secrets"
	if (numOpen == 0)
	{
		pCurrScreen->numButtons = 1;
		
		pCurrScreen->buttons[0].u = 1;
		pCurrScreen->buttons[0].d = 1;
		pCurrScreen->buttons[0].action = FE_MAKEVAR(BTN_PREVIOUS_SCREEN, 0);

		/* The cheat labels come from the LANGUAGE table, and one of them does not fit
		 * PSXBUTTON.Name (32 bytes) in a shipped language: the German "Sorry, keine
		 * Geheimnisse freigespielt" is 37 chars. sprintf-ing that does not just clip -
		 * under glibc's _FORTIFY_SOURCE it ABORTS the process, and without fortify it
		 * runs into the rest of the button. Clip it instead. */
		snprintf(pCurrScreen->buttons[0].Name, sizeof(pCurrScreen->buttons[0].Name),
			"%s", GET_GAME_TXT(cheatText[0]));

		return 0;
	}

	pCurrScreen->numButtons = numOpen;

	evilRuss[0] = AvailableCheats.cheat1;
	evilRuss[1] = AvailableCheats.cheat2;
	evilRuss[2] = AvailableCheats.cheat3;
	evilRuss[3] = AvailableCheats.cheat4;
	evilRuss[4] = 0;
	evilRuss[5] = 0;

	if (numOpen >= 6)
	{
		pCurrScreen->buttons[4] = pCurrScreen->buttons[3];
		pCurrScreen->buttons[5] = pCurrScreen->buttons[3];
	}

	k = 0;

	for (i = 0; i < 6; i++)
	{
		if (evilRuss[i] || gFurthestMission == 40)
		{
			/* ...and the same clip for the cheat rows (see the note above). */
			snprintf(pCurrScreen->buttons[i].Name, sizeof(pCurrScreen->buttons[i].Name),
				"%s", GET_GAME_TXT(cheatText[i + 1]));
			cheatOn[k++] = i;
		}
	}

	if (numOpen == 2) 
	{
		pCurrScreen->buttons[0].action = hackLookup1[cheatOn[0]];
		pCurrScreen->buttons[1].action = hackLookup1[cheatOn[1]];
	
		pCurrScreen->buttons[0].var = hackLookup2[cheatOn[0]];
		pCurrScreen->buttons[1].var = hackLookup2[cheatOn[1]];
		
		pCurrScreen->buttons[0].d = 2;
		pCurrScreen->buttons[0].u = 2;
		
		pCurrScreen->buttons[1].d = 1;
		pCurrScreen->buttons[1].u = 1;
		
		currSelIndex = 0;
	
		return 0;
	}

	if (numOpen == 1)
	{
		pCurrScreen->buttons[0].action = hackLookup1[cheatOn[0]];
		pCurrScreen->buttons[0].var = hackLookup2[cheatOn[0]];
		pCurrScreen->buttons[0].d = 1;
		pCurrScreen->buttons[0].u = 1;
		
		currSelIndex = 0;
	
		return 0;
	}

	if (numOpen == 3) 
	{
		pCurrScreen->buttons[0].action = hackLookup1[cheatOn[0]];
		pCurrScreen->buttons[1].action = hackLookup1[cheatOn[1]];
		pCurrScreen->buttons[2].action = hackLookup1[cheatOn[2]];
		
		pCurrScreen->buttons[0].var = hackLookup2[cheatOn[0]];
		pCurrScreen->buttons[1].var = hackLookup2[cheatOn[1]];
		pCurrScreen->buttons[2].var = hackLookup2[cheatOn[2]];
		
		pCurrScreen->buttons[0].d = 2;
		pCurrScreen->buttons[0].u = 3;
		
		pCurrScreen->buttons[1].d = 3;
		pCurrScreen->buttons[1].u = 1;
		
		pCurrScreen->buttons[2].d = 1;
		pCurrScreen->buttons[2].u = 2;
		
		currSelIndex = 0;
		return 0;
	}

	if (numOpen >= 4) 
	{
		pCurrScreen->buttons[0].action = hackLookup1[cheatOn[0]];
		pCurrScreen->buttons[1].action = hackLookup1[cheatOn[1]];
		pCurrScreen->buttons[2].action = hackLookup1[cheatOn[2]];
		pCurrScreen->buttons[3].action = hackLookup1[cheatOn[3]];

		pCurrScreen->buttons[0].var = hackLookup2[cheatOn[0]];
		pCurrScreen->buttons[1].var = hackLookup2[cheatOn[1]];
		pCurrScreen->buttons[2].var = hackLookup2[cheatOn[2]];
		pCurrScreen->buttons[3].var = hackLookup2[cheatOn[3]];
		
		pCurrScreen->buttons[0].d = 2;
		pCurrScreen->buttons[0].u = 4;
		
		pCurrScreen->buttons[1].d = 3;
		pCurrScreen->buttons[1].u = 1;
		
		pCurrScreen->buttons[2].d = 4;
		pCurrScreen->buttons[2].u = 2;
		
		pCurrScreen->buttons[3].d = 1;
		pCurrScreen->buttons[3].u = 3;

		// [A] adding bonus gallery
		if(numOpen >= 5)
		{
			pCurrScreen->buttons[4].action = hackLookup1[cheatOn[4]];
			pCurrScreen->buttons[4].var = hackLookup2[cheatOn[4]];
			pCurrScreen->buttons[4].y += 40;
			pCurrScreen->buttons[4].s_y += 40;

			pCurrScreen->buttons[0].u = 5;
			pCurrScreen->buttons[3].d = 5;
			
			pCurrScreen->buttons[4].d = 1;
			pCurrScreen->buttons[4].u = 4;
		}

		// [A] adding mini cars cheat
		if (numOpen >= 6)
		{
			pCurrScreen->buttons[5].action = hackLookup1[cheatOn[5]];
			pCurrScreen->buttons[5].var = hackLookup2[cheatOn[5]];
			pCurrScreen->buttons[5].y += 80;
			pCurrScreen->buttons[5].s_y += 80;

			pCurrScreen->buttons[0].u = 6;
			pCurrScreen->buttons[4].d = 6;

			pCurrScreen->buttons[5].d = 1;
			pCurrScreen->buttons[5].u = 5;
		}

		currSelIndex = 0;
		return 0;
	}

	pCurrScreen->numButtons = 0;
	currSelIndex = 0;

	return 0;
}


// [D] [T]
int ImmunityOnOffScreen(int bSetup)
{
	if (bSetup)
	{
		if (gPlayerImmune == 0)
			pCurrButton = pCurrScreen->buttons + 1;
		else
			pCurrButton = pCurrScreen->buttons;
		return 1;
	}

	return 0;
}

// [D] [T]
int InvincibleOnOffScreen(int bSetup)
{
	if (bSetup)
	{
		if (gInvincibleCar == 0) 
			pCurrButton = pCurrScreen->buttons + 1;
		else 
			pCurrButton = pCurrScreen->buttons;

		return 1;
	}
	return 0;
}

int MiniCarsOnOffScreen(int bSetup)
{
	if (bSetup)
	{
		if (ActiveCheats.cheat13 == 0)
			pCurrButton = pCurrScreen->buttons + 1;
		else
			pCurrButton = pCurrScreen->buttons;

		return 1;
	}
	return 0;
}

// JERICHO-HOOK: the frontend Mods manager. Modules are toggled and reordered
// here (pre-session, so re-activating the runtime is safe) instead of the
// in-game pause menu. Cross = toggle, Left/Right = move load order,
// up/down navigates, Back returns.
/* JERICHO: the Credits page. The mods screen's Credits button turns the screen
 * into this page: the same background, one BACK button bottom-right, and two
 * columns of lines centred over it in the frontend's face. Names come from README.md's
 * Credits section and REDRIVER2.md's upstream credits -- keep them in step. */
static const char* const kJerichoCreditsLeft[] = {	/* JERICHO, and thanks */
	"JERICHO",
	"Created by Jaret (Njurse)",
	"",
	"Special thanks",
	"Commando52Colton - Valuable advice",
	"on palettes, texture paging, mission script research",
};

static const char* const kJerichoCreditsRight[] = {	/* the engine underneath */
	"Built on REDRIVER2 / OpenDriver2",
	"thank you to:",
	"",
	"SoapyMan - lead reverse engineer, programmer",
	"Fireboyd78 - code refactoring, improvements",
	"Krishty, someone972 - early formats decoding",
	"Gh0stBlade - HLE emulator base for Psy-Cross",
	"Ben Lincoln - TDR",
	"Stohrendorf - Symdump",
	"and every REDRIVER2 contributor",
};

#define JERICHO_CREDITS_GAP	28	/* between the two columns, 640-wide FE space */
#define JERICHO_CREDITS_MAX_W	620	/* the pair must fit inside this */
#define JERICHO_CREDITS_MID_Y	230	/* vertical centre of both columns (BACK sits at 440) */

static int JerichoCreditsColumnW(const char* const* lines, int n, int scale)
{
	int i, w = 0;

	for (i = 0; i < n; i++)
	{
		int lw = jer_text_width(JER_TEXT_FE, scale, lines[i]);

		if (lw > w)
			w = lw;
	}

	return w;
}

static void JerichoCreditsColumn(const char* const* lines, int n, int cx, int scale, int step)
{
	int y = JERICHO_CREDITS_MID_Y - (n * step) / 2;
	int i;

	/* the column's heading line (index 0) in the cream the frontend uses for
	 * titles; the rest in plain white */
	for (i = 0; i < n; i++, y += step)
		jer_text_draw_centred(JER_TEXT_FE, scale, cx, y, lines[i],
			255, i == 0 ? 235 : 255, i == 0 ? 170 : 255);
}

/* Two columns, centred as a pair: JERICHO's own credits on the left, the
 * REDRIVER2 / OpenDriver2 thanks on the right. The scale steps down until the
 * pair fits, so a longer line can never push a column off the screen or into the
 * other one. */
static void JerichoDrawCredits(void)
{
	int nL = (int)(sizeof(kJerichoCreditsLeft) / sizeof(kJerichoCreditsLeft[0]));
	int nR = (int)(sizeof(kJerichoCreditsRight) / sizeof(kJerichoCreditsRight[0]));
	static const int scales[] = { 150, 140, 130, 115, 100, 90 };
	int k, scale = 90, wL = 0, wR = 0, left;

	for (k = 0; k < (int)(sizeof(scales) / sizeof(scales[0])); k++)
	{
		scale = scales[k];
		wL = JerichoCreditsColumnW(kJerichoCreditsLeft, nL, scale);
		wR = JerichoCreditsColumnW(kJerichoCreditsRight, nR, scale);

		if (wL + JERICHO_CREDITS_GAP + wR <= JERICHO_CREDITS_MAX_W)
			break;
	}

	left = 320 - (wL + JERICHO_CREDITS_GAP + wR) / 2;

	JerichoCreditsColumn(kJerichoCreditsLeft, nL, left + wL / 2, scale, (scale * 22) / 100);
	JerichoCreditsColumn(kJerichoCreditsRight, nR, left + wL + JERICHO_CREDITS_GAP + wR / 2, scale, (scale * 22) / 100);
}

/* The page's own setup and pad handling. BACK (cross on it, or triangle) returns
 * to the mods list rather than leaving the screen. */
static int JerichoCreditsPage(int bSetup)
{
	if (bSetup || gJerichoModsNeedSetup)
	{
		PSXBUTTON* btn = &pCurrScreen->buttons[0];

		gJerichoModsNeedSetup = 0;
		pCurrScreen->numButtons = 1;

		btn->x = 470;
		btn->y = 440;
		btn->w = 120;
		btn->h = 26;
		btn->s_x = 500;
		btn->s_y = 440;
		sprintf(btn->Name, "Back");
		btn->u = btn->d = btn->l = btn->r = 0;
		btn->action = 0;	/* handled below, so the engine does not leave the screen */
		btn->var = -1;

		pCurrButton = btn;
		bRedrawFrontend = 1;

		return bSetup ? 1 : 0;
	}

	if (feNewPad & (MPAD_CROSS | MPAD_TRIANGLE))
	{
		FESound(0);
		gJerichoCreditsOpen = 0;
		gJerichoModsNeedSetup = 1;	/* rebuild the mods list */
		bRedrawFrontend = 1;
		return 1;
	}

	return 0;
}

int JerichoModsScreen(int bSetup)
{
	/* JERICHO: the Mods list is a registered frontend menu now -- "jericho.mods",
	 * built and drawn in Game/C/JERICHO/src/jer_modstore.c as a real table (name /
	 * version / state / source), with the mod repo browser behind it.
	 *
	 * This screen survives for two reasons: it is what the Options screen's button
	 * targets (BTN_NEXT_SCREEN), and it hosts the Credits page. It lays out no
	 * buttons of its own -- it hands its PLACE to the menu, so backing out of the
	 * menu returns to whatever this screen would have returned to: the same trick
	 * the update screen uses (jer_update.c). */
	if (bSetup)
	{
		/* A fresh entry opens on the mods list. The Credits action, though,
		 * arrives BY entering this screen (jer_mods_open_credits): the flag it
		 * set has to survive the setup call, or the page it asked for is
		 * cleared before its first frame. */
		if (gJerichoCreditsPending)
			gJerichoCreditsPending = 0;
		else
			gJerichoCreditsOpen = 0;
	}

	if (gJerichoCreditsOpen)
		return JerichoCreditsPage(bSetup);

	if (bSetup)
		jer_log("[mods] the Mods screen handed over to the jericho.mods table\n");

	jer_modstore_open_menu();
	return 1;
}

/* ------------------------------------------------------------------- */
/* The Mods menu's host-side actions                                     */
/*                                                                      */
/* They live here rather than in jer_modstore.c because they are the     */
/* FRONTEND's own business: the dev-only compile gate, the compile       */
/* prompt, the Credits page. The menu asks and does not care how.        */
/* ------------------------------------------------------------------- */

/* Non-zero when this install can run an in-game build: a src_rebuild tree and
 * the build_game shim are reachable from the exe. A shipped package has
 * neither, so the Mods menu row and the modstore rebuild prompt stay hidden
 * there; a Release built from a source checkout keeps them. */
int jer_mods_compile_available(void)
{
	return jer_build_can_compile();
}

/* Ask, then hand the deep mods to the build (jer_compile.c): its Yes action
 * exits the game, builds, and starts the new exe. */
void jer_mods_request_compile(void)
{
	jer_prompt_begin("Compile the deep mods? JERICHO closes, rebuilds and restarts.", jer_compile_request);
}

/* Open the Credits page. It is a page of THIS screen, so open it in place of the
 * Mods menu (jer_frontend_replace): Back then re-opens the menu in the same slot,
 * and the nav stack still returns to whatever opened the Mods screen. The
 * pending flag tells the setup call that follows this entry to keep the page
 * flag instead of clearing it as a fresh entry would. */
void jer_mods_open_credits(void)
{
	gJerichoCreditsOpen = 1;
	gJerichoCreditsPending = 1;
	gJerichoModsNeedSetup = 1;
	jer_frontend_replace(JERICHO_MODS_SCREEN);
	bRedrawFrontend = 1;
}

/* Enter the Mods screen exactly as its frontend button does. Used by the store's
 * test lever to prove the hand-over path without a pad. */
void jer_mods_open_screen(void)
{
	jer_frontend_goto(JERICHO_MODS_SCREEN);
}

/* Leave the game because a host action asked to (the mod store's rebuild
 * prompt): bQuitToSystem == 2 is the engine's own "quit to system" answer, so
 * the frontend unwinds and State_NONE ends the main loop. */
void jer_frontend_quit(void)
{
	bQuitToSystem = 2;
	jer_log("[jericho] quit requested to rebuild the game\n");
}

// [D] [T]
int GamePlayScreen(int bSetup)
{
	if (bSetup)
	{
		pCurrScreen->buttons[2].action = (allowVibration == 0) ? FE_MAKEVAR(BTN_DISABLED, 0) : FE_MAKEVAR(BTN_NEXT_SCREEN, 17);
	}

	return 0;
}

// [D] [T]
int GameNameScreen(int bSetup)
{
	int offset;

	if (bSetup) 
	{
		
		if (GameType == GAME_TAKEADRIVE && NumPlayers == 2) 
			offset = 48;
		else if (GameType == GAME_COPSANDROBBERS)
			offset = 32;
		else if (GameType == GAME_CAPTURETHEFLAG)
			offset = 40;
		else if (GameType == GAME_CHECKPOINT && NumPlayers == 2)
			offset = 56;
		else
			offset = (GameType - 4U) * 8;

		strcpy(pCurrScreen->buttons[0].Name, GAMEMODE_AREA_NAME(GameLevel, offset, 0));
		strcpy(pCurrScreen->buttons[1].Name, GAMEMODE_AREA_NAME(GameLevel, offset, 1));
	}

	return 0;
}
// [D] [T]
int CheatNumlayerSelect(int bSetup)
{
	if (bSetup) 
	{
		if (numPadsConnected == 2) 
		{
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_NEXT_SCREEN, 32);
		}
		else 
		{
			pCurrScreen->buttons[1].action = FE_MAKEVAR(BTN_DISABLED, 0);
		}
		return 0;
	}
	return 0;
}

int BuildButtonsVertical(int count, int xStart, int yStart)
{
	int numButtons = count;

	if (numButtons > 8)
		numButtons = 8;

	for (int i = 0; i < numButtons; i++)
	{
		int prevButton = i - 1;
		int nextButton = i + 1;

		if (prevButton < 0)
			prevButton += numButtons;

		if (nextButton >= numButtons)
			nextButton -= numButtons;

		PSXBUTTON& btn = pCurrScreen->buttons[i];

		// copy button 0
		btn = PsxScreens[4].buttons[0];

		btn.u = prevButton + 1;
		btn.d = nextButton + 1;

		btn.l = 0;//i + 1;
		btn.r = 0;//i + 1;

		btn.s_x = xStart + 200;
		btn.s_y = yStart + i * 38;

		btn.x = xStart;
		btn.y = yStart + i * 38;

		btn.action = 0;

		btn.var = i;

		sprintf(btn.Name, "Button %d", i + 1);
	}

	pCurrScreen->numButtons = numButtons;

	return numButtons;
}

#ifndef PSX
#include "../utils/fs.h"
#define MAX_REPLAY_NAME_LENGTH 48
char gFEReplayList[8][MAX_REPLAY_NAME_LENGTH];
int gFEReplayCount = 0;
#endif

int UserReplaySelectScreen(int bSetup)
{
#ifndef PSX
	if (bSetup)
	{
		FS_FINDDATA* fd;
		const char* filename;
		const char* tmp;

		gFEReplayCount = 0;
		fd = NULL;
		filename = FS_FindFirst("Replays/*.D2RP", &fd);

		while (filename)
		{
			strncpy(gFEReplayList[gFEReplayCount], filename, MAX_REPLAY_NAME_LENGTH);
			tmp = strchr(filename, '.');
			gFEReplayList[gFEReplayCount][MAX_REPLAY_NAME_LENGTH-1] = 0;

			gFEReplayCount++;
			filename = FS_FindNext(fd);

			// [A] don't load more than 8
			if (gFEReplayCount >= 8)
				break;
		}

		FS_FindClose(fd);

		if (gFEReplayCount)
		{
			int yOfs = 170;

			if (gFEReplayCount < 6)
				yOfs = 208;

			int numButtons = BuildButtonsVertical(gFEReplayCount, 160, yOfs);

			for (int i = 0; i < numButtons; i++)
			{
				PSXBUTTON& btn = pCurrScreen->buttons[i];

				/* snprintf, not strncpy with the full destination size: a replay
				 * name can be MAX_REPLAY_NAME_LENGTH-1 chars, and strncpy stops
				 * copying WITHOUT a terminator once it has filled sizeof(btn.Name),
				 * so the strchr below (and every draw of the label) would read into
				 * the next button. */
				snprintf(btn.Name, sizeof(btn.Name), "%s", gFEReplayList[i]);

				tmp = strchr(btn.Name, '.');

				if (tmp)
					btn.Name[tmp - btn.Name] = 0;
				else
					btn.Name[15] = 0;
			}
		}
		else
		{
			BuildButtonsVertical(1, 160, 208);

			PSXBUTTON& btn = pCurrScreen->buttons[0];
			btn.action = FE_MAKEVAR(BTN_PREVIOUS_SCREEN, 0);
			strcpy(btn.Name, G_LTXT(GTXT_NoSavedData));
		}

		return 0;
	}

	if (gFEReplayCount)
	{
		if (feNewPad & MPAD_CROSS)
		{
			char filename[64];
			int selectedReplay = pCurrButton->var;

			sprintf(filename, "Replays/%s", gFEReplayList[selectedReplay]);

			if (LoadReplayFromFile(filename))
			{
				StoredGameType = GameType;
				GameType = GAME_LOADEDREPLAY;
				SetState(STATE_GAMESTART);
			}
			else
			{
				SetState(STATE_INITFRONTEND);
			}
		}
	}
#endif

	return 0;
}

char* TimeOfDayNames[] = {
	G_LTXT_ID(GTXT_Dawn),
	G_LTXT_ID(GTXT_Day),
	G_LTXT_ID(GTXT_Dusk),
	G_LTXT_ID(GTXT_Night)
};

char* WeatherNames[] = {
	G_LTXT_ID(GTXT_Clear),
	G_LTXT_ID(GTXT_Rainy),
	G_LTXT_ID(GTXT_Wet),
};

char* TimeOfDayItems[] = {
	G_LTXT_ID(GTXT_TimeOfDay),
	G_LTXT_ID(GTXT_Condition),
	G_LTXT_ID(GTXT_Continue),
};

int TimeOfDaySelectScreen(int bSetup)
{
	int numButtons, i;
	int dir;
	char text[64];
	int ypos[2] = { 208, 246 };

	if (bSetup)
	{
		// setup time we want to reset it
		wantedWeather = 0;
		wantedTimeOfDay = TIME_DAY;

#ifndef PSX
		/* Arriving from the car-select screen (or the stock car screen's own
		 * back) leaves the extra on a CAR icon (loaded[0] == -1); Day/Night shows
		 * the city, so put it back. Coming from the city screen the city is already
		 * up (loaded[0] != -1) and is left untouched. */
		if (loaded[0] == -1)
			FrontendShowCityExtra();
#endif

		numButtons = BuildButtonsVertical(3, 168, 208);

		for (i = 0; i < numButtons; i++)
		{
			PSXBUTTON& btn = pCurrScreen->buttons[i];
			sprintf(btn.Name, "%s%c", GET_GAME_TXT(TimeOfDayItems[i]), i == 2 ? 0 : ':');

			if (i == 2)
			{
				btn.y += 32;
				btn.s_y += 32;
				btn.action = FE_MAKEVAR(BTN_NEXT_SCREEN, 14);
			}
			else
			{
				btn.x -= 80;
				btn.l = btn.r = i + 1;
			}
		}

		pNewButton = &pCurrScreen->buttons[2];
		iScreenSelect = SCREEN_TIMEOFDAY;

		return 0;
	}

	FEPrintString(GET_GAME_TXT(TimeOfDayNames[wantedTimeOfDay]), 590, ypos[0], 4, 128, 128, 128);
	FEPrintString(GET_GAME_TXT(WeatherNames[wantedWeather]), 590, ypos[1], 4, 128, 128, 128);

	if (feNewPad & MPAD_TRIANGLE)
	{
		// reset back
		wantedWeather = -1;
		wantedTimeOfDay = -1;

		iScreenSelect = SCREEN_NONE;
	}

	dir = 0;

	if (feNewPad & MPAD_D_LEFT)
	{
		dir = -1;
	}
	else if (feNewPad & MPAD_D_RIGHT)
	{
		dir = 1;
	}

	if (dir != 0)
	{
		i = pCurrButton - pCurrScreen->buttons;

		if (i == 0)
		{
			wantedTimeOfDay += dir;
			wantedTimeOfDay &= 3;

			gWantNight = wantedTimeOfDay == TIME_DUSK || wantedTimeOfDay == TIME_NIGHT;
		}
		else if (i == 1)
		{
			wantedWeather += dir;
			if (wantedWeather < 0)
				wantedWeather += 3;
			else if (wantedWeather >= 3)
				wantedWeather -= 3;
		}
		else
			dir = 0;
	}

	return 0;
}

int DemoScreen(int bSetup)
{
	if (bSetup)
		return 0;
	
	if (feNewPad & MPAD_CROSS)
	{
		pScreenStack[ScreenDepth] = pCurrScreen;
		pButtonStack[ScreenDepth] = pCurrButton;
		
		FESound(2);
		GameType = GAME_PURSUIT;
	
		GameLevel = 1;
		gWantNight = 0;
		gSubGameNumber = 0;
		
		SetState(STATE_GAMESTART);
		
		return 0;
	}

	if(mainScreenLoaded)
	{
		if (feNewPad & MPAD_CIRCLE)
		{
			LoadBackgroundFile(contNames[0]);
			FESound(2);
		}
	}
	else if(bQuitToSystem)
	{
		bQuitToSystem = 0;
		LoadBackgroundFile("DATA\\GFX.RAW");
	}

	return 0;
}
