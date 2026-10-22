#ifndef SYSTEM_H
#define SYSTEM_H

struct DRAW_MODE
{
	short x1, y1;
	short x2, y2;
	short width, height;
	short framex, framey;
};

enum CDTYPE
{
	CDTYPE_NODISC = 0,
	CDTYPE_SHELLOPEN = 1,
	CDTYPE_DISCERROR = 2,
	CDTYPE_WRONGDISC = 3,
	CDTYPE_CORRECTDISC = 4,
};

enum CITYTYPE
{
	CITYTYPE_DAY = 0,
	CITYTYPE_NIGHT = 1,
	CITYTYPE_MULTI_DAY = 2,
	CITYTYPE_MULTI_NIGHT = 3,
};


extern volatile char* _overlay_buffer;		// 0x1C0000
extern volatile char* _frontend_buffer;		// 0xFB400
extern volatile char* _other_buffer;		// 0xF3000
extern volatile char* _other_buffer2;		// 0xE7000

extern volatile OTTYPE* _OT1;				// 0xF3000
extern volatile OTTYPE* _OT2;				// 0xF7200

extern volatile char* _primTab1;			// 0xFB400
extern volatile char* _primTab2;			// 0x119400
extern volatile char* _sbank_buffer;		// 0x180000
extern volatile char* _replay_buffer;		// 0x1FABBC

extern char gDataFolder[32];

/* JERICHO: how many cities are Driver 2's own. Rows 0..CITY_D2_COUNT-1 of
 * LevelNames / LevelFiles / CosmeticFiles are the stock four and read from
 * gDataFolder; the rows after them are the Driver 1 car-data cities and read from
 * gD1DataFolder. Keep CITY_COUNT in step with those three tables.
 *
 * JERICHO: CITY_COUNT IS A CAPACITY, NOT A COUNT. The stock eleven rows are filled
 * at compile time; the CITY_CUSTOM_MAX rows after them are filled at RUNTIME by
 * JerCityRegistryInit() from JERICHO/CONFIG/cities.ini - one arbitrary car-data
 * bank per row - and stay NULL until a bank is installed there. Every [CITY_COUNT]
 * table is sized for the capacity, so:
 *
 *   - ask JerCityCount() how many cities this install actually has, and
 *     JerCityRegistered(city) whether one particular row is usable;
 *   - loop to CITY_COUNT only to sweep STORAGE (free, reset, dump) - never to
 *     build a list a player can see, which would show the empty rows;
 *   - never index LevelNames/LevelFiles/CosmeticFiles on an unregistered row.
 *     They are NULL there and a `%s` of that is a crash: use JerBuildCityPath. */
#define CITY_D2_COUNT 4
#define CITY_D1_COUNT 7
#define CITY_CUSTOM_MAX 16
#define CITY_COUNT    (CITY_D2_COUNT + CITY_D1_COUNT + CITY_CUSTOM_MAX)

extern char gD1DataFolder[32];
extern const char* JerGetCityDataRoot(int city);

/* ---- JERICHO: the runtime city registry ---------------------------------
 *
 * WHY THIS EXISTS: a city index used to be a row of the three compile-time tables
 * above, so the engine could only open a car-data file it had been compiled to
 * name ("LEVELS\\CHICAGO.LEV", "JERICHO\\MODS\\carhacks\\D1CARS\\MIAMI.LEV", ...). carhacks' cross-city
 * import is policy ("slot 5 = city 2, model 7") and is already index-based, so a
 * bank that is not one of those compiled names could not be offered at all - the
 * engine would never open the filename. Rows CITY_D2_COUNT+... are that gap
 * closed: registered from config, each with its own display name and its own
 * arbitrary (sub)folder-relative paths, and every city-indexed path carhacks
 * already walks works on it unchanged.
 *
 * A registered bank is a CAR-DATA SOURCE, exactly like Driver 1's seven: importable,
 * never played - no GameLevel, no frontend city row, no spool, no loading screen. */
