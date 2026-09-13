#include "driver2.h"
#include "models.h"
#include "system.h"
#include "spool.h"
#include "mission.h"
#include "cars.h"
#include "cosmetic.h"
#include "jericho.h"	// JERICHO-HOOK: mod runtime (inert without modules)
#include "jer_events.h"	// JERICHO-HOOK: event argument structs

#if USE_PC_FILESYSTEM
extern int gContentOverride;
#endif

MODEL dummyModel = { 0 };

char* modelname_buffer = NULL;
char *car_models_lump = NULL;

MODEL* modelpointers[MAX_MODEL_SLOTS];
MODEL* pLodModels[MAX_MODEL_SLOTS];

int num_models_in_pack = 0;

u_short *Low2HighDetailTable = NULL;
u_short *Low2LowerDetailTable = NULL;

// [A]
int permanentModelSlotBitfield[MAX_MODEL_SLOTS / 32];
int litSprites[MAX_MODEL_SLOTS / 32];

// [A] returns freed slot count
int CleanSpooledModelSlots()
{
	int i;
	int num_freed;

	num_freed = 0;

	// assign model pointers
	for (i = 0; i < MAX_MODEL_SLOTS; i++) // [A] bug fix. Init with dummyModel
	{
		// if bit does not indicate usage - reset to dummy model
		if((permanentModelSlotBitfield[i >> 5] & 1 << (i & 31)) == 0)
		{
			if(modelpointers[i] != &dummyModel)
			{
				modelpointers[i] = &dummyModel;
				pLodModels[i] = &dummyModel;
				litSprites[i >> 5] &= ~(1 << (i & 31));

				num_freed++;
			}
		}
	}

	return num_freed;
}

// [A]
void ProcessModel(int modelIdx)
{
	MODEL* model;
	int lit;

	model = modelpointers[modelIdx];
	model->tri_verts = 0; // [A] this is used as additional flags for animated models and triangle processing
	lit = 0;

	if (gTimeOfDay == TIME_NIGHT)
	{
		if (GameLevel == 0)
		{
			// chicago
		}
		else if (GameLevel == 1)
		{
			// havana
			if (model->shape_flags & SHAPE_FLAG_SPRITE)
				lit = modelIdx != 1223 && !(model->flags2 & MODEL_FLAG_TREE);
		}
		else if (GameLevel == 2)
		{
			// vegas
			if (model->shape_flags & SHAPE_FLAG_SPRITE)
			{
				if (gMultiplayerLevels)
					lit = modelIdx == 263 || modelIdx == 275;
				else
					lit = modelIdx == 945 || modelIdx == 497;
			}
		}
		else if (GameLevel == 3)
		{
			// rio
		}
	}

	if(lit)
		litSprites[modelIdx >> 5] |= 1 << (modelIdx & 31);
}

// [D] [T]
void ProcessMDSLump(char *lump_file, int lump_size)
{
	char* mdsfile;
	MODEL *model;
	MODEL *parentmodel;
	int modelAmts;
	int i, size;
	int litModel;

	modelAmts = *(int *)lump_file;
	mdsfile = (lump_file + 4);
	num_models_in_pack = modelAmts;

	// [A] usage bits
	ClearMem((char*)permanentModelSlotBitfield, sizeof(permanentModelSlotBitfield));
	ClearMem((char*)litSprites, sizeof(litSprites));

	// assign model pointers
	for (i = 0; i < MAX_MODEL_SLOTS; i++) // [A] bug fix. Init with dummyModel
	{
		modelpointers[i] = &dummyModel;
		pLodModels[i] = &dummyModel;
	}

	for (i = 0; i < modelAmts; i++)
	{
		size = *(int*)mdsfile;
		mdsfile += sizeof(int);

		if (size)
		{
			// add the usage bit
			permanentModelSlotBitfield[i >> 5] |= 1 << (i & 31);
			
			model = (MODEL*)mdsfile;
			modelpointers[i] = model;

			ProcessModel(i);
		}

		mdsfile += size;
	}

	// process parent instances
	for (i = 0; i < modelAmts; i++)
	{
		model = modelpointers[i];
		if (model->instance_number != -1) 
		{
			parentmodel = modelpointers[model->instance_number];
#if MODEL_RELOCATE_POINTERS
			// convert to real offsets
			model->vertices = (int)(char*)parentmodel + parentmodel->vertices;
			model->normals = (int)(char*)parentmodel + parentmodel->normals;
			model->point_normals = (int)(char*)parentmodel + parentmodel->point_normals;
			if (parentmodel->collision_block != 0)
				model->collision_block = (int)(char*)parentmodel + parentmodel->collision_block;
#else
			if (parentmodel->collision_block != 0)
				model->collision_block = parentmodel->collision_block;
#endif
		}
	}

#if MODEL_RELOCATE_POINTERS
	// process models without parents
	for (i = 0; i < modelAmts; i++)
	{
		model = modelpointers[i];
		model->poly_block += (int)(char*)model;

		if (model->instance_number == -1) 
		{
			model->vertices += (int)model;
			model->normals += (int)model;
			model->point_normals += (int)model;

			if (model->collision_block != 0)
				model->collision_block += (int)model;
		}
	}
#endif
}

