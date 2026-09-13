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
char* GetCarImportModels(int slot);
char* GetCarImportCosmetics(int slot);

// Which city the level is importing from (-1 = none), and that city's car
// palettes. cars.c needs both: a foreign vehicle's texture pages must map to the
// palette slots its own city's palettes were stored in, not the host's.
int GetCarImportCity(void);
char* GetCarImportPallet(int* size);

// The imported city's LUMP_TEXTUREINFO body - the page lists LoadPermanentTPages
// walks. texture.c parses it (the TP/TEXINF types the layout needs live there).
char* GetCarImportTextureInfo(int* size);

// Where the imported city's permanent page data starts in its level file (-1 if
// none), and a raw ranged read of that file. texture.c uses both to carve out a
// page: the entries are concatenated there, each sector-aligned, which is exactly
// how LoadPermanentTPages walks them.
int GetCarImportPageBase(void);
int ReadCarImportFile(int offset, void* dst, int len);

extern int ProcessCarModelLump(char *lump_ptr, int lump_size); // 0x00064E6C

extern MODEL* FindModelPtrWithName(char *name); // 0x0005D40C

extern int FindModelIdxWithName(char *name); // 0x0005D4C4

#endif
