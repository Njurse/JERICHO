#include "driver2.h"
#include "models.h"
#include "system.h"
#include "spool.h"
#include "mission.h"
#include "cars.h"
#include "cosmetic.h"
#include "gamesnd.h"	// JERICHO: imported cars get their own engine/idle sounds
#include "texture.h"	// JERICHO: CarImportResetState (cross-city per-level reset)
#include "pedest.h"	// JERICHO: pmJerichoModels, the imported on-foot character
#include "motion_c.h"	// JERICHO: StoreVertexLists after a ped model import
#include "jericho.h"	// JERICHO-HOOK: mod runtime (inert without modules)
#include "jer_events.h"	// JERICHO-HOOK: event argument structs
#include "../ASM/compres.h"	// decomp_asm: the wheel page image, for its CLUT row

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
#define CAR_IMPORT_LUMP_WHEELS	200	// JERICHO: optional wheel models (the D1 bake)
#define CAR_IMPORT_LUMP_SOUNDS	201	// JERICHO: embedded per-model car sounds (the D1 bake)
#define CAR_IMPORT_LUMP_SIRENS	202	// JERICHO: per-model cop flags (the D1 bake)
#define CAR_IMPORT_LUMP_MODELS_D2	1	// LUMP_MODELS - the level's model table (DATA2)
#define CAR_IMPORT_LUMP_TEXTURENAMES	5	// LUMP_TEXTURENAMES (DATA1)
#define CAR_IMPORT_LUMP_MODELNAMES	12	// LUMP_MODELNAMES (DATA1)
#define CAR_IMPORT_MODEL_SLOTS	13	// LUMP_CAR_MODELS is a fixed 13-entry table

extern int PolySizes[56];		// draw.c: the poly-stream walk