#define JER_CITY_FILE_LEVEL     0	/* the car-data .LEV (LUMP_CAR_MODELS etc.) */
#define JER_CITY_FILE_COSMETIC  1	/* the .LCF beside it (optional; may be none) */

/* How many cities this install has: the stock eleven plus the banks whose files are
 * present. THE COUNT TO LOOP TO for anything a player can see. */
int JerCityCount(void);
/* First row a bank can occupy (the stock rows are 0..this-1). */
int JerCityFirstCustom(void);
/* 1 when the row has a name AND a file, i.e. it is safe to read the tables. */
int JerCityRegistered(int city);
/* Display name, "" when unregistered. Never NULL. */
const char* JerCityName(int city);
/* The table's own relative path for that file kind, "" when there is none.
 * This is what cities.ini said - NOT rooted, NOT slash-normalised. */
const char* JerCityFile(int city, int kind);

/* Register one arbitrary bank and answer its city index, or -1 when there is no
 * free row, the name/level file is empty, or the level file is not on disk. The
 * strings are COPIED into engine storage, so the caller may free its own. A NULL
 * `levFile` unregisters that index (the entry was removed). `lcfFile` may be NULL
 * or "" - a bank with no colours file is legal and the cars keep their derived
 * cosmetics. */
int JerCityRegister(const char* name, const char* levFile, const char* lcfFile);

/* Build a city's on-disk path for fopen: data root + table entry, slashes
 * normalised (`FS_FixPathSlashes`). `arenaVariant` asks for the M<CITY>.LEV
 * multiplayer pool, which only Driver 2's own four cities have. Returns 1 on
 * success and 0 for an unregistered row or a short buffer - never builds a path
 * out of a NULL table entry. */
int JerCityPath(int city, char* out, int outSize, int kind, int arenaVariant);
/* The same path, opened: 1 when it exists, 0 otherwise. Used by the registry's own
 * presence probe, so it is exactly the file the loader will later read. */
int JerCityFileExists(int city, int kind);

/* Register a bank from a MODULE (path relative to the game's data folder, the
 * same spelling a city_N key uses). The row is module-owned: a later config
 * refresh re-probes its file instead of clearing it for having no city_N key.
 * A city_N key for that slot takes ownership instead. Answers the city index,
 * or -1 when the file is not on disk / no row is free. */
int JerCityRegister(const char* name, const char* levFile, const char* lcfFile);

/* Read JERICHO/CONFIG/cities.ini and register every bank whose files are present.
 * Idempotent; called once at boot from JerCityRegistryBoot, and safe to call from
 * a module. Answers the number of registered banks. */
int JerCityRegistryInit(void);
/* Re-scan the config and the disk: registers banks that have appeared since the
 * last scan and DROPS a bank whose level file has gone. Called before a car list
 * or a level's car data is built, so availability follows the disk without a
 * restart. Answers the number of registered banks. */
int JerCityRegistryRefresh(void);

#define PSX_MALLOC_SIZE 870332

extern volatile char* malloctab;
extern volatile char* mallocptr;

#if USE_CRT_MALLOC

extern char* sys_malloc(int size, char* funcname, int line);
extern char* sys_tempalloc(int size);
extern void sys_tempfree();

extern void sys_freeall();

#define D_TEMPFREE()		sys_tempfree()
#define D_TEMPALLOC(size)	sys_tempalloc(size)

#ifdef _DEBUG
#define D_MALLOC(size)		sys_malloc(size, FUNCNAME, __LINE__)

#else
#define D_MALLOC(size)		sys_malloc(size, NULL, 0)
#endif

#else
#define D_MALLOC(size)		(char*)mallocptr; mallocptr += (((size) + 3) & -4);
#define D_TEMPALLOC(size)	(char*)mallocptr
#define D_TEMPFREE()
#endif

#ifdef _DEBUG
#define DMalloc_DebugPrint printWarning
#else
#define DMalloc_DebugPrint (void)
#endif

