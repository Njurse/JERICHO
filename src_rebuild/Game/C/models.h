#ifndef MODELS_H
#define MODELS_H

enum CarModelType
{
	CAR_MODEL_CLEAN = 1,
	CAR_MODEL_DAMAGED,
	CAR_MODEL_LOWDETAIL
};

extern MODEL dummyModel;

extern char* modelname_buffer;
extern char *car_models_lump;

extern MODEL* modelpointers[MAX_MODEL_SLOTS];
extern MODEL* pLodModels[MAX_MODEL_SLOTS];
extern int litSprites[MAX_MODEL_SLOTS / 32];

extern unsigned short *Low2HighDetailTable;
extern unsigned short *Low2LowerDetailTable;

extern int num_models_in_pack;

extern int CleanSpooledModelSlots();
extern void ProcessModel(int modelIdx);

extern void ProcessMDSLump(char *lump_file, int lump_size); // 0x00064CFC

// JERICHO cross-city car import. InitCarImport() reads whatever another city's
// level file the module asked for (via JER_ARGS_CAR_DATA_SOURCE.modelSource[]) and
// holds it for the level; the getters answer "use the foreign block instead of
// the level's own" for a slot, or NULL for the normal case. Called from
// SetupResidentModels, so both the geometry and the colours are taken from it.
void InitCarImport(void);

// JERICHO: read in the cities the set names WITHOUT disturbing what is already
// loaded -- for adding a city's data to a level that is already running (the hot
// load). InitCarImport() is a level-start function and must never be called
// mid-level: see its sibling's comment in models.c.
void InitCarImportMidLevel(void);
char* GetCarImportModels(int slot);

// JERICHO cross-city HOT LOAD: build resident `slot`'s geometry at RUNTIME from the
// import data its source city asked for -- for a car added to the resident set
// AFTER the level loaded (mp: a joiner's picked car), which the level's own build
// cannot cover because it happens against the level's heap while it loads.
// Returns the bytes used, or 0 when nothing was built: no import source, already
// built, or it does not fit the pool -- and then the slot is left as it was, since
// a half-built car is worse than a substitute. Call InitCarImport() first, so the
// city's data is actually there.
int JerHotLoadCarModel(int slot);

// JERICHO cross-city UNLOAD: give back the geometry a hot-loaded slot holds and return its pool
// block, so the slot can be built again later. Returns 1 when something was given back, 0 when
// the slot held no hot-load geometry (it was built at level load, or never built) - in which
// case the caller has nothing to release on this side. Anything still driving that slot must be
// rebuilt afterwards; mp does that through its own swap path.
int JerReleaseCarGeometry(int slot);

// JERICHO cross-city unload, all of it: hand back the imported cities' buffers and the hot-load
// pool. Called when no map is loaded (the frontend, after leaving a session) - a level load does
// the same work through InitCarImport, so this exists for the case where no level will come along
// to clean up. Returns how many city buffers were freed.
int JerReleaseCarImport(void);char* GetCarImportCosmetics(int slot);

// Which city the level is importing from (-1 = none), and that city's car
// palettes. cars.c needs both: a foreign vehicle's texture pages must map to the
// palette slots its own city's palettes were stored in, not the host's.
//
// These are the SINGLE-CITY forms: they answer with the FIRST city the level
// holds, which is what a level naming one city (the normal case) means. A set may
// name MORE than one city (gCarImports[4] in models.c), so a caller that has a
// SLOT in hand should use the ForCity forms with GetCarModelSourceCity(slot) and
// the two agree. CarImportCityHeld says whether a city is loaded at all.
int GetCarImportCity(void);
int CarImportCityHeld(int city);

char* GetCarImportPallet(int* size);
char* GetCarImportPalletForCity(int city, int* size);

// The imported city's LUMP_TEXTUREINFO body - the page lists LoadPermanentTPages
// walks. texture.c parses it (the TP/TEXINF types the layout needs live there).
char* GetCarImportTextureInfo(int* size);
char* GetCarImportTextureInfoForCity(int city, int* size);

// Where the imported city's permanent page data starts in its level file (-1 if
// none), and a raw ranged read of that file. texture.c uses both to carve out a
// page: the entries are concatenated there, each sector-aligned, which is exactly
// how LoadPermanentTPages walks them.
int GetCarImportPageBase(void);
int GetCarImportPageBaseForCity(int city);
int ReadCarImportFile(int offset, void* dst, int len);
int ReadCarImportFileForCity(int city, int offset, void* dst, int len);

extern int ProcessCarModelLump(char *lump_ptr, int lump_size); // 0x00064E6C

extern MODEL* FindModelPtrWithName(char *name); // 0x0005D40C

extern int FindModelIdxWithName(char *name); // 0x0005D4C4

#endif