typedef struct
{
	char* region;		// malloc'd DATA1 copy the car-models block points into
	char* carModels;	// LUMP_CAR_MODELS body, or NULL
	int carModelsSize;
	unsigned carModelsPresent;	// bit N = model N really holds a car in this file
	char* pallet;		// LUMP_PALLET body (car palettes), or NULL
	int palletSize;
	char* cosmetics;	// the city's .LCF (car colours), or NULL
	int cosmeticsSize;
	char* texInfo;		// LUMP_TEXTUREINFO body (page lists), or NULL
	int texInfoSize;
	char* carWheels;	// LUMP_CAR_WHEELS body, or NULL (no wheel segment)
	int carWheelsSize;
	char* carSounds;	// LUMP_CAR_SOUNDS body, or NULL (no embedded car sounds)
	int carSoundsSize;
	unsigned carSirenMask;	// bit N = model N is a cop car (LUMP_CAR_SIRENS)
	char* wheelMem;		// malloc'd copies of the wheel MODELs below
	MODEL* wheels[CAR_IMPORT_WHEEL_VARIANTS];
	char* wheelTexMem[CAR_IMPORT_WHEEL_VARIANTS];	// per-model D2 texture-source blocks
	unsigned char wheelSet[CAR_IMPORT_WHEEL_VARIANTS][8];	// source-city set numbers
	int wheelSetCount[CAR_IMPORT_WHEEL_VARIANTS];		// ...before CarSetRemap
	char* data2;		// malloc'd DATA2 (D2 levels: the model table lives here)
	int data2Size;
	char* models;		// LUMP_MODELS body in data2
	int modelsSize;
	char* modelNames;	// LUMP_MODELNAMES body in region
	int modelNamesSize;
	char* textureNames;	// LUMP_TEXTURENAMES body in region
	int textureNamesSize;
	JER_WHEEL_TEX wheelTex[CAR_IMPORT_WHEEL_VARIANTS];	// D2 TEXINF wheel sprites
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

	if (imp->wheelMem)
		free(imp->wheelMem);

	if (imp->data2)
		free(imp->data2);

	for (int w = 0; w < CAR_IMPORT_WHEEL_VARIANTS; w++)
	{
		if (imp->wheelTexMem[w])
			free(imp->wheelTexMem[w]);
	}

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
/* JERICHO: read car data out of an ARBITRARY file - the loader behind the
 * load-lev hook. Everything past the path resolution - the lump header, the
 * citylump table, DATA1, the car models, the palettes, the page lists - is
 * independent of which disc the file came from, so this takes paths rather than a
 * city index. LoadCarImport below is the stock wrapper that turns a city into its
 * two paths and calls this.
 *
 * `lcfPath` may be NULL: the import then has no colours of its own.
 * Returns 1 on success; on failure anything it allocated is already freed. */
int JerLoadCarImportFromFile(const char* levPath, const char* lcfPath, CAR_IMPORT* imp)
{
	unsigned int table[8];
	FILE* fp;
	long data1Off, data1Size;

	memset(imp, 0, sizeof(*imp));

	if (levPath == NULL)
		return 0;

	fp = fopen(levPath, "rb");

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

	/* JERICHO: a real Driver 2 level keeps its model table and its wheel models
	 * in DATA2, and the model NAMES in DATA1. An imported D2 car should wear its
	 * own city's wheel texture, so read both halves when present. A Driver 1 bake
	 * has no DATA2; it carries LUMP_CAR_WHEELS instead. */
	{
		unsigned int data2Off = table[4];
		unsigned int data2Size = table[5];

		if (data2Off > 0 && data2Size > 8 && data2Size <= 4 * 1024 * 1024)
		{
			imp->data2 = (char*)malloc(data2Size);

			if (imp->data2 != NULL &&
				fseek(fp, data2Off, SEEK_SET) == 0 &&
				fread(imp->data2, 1, data2Size, fp) == (size_t)data2Size)
			{
				imp->data2Size = (int)data2Size;
			}
			else
			{
				free(imp->data2);
				imp->data2 = NULL;
				imp->data2Size = 0;
			}
		}
	}

	fclose(fp);

	// DATA1's body is itself a container lump, so its segments start 8 bytes in
	FindLumpSegment(imp->region + 8, (int)data1Size - 8, CAR_IMPORT_LUMP_MODELS, &imp->carModels, &imp->carModelsSize);

	// JERICHO: WHICH models does this file actually carry?
	//
	// Asked here, at the one place the table is read, so a module can offer exactly
	// the cars that were imported instead of being handed a list to hardcode. That
	// matters most for a Driver 1 city, whose cars were transplanted onto Driver 2's
	// compliant models (tools/bake.py) - the module has no way to know which numbers
	// those are, and must not assume a layout.
	//
	// The table is a fixed 13 entries of three int32 (clean/damaged/low offsets)
	// starting 4 bytes in - the same walk GetCarImportModels does.
	imp->carModelsPresent = 0;
	if (imp->carModels != NULL)
	{
		int* offs = (int*)(imp->carModels + 4);
		int m;

		for (m = 0; m < CAR_IMPORT_MODEL_SLOTS; m++)
		{
			int clean = offs[m * 3];

			if (clean != -1 && clean < imp->carModelsSize)
				imp->carModelsPresent |= 1u << m;
		}
	}

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
	FindLumpSegment(imp->region + 8, (int)data1Size - 8, CAR_IMPORT_LUMP_TEXTURENAMES, &imp->textureNames, &imp->textureNamesSize);

	// JERICHO: optional wheel models. A Driver 1 bake writes them beside the car
	// models so an imported D1 car can wear D1 wheels; a real Driver 2 level has
	// no such segment and keeps the host level's shared wheel models.
	FindLumpSegment(imp->region + 8, (int)data1Size - 8, CAR_IMPORT_LUMP_WHEELS, &imp->carWheels, &imp->carWheelsSize);

	// JERICHO: the synthetic Driver 1 LEVs embed a per-model car-sound table in
	// their own segment. Optional: D2 levels and older D1 bakes simply have none,
	// and gamesnd keeps the host-city fallback for them.
	FindLumpSegment(imp->region + 8, (int)data1Size - 8, CAR_IMPORT_LUMP_SOUNDS, &imp->carSounds, &imp->carSoundsSize);

	// JERICHO: cop flags from the bake. Optional; an older bank has none, and
	// CarHasSiren then keeps the host-city behaviour.
	{
		char* sirens = NULL;
		int sirenSize = 0;
		int s;

		imp->carSirenMask = 0;
		FindLumpSegment(imp->region + 8, (int)data1Size - 8, CAR_IMPORT_LUMP_SIRENS, &sirens, &sirenSize);

		for (s = 0; sirens != NULL && s < CAR_IMPORT_MODEL_SLOTS && s < sirenSize; s++)
		{
			if ((unsigned char)sirens[s])
				imp->carSirenMask |= 1u << s;
		}
	}

	// JERICHO: a Driver 1 bake (no DATA2) from before v0.10.9 carries neither
	// segment, so its cars keep the host's wheels and engine. Say so once.
	if (imp->data2 == NULL && (imp->carWheels == NULL || imp->carSounds == NULL))
	{
		static int staleD1Warned = 0;

		printWarning("carimport: %s is an old Driver 1 bake (no %s%s%s) - re-run BAKE_D1_CARS for D1 wheels and sounds\n",
			levPath, imp->carWheels == NULL ? "wheels" : "",
			imp->carWheels == NULL && imp->carSounds == NULL ? ", " : "",
			imp->carSounds == NULL ? "sounds" : "");

		if (!staleD1Warned)
		{
			staleD1Warned = 1;
			jer_error("JERICHO/MODS/carhacks/D1CARS is out of date: re-run BAKE_D1_CARS for Driver 1 wheels and sounds");
		}
	}

	// JERICHO: a real Driver 2 level's named wheel models. DATA2 holds
	// LUMP_MODELS (the model table) and DATA1 holds LUMP_MODELNAMES; the names are
	// indexed by the same slot number the engine's FindModelIdxWithName uses.
	FindLumpSegment(imp->region + 8, (int)data1Size - 8, CAR_IMPORT_LUMP_MODELNAMES, &imp->modelNames, &imp->modelNamesSize);

	if (imp->data2 != NULL)
		FindLumpSegment(imp->data2 + 8, imp->data2Size - 8, CAR_IMPORT_LUMP_MODELS_D2, &imp->models, &imp->modelsSize);

	// the car colours, beside the car data as LEVELS\<city>.LCF - or none at all
	if (lcfPath != NULL)
		imp->cosmetics = ReadWholeFile(lcfPath, &imp->cosmeticsSize);

	return 1;
}

/* The stock path: a city index -> the two paths that city's data lives at, then
 * the loader above. Unchanged in behaviour for Driver 2's four cities.
 *
 * JERICHO: the paths come from JerBuildCityPath, which is the ONE place a city's
 * file is resolved (data root + table entry + FS_FixPathSlashes) and which
 * refuses an unregistered row - a bank slot with no bank in it has NULL table
 * entries, and the old `sprintf("%s%s", root, NULL)` would have crashed there. It
 * also carries the slash normalisation this function used to do by hand: every
 * path table here is Windows-style and the rest of the engine runs such a path
 * through FS_FixPathSlashes, so without it the opens only ever matched on Windows
 * (on Linux '\\' is not a separator, every import failed, and each imported slot
 * silently kept the level's own model). */
static int LoadCarImport(int city, CAR_IMPORT* imp)
{
	char level[192], cosmetic[192];
	const char* lcfPath = NULL;

	if (!JerCityRegistered(city))
		return 0;

	// the full single-player level first, then the arena variant
	if (!JerCityPath(city, level, sizeof(level), JER_CITY_FILE_LEVEL, 0))
		return 0;

	// the arena variant of the file; Driver 1's car-data cities and the runtime
	// banks have none.
	//
	// Probe with a direct fopen, NOT FileExists(): `level` is already rooted
	// (JerBuildCityPath prepends "DRIVER2\" or "DRIVER\"), and FileExists() prefixes
	// gDataFolder AGAIN - so it tested "DRIVER2\DRIVER2\LEVELS\VEGAS.LEV", always missing,
	// and the loader fell through to MLEVELS every time. pageBase was then computed from
	// the M file while ReadCarImportFileForCity read the single-player one; the two DATA1
	// sizes differ, so every imported set read short and landed inside DATA1 - the
	// "set N looks corrupt (289676305 clut rows)" rot. The read path fopens this same
	// rooted string, so the probe must too.
	{
		FILE* probe = fopen(level, "rb");

		if (probe)
			fclose(probe);
		else if (city < CITY_D2_COUNT)
			JerCityPath(city, level, sizeof(level), JER_CITY_FILE_LEVEL, 1);
	}

	// A bank may have no colours file at all, which is legal: the cars then keep the
	// cosmetics derived from the model. JerBuildCityPath answers 0 for that, and the
	// loader takes NULL rather than trying to read a file that is not named.
	if (JerCityPath(city, cosmetic, sizeof(cosmetic), JER_CITY_FILE_COSMETIC, 0))
		lcfPath = cosmetic;

	return JerLoadCarImportFromFile(level, lcfPath, imp);
}

// JERICHO: load a city's car data NOW, into its own import slot, and answer whether
// it is held.
//
// The module needs this at FRONTEND time. The car-select list is built before the
// level's car data is read, so a Driver 1 city whose import is not held yet offers
// NOTHING - the menu reads "car list: ... MIAMI 0/10 (0 the level already has)" for
// every transplanted city and there is no car to choose. Asking here makes the
// answer available when the question is actually asked.
//
// A no-op when the city is already held, so calling it every time the list is
// rebuilt costs one pointer test.
int JerLoadCarImportForCity(int city)
{
	if (city < 0 || city >= CITY_COUNT)
		return 0;

	if (gCarImports[city].region != NULL)
		return 1;

	return LoadCarImport(city, &gCarImports[city]);
}

/* ---------------------------------------------------------------------------
 * JERICHO: on-foot character import
 *
 * The six JERI_* models are ordinary named models in Old Havana's LUMP_MODELS,
 * and the city's model table/names are already held by the SAME CAR_IMPORT the
 * car importer reads (LoadCarImport finds LUMP_MODELS in DATA2 and
 * LUMP_MODELNAMES in DATA1). So the ped import does not need a second file
 * reader: it finds the six names in that table and copies the model blobs into
 * a block this file owns.
 *
 * This block MUST NOT point into the held city buffer: chkImportReleaseAll and
 * InitCarImport free it, and pmJerichoModels would then be dangling. The copy
 * relocates every pointer into the copy, exactly as ProcessMDSLump does for the
 * level's own models.
 * ------------------------------------------------------------------------- */
#define JER_PED_IMPORT_MODELS	6
#define JER_PED_LEG_MODELS	6
#define JER_PED_HAND_MODELS	2
#define JER_PED_BAKED_MODELS	(JER_PED_IMPORT_MODELS + JER_PED_LEG_MODELS + JER_PED_HAND_MODELS)

static const char* const sJerPedModelNames[JER_PED_IMPORT_MODELS] =
{
	"JERI_TORSO", "JERI_HEAD", "JERI_U_ARM_LEFT",
	"JERI_L_ARM_LEFT", "JERI_U_ARM_RIGHT", "JERI_L_ARM_RIGHT"
};

static int sJerPedModelIndex[JER_PED_IMPORT_MODELS];
static int sJerPedProbeCity = -1;
static int sJerPedProbeOk;

static char* sJerPedMem;		/* level-lifetime copy of the six model blobs */
static int sJerPedMemSize;
static MODEL* sJerPedModels[JER_PED_IMPORT_MODELS];
static int sJerPedEntryCount;
static char sJerPedEntryNames[JER_PED_MAX_ENTRIES][64];
static int sJerPedReady;
static int sJerPedBuiltCity = -1;
static int sJerPedSet[64];		/* SOURCE-city sets, before CarSetRemap */
static int sJerPedSetCount;

static int JerPedFindModelName(CAR_IMPORT* imp, const char* name)
{
	char* p;
	char* end;
	int n = 0;

	if (imp == NULL || imp->modelNames == NULL || imp->modelNamesSize <= 0)
		return -1;

	p = imp->modelNames;
	end = p + imp->modelNamesSize;

	while (p < end)
	{
		if (strcmp(p, name) == 0)
			return n;

		/* The engine's own FindModelIdxWithName counts EMPTY name slots as
		 * entries (its walk is `while (*str++)`), and Havana's table has gaps.
		 * Stopping at the first empty string hid every JERI model. */
		while (p < end && *p != 0)
			p++;

		if (p < end)
			p++;

		n++;
	}

	return -1;
}

/* One entry of LUMP_MODELS: [int size][MODEL data]. */
static int JerPedModelBlob(CAR_IMPORT* imp, int index, MODEL** out, int* outSize)
{
	char* cur;
	int count, i;

	if (imp == NULL || imp->models == NULL || imp->modelsSize < 4 || index < 0)
		return 0;

	count = *(int*)imp->models;
	cur = imp->models + 4;

	for (i = 0; i < count; i++)
	{
		int size;

		if (cur + 4 > imp->models + imp->modelsSize)
			return 0;

		size = *(int*)cur;

		if (size < 0 || cur + 4 + size > imp->models + imp->modelsSize)
			return 0;

		if (size == 0)
		{
			/* An empty model slot. The table has gaps; they still count as model
			 * indices, so skip the header and keep walking. */
			cur += 4;
			continue;
		}

		if (i == index)
		{
			*out = (MODEL*)(cur + 4);
			*outSize = size;
			return 1;
		}

		cur += 4 + size;
	}

	return 0;
}

/* The textured poly types, walked exactly as PolySizes expects. */
static int JerPedIsTexturedPoly(int type)
{
	return type == 4 || type == 5 || type == 6 || type == 7 ||
		type == 20 || type == 21 || type == 22 || type == 23;
}

static void JerPedCollectModelSets(MODEL* m)
{
	char* p;
	int i;

	if (m == NULL || m->poly_block == 0 || m->num_polys <= 0)
		return;

	p = (char*)m + m->poly_block;

	for (i = 0; i < m->num_polys; i++)
	{
		int type = (u_char)*p & 0x1f;
		int step = (type >= 0 && type < 56) ? PolySizes[type] : 0;

		if (step <= 0)
			return;

		if (JerPedIsTexturedPoly(type))
		{
			int set = (u_char)p[1];

			if (set != 0 && set != 255)
			{
				int k, seen = 0;

				for (k = 0; k < sJerPedSetCount; k++)
				{
					if (sJerPedSet[k] == set)
					{
						seen = 1;
						break;
					}
				}

				if (!seen && sJerPedSetCount < (int)(sizeof(sJerPedSet) / sizeof(sJerPedSet[0])))
					sJerPedSet[sJerPedSetCount++] = set;
			}
		}

		p += step;
	}
}

/* Rewrite the copied model's texture-set bytes through the SAME remap the car
 * and wheel imports use. A host-owned set number gets a free destination index
 * and the pin fills that index; leaving the source number would sample the
 * host's page. */
static void JerPedRemapModelSets(MODEL* m)
{
	char* p;
	int i;

	if (m == NULL || m->poly_block == 0 || m->num_polys <= 0)
		return;

	p = (char*)m + m->poly_block;

	CarSetRemapEnable(1);

	for (i = 0; i < m->num_polys; i++)
	{
		int type = (u_char)*p & 0x1f;
		int step = (type >= 0 && type < 56) ? PolySizes[type] : 0;

		if (step <= 0)
			break;

		if (JerPedIsTexturedPoly(type))
			p[1] = (char)(u_char)CarSetRemap((u_char)p[1]);

		p += step;
	}

	CarSetRemapEnable(0);
}

/* Preflight: is the city's model table held, and are all six Jericho models in
 * it with usable geometry? No VRAM is touched here, so the frontend can ask. */
int JerPedImportProbe(int city)
{
	CAR_IMPORT* imp;
	int i;

	if (city < 0 || city >= CITY_COUNT)
		return 0;

	if (sJerPedProbeCity == city)
		return sJerPedProbeOk;

	sJerPedProbeCity = city;
	sJerPedProbeOk = 0;

	if (!JerLoadCarImportForCity(city))
	{
		printInfo("ped import: %s data is not available\n", LevelNames[city]);
		return 0;
	}

	imp = &gCarImports[city];

	{
		char path[192];

		path[0] = 0;
		JerCityPath(city, path, sizeof(path), JER_CITY_FILE_LEVEL, 0);

		printInfo("ped import: %s file '%s' names %d models %d first '%s'\n",
			LevelNames[city], path, imp->modelNamesSize, imp->modelsSize,
			(imp->modelNames != NULL) ? imp->modelNames : "");
	}

	if (imp->models == NULL || imp->modelNames == NULL || imp->modelsSize < 4)
	{
		printInfo("ped import: %s has no LUMP_MODELS/model-name table\n", LevelNames[city]);
		return 0;
	}

	JerPedPageListsReport(city);

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
	{
		MODEL* src = NULL;
		int size = 0;

		sJerPedModelIndex[i] = JerPedFindModelName(imp, sJerPedModelNames[i]);

		if (sJerPedModelIndex[i] < 0 ||
			!JerPedModelBlob(imp, sJerPedModelIndex[i], &src, &size) ||
			src == NULL || src->poly_block <= 0 || src->num_polys <= 0)
		{
			printInfo("ped import: %s model %s is missing or unusable (idx %d size %d"
				" instance %d poly %d polys %d verts %d)\n",
				LevelNames[city], sJerPedModelNames[i], sJerPedModelIndex[i], size,
				(src != NULL) ? (int)src->instance_number : -1,
				(src != NULL) ? src->poly_block : -1,
				(src != NULL) ? (int)src->num_polys : -1,
				(src != NULL) ? (int)src->num_vertices : -1);
			return 0;
		}
	}

	sJerPedProbeOk = 1;

	printInfo("ped import: %s holds all six JERI models (first index %d)\n",
		LevelNames[city], sJerPedModelIndex[0]);

	return 1;
}

/* Copy the six model blobs out of the held city buffer into a block this file
 * owns. Every offset is relocated into the copy; a model that is an INSTANCE of
 * a parent outside the six is refused, because its vertices would otherwise
 * point into a buffer the car-import teardown can free. */
int JerPedImportBuild(int city)
{
	CAR_IMPORT* imp;
	MODEL* src[JER_PED_IMPORT_MODELS];
	MODEL* built[JER_PED_IMPORT_MODELS];
	int size[JER_PED_IMPORT_MODELS];
	int total = 0;
	int i, k;
	char* mem;
	char* cur;

	if (sJerPedReady && sJerPedBuiltCity == city)
		return 1;

	if (!JerPedImportProbe(city))
		return 0;

	imp = &gCarImports[city];

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
	{
		if (!JerPedModelBlob(imp, sJerPedModelIndex[i], &src[i], &size[i]) ||
			src[i] == NULL || size[i] < (int)sizeof(MODEL))
		{
			printInfo("ped import: could not locate %s in %s\n",
				sJerPedModelNames[i], LevelNames[city]);
			return 0;
		}

		total += (size[i] + 3) & ~3;
	}

	mem = (char*)malloc(total);

	if (mem == NULL)
	{
		printInfo("ped import: no memory for %d bytes of %s ped models\n", total, LevelNames[city]);
		return 0;
	}

	cur = mem;

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
	{
		MODEL* m;

		built[i] = (MODEL*)cur;
		memcpy(cur, src[i], size[i]);
		cur += (size[i] + 3) & ~3;

		m = built[i];

		/* On PC the offsets stay MODEL-relative (MODEL_RELOCATE_POINTERS is 0);
		 * on PSX they are made absolute. Keep both layouts correct so the copy is
		 * self-contained on either build. */
#if MODEL_RELOCATE_POINTERS
		m->vertices += (int)m;
		m->normals += (int)m;
		m->point_normals += (int)m;
		m->poly_block += (int)m;

		if (m->collision_block != 0)
			m->collision_block += (int)m;
#endif
	}

	/* A DAM-style instance (instance_number names another model) is made
	 * self-contained by pointing its vertex/normal fields at the COPY of its
	 * parent. The parent must be one of the six; anything else would leave an
	 * address in the city buffer that teardown frees. */
	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
	{
		MODEL* m = built[i];
		int parentIndex = m->instance_number;

		if (parentIndex < 0)
			continue;

		for (k = 0; k < JER_PED_IMPORT_MODELS; k++)
		{
			if (sJerPedModelIndex[k] == parentIndex)
				break;
		}

		if (k >= JER_PED_IMPORT_MODELS)
		{
			printInfo("ped import: %s is an instance of model %d, which is not one of the six - refusing\n",
				sJerPedModelNames[i], parentIndex);
			free(mem);
			return 0;
		}

#if MODEL_RELOCATE_POINTERS
		m->vertices = (int)((char*)built[k] + built[k]->vertices) - (int)m;
		m->normals = (int)((char*)built[k] + built[k]->normals) - (int)m;
		m->point_normals = (int)((char*)built[k] + built[k]->point_normals) - (int)m;

		if (built[k]->collision_block != 0)
			m->collision_block = (int)((char*)built[k] + built[k]->collision_block) - (int)m;
		else
			m->collision_block = 0;
#else
		m->vertices = (int)((char*)built[k] - (char*)m) + built[k]->vertices;
		m->normals = (int)((char*)built[k] - (char*)m) + built[k]->normals;
		m->point_normals = (int)((char*)built[k] - (char*)m) + built[k]->point_normals;

		if (built[k]->collision_block != 0)
			m->collision_block = (int)((char*)built[k] - (char*)m) + built[k]->collision_block;
		else
			m->collision_block = 0;
#endif

		m->instance_number = -1;
	}

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
	{
		if (built[i]->poly_block <= 0 || built[i]->num_polys <= 0)
		{
			printInfo("ped import: %s has no usable polygon data after the copy\n",
				sJerPedModelNames[i]);
			free(mem);
			return 0;
		}
	}

	/* The texture sets the six models actually paint with. Record the SOURCE
	 * numbers first, then rewrite the copies through the host-vs-import remap. */
	sJerPedSetCount = 0;

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
		JerPedCollectModelSets(built[i]);

	if (sJerPedSetCount <= 0)
	{
		printInfo("ped import: the JERI models name no texture sets - refusing\n");
		free(mem);
		return 0;
	}

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
		JerPedRemapModelSets(built[i]);

	{
		char line[160];
		int n = 0;

		line[0] = 0;

		for (i = 0; i < sJerPedSetCount && n < (int)sizeof(line) - 8; i++)
			n += snprintf(line + n, sizeof(line) - n, " %d", sJerPedSet[i]);

		printInfo("ped import: %s JERI texture sets:%s\n", LevelNames[city], line);
	}

	if (!JerPedPinTpages(city, sJerPedSet, sJerPedSetCount))
	{
		printInfo("ped import: %s pages could not be pinned - refusing the import\n",
			LevelNames[city]);
		JerPedReleasePins();
		sJerPedSetCount = 0;
		free(mem);
		return 0;
	}

	/* All six copies and all their pages are ready. Publish the pointers; the
	 * caller decides when to install them (StoreVertexLists + SetSkel...). */
	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
		pmJerichoModels[0][i] = built[i];

	if (sJerPedMem != NULL)
		free(sJerPedMem);

	sJerPedMem = mem;
	sJerPedMemSize = total;

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
		sJerPedModels[i] = built[i];

	sJerPedBuiltCity = city;
	sJerPedReady = 1;
	sJerPedEntryCount = 1;
	strncpy(sJerPedEntryNames[0], "Jericho", sizeof(sJerPedEntryNames[0]) - 1);
	sJerPedEntryNames[0][sizeof(sJerPedEntryNames[0]) - 1] = 0;

	printInfo("ped import: copied %d bytes of %s JERI models (first %p)\n",
		total, LevelNames[city], (void*)sJerPedModels[0]);

	return 1;
}

int JerPedBakedAvailable(void)
{
	FILE* f;
	char magic[8];

	f = fopen("DRIVER2\\LEVELS\\JMPPED.LEV", "rb");

	if (f == NULL)
		return 0;

	if (fread(magic, 1, 8, f) != 8)
	{
		fclose(f);
		return 0;
	}

	fclose(f);

	return memcmp(magic, "JMPPED\0\0", 8) == 0;
}

/* Load the self-contained bake. The file stores twelve MODEL-relative blobs and
 * three texture pages (two raw area pages and one compressed permanent page). */
typedef struct JER_PED_V5_PAGE
{
	int set;
	int npal;
	int clutLen;
	int imageLen;
	int flags;
	const unsigned char* clut;
	const unsigned char* image;
} JER_PED_V5_PAGE;

/* v5 archive: entry table, each entry carrying its own 14 model blobs and page
 * records. The models are copied into one block; page pointers live in `file`
 * until every page has been uploaded. */
static int JerPedImportBakedV5(unsigned char* file, long len, int city)
{
	unsigned char* p = file + 16;
	unsigned char* end = file + len;
	const unsigned char* pp;
	const unsigned char* pageEnd;
	unsigned char* blobs[JER_PED_MAX_ENTRIES][JER_PED_BAKED_MODELS];
	int sizes[JER_PED_MAX_ENTRIES][JER_PED_BAKED_MODELS];
	char names[JER_PED_MAX_ENTRIES][64];
	JER_PED_V5_PAGE pages[JER_PED_MAX_ENTRIES * 16];
	MODEL* built[JER_PED_MAX_ENTRIES][JER_PED_BAKED_MODELS];
	int entryCount, pageTotal = 0, total = 0;
	int e, slot, i, k;
	char* mem = NULL;
	char* cur;
	unsigned int version, count;

	version = p[-8] | p[-7] << 8 | p[-6] << 16 | (unsigned)p[-5] << 24;
	count = p[-4] | p[-3] << 8 | p[-2] << 16 | (unsigned)p[-1] << 24;

	if (version != 5)
	{
		printInfo("ped import: v5 parser got version %u\n", version);
		goto fail;
	}

	entryCount = (int)count;

	if (entryCount < 1 || entryCount > JER_PED_MAX_ENTRIES)
	{
		printInfo("ped import: v5 archive has %d entries (max %d)\n",
			entryCount, JER_PED_MAX_ENTRIES);
		goto fail;
	}

	for (e = 0; e < entryCount; e++)
	{
		unsigned int nameLen, modelCount, pageCount;

		pp = p;
		if (pp + 4 > end) goto fail;
		nameLen = pp[0] | pp[1] << 8 | pp[2] << 16 | (unsigned)pp[3] << 24;
		pp += 4;

		if (nameLen == 0 || nameLen >= sizeof(names[e]) || pp + nameLen > end)
			goto fail;
		memcpy(names[e], pp, nameLen);
		names[e][nameLen] = 0;
		pp += nameLen;

		if (pp + 4 > end) goto fail;
		modelCount = pp[0] | pp[1] << 8 | pp[2] << 16 | (unsigned)pp[3] << 24;
		pp += 4;
		if (modelCount != JER_PED_BAKED_MODELS)
			goto fail;

		for (slot = 0; slot < JER_PED_BAKED_MODELS; slot++)
		{
			unsigned int size;

			if (pp + 4 > end) goto fail;
			size = pp[0] | pp[1] << 8 | pp[2] << 16 | (unsigned)pp[3] << 24;
			pp += 4;
			if (size < sizeof(MODEL) || pp + size > end)
				goto fail;

			blobs[e][slot] = (unsigned char*)pp;
			sizes[e][slot] = (int)size;
			pp += size;
			total += (size + 3) & ~3;
		}

		if (pp + 4 > end) goto fail;
		pageCount = pp[0] | pp[1] << 8 | pp[2] << 16 | (unsigned)pp[3] << 24;
		pp += 4;
		if (pageCount > 16 || pageTotal + (int)pageCount > (int)(sizeof(pages) / sizeof(pages[0])))
			goto fail;

		for (i = 0; i < (int)pageCount; i++)
		{
			unsigned int set, npal, clutLen, imageLen, flags;

			if (pp + 20 > end) goto fail;
			set = pp[0] | pp[1] << 8 | pp[2] << 16 | (unsigned)pp[3] << 24; pp += 4;
			npal = pp[0] | pp[1] << 8 | pp[2] << 16 | (unsigned)pp[3] << 24; pp += 4;
			clutLen = pp[0] | pp[1] << 8 | pp[2] << 16 | (unsigned)pp[3] << 24; pp += 4;
			imageLen = pp[0] | pp[1] << 8 | pp[2] << 16 | (unsigned)pp[3] << 24; pp += 4;
			flags = pp[0] | pp[1] << 8 | pp[2] << 16 | (unsigned)pp[3] << 24; pp += 4;
			pageEnd = pp + clutLen + imageLen;
			if (pageEnd > end) goto fail;
			pages[pageTotal].set = (int)set;
			pages[pageTotal].npal = (int)npal;
			pages[pageTotal].clutLen = (int)clutLen;
			pages[pageTotal].imageLen = (int)imageLen;
			pages[pageTotal].flags = (int)flags;
			pages[pageTotal].clut = pp;
			pages[pageTotal].image = pp + clutLen;
			pageTotal++;
			pp = pageEnd;
		}

		p = (unsigned char*)pp;
	}

	if (p != end)
		goto fail;

	mem = (char*)malloc(total);
	if (mem == NULL)
		goto fail;

	cur = mem;

	for (e = 0; e < entryCount; e++)
	{
		for (slot = 0; slot < JER_PED_BAKED_MODELS; slot++)
		{
			MODEL* m;

			built[e][slot] = (MODEL*)cur;
			memcpy(cur, blobs[e][slot], sizes[e][slot]);
			cur += (sizes[e][slot] + 3) & ~3;
			m = built[e][slot];

#if MODEL_RELOCATE_POINTERS
			m->vertices += (int)m;
			m->normals += (int)m;
			m->point_normals += (int)m;
			m->poly_block += (int)m;

			if (m->collision_block != 0)
				m->collision_block += (int)m;
#endif
		}
	}

	for (e = 0; e < entryCount; e++)
	{
		for (slot = 0; slot < JER_PED_BAKED_MODELS; slot++)
		{
			MODEL* m = built[e][slot];
			int parent = m->instance_number;

			if (parent < 0)
				continue;

			if (parent >= JER_PED_BAKED_MODELS)
			{
				printInfo("ped import: entry %d model %d names parent %d outside the bake\n",
					e, slot, parent);
				goto fail;
			}

#if MODEL_RELOCATE_POINTERS
			m->vertices = (int)((char*)built[e][parent] + built[e][parent]->vertices) - (int)m;
			m->normals = (int)((char*)built[e][parent] + built[e][parent]->normals) - (int)m;
			m->point_normals = (int)((char*)built[e][parent] + built[e][parent]->point_normals) - (int)m;

			if (built[e][parent]->collision_block != 0)
				m->collision_block = (int)((char*)built[e][parent] + built[e][parent]->collision_block) - (int)m;
			else
				m->collision_block = 0;
#else
			m->vertices = (int)((char*)built[e][parent] - (char*)m) + built[e][parent]->vertices;
			m->normals = (int)((char*)built[e][parent] - (char*)m) + built[e][parent]->normals;
			m->point_normals = (int)((char*)built[e][parent] - (char*)m) + built[e][parent]->point_normals;

			if (built[e][parent]->collision_block != 0)
				m->collision_block = (int)((char*)built[e][parent] - (char*)m) + built[e][parent]->collision_block;
			else
				m->collision_block = 0;
#endif

			m->instance_number = -1;
		}
	}

	for (e = 0; e < entryCount; e++)
	{
		for (slot = 0; slot < JER_PED_BAKED_MODELS; slot++)
		{
			if (built[e][slot]->poly_block <= 0 || built[e][slot]->num_polys <= 0)
			{
				printInfo("ped import: entry %d model %d has no polygon data\n", e, slot);
				goto fail;
			}
		}
	}

	sJerPedSetCount = 0;

	for (e = 0; e < entryCount; e++)
		for (slot = 0; slot < JER_PED_BAKED_MODELS; slot++)
			JerPedCollectModelSets(built[e][slot]);

	if (sJerPedSetCount <= 0)
		goto fail;

	for (e = 0; e < entryCount; e++)
		for (slot = 0; slot < JER_PED_BAKED_MODELS; slot++)
			JerPedRemapModelSets(built[e][slot]);

	for (i = 0; i < pageTotal; i++)
	{
		int dst = CarImportPinDstSet(pages[i].set);

		if (!JerPedUploadBakedPage(pages[i].set, dst, pages[i].npal,
			pages[i].clut, pages[i].image, pages[i].flags & 1))
		{
			printInfo("ped import: v5 page set %d could not be uploaded\n", pages[i].set);
			JerPedReleasePins();
			sJerPedSetCount = 0;
			goto fail;
		}
	}

	for (e = 0; e < entryCount; e++)
	{
		for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
			pmJerichoModels[e][i] = built[e][i];

		for (i = 0; i < JER_PED_LEG_MODELS; i++)
			pmJerichoLegModels[e][i] = built[e][JER_PED_IMPORT_MODELS + i];

		for (i = 0; i < JER_PED_HAND_MODELS; i++)
			pmJerichoHandModels[e][i] = built[e][JER_PED_IMPORT_MODELS + JER_PED_LEG_MODELS + i];
	}

	if (sJerPedMem != NULL)
		free(sJerPedMem);

	sJerPedMem = mem;
	sJerPedMemSize = total;

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
		sJerPedModels[i] = built[0][i];

	sJerPedEntryCount = entryCount;
	for (e = 0; e < entryCount; e++)
	{
		strncpy(sJerPedEntryNames[e], names[e], sizeof(sJerPedEntryNames[e]) - 1);
		sJerPedEntryNames[e][sizeof(sJerPedEntryNames[e]) - 1] = 0;
	}

	sJerPedBuiltCity = city;
	sJerPedReady = 1;

	printInfo("ped import: baked JMPPED.LEV v5 loaded (%d entries, %d models, %d pages) for %s\n",
		entryCount, entryCount * JER_PED_BAKED_MODELS, pageTotal,
		(city >= 0 && city < CITY_COUNT) ? LevelNames[city] : "any level");

	free(file);
	return 1;

fail:
	if (mem != NULL)
		free(mem);
	free(file);
	return 0;
}

int JerPedEntryCount(void)
{
	return sJerPedEntryCount;
}

const char* JerPedEntryName(int entry)
{
	if (entry < 0 || entry >= sJerPedEntryCount)
		return "";
	return sJerPedEntryNames[entry];
}

MODEL* JerPedModelForEntry(int entry, int slot)
{
	if (entry < 0 || entry >= sJerPedEntryCount || slot < 0 || slot >= JER_PED_BAKED_MODELS)
		return NULL;

	if (slot < JER_PED_IMPORT_MODELS)
		return pmJerichoModels[entry][slot];
	if (slot < JER_PED_IMPORT_MODELS + JER_PED_LEG_MODELS)
		return pmJerichoLegModels[entry][slot - JER_PED_IMPORT_MODELS];
	return pmJerichoHandModels[entry][slot - JER_PED_IMPORT_MODELS - JER_PED_LEG_MODELS];
}

int JerPedImportBaked(int city)
{
	FILE* f;
	unsigned char* file;
	unsigned char* p;
	unsigned char* end;
	unsigned char* blobs[JER_PED_BAKED_MODELS];
	int sizes[JER_PED_BAKED_MODELS];
	MODEL* built[JER_PED_BAKED_MODELS];
	unsigned int version, count, pageCount;
	int total = 0;
	int i, k;
	long len;
	char* mem;
	char* cur;

	if (sJerPedReady)
		return 1;

	f = fopen("DRIVER2\\LEVELS\\JMPPED.LEV", "rb");

	if (f == NULL)
	{
		printInfo("ped import: JMPPED.LEV is missing (bake with tools/bake_jmpped.py)\n");
		return 0;
	}

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (len < 24)
	{
		fclose(f);
		return 0;
	}

	file = (unsigned char*)malloc(len);

	if (file == NULL || fread(file, 1, len, f) != (size_t)len)
	{
		fclose(f);
		free(file);
		return 0;
	}

	fclose(f);
	end = file + len;

	if (memcmp(file, "JMPPED\0\0", 8) != 0)
	{
		printInfo("ped import: JMPPED.LEV has a bad magic\n");
		free(file);
		return 0;
	}

	p = file + 8;
	version = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; p += 4;
	count = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; p += 4;

	if (version == 5)
		return JerPedImportBakedV5(file, len, city);

	if (version != 3 || count != JER_PED_BAKED_MODELS || p + 4 > end)
	{
		printInfo("ped import: JMPPED.LEV version %u has %u models\n", version, count);
		free(file);
		return 0;
	}

	for (i = 0; i < JER_PED_BAKED_MODELS; i++)
	{
		unsigned int size;

		if (p + 4 > end)
			goto fail;

		size = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; p += 4;

		if (size < sizeof(MODEL) || p + size > end)
		{
			printInfo("ped import: JMPPED.LEV model %d is short (%u)\n", i, size);
			goto fail;
		}

		blobs[i] = p;
		sizes[i] = (int)size;
		p += size;
		total += (size + 3) & ~3;
	}

	if (p + 4 > end)
		goto fail;

	pageCount = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; p += 4;

	if (pageCount < 2)
		goto fail;

	mem = (char*)malloc(total);

	if (mem == NULL)
		goto fail;

	cur = mem;

	for (i = 0; i < JER_PED_BAKED_MODELS; i++)
	{
		MODEL* m;

		built[i] = (MODEL*)cur;
		memcpy(cur, blobs[i], sizes[i]);
		cur += (sizes[i] + 3) & ~3;
		m = built[i];

#if MODEL_RELOCATE_POINTERS
		m->vertices += (int)m;
		m->normals += (int)m;
		m->point_normals += (int)m;
		m->poly_block += (int)m;

		if (m->collision_block != 0)
			m->collision_block += (int)m;
#endif
	}

	for (i = 0; i < JER_PED_BAKED_MODELS; i++)
	{
		MODEL* m = built[i];
		int parent = m->instance_number;

		if (parent < 0)
			continue;

		if (parent >= JER_PED_BAKED_MODELS)
		{
			printInfo("ped import: baked model %d names parent %d outside the bake\n",
				i, parent);
			free(mem);
			goto fail;
		}

#if MODEL_RELOCATE_POINTERS
		m->vertices = (int)((char*)built[parent] + built[parent]->vertices) - (int)m;
		m->normals = (int)((char*)built[parent] + built[parent]->normals) - (int)m;
		m->point_normals = (int)((char*)built[parent] + built[parent]->point_normals) - (int)m;

		if (built[parent]->collision_block != 0)
			m->collision_block = (int)((char*)built[parent] + built[parent]->collision_block) - (int)m;
		else
			m->collision_block = 0;
#else
		m->vertices = (int)((char*)built[parent] - (char*)m) + built[parent]->vertices;
		m->normals = (int)((char*)built[parent] - (char*)m) + built[parent]->normals;
		m->point_normals = (int)((char*)built[parent] - (char*)m) + built[parent]->point_normals;

		if (built[parent]->collision_block != 0)
			m->collision_block = (int)((char*)built[parent] - (char*)m) + built[parent]->collision_block;
		else
			m->collision_block = 0;
#endif

		m->instance_number = -1;
	}

	for (i = 0; i < JER_PED_BAKED_MODELS; i++)
	{
		if (i < JER_PED_IMPORT_MODELS)
			sJerPedModelIndex[i] = i;

		if (built[i]->poly_block <= 0 || built[i]->num_polys <= 0)
		{
			printInfo("ped import: baked model %d has no polygon data\n", i);
			free(mem);
			goto fail;
		}
	}

	/* The legs are retextured to the torso's suit set/id at bake time, so they
	 * follow the same remap and pinned page as the torso. */

	sJerPedSetCount = 0;

	for (i = 0; i < JER_PED_BAKED_MODELS; i++)
		JerPedCollectModelSets(built[i]);

	if (sJerPedSetCount <= 0)
	{
		free(mem);
		goto fail;
	}

	for (i = 0; i < JER_PED_BAKED_MODELS; i++)
		JerPedRemapModelSets(built[i]);

	for (i = 0; i < (int)pageCount; i++)
	{
		unsigned int set, npal, clutLen, imageLen, flags;
		const unsigned char* cluts;
		const unsigned char* image;
		int dst;

		if (p + 20 > end)
			break;

		set = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; p += 4;
		npal = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; p += 4;
		clutLen = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; p += 4;
		imageLen = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; p += 4;
		flags = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; p += 4;

		if (p + clutLen + imageLen > end)
			break;

		cluts = p;
		image = p + clutLen;
		dst = CarImportPinDstSet((int)set);

		if (!JerPedUploadBakedPage((int)set, dst, (int)npal, cluts, image, (int)(flags & 1)))
		{
			free(mem);
			JerPedReleasePins();
			goto fail;
		}

		p += clutLen + imageLen;
	}

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
		pmJerichoModels[0][i] = built[i];

	for (i = 0; i < JER_PED_LEG_MODELS; i++)
		pmJerichoLegModels[0][i] = built[JER_PED_IMPORT_MODELS + i];

	for (i = 0; i < JER_PED_HAND_MODELS; i++)
		pmJerichoHandModels[0][i] = built[JER_PED_IMPORT_MODELS + JER_PED_LEG_MODELS + i];

	if (sJerPedMem != NULL)
		free(sJerPedMem);

	sJerPedMem = mem;
	sJerPedMemSize = total;

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
		sJerPedModels[i] = built[i];

	sJerPedBuiltCity = city;
	sJerPedReady = 1;
	sJerPedEntryCount = 1;
	strncpy(sJerPedEntryNames[0], "Jericho", sizeof(sJerPedEntryNames[0]) - 1);
	sJerPedEntryNames[0][sizeof(sJerPedEntryNames[0]) - 1] = 0;

	printInfo("ped import: baked JMPPED.LEV loaded (%d models, %u pages) for %s\n",
		JER_PED_BAKED_MODELS, pageCount, (city >= 0 && city < CITY_COUNT) ? LevelNames[city] : "any level");

	free(file);
	return 1;

fail:
	free(file);
	return 0;
}

MODEL* JerPedModel(int index)
{
	if (index < 0 || index >= JER_PED_IMPORT_MODELS)
		return NULL;

	return sJerPedModels[index];
}

int JerPedImportReady(void)
{
	return sJerPedReady;
}

int JerPedTextureSetCount(void)
{
	return sJerPedSetCount;
}

int JerPedTextureSet(int index)
{
	if (index < 0 || index >= sJerPedSetCount)
		return 0;

	return sJerPedSet[index];
}

/* The JERI texture sets are not in Havana's permanent or special page lists.
 * They are streamed AREA pages: each area has a list of page numbers and its
 * page data starts at gfx_offset, 17 sectors per page. Read the page from the
 * full Havana level file and upload it into the lower-half pool. */
int JerPedAreaPage(int city, int set, int dstSet)
{
	CAR_IMPORT* imp;
	char* spool = NULL;
	int spoolSize = 0;
	char* ptr;
	char* areas;
	char* pages;
	char path[192];
	FILE* fp;
	char* buf;
	int musicSize, nareas, a, i;
	int sector = -1;
	int spoolBase;

	if (city < 0 || city >= CITY_COUNT)
		return 0;

	imp = &gCarImports[city];
	spoolBase = citylumps[city][CITYLUMP_SPOOL].x;

	if (imp->data2 == NULL)
	{
		printInfo("ped import: %s has no DATA2 for area pages\n", LevelNames[city]);
		return 0;
	}

	printInfo("ped import: %s DATA2 %d bytes for area-page lookup\n",
		LevelNames[city], imp->data2Size);

	if (!FindLumpSegment(imp->data2 + 8, imp->data2Size - 8, 26, &spool, &spoolSize) ||
		spool == NULL || spoolSize < 8)
	{
		printInfo("ped import: %s has no LUMP_SPOOLINFO for area pages\n", LevelNames[city]);
		return 0;
	}

	printInfo("ped import: %s spool info %d bytes\n", LevelNames[city], spoolSize);

	ptr = spool + 4;
	musicSize = *(int*)ptr;
	ptr += 4 + musicSize;

	if (ptr + 4 > spool + spoolSize)
		return 0;

	nareas = *(int*)ptr;
	ptr += 4;

	if (nareas <= 0 || ptr + nareas * 16 > spool + spoolSize)
	{
		printInfo("ped import: %s spool area count %d is out of range\n",
			LevelNames[city], nareas);
		return 0;
	}

	areas = ptr;
	ptr += nareas * 16;

	if (ptr + nareas * 16 > spool + spoolSize)
	{
		printInfo("ped import: %s spool area page table is short\n", LevelNames[city]);
		return 0;
	}

	pages = ptr;

	for (a = 0; a < nareas && sector < 0; a++)
	{
		unsigned short gfx = *(unsigned short*)(areas + a * 16);
		unsigned char ntpages = *(unsigned char*)(areas + a * 16 + 10);

		for (i = 0; i < ntpages; i++)
		{
			if ((unsigned char)pages[a * 16 + i] == set)
			{
				sector = (int)gfx + i * 17;
				break;
			}
		}
	}

	if (sector < 0)
	{
		printInfo("ped import: %s area lists have no page %d (areas %d)\n",
			LevelNames[city], set, nareas);
		return 0;
	}

	if (!JerCityPath(city, path, sizeof(path), JER_CITY_FILE_LEVEL, 0))
		return 0;

	fp = fopen(path, "rb");

	if (fp == NULL)
	{
		printInfo("ped import: cannot open %s for area page %d\n", path, set);
		return 0;
	}

	buf = (char*)malloc(17 * CDSECTOR_SIZE);

	if (buf == NULL)
	{
		fclose(fp);
		return 0;
	}

	if (fseek(fp, (long)spoolBase + (long)sector * CDSECTOR_SIZE, SEEK_SET) != 0 ||
		fread(buf, 1, 17 * CDSECTOR_SIZE, fp) != 17 * CDSECTOR_SIZE)
	{
		printInfo("ped import: cannot read area page %d at sector %d of %s (spool base %d)\n",
			set, sector, path, spoolBase);
		fclose(fp);
		free(buf);
		return 0;
	}

	fclose(fp);

	if (!JerPedUploadAreaPage(set, dstSet, buf))
	{
		printInfo("ped import: %s set %d area page upload failed (sector %d)\n",
			LevelNames[city], set, sector);
		free(buf);
		return 0;
	}

	free(buf);

	printInfo("ped import: %s set %d read from area page sector %d\n",
		LevelNames[city], set, sector);

	return 1;
}

/* A level boundary or session teardown. The imported pointers are cleared only
 * when they are OUR copies; a level that owns the models natively (Havana) keeps
 * its pointers until InitTanner replaces them. */
void JerPedImportReset(void)
{
	int i, e;

	JerPedReleasePins();
	JerPedSkeletonInvalidate();

	if (sJerPedMem != NULL)
	{
		for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
		{
			if (pmJerichoModels[0][i] == sJerPedModels[i])
				pmJerichoModels[0][i] = NULL;
		}

		free(sJerPedMem);
	}

	sJerPedMem = NULL;
	sJerPedMemSize = 0;
	sJerPedReady = 0;
	sJerPedBuiltCity = -1;
	sJerPedSetCount = 0;
	sJerPedProbeCity = -1;
	sJerPedProbeOk = 0;
	sJerPedEntryCount = 0;

	for (e = 0; e < JER_PED_MAX_ENTRIES; e++)
	{
		sJerPedEntryNames[e][0] = 0;

		if (e == 0)
			continue;

		for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
			pmJerichoModels[e][i] = NULL;

		for (i = 0; i < JER_PED_LEG_MODELS; i++)
			pmJerichoLegModels[e][i] = NULL;

		for (i = 0; i < JER_PED_HAND_MODELS; i++)
			pmJerichoHandModels[e][i] = NULL;
	}

	for (i = 0; i < JER_PED_IMPORT_MODELS; i++)
		sJerPedModels[i] = NULL;

	for (i = 0; i < JER_PED_LEG_MODELS; i++)
		pmJerichoLegModels[0][i] = NULL;

	for (i = 0; i < JER_PED_HAND_MODELS; i++)
		pmJerichoHandModels[0][i] = NULL;
}



// Load the foreign car data the module asked for. Called from
// SetupResidentModels once the resident models and their sources are final, so
// it runs before both the geometry (ProcessCarModelLump) and the colours
// (ProcessCosmeticsLump) are taken from it. A no-op for a stock level.
void InitCarImport(void)
{
	int i, city;
	int wanted = 0, all_held = 1;

	// JERICHO: a RESTART of the same level does not re-read the level file, so
	// ProcessCarModelLump does not run again -- and that is what records the page
	// pins the deferred palette upload rides on. Clearing the import state here
	// therefore drops the palette rows with nothing left to put them back: the
	// car keeps its geometry but loses its colours. If every source the restart
	// asked for is already held, leave the level's import alone. A genuinely
	// different source set (or a fresh level) falls through to the full reset.
	if (!NewLevel && gCarImportCity >= 0)
	{
		for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		{
			int src = GetCarModelSourceCity(i);

			if (src < 0 || src >= CITY_COUNT || residentCarModels[i] == -1)
				continue;

			wanted = 1;

			if (!CarImportCityHeld(src))
			{
				all_held = 0;
				break;
			}
		}

		if (wanted && all_held)
		{
			printInfo("cross-city: restart of the same level - keeping the held car import and its palettes\n");
			return;
		}
	}

	// JERICHO: a new level starts from a clean cross-city slate. This runs BEFORE the
	// level's car models are built, so the per-slot set lists buildNewCarFromModel
	// fills start empty (and last level's pins/ownership do not leak). See
	// CarImportResetState in texture.c.
	CarImportResetState();

	// JERICHO: and the runtime city banks are re-scanned first, so a set that names a
	// bank installed (or uninstalled) since boot is resolved against what is on disk
	// NOW. Dropping a bank here is safe: this is the one moment nothing points into
	// an import yet - the frees below are about to empty every city anyway.
	JerCityRegistryRefresh();

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

		if (src < 0 || src >= CITY_COUNT || residentCarModels[i] == -1)
			continue;

		if (gCarImports[src].region != NULL)
			continue;			// already held from an earlier slot

		if (!LoadCarImport(src, &gCarImports[src]))
		{
			printInfo("cross-city: no usable car data in %s - slots naming it keep the level's own vehicles\n",
				JerCityFile(src, JER_CITY_FILE_LEVEL));
			continue;
		}

		printInfo("cross-city: car data from %s (%d bytes of models, %d of car palettes, %d of cosmetics)\n",
			JerCityName(src), gCarImports[src].carModelsSize, gCarImports[src].palletSize,
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

		if (src < 0 || src >= CITY_COUNT || residentCarModels[i] == -1)
			continue;

		if (gCarImports[src].region != NULL)
			continue;			// already held: leave it alone

		if (!LoadCarImport(src, &gCarImports[src]))
		{
			printInfo("cross-city: no usable car data in %s - slots naming it keep the level's own vehicles\n",
				JerCityFile(src, JER_CITY_FILE_LEVEL));
			continue;
		}

		printInfo("cross-city: car data from %s read MID-LEVEL for the hot load (%d bytes of models, %d of car palettes, %d of cosmetics)\n",
			JerCityName(src), gCarImports[src].carModelsSize, gCarImports[src].palletSize,
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

/* JERICHO: does any LIVE resident slot still read this city's car data? The engine's
 * resident array is the authority: chkImportReleaseSlot and chkImportHotLoad write the
 * slot's source before this is asked, so a city whose last slot was just released is
 * already invisible here. The host city (source < 0) is never an import. */
static int CarImportCityInUse(int city)
{
	int slot;

	for (slot = 0; slot < MAX_CAR_RESIDENT_MODELS; slot++)
	{
		if (residentCarModels[slot] >= 0 && GetCarModelSourceCity(slot) == city)
			return 1;
	}

	return 0;
}

/* JERICHO: unload ONE guest city. The ordering is the point: the page lists/palette rows
 * and the deferred lump are pointers INTO gCarImports[city], so they must go before the
 * buffer is freed. Freeing the CLUT band and the (city,set) rows is what makes the city
 * reloadable when the texture column would otherwise be full. */
int JerReleaseCarImportForCity(int city)
{
	int i;

	if (city < 0 || city >= CITY_COUNT || city == GameLevel)
		return 0;

	if (gCarImports[city].region == NULL)
		return 0;

	if (CarImportCityInUse(city))
		return 0;

	CarImportForgetCity(city);
	FreeCarImport(&gCarImports[city]);

	if (gCarImportCity == city)
	{
		gCarImportCity = -1;

		for (i = 0; i < CITY_COUNT; i++)
		{
			if (gCarImports[i].region != NULL)
			{
				gCarImportCity = i;
				break;
			}
		}
	}

	printInfo("cross-city: released %s - no live car names it, so its palette rows, pages and buffer are free for another city\n",
		JerCityName(city));

	return 1;
}

int JerReleaseUnusedCarImports(void)
{
	int city, n = 0;

	for (city = 0; city < CITY_COUNT; city++)
	{
		if (CarImportCityHeld(city) && !CarImportCityInUse(city))
			n += JerReleaseCarImportForCity(city);
	}

	return n;
}

// The models `city`'s car data ACTUALLY carries, as a bitmask: bit N = model N.
// 0 when the city is not held.
//
// This is the "list what was imported" door. A module should not assume which
// numbers a city's cars sit on - a transplanted Driver 1 city's layout is decided
// by the transplant (tools/bake.py), not by any table the module can see - so it
// asks here and offers what comes back.
unsigned JerCarImportModels(int city)
{
	return CarImportCityHeld(city) ? gCarImports[city].carModelsPresent : 0;
}

// JERICHO: the models a city carries, LOADING ITS CAR DATA FIRST when it is not
// held. This is the answer a runtime bank's roster needs: unlike the stock cities a
// bank has no generated model table (nothing baked it), so the only honest source is
// the bank's own LUMP_CAR_MODELS. Returns 0 for an unregistered row, a bank that is
// not installed, or one whose data carries nothing - all of which mean "offer no
// cars from this city".
unsigned JerCityCarModels(int city)
{
	if (!JerCityRegistered(city))
		return 0;

	if (!CarImportCityHeld(city) && !JerLoadCarImportForCity(city))
		return 0;

	return JerCarImportModels(city);
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

// JERICHO: the embedded car-sound entry for `model` in a D1 city's synthetic LEV,
// or 0 when it is missing or malformed. The body is already in the D2 dynamic-bank
// shape (int num_samples, that many SAMPLE_DATA, then the VAG payload), so gamesnd
// can hand it straight to LoadSoundBankDynamic. Addresses are bank-payload-relative.
static int CarImportSoundEntry(CAR_IMPORT* imp, int model, char** bank, int* size)
{
	int off, n, i, dir, payload;

	// A Driver 2 city has no embedded D1 sound bank at all: cross-city imports of a
	// D2 city (RIO into CHICAGO, RIO into RIO's slot 5) are held and have car data,
	// but carSounds is NULL. Indexing the per-model table before this check read
	// address 8 + model*4 (0x2C for model 9) and crashed on the sound load. An
	// out-of-range model is the same class of caller bug.
	if (imp == NULL || imp->carSounds == NULL ||
		imp->carSoundsSize < 8 + CAR_IMPORT_MODEL_SLOTS * (int)sizeof(int) ||
		model < 0 || model >= CAR_IMPORT_MODEL_SLOTS)
		return 0;

	off = ((int*)(imp->carSounds + 8))[model];

	if (off < 8 + CAR_IMPORT_MODEL_SLOTS * (int)sizeof(int) ||
		off + 4 > imp->carSoundsSize)
		return 0;

	n = *(int*)(imp->carSounds + off);

	if (n != 3)
		return 0;

	dir = 4 + n * 16;

	if (off + dir > imp->carSoundsSize)
		return 0;

	payload = 0;

	for (i = 0; i < n; i++)
	{
		int a = *(int*)(imp->carSounds + off + 4 + i * 16);
		int length = *(int*)(imp->carSounds + off + 8 + i * 16);

		if (a < 0 || length < 0)
			return 0;

		if (a + length > payload)
			payload = a + length;
	}

	if (off + dir + payload > imp->carSoundsSize)
		return 0;

	*bank = imp->carSounds + off;
	*size = dir + payload;
	return 1;
}

// JERICHO: is this imported car a cop? The bake marks the source city's police
// model in LUMP_CAR_SIRENS; CarHasSiren uses this to give it Driver 2's siren
// channel instead of the host level's siren.
int GetCarImportSiren(int city, int model)
{
	if (!CarImportCityHeld(city) || model < 0 || model >= CAR_IMPORT_MODEL_SLOTS)
		return 0;

	return (gCarImports[city].carSirenMask >> model) & 1;
}

// JERICHO: the embedded car-sound bank a D1 city's car uses, or 0. Driver 1 gave
// EVERY car the same generic engine and idle, so the model number is not used to
// pick the bank: every car gets the city's one generic engine + idle pair (below).
// Indexing per model gave some cars the wrong loop and left models with no entry silent.
int GetCarImportSoundBank(int city, int model, char** bank, int* size)
{
	CAR_IMPORT* imp;
	char* b;
	int sz, m;

	if (bank)
		*bank = NULL;

	if (size)
		*size = 0;

	if (!CarImportCityHeld(city))
		return 0;

	imp = &gCarImports[city];

	// Per-model bank: the bake writes engine/idle/horn for every model, and the
	// engine now loads each imported model's own bank so its third sample is the
	// per-slot horn (or the cop siren). Old banks without per-model entries fall
	// through to the generic composed pair below.
	{
		char* mb = NULL;
		int msz = 0;

		if (CarImportSoundEntry(imp, model, &mb, &msz))
		{
			if (bank)
				*bank = mb;
			if (size)
				*size = msz;
			return 1;
		}
	}

	if (imp->carSounds == NULL ||
		imp->carSoundsSize < 8 + CAR_IMPORT_MODEL_SLOTS * (int)sizeof(int))
		return 0;

	if (*(int*)imp->carSounds != 1 ||
		*(int*)(imp->carSounds + 4) != CAR_IMPORT_MODEL_SLOTS)
		return 0;

	// Engine: the first valid entry (the bake gives it D1 loop 1, the engine). Idle: the
	// engine sample of the NEXT entry with a different loop (D1 loop 2, the low idle
	// rumble). The bake writes each entry as engine, engine-again-as-idle, horn, so
	// using one entry alone played the engine for the idle too. The pair is composed
	// into one D2 bank (engine, idle, horn) once per held segment.
	{
		static char* composed[CITY_COUNT];
		static char* composedFrom[CITY_COUNT];
		static int composedSize[CITY_COUNT];
		char *eb = NULL, *ib = NULL;
		int esz = 0, isz = 0;
		int* ed;
		int* id;

		if (composedFrom[city] == imp->carSounds && composed[city] != NULL)
		{
			if (bank) *bank = composed[city];
			if (size) *size = composedSize[city];
			return 1;
		}

		for (m = 0; m < CAR_IMPORT_MODEL_SLOTS; m++)
		{
			if (!CarImportSoundEntry(imp, m, &b, &sz))
				continue;

			if (eb == NULL)
			{
				eb = b; esz = sz;
			}
			else if (((int*)b)[2] != ((int*)eb)[2])	/* a different loop length: another D1 loop */
			{
				ib = b; isz = sz;
				break;
			}
		}

		if (eb == NULL)
			return 0;

		ed = (int*)(eb + 4);	/* SAMPLE_DATA[3]: address, length, loop, samplerate */

		if (ib == NULL)
		{
			/* only one loop in the segment: engine for both, as before */
			if (bank) *bank = eb;
			if (size) *size = esz;
			return 1;
		}

		id = (int*)(ib + 4);
		free(composed[city]);
		composedSize[city] = 4 + 3 * 16 + ed[1] + id[1] + ed[9];
		composed[city] = (char*)malloc(composedSize[city]);

		if (composed[city] == NULL)
		{
			composedFrom[city] = NULL;
			if (bank) *bank = eb;
			if (size) *size = esz;
			return 1;
		}

		{
			int* hd = (int*)composed[city];
			char* pay = composed[city] + 4 + 3 * 16;
			char* epay = eb + 4 + 3 * 16;
			char* ipay = ib + 4 + 3 * 16;

			hd[0] = 3;
			/* engine */
			hd[1] = 0; hd[2] = ed[1]; hd[3] = ed[2]; hd[4] = ed[3];
			memcpy(pay, epay + ed[0], ed[1]);
			/* idle */
			hd[5] = ed[1]; hd[6] = id[1]; hd[7] = id[2]; hd[8] = id[3];
			memcpy(pay + ed[1], ipay + id[0], id[1]);
			/* horn: the engine entry's */
			hd[9] = ed[1] + id[1]; hd[10] = ed[9]; hd[11] = ed[10]; hd[12] = ed[11];
			memcpy(pay + ed[1] + id[1], epay + ed[8], ed[9]);
		}

		composedFrom[city] = imp->carSounds;

		printInfo("cross-city: %s D1 car sounds: engine %d bytes @%d Hz, idle %d bytes @%d Hz\n",
			LevelNames[city], ed[1], ed[3], id[1], id[3]);

		if (bank) *bank = composed[city];
		if (size) *size = composedSize[city];
		return 1;
	}
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
	char filename[192];
	FILE* fp;

	if (!CarImportCityHeld(city) || offset < 0 || len <= 0 || dst == NULL)
		return 0;

	// the same single place a city's file is resolved as LoadCarImport - including
	// the refusal of an unregistered row and the slash normalisation
	if (!JerCityPath(city, filename, sizeof(filename), JER_CITY_FILE_LEVEL, 0))
		return 0;

	fp = fopen(filename, "rb");

	// the arena variant of the file; Driver 1's car-data cities and the runtime
	// banks have none
	if (fp == NULL && city < CITY_D2_COUNT)
	{
		JerCityPath(city, filename, sizeof(filename), JER_CITY_FILE_LEVEL, 1);
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

// ---------------------------------------------------------------------------
// JERICHO: an imported city's own wheel models
// ---------------------------------------------------------------------------
//
// The engine draws every car's wheels from three GLOBAL models
// (gCleanWheelModelPtr/gFastWheelModelPtr/gDamWheelModelPtr), found by name in
// the host level. An imported Driver 1 car therefore wore the host's wheels -
// including the host's wheel TEXTURE. The D1 bake now writes a LUMP_CAR_WHEELS
// segment beside the car models; BuildCarImportWheels reads its TEXTURE (page,
// rects) for DrawWheelObject, and the host wheel models stay the meshes.
//
// The models are FT4-only and carry no normals (the wheel draw never uses
// them). The source-city set index has to go through the same CarSetRemap the
// car polys use, or a wheel would sample the host page its source number
// collides with. That remap is only settled once the city's cars have been
// built, so the builder runs from ProcessCarModelLump / the hot-load path, NOT
// from the file read.

static void RemapWheelSets(MODEL* m)
{
	char* p;
	int i;

	if (m == NULL || m->poly_block <= 0 || m->num_polys <= 0)
		return;

	p = (char*)m + m->poly_block;

	CarSetRemapEnable(1);

	for (i = 0; i < m->num_polys; i++)
	{
		int type = (u_char)*p & 0x1f;
		int step = (type >= 0 && type < 56) ? PolySizes[type] : 0;

		if (step <= 0)
			break;

		if (type == 20 || type == 21 || type == 22 || type == 23)
			p[1] = (char)(u_char)CarSetRemap((u_char)p[1]);

		p += step;
	}

	CarSetRemapEnable(0);
}

// The wheel's SOURCE-CITY set numbers, before RemapWheelSets rewrites them.
// LoadImportedTPages needs these: it maps a source set to the index its page was
// loaded at with CarImportPinDstSet, exactly as it does for a car's sets. Feeding
// it the remapped index (what CollectModelSets sees after RemapWheelSets) makes
// the page-list lookup fail, the wheel page is never pinned, and the wheel
// samples whatever host page that index happens to hold - the yellow spikes.
static int CollectWheelSets(MODEL* m, unsigned char* out, int max)
{
	char* p;
	int i, n = 0;

	if (m == NULL || m->poly_block <= 0 || m->num_polys <= 0 || out == NULL)
		return 0;

	p = (char*)m + m->poly_block;

	for (i = 0; i < m->num_polys; i++)
	{
		int type = (u_char)*p & 0x1f;
		int step = (type >= 0 && type < 56) ? PolySizes[type] : 0;

		if (step <= 0)
			break;

		if (type == 20 || type == 21 || type == 22 || type == 23)
		{
			int set = (u_char)p[1];
			int k, seen = 0;

			if (set != 0 && set != 255)
			{
				for (k = 0; k < n; k++)
				{
					if (out[k] == set)
					{
						seen = 1;
						break;
					}
				}

				if (!seen && n < max)
					out[n++] = (unsigned char)set;
			}
		}

		p += step;
	}

	return n;
}

// One record poly's UV box, as the exclusive rect MapWheelUVToRect maps onto.
static void WheelPolyRect(const POLYFT4* p, int* u0, int* v0, int* u1, int* v1)
{
	const UV_INFO uv[4] = { p->uv0, p->uv1, p->uv2, p->uv3 };
	int k;

	*u0 = *u1 = uv[0].u;
	*v0 = *v1 = uv[0].v;

	for (k = 1; k < 4; k++)
	{
		if (uv[k].u < *u0) *u0 = uv[k].u;
		if (uv[k].u > *u1) *u1 = uv[k].u;
		if (uv[k].v < *v0) *v0 = uv[k].v;
		if (uv[k].v > *v1) *v1 = uv[k].v;
	}

	*u1 += 1;
	*v1 += 1;
}

// JERICHO: a D1 car keeps the HOST wheel models (CLEANWHEEL/FASTWHEEL/DAMWHEEL)
// and takes only its wheel TEXTURE from LUMP_CAR_WHEELS: the record is read in
// place for its page, its face rect (polys 2/3, the quads MangleWheelModel keeps
// as the rim) and its tread rect (polys 4/6), and the wheel draws through the
// same JER_WHEEL_TEX path as a D2 city. No wheel MODEL is copied or mangled:
// that copy sized each record from an offset not read yet, which overran the
// segment at spawn (the hot-load crash, and on Windows the camera flipping and
// the car falling through the map).
static void BuildCarImportWheels(int city)
{
	CAR_IMPORT* imp;
	int count, i;

	if (city < 0 || city >= CITY_COUNT)
		return;

	imp = &gCarImports[city];

	if (imp->region == NULL || imp->carWheels == NULL)
		return;

	for (i = 0; i < CAR_IMPORT_WHEEL_VARIANTS; i++)
	{
		if (imp->wheelTex[i].present)
			return;		// already built (level load, then a hot load)
	}

	if (imp->carWheelsSize < 4)
	{
		printInfo("cross-city: %s wheel segment is too small (%d bytes) - keeping the host wheels\n",
			LevelNames[city], imp->carWheelsSize);
		return;
	}

	count = *(int*)imp->carWheels;

	if (count < 1 || count > CAR_IMPORT_WHEEL_VARIANTS ||
		4 + count * (int)sizeof(int) > imp->carWheelsSize)
	{
		printInfo("cross-city: %s wheel segment is malformed (count %d, %d bytes) - keeping the host wheels\n",
			LevelNames[city], count, imp->carWheelsSize);
		return;
	}

	for (i = 0; i < count; i++)
	{
		int off = *(int*)(imp->carWheels + 4 + i * (int)sizeof(int));
		JER_WHEEL_TEX* wt = &imp->wheelTex[i];
		MODEL* src;
		POLYFT4* p;
		int k;

		if (off == -1)
			continue;

		if (off < 0 || off + (int)sizeof(MODEL) > imp->carWheelsSize)
		{
			printInfo("cross-city: %s wheel %d offset %d is outside the segment (%d bytes) - absent\n",
				LevelNames[city], i, off, imp->carWheelsSize);
			continue;
		}

		src = (MODEL*)(imp->carWheels + off);

		if (src->num_polys < 8 || src->num_polys > 24 || src->poly_block <= (int)sizeof(MODEL) ||
			src->poly_block + src->num_polys * (int)sizeof(POLYFT4) > imp->carWheelsSize - off)
		{
			printInfo("cross-city: %s wheel %d has an unusable MODEL header (polys %d, %d bytes) - absent\n",
				LevelNames[city], i, src->num_polys, src->poly_block);
			continue;
		}

		p = (POLYFT4*)((char*)src + src->poly_block);

		for (k = 0; k < 8; k++)
		{
			if ((p[k].id & 0x1f) != 21)
				break;
		}

		if (k < 8)
		{
			printInfo("cross-city: %s wheel %d is not the 8-quad FT4 wheel - absent\n", LevelNames[city], i);
			continue;
		}

		// the SOURCE set numbers, for the pin; CollectWheelSets only reads the record
		imp->wheelSetCount[i] = CollectWheelSets(src, imp->wheelSet[i], 8);

		if (imp->wheelSetCount[i] <= 0)
			continue;

		WheelPolyRect(&p[2], &wt->u0, &wt->v0, &wt->u1, &wt->v1);
		WheelPolyRect(&p[4], &wt->hubU0, &wt->hubV0, &wt->hubU1, &wt->hubV1);

		CarSetRemapEnable(1);
		wt->set = CarSetRemap(p[2].texture_set);
		CarSetRemapEnable(0);

		wt->clut = p[2].texture_id;
		wt->hubClut = p[4].texture_id;	// the page loader puts the wheel row here
		wt->present = (wt->set > 0 && wt->set < 128);

		printInfo("cross-city: %s wheel %d texture: source set %d -> %d, face (%d,%d)-(%d,%d), tread (%d,%d)-(%d,%d)\n",
			LevelNames[city], i, p[2].texture_set, wt->set,
			wt->u0, wt->v0, wt->u1, wt->v1, wt->hubU0, wt->hubV0, wt->hubU1, wt->hubV1);
	}

	printInfo("cross-city: %s wheels - host meshes, imported textures: clean %s, damaged %s, fast %s\n",
		LevelNames[city],
		imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].present ? "yes" : "host",
		imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].present ? "yes" : "host",
		imp->wheelTex[CAR_IMPORT_WHEEL_FAST].present ? "yes" : "host");
}

MODEL* GetCarImportWheel(int city, int kind)
{
	if (city < 0 || city >= CITY_COUNT || kind < 0 || kind >= CAR_IMPORT_WHEEL_VARIANTS)
		return NULL;

	return gCarImports[city].wheels[kind];
}

int GetCarImportWheelTex(int city, int kind, JER_WHEEL_TEX* out)
{
	if (city < 0 || city >= CITY_COUNT || kind < 0 || kind >= CAR_IMPORT_WHEEL_VARIANTS ||
		out == NULL)
		return 0;

	*out = gCarImports[city].wheelTex[kind];

	return out->present;
}

// The page loader knows how many CLUT rows the source wheel page really has.
// The WHLBOTH (hubcap) palette is the page's last row; record it for the wheel
// structs that name this source set.
void CarImportSetWheelPageCluts(int city, int sourceSet, int cluts)
{
	int v;

	if (city < 0 || city >= CITY_COUNT || cluts <= 0)
		return;

	for (v = 0; v < CAR_IMPORT_WHEEL_VARIANTS; v++)
	{
		CAR_IMPORT* imp = &gCarImports[city];

		if (imp->wheelSetCount[v] > 0 && imp->wheelSet[v][0] == sourceSet)
			imp->wheelTex[v].hubClut = (cluts > 32) ? 31 : cluts - 1;
	}
}

int CarImportIsWheelSet(int city, int sourceSet)
{
	int v, k;

	if (city < 0 || city >= CITY_COUNT)
		return 0;

	for (v = 0; v < CAR_IMPORT_WHEEL_VARIANTS; v++)
	{
		for (k = 0; k < gCarImports[city].wheelSetCount[v]; k++)
			if (gCarImports[city].wheelSet[v][k] == sourceSet)
				return 1;
	}

	return 0;
}

void CarImportSetWheelPaletteClut(int city, int sourceSet, int clut, int hub)
{
	int v, k;

	if (city < 0 || city >= CITY_COUNT || clut <= 0)
		return;

	for (v = 0; v < CAR_IMPORT_WHEEL_VARIANTS; v++)
	{
		for (k = 0; k < gCarImports[city].wheelSetCount[v]; k++)
		{
			if (gCarImports[city].wheelSet[v][k] == sourceSet)
			{
				if (hub)
					gCarImports[city].wheelTex[v].reservedHubClut = clut;
				else
					gCarImports[city].wheelTex[v].reservedClut = clut;
			}
		}
	}
}

// The CLUT row that actually paints the D2 wheel. Measured from the
// WHLBOTH sprite's dominant index and the RGB555 rows: the wheel row is the
// one with an opaque (bit 15 clear) entry at that index, not the preceding
// green/STP car-palette rows.
int CarImportWheelPaletteRow(int city, int cluts)
{
	int row;

	if (cluts <= 0)
		return 0;

	switch (city)
	{
	case 0: row = 29; break;	// CHICAGO
	case 1: row = 29; break;	// HAVANA
	case 2: row = 4; break;		// VEGAS
	case 3: row = 4; break;		// RIO
	default: row = cluts - 1; break;
	}

	if (row >= cluts)
		row = cluts - 1;

	return row;
}

// Derive a D2 wheel sprite's CLUT row from the page itself instead of a
// per-city table. WHLCLEAN (tyre) and WHLBOTH (hubcap) are separate sprites on
// the source page and are NOT painted by the same row (HAVANA: 25 and 29), so
// each is derived on its own. hub = 1 for WHLBOTH, 0 for WHLCLEAN. Returns -1
// (caller keeps the measured table) for a D1 baked city, a page that does not
// unpack, or a sprite no row paints.
int CarImportDeriveWheelPaletteRow(int city, int sourceSet, const unsigned char* page, int size, int hub)
{
	CAR_IMPORT* imp;
	const JER_WHEEL_TEX* wt;
	const unsigned char* pix;
	int npal, hist[16];
	int u0, v0, u1, v1, u, v, r, i, best = -1, bestScore = 0, dam;

	if (city < 0 || city >= CITY_COUNT || page == NULL || size < 4 || !JerCityRegistered(city))
		return -1;

	npal = *(const int*)page;

	if (npal <= 0 || npal > 32 || size <= 4 + npal * 32)
		return -1;

	imp = &gCarImports[city];
	wt = &imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN];

	/* D1 baked cities have no D2-style wheelTex metadata for DAM/FAST.
	 * Fall back to the measured table row instead of bailing. -1 is the
	 * caller's "use CarImportWheelPaletteRow()" sentinel; 0 would select
	 * page row 0, which is not a wheel row. */
	if (city >= CITY_D2_COUNT && !wt->present)
		return -1;

	if (!wt->present)
		return -1;

	/* hub: the WHLBOTH hubcap rect (on the damaged variant's last source set),
	 * else the WHLCLEAN tyre rect (on the clean set). */
	dam = hub;

	if (imp->wheelSetCount[dam ? CAR_IMPORT_WHEEL_DAMAGED : CAR_IMPORT_WHEEL_CLEAN] <= 0 ||
		imp->wheelSet[dam ? CAR_IMPORT_WHEEL_DAMAGED : CAR_IMPORT_WHEEL_CLEAN]
			[imp->wheelSetCount[dam ? CAR_IMPORT_WHEEL_DAMAGED : CAR_IMPORT_WHEEL_CLEAN] - 1] != sourceSet)
		return -1;

	if (dam)
	{
		u0 = wt->hubU0; v0 = wt->hubV0; u1 = wt->hubU1; v1 = wt->hubV1;
	}
	else
	{
		u0 = wt->u0; v0 = wt->v0; u1 = wt->u1; v1 = wt->v1;
	}

	if (u0 < 0 || v0 < 0 || u1 > 256 || v1 > 256 || u1 <= u0 || v1 <= v0)
		return -1;

	memset(hist, 0, sizeof(hist));

	/* The page image is stored compressed (LoadTPageAndCluts runs decomp_asm on
	 * it too): unpack a private copy of the 256x256 4bpp image. */
	{
		static unsigned char* sUnpacked = NULL;

		if (sUnpacked == NULL)
			sUnpacked = (unsigned char*)malloc(0x10000);

		if (sUnpacked == NULL)
			return -1;

		memset(sUnpacked, 0, 0x10000);
		decomp_asm((char*)sUnpacked, (char*)page + 4 + npal * 32);
		pix = sUnpacked;
	}

	for (v = v0; v < v1; v++)
	{
		for (u = u0; u < u1; u++)
		{
			unsigned char b = pix[v * 128 + (u >> 1)];
			hist[(u & 1) ? (b >> 4) : (b & 15)]++;
		}
	}

	/* For each row: the mean luminance step between horizontally adjacent
	 * sprite pixels, divided by the mean step between two random sprite
	 * pixels under that row. The row the art was drawn with is smooth (its
	 * ramp follows the index layout), so the ratio is lowest there; any other
	 * row turns the sprite into noise. Transparent (0x0000) entries are
	 * skipped. On HAVANA this picks 29 for WHLBOTH and 25 for WHLCLEAN. */
	{
		double bestRatio = 1e30;
		int bg = 0, pass;
		int cu[4], cv[4], votes[16] = { 0 };

		/* The sprite's background (its corner index) must be transparent
		 * (0x0000) in the right row - otherwise the square's corners draw.
		 * CHICAGO's light rows 6-9 are smooth too but opaque there. Rows that
		 * fail this are only considered when no row passes it. */
		cu[0] = u0; cu[1] = u1 - 1; cu[2] = u0; cu[3] = u1 - 1;
		cv[0] = v0; cv[1] = v0; cv[2] = v1 - 1; cv[3] = v1 - 1;

		for (i = 0; i < 4; i++)
		{
			unsigned char cb = pix[cv[i] * 128 + (cu[i] >> 1)];
			votes[(cu[i] & 1) ? (cb >> 4) : (cb & 15)]++;
		}

		for (i = 1; i < 16; i++)
			if (votes[i] > votes[bg])
				bg = i;

		for (pass = 0; pass < 2 && best < 0; pass++)
		for (r = 0; r < npal; r++)
		{
			const unsigned short* row = (const unsigned short*)(page + 4 + r * 32);
			int lum[16];
			double adj = 0.0, all = 0.0, nAll = 0.0, ratio;
			int nAdj = 0, a, b2;

			if (pass == 0 && row[bg] != 0)
				continue;

			for (i = 0; i < 16; i++)
				lum[i] = (row[i] & 31) * 3 + ((row[i] >> 5) & 31) * 6 + ((row[i] >> 10) & 31);

			for (v = v0; v < v1; v++)
			{
				for (u = u0; u < u1 - 1; u++)
				{
					unsigned char p0 = pix[v * 128 + (u >> 1)];
					unsigned char p1 = pix[v * 128 + ((u + 1) >> 1)];

					a = (u & 1) ? (p0 >> 4) : (p0 & 15);
					b2 = ((u + 1) & 1) ? (p1 >> 4) : (p1 & 15);

					if (row[a] == 0 || row[b2] == 0)
						continue;

					adj += abs(lum[a] - lum[b2]);
					nAdj++;
				}
			}

			for (a = 0; a < 16; a++)
			{
				for (b2 = 0; b2 < 16; b2++)
				{
					if (row[a] == 0 || row[b2] == 0)
						continue;

					all += (double)hist[a] * hist[b2] * abs(lum[a] - lum[b2]);
					nAll += (double)hist[a] * hist[b2];
				}
			}

			if (nAdj < 64 || nAll <= 0.0 || all <= 0.0)
				continue;	/* (near) flat or fully transparent under this row */

			ratio = (adj / nAdj) / (all / nAll);

			if (ratio < bestRatio)
			{
				bestRatio = ratio;
				best = r;
			}
		}

		bestScore = (int)(bestRatio * 100.0 + 0.5);

		printInfo("cross-city: %s wheel %s sprite (%d,%d)-(%d,%d) background index %d -> row %d (smoothness %d%%)\n",
			LevelNames[city], dam ? "hubcap" : "tyre", u0, v0, u1, v1, bg, best, bestScore);
	}

	return best;
}

// The tyre row is derived from the CLEAN sprite, and its background index is
// transparent there. The FAST sprite on the same page has its own background
// index, which that row can leave opaque (MIAMI's: a black square around the
// blur). A host whose FASTWHEEL polys are semi-transparent (CHICAGO) hid it by
// blending; HAVANA's are opaque and drew it. Give the fast variant a copy of the
// tyre row with its background cleared to 0x0000 (transparent).
void CarImportReserveFastWheelPalette(int city, int sourceSet, const unsigned char* page, int size, int tyreRow)
{
	CAR_IMPORT* imp;
	JER_WHEEL_TEX* wt;
	static unsigned char* sUnpacked = NULL;
	unsigned short row[16];
	int npal, cu[4], cv[4], votes[16] = { 0 }, i, bg = 0, clut;

	if (city < 0 || city >= CITY_COUNT || page == NULL || size < 4)
		return;

	/* Driver 1 imports only: a Driver 2 city's fast wheel names row 0, a CAR
	 * row (RIO's drew green); D2 imports keep their stock wheel rows. This
	 * includes the runtime banks (city >= JER_CITY_STOCK_COUNT), e.g. the
	 * Euro-43 beta at city 11. */
	if (city < CITY_D2_COUNT)
		return;

	imp = &gCarImports[city];
	wt = &imp->wheelTex[CAR_IMPORT_WHEEL_FAST];
	npal = *(const int*)page;

	/* A D1 bake has no D2-style wheelTex metadata for FASTWHEEL. Use the
	 * tyre row that was already derived for the clean wheel instead of
	 * leaving the fast wheel on the host palette. */
	if (!wt->present || imp->wheelSetCount[CAR_IMPORT_WHEEL_FAST] <= 0 ||
		imp->wheelSet[CAR_IMPORT_WHEEL_FAST][0] != sourceSet)
	{
		if (npal > 0 && npal <= 32 && size > 4 + npal * 32 &&
			tyreRow >= 0 && tyreRow < npal)
		{
			memcpy(row, page + 4 + tyreRow * 32, sizeof(row));
			clut = CarImportReserveWheelPalette(city, 0, (const unsigned char*)row);

			if (clut != 0)
			{
				CarImportSetWheelPaletteClut(city, sourceSet, clut, 0);
				wt->reservedClut = clut;
			}
		}

		return;
	}

	if (npal <= 0 || npal > 32 || size <= 4 + npal * 32 || tyreRow < 0 || tyreRow >= npal)
		return;

	if (wt->u0 < 0 || wt->v0 < 0 || wt->u1 > 256 || wt->v1 > 256 || wt->u1 <= wt->u0 || wt->v1 <= wt->v0)
		return;

	if (sUnpacked == NULL)
		sUnpacked = (unsigned char*)malloc(0x10000);

	if (sUnpacked == NULL)
		return;

	memset(sUnpacked, 0, 0x10000);
	decomp_asm((char*)sUnpacked, (char*)page + 4 + npal * 32);

	cu[0] = wt->u0; cu[1] = wt->u1 - 1; cu[2] = wt->u0; cu[3] = wt->u1 - 1;
	cv[0] = wt->v0; cv[1] = wt->v0; cv[2] = wt->v1 - 1; cv[3] = wt->v1 - 1;

	for (i = 0; i < 4; i++)
	{
		unsigned char cb = sUnpacked[cv[i] * 128 + (cu[i] >> 1)];
		votes[(cu[i] & 1) ? (cb >> 4) : (cb & 15)]++;
	}

	for (i = 1; i < 16; i++)
		if (votes[i] > votes[bg])
			bg = i;

	/* The fast face names its own row (MIAMI's FASTWHEEL: 8). The clean
	 * sprite's tyre row (6) painted it colour-inverted. */
	if (wt->clut >= 0 && wt->clut < npal)
		tyreRow = wt->clut;

	memcpy(row, page + 4 + tyreRow * 32, sizeof(row));

	printInfo("cross-city: %s fast wheel sprite (%d,%d)-(%d,%d) row %d, background index %d is %04x - cleared in a fast-only row\n",
		LevelNames[city], wt->u0, wt->v0, wt->u1, wt->v1, tyreRow, bg, row[bg]);

	row[bg] = 0;
	clut = CarImportReserveWheelPalette(city, 2, (const unsigned char*)row);

	if (clut != 0)
		wt->reservedClut = clut;
}

// The damaged wheel's face names its OWN CLUT row, which need not be the clean
// tyre's: MIAMI's DAMWHEEL is row 5 while the clean wheel is row 6. The clean
// palette reservation sets every variant that names the page to row 6, so the
// damaged wheel needs its own row or it draws the clean wheel's colours.
void CarImportReserveDamagedWheelPalette(int city, int sourceSet, const unsigned char* page, int size, int tyreRow)
{
	CAR_IMPORT* imp;
	JER_WHEEL_TEX* wt;
	unsigned short row[16];
	int npal, clut;

	if (city < 0 || city >= CITY_COUNT || page == NULL || size < 4)
		return;

	/* Driver 1 imports only, the same rule as the fast wheel. */
	if (city < CITY_D2_COUNT)
		return;

	imp = &gCarImports[city];
	wt = &imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED];
	npal = *(const int*)page;

	if (wt->present && imp->wheelSetCount[CAR_IMPORT_WHEEL_DAMAGED] > 0 &&
		imp->wheelSet[CAR_IMPORT_WHEEL_DAMAGED][0] == sourceSet &&
		npal > 0 && npal <= 32 && size > 4 + npal * 32 &&
		wt->clut >= 0 && wt->clut < npal)
	{
		memcpy(row, page + 4 + wt->clut * 32, sizeof(row));
		clut = CarImportReserveWheelPalette(city, 3, (const unsigned char*)row);

		if (clut != 0)
			wt->reservedClut = clut;

		printInfo("cross-city: %s damaged wheel palette row %d reserved (clut %04x)\n",
			LevelNames[city], wt->clut, clut);
		return;
	}

	/* No usable DAMWHEEL metadata: use the tyre row already derived for the
	 * clean wheel, as the fast-wheel fallback does. */
	if (npal > 0 && npal <= 32 && size > 4 + npal * 32 &&
		tyreRow >= 0 && tyreRow < npal)
	{
		memcpy(row, page + 4 + tyreRow * 32, sizeof(row));
		clut = CarImportReserveWheelPalette(city, 3, (const unsigned char*)row);

		if (clut != 0)
			wt->reservedClut = clut;

		printInfo("cross-city: %s damaged wheel palette fell back to tyre row %d (clut %04x)\n",
			LevelNames[city], tyreRow, clut);
	}
}

// A texture-only copy of a Driver 2 named wheel model. The renderer uses the
// host wheel MESH and this model only for its texture records and UVs, so the
// vertices and normals are deliberately dropped; the poly stream is copied and
// the header is made self-contained (instance_number = -1) because a D2
// DAMWHEEL is an instance whose vertices live on a parent model.
static MODEL* MakeD2WheelTextureModel(MODEL* src, int size)
{
	MODEL* m;
	int polyStart, polyLen;

	if (src == NULL || src->poly_block <= 0 || src->num_polys <= 0)
		return NULL;

	polyStart = src->poly_block;
	polyLen = size - polyStart;

	if (polyLen <= 0)
		return NULL;

	m = (MODEL*)malloc(sizeof(MODEL) + polyLen);

	if (m == NULL)
		return NULL;

	memset(m, 0, sizeof(MODEL));

	m->shape_flags = 0x8001;
	m->instance_number = -1;
	m->poly_block = (int)sizeof(MODEL);
	m->num_polys = src->num_polys;

	memcpy((char*)m + sizeof(MODEL), (char*)src + polyStart, polyLen);

	return m;
}

// A real Driver 2 city keeps its named wheels in LUMP_MODELS (DATA2) with the
// names in LUMP_MODELNAMES (DATA1). Read CLEANWHEEL / DAMWHEEL / FASTWHEEL from
// there so an imported D2 car wears ITS city's wheel texture, not the host's.
// The models are texture-only: DrawCarWheels keeps the host geometry and uses
// these records for the texture set/id and UVs.
// ---------------------------------------------------------------------------
// D2 wheel texture source
//
// The D2 level does not name a wheel MODEL. It carries the wheel art as two
// TEXINF details on a permanent page:
//   WHLCLEAN  - the clean/fast wheel sprite (HAVANA calls it WHEEL)
//   WHLBOTH   - the damaged wheel / hubcap sprite
// Their page index is the page set (all four cities keep it in permlist), and
// the rect is a 64x64 square. We record that page/rect here; the renderer keeps
// the host wheel mesh and maps its UVs onto the rect.
//
// The set still has to go through CarSetRemap, exactly like a car's or a D1
// wheel's set, and the SOURCE number is what the pin walk needs.
// ---------------------------------------------------------------------------

static int BuildD2WheelTextureRects(int city)
{
	CAR_IMPORT* imp;
	char* p;
	char* end;
	int tpageAmount;
	int page;
	int haveClean = 0;
	int haveDam = 0;
	int cleanSet = -1;
	int damSet = -1;
	int cleanU0 = 0, cleanV0 = 0, cleanU1 = 0, cleanV1 = 0;
	int damU0 = 0, damV0 = 0, damU1 = 0, damV1 = 0;

	if (city < 0 || city >= CITY_COUNT)
		return 0;

	imp = &gCarImports[city];

	if (imp->texInfo == NULL || imp->textureNames == NULL ||
		imp->texInfoSize < 16 || imp->textureNamesSize < 2)
		return 0;

	tpageAmount = *(int*)imp->texInfo;

	if (tpageAmount <= 0 || tpageAmount > 128)
		return 0;

	if ((size_t)tpageAmount * 8 + 8 > (size_t)imp->texInfoSize)
		return 0;

	p = (char*)&((TP*)(imp->texInfo + 8))[tpageAmount + 1];
	end = imp->texInfo + imp->texInfoSize;

	for (page = 0; page < tpageAmount; page++)
	{
		int count;
		int k;

		if (p + 4 > end)
			return 0;

		count = *(int*)p;
		p += 4;

		if (count < 0 || p + (size_t)count * sizeof(TEXINF) > end)
			return 0;

		for (k = 0; k < count; k++)
		{
			TEXINF* t = (TEXINF*)p;
			char* name;

			p += sizeof(TEXINF);

			if (t->nameoffset >= imp->textureNamesSize)
				continue;

			name = imp->textureNames + t->nameoffset;

			if (name >= imp->textureNames + imp->textureNamesSize)
				continue;

			if (strcmp(name, "WHLBOTH") == 0)
			{
				damSet = page;
				damU0 = t->x; damV0 = t->y;
				damU1 = t->x + t->width; damV1 = t->y + t->height;
				haveDam = 1;
			}
			else if (strcmp(name, "WHLCLEAN") == 0 || strcmp(name, "WHEEL") == 0)
			{
				cleanSet = page;
				cleanU0 = t->x; cleanV0 = t->y;
				cleanU1 = t->x + t->width; cleanV1 = t->y + t->height;
				haveClean = 1;
			}
		}
	}

	if (!haveClean && !haveDam)
		return 0;

	if (!haveClean)
	{
		cleanSet = damSet;
		cleanU0 = damU0; cleanV0 = damV0;
		cleanU1 = damU1; cleanV1 = damV1;
	}

	if (!haveDam)
	{
		damSet = cleanSet;
		damU0 = cleanU0; damV0 = cleanV0;
		damU1 = cleanU1; damV1 = cleanV1;
	}

	/* Page set -> destination index. The pin walk resolves the SOURCE set with
	 * CarImportPinDstSet; the draw uses this remapped index. */
	{
		int cleanSource = cleanSet;
		int damSource = damSet;

		CarSetRemapEnable(1);
		cleanSet = CarSetRemap(cleanSource);
		damSet = CarSetRemap(damSource);
		CarSetRemapEnable(0);

		/* SOURCE sets for the pin walk, not the remapped draw indices. */
		imp->wheelSet[CAR_IMPORT_WHEEL_CLEAN][0] = (unsigned char)cleanSource;
		imp->wheelSetCount[CAR_IMPORT_WHEEL_CLEAN] = 1;
		imp->wheelSet[CAR_IMPORT_WHEEL_FAST][0] = (unsigned char)cleanSource;
		imp->wheelSetCount[CAR_IMPORT_WHEEL_FAST] = 1;
		imp->wheelSet[CAR_IMPORT_WHEEL_DAMAGED][0] = (unsigned char)cleanSource;
		imp->wheelSetCount[CAR_IMPORT_WHEEL_DAMAGED] = 1;

		if (damSource != cleanSource)
		{
			imp->wheelSet[CAR_IMPORT_WHEEL_DAMAGED][1] = (unsigned char)damSource;
			imp->wheelSetCount[CAR_IMPORT_WHEEL_DAMAGED] = 2;
		}
	}

	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].present = 1;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].set = cleanSet;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].clut = 0;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].u0 = cleanU0;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].v0 = cleanV0;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].u1 = cleanU1;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].v1 = cleanV1;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].hubU0 = damU0;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].hubV0 = damV0;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].hubU1 = damU1;
	imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].hubV1 = damV1;

	imp->wheelTex[CAR_IMPORT_WHEEL_FAST] = imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN];

	/* A damaged wheel keeps the CLEAN tyre sprite and takes only its hubcap
	 * poly from WHLBOTH. Using WHLBOTH for the whole wheel is what garbled
	 * the tyre (a hubcap image stretched over it). */
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].present = 1;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].set = cleanSet;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].clut = 0;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].u0 = cleanU0;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].v0 = cleanV0;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].u1 = cleanU1;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].v1 = cleanV1;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].hubU0 = damU0;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].hubV0 = damV0;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].hubU1 = damU1;
	imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].hubV1 = damV1;

	return 1;
}