char* _MDL_GETTER_vertices(MODEL* mdl)
{
	if (mdl->instance_number != -1)
		mdl = modelpointers[mdl->instance_number];
	return (char*)mdl + mdl->vertices;
}

char* _MDL_GETTER_normals(MODEL* mdl)
{
	if (mdl->instance_number != -1)
		mdl = modelpointers[mdl->instance_number];
	return (char*)mdl + mdl->normals;
}

char* _MDL_GETTER_point_normals(MODEL* mdl)
{
	if (mdl->instance_number != -1)
		mdl = modelpointers[mdl->instance_number];
	return (char*)mdl + mdl->point_normals;
}

char* _MDL_GETTER_collision_block(MODEL* mdl)
{
	if (mdl->instance_number != -1)
		mdl = modelpointers[mdl->instance_number];

	if (!mdl->collision_block)
		return 0;

	return (char*)mdl + mdl->collision_block;
}

// [D] [T]
// ---------------------------------------------------------------------------
// JERICHO cross-city car import
//
// A resident slot may take its geometry (and its colours) from another city's
// level file. SetupResidentModels runs JER_EVENT_CAR_DATA_SOURCE before this, so
// by the time the models are built the module's per-slot sources are known;
// InitCarImport() reads those files once and holds them for the level.
//
// Everything here fails soft: if a file, a lump or a heap allocation is missing,
// the slot simply keeps the level's own data. See
// MODS/combatd2/carhacks/CROSS_CITY.md for the file format.

#define CAR_IMPORT_LUMP_MODELS	28	// LUMP_CAR_MODELS
#define CAR_IMPORT_LUMP_PALLET	25	// LUMP_PALLET - the car palettes (civ_clut)
#define CAR_IMPORT_LUMP_TEXINFO	34	// LUMP_TEXTUREINFO - the page lists

typedef struct
{
	char* region;		// malloc'd DATA1 copy the car-models block points into
	char* carModels;	// LUMP_CAR_MODELS body, or NULL
	int carModelsSize;
	char* pallet;		// LUMP_PALLET body (car palettes), or NULL
	int palletSize;
	char* cosmetics;	// the city's .LCF (car colours), or NULL
	int cosmeticsSize;
	char* texInfo;		// LUMP_TEXTUREINFO body (page lists), or NULL
	int texInfoSize;
	int pageBase;		// byte offset of this file's permanent page data
} CAR_IMPORT;

static CAR_IMPORT gCarImport;
static int gCarImportCity = -1;	// city the held import came from, -1 = none

// Find a segment inside a lump body. ProcessLumps advances 4-byte aligned, so
// this must too - a raw size+8 walk drifts as soon as a segment's size is not a
// multiple of four.
static int FindLumpSegment(char* body, int size, int want, char** out, int* outSize)
{
	int off = 0;

	while (off + 8 <= size)
	{
		int type = *(int*)(body + off);
		int segSize = *(int*)(body + off + 4);

		if (segSize < 0 || off + 8 + segSize > size)
			break;

		if (type == want)
		{
			*out = body + off + 8;
			*outSize = segSize;
			return 1;
		}

		off += 8 + ((segSize + 3) & ~3);
	}

	return 0;
}