#define D_MALLOC_BEGIN() \
	{ \
		volatile const char* _oldmalloc = mallocptr;

#ifdef __GNUC__
#define D_MALLOC_END() \
		D_TEMPFREE();\
		if(mallocptr > _oldmalloc)\
			DMalloc_DebugPrint("malloc(%d) in %s, line %d. Malloc usage: %d\n", mallocptr-_oldmalloc, FUNCNAME, __LINE__, (mallocptr-malloctab));\
	} // D_MALLOC_BEGIN block
#else
#define D_MALLOC_END() \
		D_TEMPFREE();\
		if(mallocptr > _oldmalloc)\
			DMalloc_DebugPrint("malloc(%d) in " FUNCNAME ", line %d. Malloc usage: %d\n", mallocptr-_oldmalloc, __LINE__, (mallocptr-malloctab));\
	} // D_MALLOC_BEGIN block
#endif

extern int leadAIRequired;
extern int leadAILoaded;
extern int pathAILoaded;

extern char* LevelNames[];
extern char* LevelFiles[];	// LEVELS\<CITY>.LEV, indexed by GameLevel; CITY_COUNT rows
extern char* LoadingScreenNames[];

struct DB
{
	char* primptr;
	OTTYPE* ot;
	char* primtab;
	int id;
	DRAWENV draw;
	DISPENV disp;
};

extern DB MPBuff[2][2];
extern DB* last;
extern DB* current;

extern DRAW_MODE draw_mode_pal;
extern DRAW_MODE draw_mode_ntsc;

#define SCREEN_FB	512
#define SCREEN_FB_H	256

#ifdef PAL_VERSION
#define SCREEN_H	256
#define draw_mode draw_mode_pal
#define video_mode MODE_PAL
#else
#define SCREEN_H	240
#define draw_mode draw_mode_ntsc
#define video_mode MODE_NTSC
#endif // PAL

// ordering table size 
#ifdef PSX
#define	OTSIZE	0x1080
#else
#define	OTSIZE	0x2000
#endif

#if USE_EXTENDED_PRIM_POINTERS
#	define PRIMTAB_SIZE 0x50000
#else
#	define PRIMTAB_SIZE 0x1e000
#endif

#define CITYLUMP_DATA1		0
#define CITYLUMP_TPAGE		1
#define CITYLUMP_DATA2		2
#define CITYLUMP_SPOOL		3

extern int citystart[8];
extern XYPAIR citylumps[8][4];

#define CDSECTOR_SIZE 2048

extern void ClearMem(char *mem, int size); // 0x0007F3E8
extern void setMem8(unsigned char *mem, unsigned char val, int size); // 0x0007F47C
extern void setMem16(unsigned short *mem, unsigned short val, int size); // 0x0007F524

extern void Init_FileSystem(); // 0x0007F848

extern int FileExists(char *filename); // 0x0007F5C8
extern int Loadfile(char *name, char *addr); // 0x0007F354
extern int LoadfileSeg(char *name, char *addr, int offset, int loadsize); // 0x0007E9D0

extern void ReportMode(int on); // 0x0007F8B8

extern void loadsectors(char *addr, int sector, int nsectors); // 0x0007F904

extern void EnableDisplay(); // 0x0007F984
extern void DisableDisplay(); // 0x0007F9F0

extern void ClearCurrentDrawBuffers();

extern void SwapDrawBuffers(); // 0x0007F6FC
extern void SwapDrawBuffers2(int player); // 0x0007ECF0

extern void UpdatePadData(); // 0x0007FA10

extern void SetupDrawBuffers(); // 0x0007EDDC
extern void SetupDrawBufferData(int num_players); // 0x0007EF0C

extern void InitaliseDrawEnv(DB *pBuff, int x, int y, int w, int h); // 0x0007F7C0

extern void ResetCityType(); // 0x0007F5B4
extern void SetCityType(CITYTYPE type); // 0x0007F118

extern CITYTYPE GetCityType();
extern CDTYPE DiscSwapped(char *filename); // 0x0007F640


#endif