static void BuildD2WheelTextureSources(int city)
{
	static const char* wheelNames[CAR_IMPORT_WHEEL_VARIANTS] = {
		"CLEANWHEEL", "DAMWHEEL", "FASTWHEEL"
	};
	CAR_IMPORT* imp;
	int idx[CAR_IMPORT_WHEEL_VARIANTS];
	char* p;
	char* end;
	int n, v, i;
	int count;
	char* cur;

	if (city < 0 || city >= CITY_COUNT)
		return;

	/* Only registered car-data cities have a level file to read. D1 baked files
	 * have no DATA2 and resolve their wheels through LUMP_CAR_WHEELS, so there is
	 * nothing to say here. */
	if (!JerCityRegistered(city))
		return;

	imp = &gCarImports[city];

	if (imp->data2 == NULL)
		return;

	if (BuildD2WheelTextureRects(city))
	{
		printInfo("cross-city: %s wheels - WHLCLEAN/WHLBOTH TEXINF sprites (sets %d/%d)\n",
			LevelNames[city],
			imp->wheelTex[CAR_IMPORT_WHEEL_CLEAN].set,
			imp->wheelTex[CAR_IMPORT_WHEEL_DAMAGED].set);
		return;
	}

	if (imp->region == NULL || imp->models == NULL || imp->modelNames == NULL)
	{
		printInfo("cross-city: %s wheels - no named wheel data; using the host wheel texture\n",
			LevelNames[city]);
		return;
	}

	if (imp->wheels[0] != NULL || imp->wheels[1] != NULL || imp->wheels[2] != NULL)
		return;

	for (v = 0; v < CAR_IMPORT_WHEEL_VARIANTS; v++)
		idx[v] = -1;

	p = imp->modelNames;
	end = p + imp->modelNamesSize;
	n = 0;

	while (p < end && *p != 0)
	{
		for (v = 0; v < CAR_IMPORT_WHEEL_VARIANTS; v++)
		{
			if (idx[v] < 0 && strcmp(p, wheelNames[v]) == 0)
				idx[v] = n;
		}

		p += strlen(p) + 1;
		n++;
	}

	if (idx[0] < 0 && idx[1] < 0 && idx[2] < 0)
	{
		printInfo("cross-city: %s wheels - no CLEANWHEEL/DAMWHEEL/FASTWHEEL model; using the host wheel texture\n",
			LevelNames[city]);
		return;
	}

	if (imp->modelsSize < 4)
		return;

	count = *(int*)imp->models;
	cur = imp->models + 4;

	for (i = 0; i < count; i++)
	{
		int size;
		MODEL* src;

		if (cur + 4 > imp->models + imp->modelsSize)
			break;

		size = *(int*)cur;
		src = (MODEL*)(cur + 4);

		if (size <= 0 || cur + 4 + size > imp->models + imp->modelsSize)
			break;

		for (v = 0; v < CAR_IMPORT_WHEEL_VARIANTS; v++)
		{
			if (i != idx[v] || imp->wheels[v] != NULL)
				continue;

			{
				MODEL* m = MakeD2WheelTextureModel(src, size);

				if (m != NULL)
				{
					imp->wheelSetCount[v] = CollectWheelSets(m, imp->wheelSet[v], 8);
					RemapWheelSets(m);
					imp->wheels[v] = m;
					imp->wheelTexMem[v] = (char*)m;
				}
			}
		}

		cur += 4 + size;
	}

	printInfo("cross-city: %s wheels - clean %s, damaged %s, fast %s (D2 named models)\n",
		LevelNames[city],
		(imp->wheels[0] != NULL) ? "imported" : "host",
		(imp->wheels[1] != NULL) ? "imported" : "host",
		(imp->wheels[2] != NULL) ? "imported" : "host");
}