static void FreeCarImport(CAR_IMPORT* imp)
{
	if (imp->region)
		free(imp->region);

	if (imp->cosmetics)
		free(imp->cosmetics);

	memset(imp, 0, sizeof(*imp));
}

// Read a whole file into a fresh buffer. NULL on any failure.
static char* ReadWholeFile(const char* filename, int* outSize)
{
	FILE* fp;
	long len;
	char* buf;

	*outSize = 0;

	fp = fopen(filename, "rb");

	if (fp == NULL)
		return NULL;

	fseek(fp, 0, SEEK_END);
	len = ftell(fp);

	if (len <= 0 || len > 4 * 1024 * 1024)
	{
		fclose(fp);
		return NULL;
	}

	buf = (char*)malloc(len);

	if (buf == NULL)
	{
		fclose(fp);
		return NULL;
	}

	if (fseek(fp, 0, SEEK_SET) != 0 || fread(buf, 1, len, fp) != (size_t)len)
	{
		free(buf);
		fclose(fp);
		return NULL;
	}

	fclose(fp);
	*outSize = (int)len;

	return buf;
}

// Read <city>'s level file and pull out its car-models block: the file's DATA1
// region (citylumps[0]), walked as segments. Returns 0 (leaving imp empty) if
// anything is missing.
static int LoadCarImport(int city, CAR_IMPORT* imp)
{
	char filename[64];
	unsigned int table[8];
	FILE* fp;
	long data1Off, data1Size;

	memset(imp, 0, sizeof(*imp));

	if (city < 0 || city >= 4)
		return 0;

	// the full single-player level first, then the arena variant
	sprintf(filename, "%s%s", gDataFolder, LevelFiles[city]);
	fp = fopen(filename, "rb");

	if (fp == NULL)
	{
		sprintf(filename, "%sM%s", gDataFolder, LevelFiles[city]);
		fp = fopen(filename, "rb");
	}

	if (fp == NULL)
		return 0;

	// the file starts with a lump header; the citylump table follows it
	if (fseek(fp, 8, SEEK_SET) != 0 || fread(table, 1, sizeof(table), fp) != sizeof(table))
	{
		fclose(fp);
		return 0;
	}

	data1Off = table[0];	// CITYLUMP_DATA1 = 0: (byte offset, byte size)
	data1Size = table[1];

	if (data1Off <= 0 || data1Size <= 8 || data1Size > 4 * 1024 * 1024)
	{
		fclose(fp);
		return 0;
	}

	// The permanent page data follows DATA1 in the file. The engine reads it from
	// the sector just past DATA1 (main.c advances the sector before handing it to
	// LoadPermanentTPages), so the base is the same sum, in bytes.
	imp->pageBase = (int)(data1Off / CDSECTOR_SIZE + data1Size / CDSECTOR_SIZE) * CDSECTOR_SIZE;

	imp->region = (char*)malloc(data1Size);

	if (imp->region == NULL)
	{
		fclose(fp);
		return 0;
	}

	if (fseek(fp, data1Off, SEEK_SET) != 0 || fread(imp->region, 1, data1Size, fp) != (size_t)data1Size)
	{
		fclose(fp);
		FreeCarImport(imp);
		return 0;
	}

	fclose(fp);

	// DATA1's body is itself a container lump, so its segments start 8 bytes in
	FindLumpSegment(imp->region + 8, (int)data1Size - 8, CAR_IMPORT_LUMP_MODELS, &imp->carModels, &imp->carModelsSize);

	if (imp->carModels == NULL)
	{
		FreeCarImport(imp);
		return 0;
	}

	// The car PALETTES are a separate lump in the same file. A foreign vehicle's
	// polygons reference ITS city's texture pages, whose colours live here - the
	// host level's palettes are a different set entirely.
	FindLumpSegment(imp->region + 8, (int)data1Size - 8, CAR_IMPORT_LUMP_PALLET, &imp->pallet, &imp->palletSize);

	// And the page LISTS, which say which texture sets that city's level loads
	// and how big each set's data is. texture.c parses this; the page bytes
	// themselves sit right after DATA1 in the file, which it reads separately.
	FindLumpSegment(imp->region + 8, (int)data1Size - 8, CAR_IMPORT_LUMP_TEXINFO, &imp->texInfo, &imp->texInfoSize);

	// the car colours live beside it, as LEVELS\<city>.LCF
	sprintf(filename, "%s%s", gDataFolder, CosmeticFiles[city]);
	imp->cosmetics = ReadWholeFile(filename, &imp->cosmeticsSize);

	return 1;
}

