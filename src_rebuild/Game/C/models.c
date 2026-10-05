#include "driver2.h"
#include "models.h"
#include "system.h"
#include "spool.h"
#include "mission.h"
#include "cars.h"
#include "cosmetic.h"
#include "texture.h"	// JERICHO: CarImportResetState (cross-city per-level reset)
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
// MODS/carhacks/docs/CROSS_CITY.md for the file format.

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

// ONE import per city, not one per level: a set can name more than one city (a
// session's agreed set, a late joiner), and the geometry/cosmetics getters below
// resolve the city from the SLOT, so two cities' cars can be built into one level.
// The PALETTE side is still single-city -- the CLUT column is the limit, see
// CROSS_CITY.md "The budget" -- so the single-city getters keep answering with the
// FIRST held city until that is lifted.
static CAR_IMPORT gCarImports[CITY_COUNT];	// CHICAGO/HAVANA/VEGAS/RIO; .region == NULL = not held
static int gCarImportCity = -1;		// the first held city, -1 = none (single-city getters)

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

	if (city < 0 || city >= CITY_COUNT)
		return 0;

	// the full single-player level first, then the arena variant
	sprintf(filename, "%s%s", GetCityDataRoot(city), LevelFiles[city]);
	fp = fopen(filename, "rb");

	// the arena variant of the file; Driver 1's car-data cities have none
	if (fp == NULL && city < CITY_D2_COUNT)
	{
		sprintf(filename, "%sM%s", GetCityDataRoot(city), LevelFiles[city]);
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
	sprintf(filename, "%s%s", GetCityDataRoot(city), CosmeticFiles[city]);
	imp->cosmetics = ReadWholeFile(filename, &imp->cosmeticsSize);

	return 1;
}

// Load the foreign car data the module asked for. Called from
// SetupResidentModels once the resident models and their sources are final, so
// it runs before both the geometry (ProcessCarModelLump) and the colours
// (ProcessCosmeticsLump) are taken from it. A no-op for a stock level.
void InitCarImport(void)
{
	int i, city;

	// JERICHO: a new level starts from a clean cross-city slate. This runs BEFORE the
	// level's car models are built, so the per-slot set lists buildNewCarFromModel
	// fills start empty (and last level's pins/ownership do not leak). See
	// CarImportResetState in texture.c.
	CarImportResetState();

	for (city = 0; city < CITY_COUNT; city++)
		FreeCarImport(&gCarImports[city]);

	gCarImportCity = -1;

	// Load EVERY city the set names, not just the first: a slot's geometry comes
	// from ITS city (GetCarImportModels resolves it per slot), so a set naming two
	// cities builds both. The FIRST one loaded is what the single-city palette
	// getters answer with - see the declaration above.
	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		int src = GetCarModelSourceCity(i);

		if (src < 0 || src >= 4 || residentCarModels[i] == -1)
			continue;

		if (gCarImports[src].region != NULL)
			continue;			// already held from an earlier slot

		if (!LoadCarImport(src, &gCarImports[src]))
		{
			printInfo("cross-city: no usable car data in %s - slots naming it keep the level's own vehicles\n",
				LevelFiles[src]);
			continue;
		}

		printInfo("cross-city: car data from %s (%d bytes of models, %d of car palettes, %d of cosmetics)\n",
			LevelNames[src], gCarImports[src].carModelsSize, gCarImports[src].palletSize,
			gCarImports[src].cosmeticsSize);

		if (gCarImportCity < 0)
			gCarImportCity = src;
	}
}