// The source-city set numbers a wheel model names, captured before CarSetRemap
// rewrote them. texture.c's pin walk needs the SOURCE number (it maps it through
// CarImportPinDstSet); the MODEL itself carries the destination index the draw
// uses. Returns the count, or 0 for an absent wheel.
int GetCarImportWheelSets(int city, int kind, unsigned char* out, int max)
{
	int i, n;

	if (city < 0 || city >= CITY_COUNT || kind < 0 || kind >= CAR_IMPORT_WHEEL_VARIANTS ||
		out == NULL || max <= 0)
		return 0;

	n = gCarImports[city].wheelSetCount[kind];

	if (n > max)
		n = max;

	for (i = 0; i < n; i++)
		out[i] = gCarImports[city].wheelSet[kind][i];

	return n;
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

/* The eight-player guest-import budget: 16 resident models - 5 domestic - 2 mp spares
 * (5/6) - 1 special (11) = 8 hot-load blocks. A player whose car cannot get a block is
 * left drawn as the level's own car, so this is a build error, not a runtime surprise. */
#if JER_HOT_CAR_BLOCKS < 8
#error "the hot-load pool must cover the eight guest import slots"
#endif

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

		JerHotPolyReset();
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

		// JERICHO: residentCarModels[] is filled from the level file AND from the
		// command line, so it can hold anything - an unbounded -car value, a stale
		// wantedCar[], a figure only another city understands. The lump's per-model
		// offset table has CAR_MODEL_LUMP_ENTRIES entries, so indexing it with a
		// larger number reads whatever follows the lump and then faults while the
		// geometry is built: the documented "crash after LUMP_CAR_MODELS, no dump".
		// An out-of-range value is therefore treated as "this slot has no model" -
		// the build below is skipped and the slot stays EMPTY, which is a state the
		// rest of the game already handles (JerCarSlotUsable reports NO_MESH for it,
		// so nothing is ever spawned on it). The import path further down has always
		// checked this; the level's own path did not. -1 is the lump's OWN "empty
		// slot" sentinel and is expected, so it is not reported.
		if (model_number != -1 && (model_number < 0 || model_number >= CAR_MODEL_LUMP_ENTRIES))
		{
			printInfo("JERICHO: car model %d in slot %d is outside the lump's 0..%d table - slot left empty\n",
				model_number, i, CAR_MODEL_LUMP_ENTRIES - 1);

			model_number = -1;
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

	// JERICHO: now that every imported city's cars have been built, their set
	// remaps are settled. Build that city's wheel texture sources with the SAME
	// mapping - a wheel built earlier would have baked a different index. A D1
	// bake carries LUMP_CAR_WHEELS; a real D2 city is read from its named models.
	for (i = 0; i < CITY_COUNT; i++)
	{
		BuildCarImportWheels(i);

		if (gCarImports[i].wheels[0] == NULL &&
			gCarImports[i].wheels[1] == NULL &&
			gCarImports[i].wheels[2] == NULL)
			BuildD2WheelTextureSources(i);
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
	CAR_POLY* jerPolyBase;
	int jerPolyCap;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	if (gCarCleanModelPtr[slot] != NULL)
	{
		// Already built by an earlier hot load: the geometry is fine, but this
		// slot may only now have been pointed at a car whose sound bank is not
		// loaded yet. Idempotent for the common repeated call.
		LoadCarSoundForSlot(slot);
		return 0;			// already built
	}

	if (GetCarModelSourceCity(slot) < 0)
		return 0;			// the level's own car: nothing to bring in

	src_lump = GetCarImportModels(slot);

	if (src_lump == NULL)
		return 0;			// that city's data is not read in

	model_number = residentCarModels[slot];

	if (model_number < 0 || model_number >= CAR_MODEL_LUMP_ENTRIES)
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

	/* JERICHO: the CAR_POLY half of the same reservation. buildNewCarFromModel's polies MUST
	 * NOT go into the level-load bump: that cursor is filled once, at level start, and a
	 * mid-match build could never give its entries back - after a few car changes it reached
	 * MAX_CAR_POLYS and every later model built 0 polys (an INVISIBLE car whose pages still
	 * uploaded). A block taken here is returned by JerReleaseCarGeometry. */
	jerPolyBase = JerHotPolyTake(slot, &jerPolyCap);

	if (jerPolyBase == NULL)
	{
		printInfo("cross-city: no free hot-load poly block for %s model %d - slot %d keeps the car it has\n",
			LevelNames[GetCarModelSourceCity(slot)], model_number, slot);

		gJerHotCarBlockOf[slot] = -1;
		gJerHotCarUsed--;

		return 0;
	}

	/* JERICHO: from here to the end of the builds, write into THIS slot's poly block, with the
	 * cursor starting at 0 in it. The level-load cursor is saved and restored, so a hot build
	 * neither reads nor advances the arena the level's own cars live in. */
	JerBuildPolyArenaPush(jerPolyBase, jerPolyCap);

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

	if (whichCP > jerPolyCap)
		printInfo("cross-city: the hot-load poly block for slot %d overflowed (%d of %d) - its polies may have run into the next slot's\n",
			slot, whichCP, jerPolyCap);

	JerBuildPolyArenaPop();

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
		JerHotPolyGive(slot);

		return 0;
	}

	/* the manifest's geometry side: the block this slot now owns in the hot-load pool */
	CarSlotResNoteGeometry(slot, JER_HOT_CAR_BLOCK_BYTES);

	// JERICHO: a city read in mid-level needs its wheel texture sources too,
	// built after this slot's polys have settled the set remap they share.
	{
		int srcCity = GetCarModelSourceCity(slot);

		BuildCarImportWheels(srcCity);

		if (gCarImports[srcCity].wheels[0] == NULL &&
			gCarImports[srcCity].wheels[1] == NULL &&
			gCarImports[srcCity].wheels[2] == NULL)
			BuildD2WheelTextureSources(srcCity);
	}

	printInfo("cross-city: hot-loaded %s model %d into resident slot %d (%d bytes budgeted, block %d, %d of %d blocks used)\n",
		LevelNames[GetCarModelSourceCity(slot)], model_number, slot, need,
		gJerHotCarBlockOf[slot], gJerHotCarUsed, JER_HOT_CAR_BLOCKS);

	// The geometry is built, so the slot is genuinely this car: append its
	// engine/idle/horn bank and let GetCarBankSample find it. A failed build
	// returned above, leaving the stand-in car's sound in place.
	LoadCarSoundForSlot(slot);

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
	JerHotPolyGive(slot);

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

	JerHotPolyReset();

	// The palette blocks went with the city buffers, so free them too - a fresh load
	// must be able to hand block 0 to whatever city arrives first.
	CarImportCityBandReset();

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