// Load the foreign car data the module asked for. Called from
// SetupResidentModels once the resident models and their sources are final, so
// it runs before both the geometry (ProcessCarModelLump) and the colours
// (ProcessCosmeticsLump) are taken from it. A no-op for a stock level.
void InitCarImport(void)
{
	int city = -1;
	int i;

	FreeCarImport(&gCarImport);
	gCarImportCity = -1;

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		int src = GetCarModelSourceCity(i);

		if (src >= 0 && residentCarModels[i] != -1)
		{
			city = src;
			break;
		}
	}

	if (city < 0)
		return;

	if (!LoadCarImport(city, &gCarImport))
	{
		printInfo("cross-city: no usable car data in %s - keeping the level's own vehicles\n", LevelFiles[city]);
		return;
	}

	gCarImportCity = city;

	printInfo("cross-city: car data from %s (%d bytes of models, %d of car palettes, %d of cosmetics)\n",
		LevelNames[city], gCarImport.carModelsSize, gCarImport.palletSize, gCarImport.cosmeticsSize);
}

// The city the level is importing vehicles from, or -1. cars.c uses this to map
// that city's car texture pages to the palette slots its palettes were stored in.
int GetCarImportCity(void)
{
	return gCarImportCity;
}

// The imported city's car palettes (LUMP_PALLET body), or NULL. `size` receives
// its length.
char* GetCarImportPallet(int* size)
{
	if (size)
		*size = gCarImport.palletSize;

	return gCarImport.pallet;
}

// The imported city's LUMP_TEXTUREINFO (its texture-page lists). Only valid for
// the level's duration, and only when an import is active. texture.c parses it -
// the TP/TEXINF types the layout needs live there.
char* GetCarImportTextureInfo(int* size)
{
	if (size)
		*size = gCarImport.texInfoSize;

	return gCarImport.texInfo;
}

// Where the imported city's permanent page data starts in its level file, or -1.
// The bytes for every entry of that city's page list are concatenated there, each
// entry sector-aligned, which is how LoadPermanentTPages carves them.
int GetCarImportPageBase(void)
{
	if (gCarImportCity < 0)
		return -1;

	return gCarImport.pageBase;
}

// Read `len` bytes at `offset` from the imported city's level file. Returns 1 on
// success. Loadsectors cannot be used for this - it is bound to
// g_CurrentLevelFileName, i.e. the level being played, not the imported city.
int ReadCarImportFile(int offset, void* dst, int len)
{
	char filename[64];
	FILE* fp;

	if (gCarImportCity < 0 || offset < 0 || len <= 0 || dst == NULL)
		return 0;

	sprintf(filename, "%s%s", gDataFolder, LevelFiles[gCarImportCity]);
	fp = fopen(filename, "rb");

	if (fp == NULL)
	{
		sprintf(filename, "%sM%s", gDataFolder, LevelFiles[gCarImportCity]);
		fp = fopen(filename, "rb");
	}

	if (fp == NULL)
		return 0;

	if (fseek(fp, offset, SEEK_SET) != 0 || fread(dst, 1, len, fp) != (size_t)len)
	{
		fclose(fp);
		return 0;
	}

	fclose(fp);

	return 1;
}