// JERICHO: the MID-LEVEL variant, for a car added to the resident set after the level
// loaded (the hot load). It differs from InitCarImport() in the one way that matters:
// it never disturbs what is already there. InitCarImport is a level-start function --
// it runs CarImportResetState() (which empties the live per-slot set lists
// buildNewCarFromModel fills, i.e. the palette/page bookkeeping of the cars already
// on the road) and frees EVERY city's buffers, including the ones the running level's
// built models point into. Called mid-level that is corruption: the symptom is
// broken car palettes. So: no reset, no frees, and the cities already held are left
// exactly as they are -- only a city nobody has read yet is loaded.
void InitCarImportMidLevel(void)
{
	int i, city;

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		int src = GetCarModelSourceCity(i);

		if (src < 0 || src >= 4 || residentCarModels[i] == -1)
			continue;

		if (gCarImports[src].region != NULL)
			continue;			// already held: leave it alone

		if (!LoadCarImport(src, &gCarImports[src]))
		{
			printInfo("cross-city: no usable car data in %s - slots naming it keep the level's own vehicles\n",
				LevelFiles[src]);
			continue;
		}

		printInfo("cross-city: car data from %s read MID-LEVEL for the hot load (%d bytes of models, %d of car palettes, %d of cosmetics)\n",
			LevelNames[src], gCarImports[src].carModelsSize, gCarImports[src].palletSize,
			gCarImports[src].cosmeticsSize);

		// ...and its PAGE LISTS, which the level-load parse only did for the cities held
		// then. Without this the pin walk cannot find this city's sets ("not in its page
		// list - skipped"), their baked index is never filled, and the car draws whatever
		// that page holds -- while its palette block is right, which is exactly the
		// "the palette is correct but the texture still seems wrong" report.
		CarImportPageListsForCity(src);

		if (gCarImportCity < 0)
			gCarImportCity = src;
	}

	(void)city;
}

// The city the level is importing vehicles from, or -1. cars.c uses this to map
// that city's car texture pages to the palette slots its palettes were stored in.
int GetCarImportCity(void)
{
	return gCarImportCity;
}

// Is `city`'s car data loaded for this level? The gate the per-city array offers
// to a caller that has a city (or a slot's city) in hand.
int CarImportCityHeld(int city)
{
	return (city >= 0 && city < CITY_COUNT && gCarImports[city].region != NULL);
}

// The imported city's car palettes (LUMP_PALLET body), or NULL. `size` receives
// its length.
char* GetCarImportPallet(int* size)
{
	return GetCarImportPalletForCity(gCarImportCity, size);
}

// The same for a NAMED city, so a caller with a slot can ask about THAT slot's
// city instead of the level's first one.
char* GetCarImportPalletForCity(int city, int* size)
{
	CAR_IMPORT* imp = CarImportCityHeld(city) ? &gCarImports[city] : NULL;

	if (size)
		*size = imp ? imp->palletSize : 0;

	return imp ? imp->pallet : NULL;
}

// The imported city's LUMP_TEXTUREINFO (its texture-page lists). Only valid for
// the level's duration, and only when an import is active. texture.c parses it -
// the TP/TEXINF types the layout needs live there.
char* GetCarImportTextureInfo(int* size)
{
	return GetCarImportTextureInfoForCity(gCarImportCity, size);
}

char* GetCarImportTextureInfoForCity(int city, int* size)
{
	CAR_IMPORT* imp = CarImportCityHeld(city) ? &gCarImports[city] : NULL;

	if (size)
		*size = imp ? imp->texInfoSize : 0;

	return imp ? imp->texInfo : NULL;
}

// Where the imported city's permanent page data starts in its level file, or -1.
// The bytes for every entry of that city's page list are concatenated there, each
// entry sector-aligned, which is how LoadPermanentTPages carves them.
int GetCarImportPageBase(void)
{
	return GetCarImportPageBaseForCity(gCarImportCity);
}

int GetCarImportPageBaseForCity(int city)
{
	if (!CarImportCityHeld(city))
		return -1;

	return gCarImports[city].pageBase;
}

// Read `len` bytes at `offset` from the imported city's level file. Returns 1 on
// success. Loadsectors cannot be used for this - it is bound to
// g_CurrentLevelFileName, i.e. the level being played, not the imported city.
int ReadCarImportFile(int offset, void* dst, int len)
{
	return ReadCarImportFileForCity(gCarImportCity, offset, dst, len);
}

