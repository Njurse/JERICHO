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

extern int ProcessCarModelLump(char *lump_ptr, int lump_size); // 0x00064E6C

extern MODEL* FindModelPtrWithName(char *name); // 0x0005D40C

extern int FindModelIdxWithName(char *name); // 0x0005D4C4

#endif