// The foreign car-models block to build `slot` from, or NULL to use the level's
// own. The block has the same layout as the level's so only the base pointer
// differs - but it must actually CARRY the model this slot asks for, or the slot
// would be left with no geometry at all (which is a dereference waiting to
// happen, see the gCarCleanModelPtr guard in CreateDentableCar).
char* GetCarImportModels(int slot)
{
	int model;
	int* offsets;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;

	if (GetCarModelSourceCity(slot) != gCarImportCity || gCarImport.carModels == NULL)
		return NULL;

	model = residentCarModels[slot];

	if (model == 13)
	{
		// same derivation the builders use
		model = 10 - (residentCarModels[0] + residentCarModels[1] + residentCarModels[2]);

		if (model < 1)
			model = 1;
		else if (model > 4)
			model = 4;
	}

	if (model < 0 || model > 12)
		return NULL;

	// the offset table must fit inside the block we read
	if (4 + (model + 1) * 3 * (int)sizeof(int) > gCarImport.carModelsSize)
		return NULL;

	offsets = (int*)(gCarImport.carModels + 4 + model * sizeof(int) * 3);

	if (offsets[0] < 0)
		return NULL;	// this city has no such model - keep the level's own

	if (offsets[0] >= gCarImport.carModelsSize)
		return NULL;

	// Damaged and low-detail must be there too. A model with only some variants
	// leaves gCarDamModelPtr/gCarLowModelPtr NULL for that slot, which is the
	// state the CreateDentableCar guard complains about, and the AI's own spawn
	// check (opponent.c) requires all three for exactly this reason.
	if (offsets[1] <= offsets[0] || offsets[1] >= gCarImport.carModelsSize)
		return NULL;

	if (offsets[2] <= offsets[1] || offsets[2] >= gCarImport.carModelsSize)
		return NULL;

	return gCarImport.carModels;
}

// The foreign car colours for `slot`, or NULL. cosmetic.c uses this so an
// imported vehicle wears its own city's paint instead of the host slot's.
char* GetCarImportCosmetics(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;

	if (GetCarModelSourceCity(slot) != gCarImportCity)
		return NULL;

	return gCarImport.cosmetics;
}