int ReadCarImportFileForCity(int city, int offset, void* dst, int len)
{
	char filename[64];
	FILE* fp;

	if (!CarImportCityHeld(city) || offset < 0 || len <= 0 || dst == NULL)
		return 0;

	sprintf(filename, "%s%s", GetCityDataRoot(city), LevelFiles[city]);
	fp = fopen(filename, "rb");

	// the arena variant of the file; Driver 1's car-data cities have none
	if (fp == NULL && city < CITY_D2_COUNT)
	{
		sprintf(filename, "%sM%s", GetCityDataRoot(city), LevelFiles[city]);
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
	int city;
	CAR_IMPORT* imp;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;

	// THIS slot's city, not the level's one guest city: two cities can be held now.
	city = GetCarModelSourceCity(slot);

	if (city < 0 || city >= CITY_COUNT)
		return NULL;

	imp = &gCarImports[city];

	if (imp->carModels == NULL)
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
	if (4 + (model + 1) * 3 * (int)sizeof(int) > imp->carModelsSize)
		return NULL;

	offsets = (int*)(imp->carModels + 4 + model * sizeof(int) * 3);

	if (offsets[0] < 0)
		return NULL;	// this city has no such model - keep the level's own

	if (offsets[0] >= imp->carModelsSize)
		return NULL;

	// Damaged and low-detail must be there too. A model with only some variants
	// leaves gCarDamModelPtr/gCarLowModelPtr NULL for that slot, which is the
	// state the CreateDentableCar guard complains about, and the AI's own spawn
	// check (opponent.c) requires all three for exactly this reason.
	if (offsets[1] <= offsets[0] || offsets[1] >= imp->carModelsSize)
		return NULL;

	if (offsets[2] <= offsets[1] || offsets[2] >= imp->carModelsSize)
		return NULL;

	return imp->carModels;
}

// The foreign car colours for `slot`, or NULL. cosmetic.c uses this so an
// imported vehicle wears its own city's paint instead of the host slot's.
char* GetCarImportCosmetics(int slot)
{
	int city;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;

	city = GetCarModelSourceCity(slot);

	if (city < 0 || city >= CITY_COUNT)
		return NULL;

	return gCarImports[city].cosmetics;
}

// JERICHO cross-city HOT LOAD. A level builds its resident car models from
// malloctab (rewound per level, main.c), so a car a module adds to the resident set
// AFTER the level has loaded has no way to be built there: the level's heap is
// live with its own allocations. JerHotLoadCarModel builds one such slot from its
// IMPORT data into this pool instead, which lives until the next level load
// (ProcessCarModelLump resets it). See models.h.
#define JER_HOT_CAR_POOL_BYTES	(512 * 1024)

// One car's worth of pool. Deliberately far above the level loader's specMemReq
// figure for the same model: that figure is the SPOOL requirement (the largest single
// pass over the model), NOT what three GetCarModel builds consume one after another.
// Reserving the spool figure is what made two hot-loaded cars overlap in the pool, and
// the symptom is a car with polys missing (no wheels, no shadow) and garbage texture
// ids -- while the very same car built at level load, out of the level's ample heap,
// looks right. The builds are checked against this block after the fact, so a car that
// somehow needs more is refused instead of corrupting its neighbour.
#define JER_HOT_CAR_BLOCK_BYTES	(64 * 1024)

// JERICHO: the pool is dealt out in BLOCKS, one per slot, and a block is given BACK when a
// car goes away (JerReleaseCarGeometry). A bump cursor could not do that, so a slot that was
// hot-loaded once consumed its space for the rest of the level - fine while nothing ever
// released a slot, wrong now that a peer leaving (or the local player leaving the session)
// must hand its resources back. JER_HOT_CAR_BLOCKS blocks fit the pool by construction.
#define JER_HOT_CAR_BLOCKS		(JER_HOT_CAR_POOL_BYTES / JER_HOT_CAR_BLOCK_BYTES)

static char* gJerHotCarPool;
static int   gJerHotCarSize;
static int   gJerHotCarUsed;					// blocks in use, for the log line
static int   gJerHotCarBlockOf[MAX_CAR_RESIDENT_MODELS];	// slot -> block, -1 = none

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

	// JERICHO: a level load rebuilds every resident model from malloctab, so any car this
	// level HOT-LOADED is superseded here; its blocks are free to reuse.
	{
		int b;

		gJerHotCarUsed = 0;

		for (b = 0; b < MAX_CAR_RESIDENT_MODELS; b++)
			gJerHotCarBlockOf[b] = -1;
	}

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

// JERICHO cross-city hot load: build resident `slot`'s models from its IMPORT data,
// at RUNTIME. Returns the bytes used (0 = nothing built, with the reason logged).
//
// This is what mp needs when a joiner picks a car this machine's running level does
// not hold: carhacks adds it to the set, InitCarImport() reads that city, and this
// builds the geometry -- after which the ordinary resolvers (GetCarModelSourceCity /
// residentCarModels) find the slot, so a peer stops being drawn as the level's own
// car of the same number. CarImportPin uploads the palette rows when the car is
// first drawn, exactly as for any imported model.
//
// REFUSES rather than half-building: a car whose geometry, normals and low-detail
// pass do not fit the pool is left exactly as it was, because a partially built
// model is worse than a substitute car.
int JerHotLoadCarModel(int slot)
{
	char* src_lump;
	char* slot_models_offset;
	char* cursor;
	char* mem;
	int* offsets;
	int model_number, cleanOfs, damOfs, lowOfs, size, need;
	MODEL* model;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	if (gCarCleanModelPtr[slot] != NULL)
		return 0;			// already built

	if (GetCarModelSourceCity(slot) < 0)
		return 0;			// the level's own car: nothing to bring in

	src_lump = GetCarImportModels(slot);

	if (src_lump == NULL)
		return 0;			// that city's data is not read in

	model_number = residentCarModels[slot];

	if (model_number < 0 || model_number > 12)
		return 0;

	slot_models_offset = src_lump + 4 + 160;
	offsets = (int *)(src_lump + 4 + model_number * sizeof(int) * 3);

	cleanOfs = offsets[0];
	damOfs = offsets[1];
	lowOfs = offsets[2];

	// What this model needs, the same way the level loader reserves it
	// (ProcessCarModelLump's specMemReq pass): all three models have to lie inside
	// one allocation.
	need = 0;

	if (cleanOfs != -1)
	{
		size = ((MODEL*)(slot_models_offset + cleanOfs))->poly_block;

		if (damOfs != -1)
			size += ((MODEL*)(slot_models_offset + damOfs))->normals;

		if (lowOfs != -1)
			size += ((MODEL*)(slot_models_offset + lowOfs))->poly_block;

		need = (size + 2048) + 2048;
	}

	if (need <= 0)
	{
		printInfo("cross-city: %s model %d has no geometry to build for slot %d\n",
			LevelNames[GetCarModelSourceCity(slot)], model_number, slot);
		return 0;
	}

	if (gJerHotCarPool == NULL)
	{
		int b;

		gJerHotCarPool = (char*)malloc(JER_HOT_CAR_POOL_BYTES);
		gJerHotCarSize = (gJerHotCarPool != NULL) ? JER_HOT_CAR_POOL_BYTES : 0;

		for (b = 0; b < MAX_CAR_RESIDENT_MODELS; b++)
			gJerHotCarBlockOf[b] = -1;
	}

	{
		int b, block = -1;

		for (b = 0; b < JER_HOT_CAR_BLOCKS; b++)
		{
			int k, taken = 0;

			for (k = 0; k < MAX_CAR_RESIDENT_MODELS; k++)
			{
				if (gJerHotCarBlockOf[k] == b)
				{
					taken = 1;
					break;
				}
			}

			if (!taken)
			{
				block = b;
				break;
			}
		}

		if (block < 0)
		{
			printInfo("cross-city: no free hot-load block for %s model %d (%d of %d in use) - slot %d keeps the car it has\n",
				LevelNames[GetCarModelSourceCity(slot)], model_number, gJerHotCarUsed, JER_HOT_CAR_BLOCKS, slot);
			return 0;
		}

		gJerHotCarBlockOf[slot] = block;
		gJerHotCarUsed++;

		cursor = gJerHotCarPool + (block * JER_HOT_CAR_BLOCK_BYTES);
	}

	if (cleanOfs != -1)
	{
		mem = slot_models_offset + cleanOfs;
		model = GetCarModel(mem, (char**)&cursor, 1);

		gCarCleanModelPtr[slot] = model;
		buildNewCarFromModel(slot, 1, mem, model);
	}

	if (damOfs != -1)
	{
		mem = slot_models_offset + damOfs;
		model = GetCarModel(mem, (char**)&cursor, 0);

		gCarDamModelPtr[slot] = model;
	}

	if (lowOfs != -1)
	{
		mem = slot_models_offset + lowOfs;
		model = GetCarModel(mem, (char**)&cursor, 1);

		gCarLowModelPtr[slot] = model;
		buildNewCarFromModel(slot, 0, mem, model);
	}

	/* The builds must have stayed inside the slot's block: two cars' models must never
	 * overlap. If one did, unbuild the slot (the substitute car, which looks right)
	 * rather than draw corrupted geometry -- and hand the block back. */
	if (cursor > gJerHotCarPool + ((gJerHotCarBlockOf[slot] + 1) * JER_HOT_CAR_BLOCK_BYTES))
	{
		printInfo("cross-city: %s model %d took more than its %d-byte hot-load block - slot %d is left unbuilt\n",
			LevelNames[GetCarModelSourceCity(slot)], model_number,
			JER_HOT_CAR_BLOCK_BYTES, slot);

		gCarCleanModelPtr[slot] = NULL;
		gCarDamModelPtr[slot] = NULL;
		gCarLowModelPtr[slot] = NULL;

		gJerHotCarBlockOf[slot] = -1;
		gJerHotCarUsed--;

		return 0;
	}

	/* the manifest's geometry side: the block this slot now owns in the hot-load pool */
	CarSlotResNoteGeometry(slot, JER_HOT_CAR_BLOCK_BYTES);

	printInfo("cross-city: hot-loaded %s model %d into resident slot %d (%d bytes budgeted, block %d, %d of %d blocks used)\n",
		LevelNames[GetCarModelSourceCity(slot)], model_number, slot, need,
		gJerHotCarBlockOf[slot], gJerHotCarUsed, JER_HOT_CAR_BLOCKS);

	return need;
}

// JERICHO cross-city unload: give back the geometry a hot-loaded slot holds, so the slot (and
// its block of the pool) can be used again. The model pointers stop pointing into the pool --
// a caller that still has a car on this slot must rebuild it first (mp does, through its swap
// path) -- and the block returns to the free list.
//
// A slot built at level load has no block: its geometry belongs to the level's own heap and
// stays there until the level ends. But if it holds a CROSS-CITY model (an import made at level
// load - the frontend pick, the host's agreed set) its pointers are cleared too: otherwise the
// slot still looks built, JerHotLoadCarModel's "already built" early-out skips the next car
// imported into it, and that car is drawn with the released car's body. A level's OWN car
// (source city -1) is never touched.
int JerReleaseCarGeometry(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	if (gJerHotCarBlockOf[slot] < 0)
	{
		if (GetCarModelSourceCity(slot) < 0 || gCarCleanModelPtr[slot] == NULL)
			return 0;		// the level's own car, or never built: not ours

		gCarCleanModelPtr[slot] = NULL;
		gCarDamModelPtr[slot] = NULL;
		gCarLowModelPtr[slot] = NULL;

		printInfo("cross-city: slot %d's level-built import let go (its bytes stay in the level heap until the level ends); a later car can be hot-loaded into it\n",
			slot);

		return 1;
	}

	gCarCleanModelPtr[slot] = NULL;
	gCarDamModelPtr[slot] = NULL;
	gCarLowModelPtr[slot] = NULL;

	gJerHotCarBlockOf[slot] = -1;
	gJerHotCarUsed--;

	return 1;
}

// JERICHO cross-city unload: give back the imported cities' data AND the hot-load pool. Called
// when there is no map any more (the frontend, after leaving a session), where nothing should
// be holding a pointer into an import buffer or into the pool. A level load does the same work
// through InitCarImport, so this is the "no level will come along to clean up" case.
int JerReleaseCarImport(void)
{
	int city, n = 0, b;

	for (city = 0; city < CITY_COUNT; city++)
	{
		if (gCarImports[city].region != NULL)
		{
			FreeCarImport(&gCarImports[city]);
			n++;
		}
	}

	gCarImportCity = -1;

	gJerHotCarUsed = 0;

	for (b = 0; b < MAX_CAR_RESIDENT_MODELS; b++)
		gJerHotCarBlockOf[b] = -1;

	if (n > 0)
		printInfo("cross-city: released %d imported city buffer(s) and the hot-load pool\n", n);

	return n;
}

// [D] [T]
MODEL* FindModelPtrWithName(char *name){	int idx;
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