int ProcessCarModelLump(char *lump_ptr, int lump_size)
{
	int size;
	int* offsets;
	char* models_offset;
	char* slot_models_offset;
	char* mem;
	MODEL* model;
	int model_number;
	int i;

	int specMemReq;

	specMemReq = 0;

	// (The cross-city source + resident-model choices are resolved by
	// JER_EVENT_CAR_DATA_SOURCE in SetupResidentModels, which runs before this.)

	models_offset = lump_ptr + 4 + 160;	// also skip model count
	offsets = (int*)(lump_ptr + 100);

	// compute special memory requirement for spooling
	for (i = 8; i < 13; i++)
	{
		int cleanOfs = offsets[0];
		int damOfs = offsets[1];
		int lowOfs = offsets[2];

		if (cleanOfs != -1)
		{
			size = ((MODEL*)(models_offset + cleanOfs))->poly_block;

			if (damOfs != -1)
				size += ((MODEL*)(models_offset + damOfs))->normals;

			if (lowOfs != -1)
				size += ((MODEL*)(models_offset + lowOfs))->poly_block;

			size = (size + 2048) + 2048;
			if (size > specMemReq)
				specMemReq = size;

			size = (damOfs - cleanOfs) + 2048;
			if (size > specMemReq)
				specMemReq = size;

			size = (lowOfs - damOfs) + 2048;
			if (size > specMemReq)
				specMemReq = size;

			if (i != 11)	// what the fuck is this hack about?
			{
				// next model offset?
				size = (offsets[3] - lowOfs) + 2048;

				if(size > specMemReq)
					specMemReq = size;
			}
		}

		offsets += 3;
	}

	startBuildNewCars(0);

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		gCarCleanModelPtr[i] = NULL;
		gCarDamModelPtr[i] = NULL;
		gCarLowModelPtr[i] = NULL;

		if (i == SPECIAL_CAR_SLOT)
		{
			startBuildNewCars(1);
			specmallocptr = (char*)mallocptr;
		}

		model_number = residentCarModels[i];

		if (model_number == 13)
		{
			model_number = 10 - (residentCarModels[0] + residentCarModels[1] + residentCarModels[2]);

			if (model_number < 1)
				model_number = 1;
			else if (model_number > 4)
				model_number = 4;
		}

		if (model_number != -1)
		{
			// JERICHO: a slot may take its geometry from another city's level file
			char* src_lump = lump_ptr;
			char* imported = GetCarImportModels(i);

			// Say so when a slot asked for a foreign model that city does not
			// have: the slot silently keeps the level's own car otherwise, which
			// looks like "the model did not load".
			if (imported == NULL && GetCarModelSourceCity(i) >= 0)
				printInfo("cross-city: slot %d asked for model %d from %s, which has no such model - using the level's own\n",
					i, residentCarModels[i], LevelNames[GetCarModelSourceCity(i)]);

			if (imported)
			{
				src_lump = imported;

				printInfo("cross-city: slot %d geometry from %s model %d\n",
					i, LevelNames[GetCarModelSourceCity(i)], residentCarModels[i]);
			}

			slot_models_offset = src_lump + 4 + 160;
			offsets = (int *)(src_lump + 4 + model_number * sizeof(int)*3);

			int cleanOfs = offsets[0];
			int damOfs = offsets[1];
			int lowOfs = offsets[2];

#if USE_PC_FILESYSTEM
			if (gContentOverride && imported == NULL)	// JERICHO: loose .MDL is the host city's
			{
				if (mem = LoadCarModelFromFile(NULL, model_number, CAR_MODEL_CLEAN))
				{
					D_MALLOC_BEGIN();
					model = GetCarModel(mem, (char**)&mallocptr, 1);
					D_MALLOC_END();

					gCarCleanModelPtr[i] = model;
					buildNewCarFromModel(i, 1, mem, model);
					cleanOfs = -1; // skip loading
				}

				if (mem = LoadCarModelFromFile(NULL, model_number, CAR_MODEL_DAMAGED))
				{
					D_MALLOC_BEGIN();
					model = GetCarModel(mem, (char**)&mallocptr, 0);
					D_MALLOC_END();

					gCarDamModelPtr[i] = model;
					damOfs = -1; // skip loading
				}

				if (mem = LoadCarModelFromFile(NULL, model_number, CAR_MODEL_LOWDETAIL))
				{
					D_MALLOC_BEGIN();
					model = GetCarModel(mem, (char**)&mallocptr, 1);
					D_MALLOC_END();

					gCarLowModelPtr[i] = model;
					buildNewCarFromModel(i, 0, mem, model);
					lowOfs = -1; // skip loading
				}
			}
#endif
			
			if (cleanOfs != -1)
			{
				D_MALLOC_BEGIN();
				mem = slot_models_offset + cleanOfs;
				model = GetCarModel(mem, (char**)&mallocptr, 1);

				gCarCleanModelPtr[i] = model;
				buildNewCarFromModel(i, 1, mem, model);

				D_MALLOC_END();
			}

			if (damOfs != -1)
			{
				D_MALLOC_BEGIN();
				mem = slot_models_offset + damOfs;
				model = GetCarModel(mem, (char**)&mallocptr, 0);

				gCarDamModelPtr[i] = model;
				D_MALLOC_END();
			}
			
			if (lowOfs != -1)
			{
				D_MALLOC_BEGIN();
				mem = slot_models_offset + lowOfs;
				model = GetCarModel(mem, (char**)&mallocptr, 1);

				gCarLowModelPtr[i] = model;
				buildNewCarFromModel(i, 0, mem, model);
				D_MALLOC_END();
			}
		}
	}

	D_MALLOC_BEGIN();

#if USE_PC_FILESYSTEM
	if (gContentOverride)
	{
		// extra spool memory needed
		specMemReq += 4096;
	}
#endif

	mallocptr = specmallocptr + specMemReq;
	specLoadBuffer = specmallocptr + specMemReq - 2048;
	D_MALLOC_END();

	return 0;
}

// [D] [T]
MODEL* FindModelPtrWithName(char *name)
{
	int idx;
	idx = FindModelIdxWithName(name);

	return idx >= 0 ? modelpointers[idx] : NULL;
}

// [D] [T]
int FindModelIdxWithName(char *name)
{
	char *str;
	int i;

	i = 0;
	str = modelname_buffer;

	while (i < num_models_in_pack)
	{
		if (!strcmp(str, name))
			return i;

		while (*str++) {} // go to next string

		i++;
	}

	return -1;
}
