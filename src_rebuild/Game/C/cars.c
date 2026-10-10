#include "driver2.h"
#include "cars.h"
#include "texture.h"
#include "overmap.h"
#include "draw.h"
#include "mission.h"
#include "system.h"
#include "debris.h"
#include "main.h"
#include "camera.h"
#include "handling.h"
#include "cosmetic.h"
#include "models.h"	// JERICHO: GetCarImportCity / GetCarImportPallet (cross-city palettes)
#include "shadow.h"
#include "civ_ai.h"
#include "mc_snd.h"
#include "gamesnd.h"
#include "players.h"
#include "cutscene.h"
#include "convert.h"
#include "glaunch.h"
#include "ASM/rndrasm.h"
#include "dr2math.h"

// Main CRUMPLE library import and my small math library (i dont see stdlib math just dr2math, clearly means if i need external libs i write them in an engine appropriate way)
#include "jericho.h"	// JERICHO-HOOK: mod runtime (inert without modules)
#include "jer_events.h"	// JERICHO-HOOK: event argument structs
#include "jer_math.h"

struct plotCarGlobals
{
	u_char* primptr;
	OTTYPE* ot;
	u_int intensity;
	u_short* pciv_clut;
	u_char* damageLevel;
	int pageIndirect;	// JERICHO: this model's tpage high word is an INDEX, not a page id
};


#ifndef PSX
#define CAR_LOD_SWITCH_DISTANCE switch_detail_distance
#else
#define CAR_LOD_SWITCH_DISTANCE 5500
#endif

// JERICHO cross-city: an IMPORTED model's tpage high word is a texture-page INDEX, not a
// page id, so the page a poly samples can follow the rectangle the import pinned it into
// (the CLUT already works this way: the GT path reads pciv_clut at draw time). A stock
// model keeps baking the resolved id, exactly as before, so its primitives are unchanged.
//
// Why it has to be an indirection rather than a value: the page id is baked when the model
// is BUILT, and CarImportPin only fills texture_pages[] when the car is DRAWN - so a baked
// id is the dummy GetTPage(0,0,960,0), which is a live slot. Measured: 206 of 254 polys of
// an imported body sampled (960,0), the host's own page, which reads as wheel wells smeared
// across the car. See carhacks/docs/HACK.md.
#define CAR_TPAGE_OF(_pg, _uv1)	\
	((u_int)(((_pg)->pageIndirect \
		? texture_pages[((_uv1) >> 16) & 0xffff] : ((_uv1) >> 16)) << 16))

// The matching bake for buildNewCarFromModel.
#define CAR_BAKE_TPAGE(_imported, _set)	\
	((u_int)((_imported) ? (u_int)CarSetRemap(_set) : (u_int)texture_pages[CarSetRemap(_set)]))

MATRIX light_matrix =
{ 
	{ 
		{ 4096, 0, 0 }, 
		{ 0, 0, 0 }, 
		{ 0, 0, 0 }
	}, 
	{ 0, 0, 0 } 
};

MATRIX colour_matrix =
{ 
	{ 
		{ 4032, 0, 0 }, 
		{ 3936, 0, 0 }, 
		{ 3520, 0, 0 }
	}, 
	{ 0, 0, 0 } 
};

// PHYSICS
CAR_DATA car_data[MAX_CARS + 2];	// all cars + Tanner cbox + Camera cbox

HUBCAP gHubcap;

MODEL* gHubcapModelPtr;
MODEL* gCleanWheelModelPtr;
MODEL* gFastWheelModelPtr;
MODEL* gDamWheelModelPtr;

// active cars
CAR_DATA* active_car_list[MAX_CARS];

u_char lightsOnDelay[MAX_CARS];
short FrontWheelRotation[MAX_CARS]; // offset 0x0
short BackWheelRotation[MAX_CARS]; // offset 0x30

SVECTOR gTempCarVertDump[MAX_CARS][MAX_DENTING_VERTS];

DENTUVS *gTempCarUVPtr;
DENTUVS gTempHDCarUVDump[MAX_CARS][MAX_DENTING_UVS];
DENTUVS gTempLDCarUVDump[MAX_CARS][MAX_DENTING_LOD_UVS];

CAR_MODEL NewCarModel[MAX_CAR_RESIDENT_MODELS];
CAR_MODEL NewLowCarModel[MAX_CAR_RESIDENT_MODELS];

MODEL* gCarLowModelPtr[MAX_CAR_RESIDENT_MODELS];
MODEL* gCarDamModelPtr[MAX_CAR_RESIDENT_MODELS];
MODEL* gCarCleanModelPtr[MAX_CAR_RESIDENT_MODELS];

/* JERICHO-DIAG: the car-body draw census (see DrawCarObject/DrawCar), off unless
 * JERICHO_DIAG_CARDRAW=1. Declared here so the draw paths can use it. */
static int jerDiagCarDraw(void);

// pedestrian palette at 0 and next are cars
// model_id, texture_number, palette
//
// JERICHO: CIV_CLUT_ROWS is 16, not 8. Rows 0..7 are the host level's own car palettes;
// rows 8..15 are a cross-city import's, which is what lets a foreign car be painted
// from its own city's palettes without overwriting the host's. The row is chosen by
// carid in buildNewCarFromModel, and read back as
// pciv_clut[(carid-1)*192 + tex*6 + palette] (pciv_clut is &civ_clut[1]).
u_short civ_clut[CIV_CLUT_ROWS][32][6];

// JERICHO: WHO wrote each civ_clut row, and thereby which imported slot's car reads
// it. A row two cities both claim is a palette collision - one car wearing another's
// colours - and until now that was only visible as a mystery colour on some vehicle.
// CarPalRowNote records the write where it happens; CarPalRowReport prints the map at
// exit, next to the slot each car was loaded into.
static int sCivClutRowWriters[CIV_CLUT_ROWS];
// One entry per city that can SHARE a row: the host, plus one per guest block.
// The 4 was a magic number that happened to equal this; tied to the macro so it
// cannot drift if the guest budget ever changes.
// How many guest cities get a palette block. Defined up here because the per-row city
// list below is sized from it; the reasoning for the VALUE is a little further down,
// beside the refusal that enforces it.
#define CIV_CLUT_GUEST_CITIES	((CIV_CLUT_ROWS - CIV_CLUT_IMPORT_ROW) / CIV_CLUT_BLOCK_ROWS)

#define CIV_CLUT_ROW_CITIES	(CIV_CLUT_GUEST_CITIES + 1)
static int sCivClutRowCity[CIV_CLUT_ROWS][CIV_CLUT_ROW_CITIES];

// JERICHO: and WHICH COLOUR COLUMN of each row was actually filled. A spawned car picks
// its variant (0..5, civ_ai.c) and the draw reads civ_clut[row][texture_id][variant+1],
// but the upload only writes the columns the lump names - so a car can ask for a column
// nothing ever wrote and silently get the row's slot 0 instead. These maxima are what the
// draw clamps against, and what the palette map reports, so "the upload and the selector
// agree" is a measurement rather than an assumption.
static int sCivClutRowMaxSlot[CIV_CLUT_ROWS];       // highest slot written in the row
static int sCivClutTexMaxSlot[CIV_CLUT_ROWS][32];   // ...and per texture_id within it

// JERICHO: WHICH resident slot baked each civ_clut row, and from which set. The census can
// say a row offers no colour variants; it cannot say which car reads it, and that is the
// difference between "some rows are thin" and "the imported special's roof panel is
// colourless". Recorded at the bake, reported by CarPalRowReport for the empty rows only.
static int sBakeRowSlot[CIV_CLUT_ROWS][8];
static int sBakeRowSet[CIV_CLUT_ROWS][8];
static int sBakeRowN[CIV_CLUT_ROWS];

static void CarBakeRowNote(int row, int slot, int set)
{
	int i;

	if (row < 0 || row >= CIV_CLUT_ROWS || slot < 0)
		return;

	for (i = 0; i < sBakeRowN[row]; i++)
		if (sBakeRowSlot[row][i] == slot && sBakeRowSet[row][i] == set)
			return;

	if (sBakeRowN[row] < 8)
	{
		sBakeRowSlot[row][sBakeRowN[row]] = slot;
		sBakeRowSet[row][sBakeRowN[row]] = set;
		sBakeRowN[row]++;
	}
}

// JERICHO: how often the draw clamped a spawned variant down to a column that exists (see
// CarClutVariant). Zero on a stock level is the invariant: the clamp must never touch the
// host's own rows 0..7. Reported by CarPalRowReport, which runs at exit.
static int sClutClampCount;

static void CarPalRowNote(int row, int city)
{
	int i;

	if (row < 0 || row >= CIV_CLUT_ROWS)
		return;

	for (i = 0; i < sCivClutRowWriters[row]; i++)
		if (sCivClutRowCity[row][i] == city)
			return;		// this city already owns the row

	if (sCivClutRowWriters[row] < CIV_CLUT_ROW_CITIES)
		sCivClutRowCity[row][sCivClutRowWriters[row]++] = city;
}

static void CarPalRowClear(void)
{
	int r;

	for (r = 0; r < CIV_CLUT_ROWS; r++)
		sCivClutRowWriters[r] = 0;

	// The colour-column coverage goes with the writers: a new level re-uploads the rows it
	// needs, so a stale maximum would let the draw pick a column this level never filled.
	memset(sCivClutRowMaxSlot, 0, sizeof(sCivClutRowMaxSlot));
	memset(sCivClutTexMaxSlot, 0, sizeof(sCivClutTexMaxSlot));
	memset(sBakeRowN, 0, sizeof(sBakeRowN));
}

// JERICHO: which civ_clut block this guest city owns.
//
// The import used to be ONE 8-row block (8..15) that every guest city shared:
// CarPalIndexInCity returned rowbase CIV_CLUT_IMPORT_ROW for all of them, so a mashup's
// cities wrote over each other - and once a refusal was added, the FIRST city kept the
// block while every car still read it. Measured in a 3-city mashup: all six imported
// slots read rows 8..15 with one city having written only 2 of those rows.
//
// A block is CIV_CLUT_BLOCK_ROWS tall because CarPalIndexInCity's `i` spans
// carTpages[city][0..7]; a narrower band cannot hold `rowbase + 6` and the fix silently
// uploads nothing.
// JERICHO: which civ_clut block a guest city owns.
//
// PURE FUNCTION OF (city, GameLevel) -- deliberately NOT of which cities happen to be
// held right now. It used to count only the HELD cities below this one, which is stable
// while a level loads and shifts the moment a city is added later: VEGAS was the level's
// only guest and took block 0 (rows 8..15) at load time, then HAVANA arrived mid-match,
// found no held city below it, and took block 0 TOO. Measured: "VEGAS palettes: uploading
// for 8 of its block's 8 rows (civ_clut 8..15)" and "HAVANA palettes: uploading for 0 of
// its block's 8 rows (civ_clut 8..15)" - the reported "the vegas car imported proper but
// not the havana one's textures and colors". The level's own city is never a guest, and
// the three non-level cities therefore rank 0,1,2 in a fixed order - exactly the three
// blocks the column affords.
// JERICHO: which palette BLOCK a guest city owns - the Nth guest, not the Nth city.
//
// This counted this city's ORDINAL until now, skipping the host. That was right when
// there were four cities (ordinals topped out at 2, inside the budget of 3) and wrong
// the moment the registry grew: FRISCO is city 5, so with HAVANA as the host its band
// came back as 4, and CarImportBankRow refused it a block no matter how FEW cities were
// actually loaded. The car then fell back to the host's row 0 - an imported car wearing
// a local car's colours, which is exactly what "the imported cars have the wrong
// textures" looks like. Measured: 0 of the import bank's 24 rows written.
//
// Counting only the guests that are HELD keeps the blocks dense and independent of the
// registry's size, so three loaded guests always fit - which is the actual budget. It also
// matches how the rest of the import reasons: about what is held, never about ordinals.
//
// But it is NOT stable: the block was recomputed from whatever is held RIGHT NOW, so the
// moment a later city was added mid-match the earlier guests slid to a new block while
// their polys were already baked to the old one - progressive colour rot across a two-seat
// cycle (VEGAS took block 0, HAVANA arrived below it, VEGAS slid to block 1). A band must
// therefore be assigned ONCE per level and kept until the level resets, so it is recorded
// in the table below on first use and only cleared by CarImportCityBandReset.
static int sCarImportBandByCity[CITY_COUNT];
static int sCarImportBandInit = 0;

void CarImportCityBandReset(void)
{
	int c;

	for (c = 0; c < CITY_COUNT; c++)
		sCarImportBandByCity[c] = -1;

	sCarImportBandInit = 1;
}

static int CarImportCityBand(int city)
{
	int c, band;

	if (city < 0 || city >= CITY_COUNT)
		return -1;

	// A static array zero-initialises to 0, which is a VALID band, so before the first
	// level reset every city would read as "owns block 0". Initialise once, here, so the
	// reset's -1 sentinel is what the first-ever call sees.
	if (!sCarImportBandInit)
		CarImportCityBandReset();

	// Already owns a band - or was refused one (the CIV_CLUT_GUEST_CITIES sentinel is
	// also recorded, so a refused city stays refused rather than being retried and
	// possibly taking a band that appeared free in the meantime). Stability is the point.
	if (sCarImportBandByCity[city] >= 0)
		return sCarImportBandByCity[city];

	// Take the lowest free band, 0..CIV_CLUT_GUEST_CITIES-1. If none is free the loop
	// falls through with band == CIV_CLUT_GUEST_CITIES, which CarImportBankRow turns
	// into a refusal - the three-guest budget is real, not a thing to widen here.
	for (band = 0; band < CIV_CLUT_GUEST_CITIES; band++)
	{
		int taken = 0;

		for (c = 0; c < CITY_COUNT; c++)
		{
			if (sCarImportBandByCity[c] == band)
			{
				taken = 1;
				break;
			}
		}

		if (!taken)
			break;
	}

	sCarImportBandByCity[city] = band;

	return band;
}

// JERICHO: how many guest cities get a palette block.
//
// WAS 2, and the reason was the base half: one guest city's palettes cost ~36 rows of the
// CLUT COLUMN, the CLUT-safe area is 210 rows (256..465), the level's own layout took
// ~157 of them, and a third city ran clutpos to 486 - 21 rows INTO the level font. While
// the import shared that strip, refusing the third was the least-bad answer.
//
// The pool removed the reason: an import's palette rows come from the lower half pool's own column
// now (rows 512..1023, texture.c), so a guest city costs the base half NOTHING. The limit
// is the civ_clut ARRAY (rows CIV_CLUT_IMPORT_ROW..CIV_CLUT_ROWS-1), not the column, and
// the measured cost is nowhere near it - a 3-city mix uses ~28 lower half pool rows out of 512.
//
// Measured at 2, and why it mattered: the third imported car came out with "NO PALETTE
// BLOCK (refused)" and fell back to the HOST's civ_clut row 0, i.e. an imported car
// wearing a local car's colours. That is what "the imported NPC cars have the wrong
// textures" looks like.
static int CarImportBankRow(int city)
{
	int band = CarImportCityBand(city);
	int base;

	if (band < 0)
		return CIV_CLUT_IMPORT_ROW;

	// Past what the column can pay for: no block at all, so the caller can refuse this
	// city out loud instead of taking a neighbour's rows or the font's.
	if (band >= CIV_CLUT_GUEST_CITIES)
		return -1;

	base = CIV_CLUT_IMPORT_ROW + band * CIV_CLUT_BLOCK_ROWS;

	if (base + CIV_CLUT_BLOCK_ROWS > CIV_CLUT_ROWS)
	{
		printInfo("cross-city: %s wants palette block %d but civ_clut holds %d - staying on the first block\n",
			LevelNames[city], band, (CIV_CLUT_ROWS - CIV_CLUT_IMPORT_ROW) / CIV_CLUT_BLOCK_ROWS);

		return CIV_CLUT_IMPORT_ROW;
	}

	return base;
}

// JERICHO: the civ_clut block a held guest city owns, for a caller that must reason about
// the whole block rather than the rows one built model happened to name. -1 when this city
// has no block at all: it is the host level, it is not held, or the column cannot afford it.
int CarImportPaletteBlockBase(int city)
{
	if (city < 0 || city >= CITY_COUNT || city == GameLevel)
		return -1;

	if (!CarImportCityHeld(city))
		return -1;

	return CarImportBankRow(city);
}

static int CarPalIndexInCity(int tpage, int city);

// JERICHO: the palette row map, printed at exit beside the page check. First which
// imported slot's car reads the import bank, then which city wrote each row - so a
// car whose colours are wrong can be traced to the row it reads and the city that
// overwrote it, instead of being a mystery on one vehicle.
// JERICHO: how many colour columns a row has for a spawned car to choose from.
int CivClutRowMaxSlot(int row)
{
	if (row < 0 || row >= CIV_CLUT_ROWS)
		return 0;

	return sCivClutRowMaxSlot[row];
}

// JERICHO: the same question for one texture_id of a row (0 = the page's own CLUT only).
int CivClutTexMaxSlot(int row, int texid)
{
	if (row < 0 || row >= CIV_CLUT_ROWS || texid < 0 || texid >= 32)
		return 0;

	return sCivClutTexMaxSlot[row][texid];
}

void CarPalRowReport(void)
{
	int r, i, slot, collisions = 0, claimed = 0;

	for (slot = 0; slot < MAX_CAR_RESIDENT_MODELS; slot++)
	{
		int city = GetCarModelSourceCity(slot);

		if (city < 0)
			continue;

		int base = (city == GameLevel) ? 0 : CarImportBankRow(city);

		if (base < 0)
			printInfo("cross-city: palette map - resident slot %d: %s model %d - NO PALETTE BLOCK (refused; civ_clut affords %d guests, so this car has no colours of its own)\n",
				slot, LevelNames[city], residentCarModels[slot], CIV_CLUT_GUEST_CITIES);
		else
			printInfo("cross-city: palette map - resident slot %d: %s model %d reads civ_clut rows %d..%d\n",
				slot, LevelNames[city], residentCarModels[slot], base, base + CIV_CLUT_BLOCK_ROWS - 1);
	}

	for (r = CIV_CLUT_IMPORT_ROW; r < CIV_CLUT_ROWS; r++)
	{
		if (sCivClutRowWriters[r] == 0)
			continue;

		claimed++;

		printInfo("cross-city: palette map - civ_clut row %d (%s) written by", r,
			(sCivClutRowMaxSlot[r] > 0) ? "has colour variants" : "slot 0 only");

		for (i = 0; i < sCivClutRowWriters[r]; i++)
			printInfo(" %s", LevelNames[sCivClutRowCity[r][i]]);

		printInfo("%s\n", (sCivClutRowWriters[r] > 1) ? "   <-- COLLISION (one car wears another's colours)" : "");

		if (sCivClutRowWriters[r] > 1)
			collisions++;
	}

	// JERICHO: and, for the rows that offer nothing, WHICH CARS read them. A colourless row
	// is only actionable once it has a car's name on it - this is that.
	for (r = CIV_CLUT_IMPORT_ROW; r < CIV_CLUT_ROWS; r++)
	{
		if (sCivClutRowMaxSlot[r] > 0 || sBakeRowN[r] == 0)
			continue;

		printInfo("cross-city: palette map - civ_clut row %d offers NO colour variants and is read by", r);

		for (i = 0; i < sBakeRowN[r]; i++)
			printInfo(" slot %d (set %d)", sBakeRowSlot[r][i], sBakeRowSet[r][i]);

		printInfo("\n");
	}

	printInfo("cross-city: palette map - %d of the import bank's %d rows written, %d of them by more than one city\n",
		claimed, CIV_CLUT_ROWS - CIV_CLUT_IMPORT_ROW, collisions);

	// JERICHO: how often the draw had to clamp a spawned variant down to a column that
	// exists. Zero on a stock level is the invariant that matters: the clamp must never
	// touch the host's own rows 0..7, and this is the number that says so.
	printInfo("cross-city: palette map - %d draw-time variants clamped to a column that exists\n", sClutClampCount);
}

#define MAX_CAR_POLYS	(200 * 2) * MAX_CAR_RESIDENT_MODELS

int whichCP = 0;
int baseSpecCP = 0;
CAR_POLY carPolyBuffer[MAX_CAR_POLYS + 1];

/* JERICHO: WHERE buildNewCarFromModel writes, and how far it may go.
 *
 * carPolyBuffer is the LEVEL-LOAD arena: a bump cursor (`whichCP`) that the 12 resident
 * slots fill ONCE at level start, with no way to give any of it back. A car imported
 * MID-MATCH was built into the same bump, so every car change permanently consumed a few
 * hundred entries. Measured: by the fourth or fifth change `whichCP` == MAX_CAR_POLYS and
 * buildNewCarFromModel's loop condition (`newNumPolys < MAX_CAR_POLYS`) was already false,
 * so the model was built with 0 polys - the car was INVISIBLE while its pages still
 * uploaded, which reads as "the VRAM changes but the models are not there".
 *
 * So a hot-loaded car takes a block of its OWN arena instead (JerHotPolyTake), and gives it
 * back when its slot is released (JerHotPolyGive). The level-load build is untouched. */
CAR_POLY* gJerCarPolyBase = carPolyBuffer;
int gJerCarPolyCap = MAX_CAR_POLYS;

#define JER_HOT_POLY_BLOCKS		8
#define JER_HOT_POLY_PER_BLOCK	((MAX_CAR_POLYS / 4))	/* 1200: three builds of a ~250-poly car */
/* One spare entry per block, exactly as the level arena has (carPolyBuffer[MAX_CAR_POLYS + 1]):
 * the build loop tests the cap BEFORE a GT4/FT4 iteration, and such a poly adds TWO records,
 * so a straddling one writes at index cap. Without the spare that entry is the first of the
 * NEXT slot's block. */
#define JER_HOT_POLY_STRIDE		(JER_HOT_POLY_PER_BLOCK + 1)

static CAR_POLY* sJerHotPoly;
static int sJerHotPolyBlockOf[MAX_CAR_RESIDENT_MODELS];
static int sJerHotPolyInited;

static void JerHotPolyInit(void)
{
	int i;

	if (sJerHotPolyInited)
		return;

	sJerHotPolyInited = 1;
	sJerHotPoly = (CAR_POLY*)malloc(sizeof(CAR_POLY) * JER_HOT_POLY_STRIDE * JER_HOT_POLY_BLOCKS);

	/* The table is a static, so it starts ZEROED - which reads as "slot k holds block 0" and
	 * made the first level boundary claim to free 12 blocks. Fill it before anything reads
	 * it. */
	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		sJerHotPolyBlockOf[i] = -1;
}

/* A level boundary: the cars built at load and the hot loads are all going away, so the
 * whole table is free again. Without this, blocks a previous level's hot loads held stay
 * marked taken for the life of the PROCESS, and a level that used all eight leaves the next
 * one unable to hot-load a car at all. Mirrors the gJerHotCarBlockOf reset in models.c. */
void JerHotPolyReset(void)
{
	int i, n = 0;

	JerHotPolyInit();		/* must be initialised before it can be counted */

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		if (sJerHotPolyBlockOf[i] >= 0)
			n++;

		sJerHotPolyBlockOf[i] = -1;
	}

	if (n > 0)
		printInfo("cross-city: the level boundary freed %d hot-load poly block(s)\n", n);
}

/* A free block of the hot polies, or NULL when all eight are held. */
CAR_POLY* JerHotPolyTake(int slot, int* cap)
{
	int b, k, taken;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;

	JerHotPolyInit();

	if (sJerHotPoly == NULL)
		return NULL;

	for (b = 0; b < JER_HOT_POLY_BLOCKS; b++)
	{
		taken = 0;

		for (k = 0; k < MAX_CAR_RESIDENT_MODELS; k++)
		{
			if (sJerHotPolyBlockOf[k] == b)
			{
				taken = 1;
				break;
			}
		}

		if (!taken)
		{
			sJerHotPolyBlockOf[slot] = b;

			/* the block is a FRESH 1200-entry region, so the cursor starts at 0 in it -
			 * the caller saves/restores the level-load cursor around the build */
			if (cap != NULL)
				*cap = JER_HOT_POLY_PER_BLOCK;

			return sJerHotPoly + (b * JER_HOT_POLY_STRIDE);
		}
	}

	return NULL;
}

void JerHotPolyGive(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	sJerHotPolyBlockOf[slot] = -1;
}

/* How many hot-load poly blocks are held, for the run summary. It is the one number that
 * shows whether a level boundary gave them all back. */
int JerHotPolyBlocksUsed(void)
{
	int i, n = 0;

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		if (sJerHotPolyBlockOf[i] >= 0)
			n++;
	}

	return n;
}

/* Save the level-load arena, point builds at `base` (a fresh block, so the cursor restarts
 * at 0 in it), and restore it afterwards. The pair keeps the arena's internals in this file:
 * a caller only has to say "build into this block" and "done". */
static CAR_POLY* sJerArenaSaveBase;
static int sJerArenaSaveCap;
static int sJerArenaSaveCP;

void JerBuildPolyArenaPush(CAR_POLY* base, int cap)
{
	sJerArenaSaveBase = gJerCarPolyBase;
	sJerArenaSaveCap = gJerCarPolyCap;
	sJerArenaSaveCP = whichCP;

	gJerCarPolyBase = base;
	gJerCarPolyCap = cap;
	whichCP = 0;
}

void JerBuildPolyArenaPop(void)
{
	gJerCarPolyBase = sJerArenaSaveBase;
	gJerCarPolyCap = sJerArenaSaveCap;
	whichCP = sJerArenaSaveCP;
}

static int gt3DiagCount = 0;

char LeftLight = 0;
char RightLight = 0;
char TransparentObject = 0;

// [D] [T]
void plotCarPolyB3(int numTris, CAR_POLY *src, SVECTOR *vlist, plotCarGlobals *pg)
{
	int Z;
	int indices;
	u_int FT3rgb;
	SVECTOR *v2;
	SVECTOR *v1;
	SVECTOR *v0;
	POLY_F3 *prim;
	OTTYPE *ot;

	prim = (POLY_F3 *)pg->primptr;
	FT3rgb = pg->intensity;
	ot = pg->ot;

	while (numTris > 0)
	{
		indices = src->vindices;
		v0 = vlist + (indices & 0xff);
		v1 = vlist + (indices >> 8 & 0xff);
		v2 = vlist + (indices >> 16 & 0xff);

		gte_ldv3(v0, v1, v2);

		gte_rtpt();

		gte_nclip();
		gte_stopz(&Z);
		gte_avsz3();

		if (Z > -1) 
		{
			*(u_int*)&prim->r0 = FT3rgb | 0x20000000;

			gte_stsxy3(&prim->x0, &prim->x1, &prim->x2);

			gte_stotz(&Z);

			setPolyF3(prim);
			addPrim(ot + (Z >> 1), prim);

			prim++;
		}

		numTris--;
		src++;
	}

	pg->primptr = (unsigned char*)prim;
}

// [D] [T]
// JERICHO: defined with the GT plotters below, but the FT plotter needs it first - an FT
// poly's clut is a civ_clut INDEX resolved at draw time, exactly like a GT poly's.
static u_short CarClutLookup(plotCarGlobals* pg, int ci, int palette);

void plotCarPolyFT3(int numTris, CAR_POLY *src, SVECTOR *vlist, plotCarGlobals *pg)
{
	int indices;
	int ofse;
	SVECTOR *v2;
	SVECTOR *v1;
	SVECTOR *v0;
	int Z;
	POLY_FT3 *prim;
	OTTYPE *ot;
	int FT3rgb;
	int reg;

	prim = (POLY_FT3 *)pg->primptr;
	FT3rgb = pg->intensity | 0x24000000;
	ot = pg->ot;

	gte_ldrgb(&FT3rgb);

	while (numTris > 0)
	{
		indices = src->vindices;
		v0 = vlist + (indices & 0xff);
		v1 = vlist + (indices >> 8 & 0xff);
		v2 = vlist + (indices >> 16 & 0xff);

		gte_ldv3(v0, v1, v2);

		gte_rtpt();

		gte_nclip();
		gte_stopz(&Z);
		gte_avsz3();

		if (Z > -1) 
		{
			ofse = pg->damageLevel[src->originalindex];
			*(u_int*)&prim->r0 = FT3rgb;
			// JERICHO: `src->clut_uv0 + ofse` adds the damage offset to the WHOLE word, so the
			// dent's UV shift carried into the CLUT id whenever the uv pair was within 128 of
			// 0xFFFF - i.e. denting an FT poly could change which palette that poly used (the
			// "the palette of a dented car goes wrong" report). The GT paths already mask the
			// low word; this now does too, keeping the uv0->uv1 carry the offset needs.
			// JERICHO: the high word is a civ_clut INDEX (baked exactly like a GT poly's), so it
			// is resolved HERE, at draw time - the pin re-points the row's slot 0 when an
			// imported page's CLUTs finally reach VRAM, which is long after the bake. Using
			// the baked word directly is what drew an imported car's underside from the
			// GetClut(960,16) dummy - the "the bottom of the imported car is corrupted" report.
			*(u_int*)&prim->u0 = CarClutLookup(pg, src->clut_uv0 >> 0x10, 0) << 0x10 | ((src->clut_uv0 & 0xffff) + ofse);
			// JERICHO: same for the tpage word - a carry here would dent the poly onto another
			// texture page. Mask the tpage id, keep the uv carry.
			*(u_int*)&prim->u1 = CAR_TPAGE_OF(pg, src->tpage_uv1) | ((src->tpage_uv1 & 0xffff) + ofse);
			*(u_int*)&prim->u2 = src->uv3_uv2 + ofse;

			gte_stsxy3(&prim->x0, &prim->x1, &prim->x2);

			gte_stotz(&Z);

			setPolyFT3(prim);
			addPrim(ot + (Z >> 1), prim);

			prim++;
		}
		numTris--;
		src++;
	}

	pg->primptr = (unsigned char*)prim;
}

// JERICHO: the colour COLUMN a poly may use, bounded by what its row actually holds.
//
// The variant is the spawner's choice (ap.palette, 0..5 in civ_ai.c) but the upload only
// fills the columns a city's lump names, so an unbounded variant reads whatever the
// NEIGHBOURING texture_id's colour happens to be, or an empty slot. That is a car wearing
// someone else's palette, with no other symptom to go on.
//
// Only the import bank is constrained. Slots are 0..5 within a texture_id's 6-entry group,
// where slot 0 is the page's own CLUT and is always refilled - so 0 is always a valid
// answer. Rows 0..7 are the host level's OWN car palettes, filled by a path that records no
// coverage; clamping those to 0 would pin every stock car to one colour, which is a worse
// bug than the one being fixed, so they are left alone.
static int CarClutVariant(int clutIdx, int palette)
{
	int row, texid, max;

	if (clutIdx < 0)
		return palette;

	row = clutIdx / (6 * 32) + 1;		// pg->pciv_clut is &civ_clut[1]
	texid = (clutIdx % (6 * 32)) / 6;

	if (row < CIV_CLUT_IMPORT_ROW || row >= CIV_CLUT_ROWS)
		return palette;

	max = CivClutTexMaxSlot(row, texid);

	if (max < 0)
		max = 0;

	if (palette > max)
	{
		sClutClampCount++;

		return max;
	}

	return palette;
}

// [JERICHO] The CLUT a car BODY poly actually samples, with a fallback when the spawned
// palette names a column this city's lump never filled.
//
// `civ_clut[..][..][0]` (the group's own "page CLUT") is always written by
// buildNewCarFromModel, but the columns above it are only filled for the palette numbers a
// city's lump names. The spawner picks ap.palette from 0..5 regardless, so a column past
// what was uploaded stays 0 - and a CLUT word of 0 is GetClut(0,0), the DISPLAY
// FRAMEBUFFER. The body then paints itself with whatever is on screen: "the car is
// invisible", while its untextured/FT polys (wheel arcs, underside, mirrors) still draw.
// CarClutVariant deliberately does not clamp the host rows (that would pin every stock car
// to one colour), so the empty column is caught HERE instead: fall back to the group's own
// CLUT, which is always filled, rather than to the framebuffer.
static int sCarClutFallbackCount;

static u_short CarClutLookup(plotCarGlobals* pg, int ci, int palette)
{
	int slot = ci + CarClutVariant(ci, palette);
	int j;

	if (pg->pciv_clut[slot] != 0)
		return pg->pciv_clut[slot];

	/* slots of this texture_id's group are pciv_clut[ci - 1 .. ci + 4] */
	if (ci >= 1)
	{
		for (j = 0; j < 6; j++)
		{
			if (pg->pciv_clut[ci - 1 + j] != 0)
			{
				sCarClutFallbackCount++;
				return pg->pciv_clut[ci - 1 + j];
			}
		}
	}

	return 0;
}

// [D] [T]
void plotCarPolyGT3(int numTris, CAR_POLY *src, SVECTOR *vlist, SVECTOR *nlist, plotCarGlobals *pg, int palette)
{
	int Z;
	int otz;	
	SVECTOR* v2;
	SVECTOR *v1;
	SVECTOR *v0;
	u_int indices;
	POLY_GT3 *prim;
	u_int r0,r1,r2;
	int ofse;

	prim = (POLY_GT3 *)pg->primptr;

	int GT3rgb = pg->intensity | 0x34000000;
	gte_ldrgb(&GT3rgb);

	while (numTris > 0)
	{
		indices = src->vindices;

		v0 = vlist + (indices & 0xff);
		v1 = vlist + (indices >> 8 & 0xff);
		v2 = vlist + (indices >> 16 & 0xff);

		gte_ldv3(v0, v1, v2);

		gte_rtpt();
		gte_nclip();

		gte_stopz(&Z);

		gte_avsz3();
		
		gte_stotz(&otz);

		if (Z > -1 && otz > 0)
		{
			indices = src->nindices;

			r0 = (u_int)(ushort)nlist[indices & 0xff].pad;
			r1 = (u_int)(ushort)nlist[indices >> 8 & 0xff].pad;
			r2 = (u_int)(ushort)nlist[indices >> 16 & 0xff].pad;

			*(u_int*)&prim->r0 = (r0 & 0xff) << 0x10 | r0;
			*(u_int*)&prim->r1 = (r1 & 0xff) << 0x10 | r1;
			*(u_int*)&prim->r2 = (r2 & 0xff) << 0x10 | r2;

			ofse = pg->damageLevel[src->originalindex];

			// JERICHO-DIAG: an imported car indexes the second civ_clut bank (rows 8..15),
			// so its clut index is >= 8*192. Log the id it resolves to.
			if ((src->clut_uv0 >> 0x10) >= CIV_CLUT_IMPORT_ROW * 192 && gt3DiagCount < 4)
			{
				gt3DiagCount++;
				printInfo("cross-city: imported GT clut index %d -> CLUT id %04x (palette %d)\n",
					(src->clut_uv0 >> 0x10), CarClutLookup(pg, src->clut_uv0 >> 0x10, palette), palette);
			}

			*(u_int*)&prim->u0 = CarClutLookup(pg, src->clut_uv0 >> 0x10, palette) << 0x10 | (src->clut_uv0 & 0xffff) + ofse;
			// JERICHO: same for the tpage word - a carry here would dent the poly onto another
			// texture page. Mask the tpage id, keep the uv carry.
			*(u_int*)&prim->u1 = CAR_TPAGE_OF(pg, src->tpage_uv1) | ((src->tpage_uv1 & 0xffff) + ofse);
			*(u_int*)&prim->u2 = src->uv3_uv2 + ofse;

			gte_stsxy3(&prim->x0, &prim->x1, &prim->x2);

			setPolyGT3(prim);
			addPrim(pg->ot + (otz >> 1), prim);

			prim++;
		}

		src++;
		numTris--;
	}

	pg->primptr = (unsigned char*)prim;
}


#ifdef DYNAMIC_LIGHTING
void plotCarPolyGT3Lit(int numTris, CAR_POLY* src, SVECTOR* vlist, SVECTOR* nlist, plotCarGlobals* pg, int palette)
{
	int Z;
	int otz;
	SVECTOR* v2;
	SVECTOR* v1;
	SVECTOR* v0;
	u_int indices;
	POLY_GT3* prim;
	u_int r0, r1, r2;
	int ofse;

	prim = (POLY_GT3*)pg->primptr;

	int GT3rgb = pg->intensity | 0x34000000;
	gte_ldrgb(&GT3rgb);

	while (numTris > 0)
	{
		indices = src->vindices;

		v0 = vlist + (indices & 0xff);
		v1 = vlist + (indices >> 8 & 0xff);
		v2 = vlist + (indices >> 16 & 0xff);

		gte_ldv3(v0, v1, v2);

		gte_rtpt();
		gte_nclip();

		gte_stopz(&Z);

		gte_avsz3();

		gte_stotz(&otz);

		if (Z > -1 && otz > 0)
		{
			indices = src->nindices;

			r0 = (u_int)(ushort)nlist[indices & 0xff].pad;
			r1 = (u_int)(ushort)nlist[indices >> 8 & 0xff].pad;
			r2 = (u_int)(ushort)nlist[indices >> 16 & 0xff].pad;

			*(u_int*)&prim->r0 = (r0 & 0xff) << 0x10 | r0;
			*(u_int*)&prim->r1 = (r1 & 0xff) << 0x10 | r1;
			*(u_int*)&prim->r2 = (r2 & 0xff) << 0x10 | r2;

			ofse = pg->damageLevel[src->originalindex];

			// JERICHO-DIAG: an imported car indexes the second civ_clut bank (rows 8..15),
			// so its clut index is >= 8*192. Log the id it resolves to.
			if ((src->clut_uv0 >> 0x10) >= CIV_CLUT_IMPORT_ROW * 192 && gt3DiagCount < 4)
			{
				gt3DiagCount++;
				printInfo("cross-city: imported GT clut index %d -> CLUT id %04x (palette %d)\n",
					(src->clut_uv0 >> 0x10), CarClutLookup(pg, src->clut_uv0 >> 0x10, palette), palette);
			}

			*(u_int*)&prim->u0 = CarClutLookup(pg, src->clut_uv0 >> 0x10, palette) << 0x10 | (src->clut_uv0 & 0xffff) + ofse;
			// JERICHO: same for the tpage word - a carry here would dent the poly onto another
			// texture page. Mask the tpage id, keep the uv carry.
			*(u_int*)&prim->u1 = CAR_TPAGE_OF(pg, src->tpage_uv1) | ((src->tpage_uv1 & 0xffff) + ofse);
			*(u_int*)&prim->u2 = src->uv3_uv2 + ofse;

			gte_stsxy3(&prim->x0, &prim->x1, &prim->x2);

			SVECTOR tmpPos;
			gte_ldv0(v0);
			gte_rtps();
			gte_stsv(&tmpPos);
			GetDLightLevel(&tmpPos, (u_int*)&prim->r0);

			gte_ldv0(v1);
			gte_rtps();
			gte_stsv(&tmpPos);
			GetDLightLevel(&tmpPos, (u_int*)&prim->r1);

			gte_ldv0(v2);
			gte_rtps();
			gte_stsv(&tmpPos);
			GetDLightLevel(&tmpPos, (u_int*)&prim->r2);

			setPolyGT3(prim);
			addPrim(pg->ot + (otz >> 1), prim);

			prim++;
		}

		src++;
		numTris--;
	}

	pg->primptr = (unsigned char*)prim;
}
#endif // DYNAMIC_LIGHTING


// [D] [T]
void plotCarPolyGT3nolight(int numTris, CAR_POLY *src, SVECTOR *vlist, plotCarGlobals *pg, int palette)
{
	int Z;
	int otz;
	SVECTOR* v2;
	SVECTOR* v1;
	SVECTOR* v0;
	u_int indices;
	POLY_FT3* prim;
	int ofse;

	prim = (POLY_FT3*)pg->primptr;

	int GT3rgb = pg->intensity | 0x34000000;
	gte_ldrgb(&GT3rgb);

	while (numTris > 0)
	{
		indices = src->vindices;

		v0 = vlist + (indices & 0xff);
		v1 = vlist + (indices >> 8 & 0xff);
		v2 = vlist + (indices >> 16 & 0xff);

		gte_ldv3(v0, v1, v2);

		gte_rtpt();
		gte_nclip();
		gte_stopz(&Z);

		gte_avsz3();
		gte_stotz(&otz);

		if (Z > -1 && otz > 0)
		{
			*(u_int*)&prim->r0 = GT3rgb;

			ofse = pg->damageLevel[src->originalindex];

			// JERICHO-DIAG: an imported car indexes the second civ_clut bank (rows 8..15),
			// so its clut index is >= 8*192. Log the id it resolves to.
			if ((src->clut_uv0 >> 0x10) >= CIV_CLUT_IMPORT_ROW * 192 && gt3DiagCount < 4)
			{
				gt3DiagCount++;
				printInfo("cross-city: imported GT clut index %d -> CLUT id %04x (palette %d)\n",
					(src->clut_uv0 >> 0x10), CarClutLookup(pg, src->clut_uv0 >> 0x10, palette), palette);
			}

			*(u_int*)&prim->u0 = CarClutLookup(pg, src->clut_uv0 >> 0x10, palette) << 0x10 | (src->clut_uv0 & 0xffff) + ofse;
			// JERICHO: same for the tpage word - a carry here would dent the poly onto another
			// texture page. Mask the tpage id, keep the uv carry.
			*(u_int*)&prim->u1 = CAR_TPAGE_OF(pg, src->tpage_uv1) | ((src->tpage_uv1 & 0xffff) + ofse);
			*(u_int*)&prim->u2 = src->uv3_uv2 + ofse;

			gte_stsxy3(&prim->x0, &prim->x1, &prim->x2);

			setPolyFT3(prim);
			addPrim(pg->ot + (otz >> 1), prim);

			prim++;
		}

		src++;
		numTris--;
	}

	pg->primptr = (unsigned char*)prim;
}

MATRIX save_colour_matrix;
MATRIX save_light_matrix;

// [D] [T]
void setupLightingMatrices(void)
{
	gte_ReadColorMatrix(&save_colour_matrix);
	gte_ReadLightMatrix(&save_light_matrix);

	gte_SetColorMatrix(&colour_matrix);
	gte_SetLightMatrix(&light_matrix);

	if (gTimeOfDay == TIME_NIGHT)
	{
		gte_SetBackColor(48, 48, 48);
	}
	else
	{
		gte_SetBackColor(140, 140, 140);
	}
}

// [D] [T]
void restoreLightingMatrices(void)
{
	gte_SetColorMatrix(&save_colour_matrix);
	gte_SetLightMatrix(&save_light_matrix);
}

// [D] [T]
void ComputeCarLightingLevels(CAR_DATA* cp, char detail)
{
#ifdef PSX
	MATRIX& scratchPadMat = *(MATRIX*)((u_char*)getScratchAddr(0) + 0x100);
#else
	MATRIX scratchPadMat;
#endif

	int doLight;
	int orW, orY;
	MODEL* model;
	int num_norms, count;
	SVECTOR* ppads;
	SVECTOR* norms;
	SVECTOR colour, lightsourcevector;
	CVECTOR c0, c1, c2;
	u_int GT3rgb;

	if (gTimeOfDay == TIME_NIGHT)
	{
		lightsourcevector = night_vectors[GameLevel];
		colour = night_colours[GameLevel];
	}
	else
	{
		lightsourcevector = day_vectors[GameLevel];
		colour = day_colours[GameLevel];
	}

	InvertMatrix(&cp->hd.where, &scratchPadMat);
	SetRotMatrix(&scratchPadMat);

	gte_ldv0(&lightsourcevector);
	gte_rtv0();
	gte_stsv(light_matrix.m[0]);

	colour_matrix.m[0][0] = colour.vx;
	colour_matrix.m[1][0] = colour.vy;
	colour_matrix.m[2][0] = colour.vz;

	orY = ABS(cp->st.n.orientation[1] - cp->ap.qy);
	orW = ABS(cp->st.n.orientation[3] - cp->ap.qw);

	doLight = 0;

	if ((orY + orW > 200) || (cp->lowDetail != (detail | lightning)))
		doLight = 1;

	if ((M_BIT(gTimeOfDay) & (M_BIT(TIME_DAWN) | M_BIT(TIME_DUSK))) && (cp->id & 15) == (CameraCnt & 15))
		doLight = 1;

	if (doLight)
	{
		setupLightingMatrices();

		GT3rgb = combointensity & 0xffffffU | 0x34000000;
		gte_ldrgb(&GT3rgb);

		cp->ap.qy = cp->st.n.orientation[1];
		cp->ap.qw = cp->st.n.orientation[3];
		cp->lowDetail = detail | lightning;

		// JERICHO: a slot this level did not load a model into has no geometry to
		// light, and dereferencing the NULL pointer was the access violation the
		// crash dump named (ComputeCarLightingLevels +0x1ED, rva 0x812D). Skip the
		// normals pass, but still restore the matrices set up above -- bailing out
		// entirely would leave the lighting matrices applied to whatever draws next.
		if (cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS)
			model = detail ? gCarCleanModelPtr[cp->ap.model] : gCarLowModelPtr[cp->ap.model];
		else
			model = NULL;

		if (model != NULL)
		{
			num_norms = model->num_point_normals / 3;
			norms = GET_MODEL_DATA(SVECTOR, model, point_normals);

			ppads = gTempCarVertDump[cp->id];
			count = num_norms;// +1;

			while (count >= 0)
			{
				gte_ldv3(&norms[0], &norms[1], &norms[2]);
				gte_ncct();
				gte_strgb3(&c0, &c1, &c2);

				ppads[0].pad = *(short*)&c0;
				ppads[1].pad = *(short*)&c1;
				ppads[2].pad = *(short*)&c2;

				count--;
				norms += 3;
				ppads += 3;
			}
		}

		restoreLightingMatrices();
	}
}

// [D] [T]
void DrawWheelObject(MODEL* model, SVECTOR* verts, int transparent, int wheelnum)
{
	ushort clut;
	ushort tpage;
	int i;
	int Z;
	int otZ;
	int combo;
	POLY_FT4* poly;
	POLYFT4* src;
	u_int dim;
	u_int bright;

	src = GET_MODEL_DATA(POLYFT4, model, poly_block);
	poly = (POLY_FT4*)current->primptr;

	clut = texture_cluts[src->texture_set][src->texture_id];
	tpage = texture_pages[src->texture_set] | 0x20;


	// This controls the color of the wheels
	if (gTimeOfDay > -1)
	{
		if (gTimeOfDay < TIME_NIGHT)
		{
			bright = combointensity & 0xffffffU | 0x2c000000;
			dim = (combointensity & 0xfcfcfcU) >> 2 | 0x2c000000;
		}
		else if (gTimeOfDay == TIME_NIGHT)
		{
			combo = (combointensity & 0xffU) / 3;
			combo = combo << 0x10 | combo << 8 | combo;

			bright = combo | 0x2c000000;
			dim = (combo & 0xfcfcfc) >> 2 | 0x2c000000;
		}
	}

	i = model->num_polys;

	while (i-- != -1)
	{
		gte_ldv3(&verts[src->v0], &verts[src->v1], &verts[src->v2]);

		gte_rtpt();
		gte_nclip();

		gte_stopz(&Z);
		gte_stsxy0(&poly->x0);

		gte_ldv0(&verts[src->v3]);

		gte_rtps();
		gte_avsz4();

		gte_stotz(&otZ);

		if (otZ > 2)
		{
			setPolyFT4(poly);
			addPrim(current->ot + (otZ >> 1) + 5, poly);

			if (i < 2 || Z < 0)
				*(u_int*)&poly->r0 = 0x2c000000;
			else if (((i ^ wheelnum >> 1) & 1) == 0)
				*(u_int*)&poly->r0 = dim;
			else
				*(u_int*)&poly->r0 = bright;

			setSemiTrans(poly, transparent);

			gte_stsxy3(&poly->x1, &poly->x3, &poly->x2);

			*(u_short*)&poly->u0 = *(u_short*)&src->uv0;
			*(u_short*)&poly->u1 = *(u_short*)&src->uv1;
			*(u_short*)&poly->u2 = *(u_short*)&src->uv3;
			*(u_short*)&poly->u3 = *(u_short*)&src->uv2;
			poly->clut = clut;
			poly->tpage = tpage;

			poly++;
		}
		src++;
	}
	current->primptr = (char*)poly;
}

// [D] [T]
void DrawCarWheels(CAR_DATA *cp, MATRIX *RearMatrix, VECTOR *pos, int zclip)
{
	short wheelSize;
	int FW1z, FW2z;
	int BW1z, BW2z;
	int FrontWheelIncrement, BackWheelIncrement;
	int sizeScale;
	int wheelnum;
	SVECTOR* VertPtr;
	SVECTOR* wheelDisp;
	WHEEL* wheel;
	int car_id;
	MODEL* WheelModelBack;
	MODEL* WheelModelFront;
	MODEL* model;
	SVECTOR* bend;
	SVECTOR wheelVerts[24];	// per-wheel copy for the damage transform
	int numWheelVerts;

#ifdef PSX
	MATRIX& WheelPos = *(MATRIX*)((u_char*)getScratchAddr(0) + sizeof(MATRIX) * 2);
	SVECTOR& sWheelPos = *(SVECTOR*)((u_char*)getScratchAddr(0) + sizeof(MATRIX) * 2 + sizeof(VECTOR));
	static_assert(sizeof(MATRIX) * 2 + sizeof(VECTOR) + sizeof(SVECTOR) * 25 < 1024 - sizeof(_pct), "Scratchpad overflow");
#else
	VECTOR WheelPos;
	SVECTOR sWheelPos;
#endif

	D_CHECK_ERROR(cp < car_data, "Invalid car");

	car_id = CAR_INDEX(cp);

	BackWheelIncrement = FrontWheelIncrement = cp->hd.wheel_speed >> 8;

	if (cp->wheelspin != 0)
		BackWheelIncrement = 700;

	if (cp->hd.wheel[0].locked != 0)
		FrontWheelIncrement = 0;

	if (cp->hd.wheel[3].locked != 0)
		BackWheelIncrement = 0;

	if (FrontWheelIncrement + 400U < 801)
		WheelModelFront = gCleanWheelModelPtr;
	else
		WheelModelFront = gFastWheelModelPtr;

	if (BackWheelIncrement + 400U < 801)
		WheelModelBack = gCleanWheelModelPtr;
	else
		WheelModelBack = gFastWheelModelPtr;

	wheelSize = car_cosmetics[cp->ap.model].wheelSize;
	
	sizeScale = (wheelSize * 14142) / 10000;

	// rotate wheel verts

	FW1z = FIXEDH(RSIN(FrontWheelRotation[car_id]) * sizeScale);
	FW2z = FIXEDH(RCOS(FrontWheelRotation[car_id]) * sizeScale);

	VertPtr = GET_MODEL_DATA(SVECTOR, WheelModelFront, vertices);

	VertPtr[8].vz = FW1z;
	VertPtr[15].vz = FW1z;
	VertPtr[11].vy = FW1z;
	VertPtr[12].vy = FW1z;

	VertPtr[9].vy = -FW1z;
	VertPtr[14].vy = -FW1z;
	VertPtr[10].vz = -FW1z;
	VertPtr[13].vz = -FW1z;

	VertPtr[8].vy = FW2z;
	VertPtr[15].vy = FW2z;
	VertPtr[9].vz = FW2z;
	VertPtr[14].vz = FW2z;

	VertPtr[10].vy = -FW2z;
	VertPtr[13].vy = -FW2z;
	VertPtr[11].vz = -FW2z;
	VertPtr[12].vz = -FW2z;

	VertPtr[23].vz = 0;
	VertPtr[22].vz = 0;
	VertPtr[21].vy = 0;
	VertPtr[20].vy = 0;
	VertPtr[19].vy = 0;
	VertPtr[18].vy = 0;
	VertPtr[17].vz = 0;
	VertPtr[16].vz = 0;

	VertPtr[23].vy = wheelSize;
	VertPtr[22].vy = wheelSize;
	VertPtr[21].vz = wheelSize;
	VertPtr[20].vz = wheelSize;

	VertPtr[19].vz = -wheelSize;
	VertPtr[18].vz = -wheelSize;
	VertPtr[17].vy = -wheelSize;
	VertPtr[16].vy = -wheelSize;

	BW1z = FIXEDH(RSIN(BackWheelRotation[car_id]) * sizeScale);
	BW2z = FIXEDH(RCOS(BackWheelRotation[car_id]) * sizeScale);

	VertPtr = GET_MODEL_DATA(SVECTOR, WheelModelBack, vertices);
	
	VertPtr[8].vz = BW1z;
	VertPtr[15].vz = BW1z;
	VertPtr[11].vy = BW1z;
	VertPtr[12].vy = BW1z;

	VertPtr[9].vy = -BW1z;
	VertPtr[14].vy = -BW1z;
	VertPtr[10].vz = -BW1z;
	VertPtr[13].vz = -BW1z;

	VertPtr[8].vy = BW2z;
	VertPtr[15].vy = BW2z;
	VertPtr[9].vz = BW2z;
	VertPtr[14].vz = BW2z;

	VertPtr[10].vy = -BW2z;
	VertPtr[13].vy = -BW2z;
	VertPtr[11].vz = -BW2z;
	VertPtr[12].vz = -BW2z;

	VertPtr[23].vz = 0;
	VertPtr[22].vz = 0;
	VertPtr[21].vy = 0;
	VertPtr[20].vy = 0;
	VertPtr[19].vy = 0;
	VertPtr[18].vy = 0;
	VertPtr[17].vz = 0;
	VertPtr[16].vz = 0;

	VertPtr[23].vy = wheelSize;
	VertPtr[22].vy = wheelSize;
	VertPtr[21].vz = wheelSize;
	VertPtr[20].vz = wheelSize;

	VertPtr[19].vz = -wheelSize;
	VertPtr[18].vz = -wheelSize;
	VertPtr[17].vy = -wheelSize;
	VertPtr[16].vy = -wheelSize;

	// per-wheel yaw: built per wheel in the draw loop below, so each wheel's
	// rotation matrix is its own (steering input for the fronts, none for the
	// rears, plus that wheel's own damage deviation)

	wheelDisp = car_cosmetics[cp->ap.model].wheelDisp;
	wheel = cp->hd.wheel;
	wheelnum = 0;

	// JERICHO-HOOK: query per-wheel damage bend -> modules (crumple package)
	{
		JER_ARGS_QUERY_PTR jerArgs;

		jerArgs.carId = cp->id;
		jerArgs.result = NULL;
		jer_fire(JER_EVENT_GET_WHEEL_BEND, &jerArgs);
		bend = (SVECTOR*)jerArgs.result;
	}

	do {
		if ((wheelnum & 1) != 0)
			model = WheelModelBack;
		else
			model = WheelModelFront;

		VertPtr = GET_MODEL_DATA(SVECTOR, model, vertices);

		// per-wheel copy: both wheels of an axle share the model's vertex
		// buffer, so the crumple damage transform needs its own buffer
		numWheelVerts = 0;
		if (model->num_vertices > 0 && model->num_vertices <= 24)
		{
			numWheelVerts = model->num_vertices;
			memcpy(wheelVerts, VertPtr, numWheelVerts * sizeof(SVECTOR));
		}

		if (cp->ap.flags & (1 << wheelnum)) // [A] used appearance flags to store hubcap presence
		{
			model = gDamWheelModelPtr;
		}

		// Nattdy - crumple wheel damage: draw the wheel at the same bent local
		// position the physics raycast uses (wheelDisp + bend), so a crooked
		// wheel looks exactly as crooked as it behaves.
		{
			SVECTOR wheelDispBent = *wheelDisp;

			if (bend != NULL)
			{
				wheelDispBent.vx += bend[wheelnum].vx;
				wheelDispBent.vy += bend[wheelnum].vy;
				wheelDispBent.vz += bend[wheelnum].vz;
			}

			if ((wheelnum & 2) == 0)
				sWheelPos.vx = 17 - wheelDispBent.vx;
			else
				sWheelPos.vx = -17 - wheelDispBent.vx;

			sWheelPos.vz = -wheelDispBent.vz;
			sWheelPos.vy = (-wheelSize - wheelDispBent.vy) - wheel->susCompression + 14;
		}

		gte_SetRotMatrix(RearMatrix);
		gte_ldv0(&sWheelPos);

		gte_rtv0();

		gte_stlvl(&WheelPos);

		WheelPos.vx += pos->vx;
		WheelPos.vy += pos->vy;
		WheelPos.vz += pos->vz;

		gte_SetTransVector(&WheelPos);

		// Nattdy - crumple wheel damage: every wheel gets its OWN yaw matrix.
		// Front wheels follow the steering input (cp->wheel_angle), rear
		// wheels are unsteered; a bent wheel adds its steering deviation
		// (lateral bend -> up to wheelSteerScale degrees) so it visibly
		// BREAKS from the transform it normally follows, and a hit spanning
		// an axle skews each wheel by its own damage.
		// JERICHO-HOOK: the deviation scales are owned by the crumple module
		{
			MATRIX wheelMat, steerMat;
			JER_ARGS_WHEEL_PARAMS jerScales;
			int steerAngle = 0;
			int steerScale;

			jerScales.carId = cp->id;
			jerScales.frontScale = 0;
			jerScales.rearScale = 0;
			jerScales.scrubForce = 0;
			jer_fire(JER_EVENT_GET_WHEEL_PARAMS, &jerScales);

			steerScale = (wheelnum & 1) ? jerScales.rearScale : jerScales.frontScale;

			if (bend != NULL)
				steerAngle += FIXEDH(bend[wheelnum].vx * steerScale);

			if ((wheelnum & 1) == 0)
				steerAngle += cp->wheel_angle;

			steerMat.m[0][0] = RCOS(steerAngle);
			steerMat.m[0][2] = RSIN(steerAngle);
			steerMat.m[1][1] = ONE;
			steerMat.m[2][1] = 0;
			steerMat.m[1][2] = 0;
			steerMat.m[1][0] = 0;
			steerMat.m[0][1] = 0;
			steerMat.m[2][0] = -steerMat.m[0][2];
			steerMat.m[2][2] = steerMat.m[0][0];

			MulMatrix0(RearMatrix, &steerMat, &wheelMat);
			gte_SetRotMatrix(&wheelMat);
		}

		// Nattdy - crumple wheel damage: slant this wheel's mesh by its bend
		// (camber from the lateral bend, toe from the longitudinal bend),
		// applied to the per-wheel copy AFTER the spin rotation. The wheel
		// position itself already includes the bend via sWheelPos above.
		{
			// JERICHO-HOOK: wheel draw -> modules (crumple package, visual mesh;
			// hide=1 skips the wheel entirely, e.g. a totaled wreck)
			JER_ARGS_DRAW_WHEEL jerArgs;

			jerArgs.carId = cp->id;
			jerArgs.wheelnum = wheelnum;
			jerArgs.verts = (numWheelVerts > 0) ? wheelVerts : NULL;
			jerArgs.numVerts = numWheelVerts;
			jerArgs.hide = 0;
			jer_fire(JER_EVENT_DRAW_WHEEL, &jerArgs);

			if (jerArgs.hide == 0)
			{
				if (numWheelVerts > 0)
					DrawWheelObject(model, wheelVerts, TransparentObject, wheelnum);
				else
					DrawWheelObject(model, VertPtr, TransparentObject, wheelnum);
			}
		}

		wheelDisp++;
		wheel++;
		wheelnum++;
	} while (wheelnum < 4);
}

// [D] [T]
void PlayerCarFX(CAR_DATA *cp)
{
	int WheelSpeed;
	WheelSpeed = cp->hd.wheel_speed;

	if (WheelSpeed + 199U < 0x4b0c7)
	{
		if (cp->wheel_angle < -200)
			AddIndicatorLight(cp, 0);
		else if (cp->wheel_angle > 200)
			AddIndicatorLight(cp, 1);
	}

	if( WheelSpeed < 0 && cp->thrust > 0 ||
		WheelSpeed > 0 && cp->thrust < 0)
	{
		AddBrakeLight(cp);
	}

	if (WheelSpeed < 0 && cp->thrust < 0)
		AddReverseLight(cp);
}

// [D] [T]
void plotNewCarModel(CAR_MODEL* car, int palette, int flatColor)
{
#ifdef PSX
	plotCarGlobals& _pg = *(plotCarGlobals*)((u_char*)getScratchAddr(0) + 1024 - sizeof(plotCarGlobals) - sizeof(_pct));
#else
	plotCarGlobals _pg;
#endif

	u_int lightlevel;
	u_int underIntensity;
	SVECTOR v = { 0, -4096, 0 };

	lightlevel = combointensity | 0x3000000;

	if (car == NULL)
		return;

	if (gTimeOfDay > -1)
	{
		if (gTimeOfDay < TIME_NIGHT)
			lightlevel = combointensity | 0x30000000;
		else if (gTimeOfDay == TIME_NIGHT)
			lightlevel = 0x302a2a2a;
	}

	setupLightingMatrices();

	gte_ldv0(&v);
	gte_ldrgb(&lightlevel);

	gte_nccs();

	_pg.primptr = (u_char*)current->primptr;
	_pg.intensity = 0;
	_pg.pciv_clut = (u_short*)&civ_clut[1];
	_pg.damageLevel = (u_char*)gTempCarUVPtr;
	_pg.pageIndirect = car->imported;	// JERICHO: imported models carry an index (CAR_TPAGE_OF)

	_pg.ot = (OTTYPE*)(current->ot + 28);

	gte_strgb(&underIntensity);

	// draw wheel arcs
	plotCarPolyB3(car->numB3, car->pB3, car->vlist, &_pg);
	_pg.intensity = (flatColor >= 0) ? flatColor : (underIntensity & 0xffffff);

	// draw car bottom
	_pg.ot = (OTTYPE*)(current->ot + 16);
	plotCarPolyFT3(car->numFT3, car->pFT3, car->vlist, &_pg);

	// draw car body
	_pg.ot = (OTTYPE*)(current->ot + 4);
	if (flatColor >= 0)
	{
		// flat body colour (0 = totaled black, else e.g. an icy cyan): gouraud
		// shading off ("damping off"), the packed colour used as the intensity
		_pg.intensity = flatColor;
		plotCarPolyGT3nolight(car->numGT3, car->pGT3, car->vlist, &_pg, palette);
	}
	else if (gTimeOfDay == TIME_NIGHT)
	{
		_pg.intensity = (combointensity & 0xfcfcf0U) >> 2;
#ifdef DYNAMIC_LIGHTING
		(gEnableDlights ? plotCarPolyGT3Lit : plotCarPolyGT3)(car->numGT3, car->pGT3, car->vlist, car->nlist, &_pg, palette);
#else
		plotCarPolyGT3nolight(car->numGT3, car->pGT3, car->vlist, &_pg, palette);
#endif // DYNAMIC_LIGHTING
	}
	else
	{
		_pg.intensity = combointensity & 0xffffff;
#ifdef DYNAMIC_LIGHTING
		(gEnableDlights ? plotCarPolyGT3Lit : plotCarPolyGT3)(car->numGT3, car->pGT3, car->vlist, car->nlist, &_pg, palette);
#else
		plotCarPolyGT3(car->numGT3, car->pGT3, car->vlist, car->nlist, &_pg, palette);
#endif
	}

	current->primptr = (char*)_pg.primptr;

	restoreLightingMatrices();
}

// [A]
void startBuildNewCars(int isSpecial)
{
	if(isSpecial)
		baseSpecCP = whichCP;
	else
		whichCP = 0;
}


/* Cross-city car data (see cars.h): -1 = the level's own city. Resolved in one
 * place so the .MDL/.COS/.DEN loaders all agree on which LEVELS\<CITY> folder
 * to read. Set by a module via JER_EVENT_CAR_DATA_SOURCE. */
int gCarDataSourceLevel = -1;

/* JERICHO: this names the LEVELS\<CITY> folder the per-car CARMODEL_* overrides
 * are read from, so it must answer for the Driver 1 car-data cities too. Those
 * have no override folder on the disc, which is fine: the loaders test the file
 * with FileExists first and simply find nothing, so a D1 car keeps the cosmetics
 * derived for it. Note the overrides resolve under gDataFolder, so a D1 override
 * folder would have to be created under DRIVER2\ - see SPOOFED_CITIES.md. */
const char* GetCarDataFolder(void)
{
	if (gCarDataSourceLevel >= 0 && gCarDataSourceLevel < CITY_COUNT)
		return LevelNames[gCarDataSourceLevel];

	return LevelNames[GameLevel];
}

#if USE_PC_FILESYSTEM
static char* CarModelTypeNames[] = {
	"CLEAN",
	"DAMAGED",
	"LOW",
};

// [A] loads car model from file
char* LoadCarModelFromFile(char* dest, int modelNumber, int type)
{
	char* mem;
	char filename[64];

	sprintf(filename, "LEVELS\\%s\\CARMODEL_%d_%s.MDL", GetCarDataFolder(), modelNumber, CarModelTypeNames[type - 1]);
	if (FileExists(filename))
	{
		mem = (char*)(dest ? dest : (_other_buffer + modelNumber * 0x10000 + (type - 1) * 0x4000));

		// get model from file
		Loadfile(filename, mem);
		return mem;
	}

	return NULL;
}
#endif

// [D] [T]
MODEL* GetCarModel(char* src, char** dest, int KeepNormals)
{
	int size;
	MODEL* model;
	char* mem;

	model = (MODEL*)*dest;

	if (KeepNormals == 0)
		size = ((MODEL*)src)->normals;
	else
		size = ((MODEL*)src)->poly_block;

	// if loaded externally don't copy from source lump
	memcpy((u_char*)*dest, (u_char*)src, size);

	if (KeepNormals == 0)
		size = model->normals;
	else
		size = model->poly_block;

	//*dest += size + 2;
	*dest = (char*)((u_intptr)model + size + 3 & ~3);

#if MODEL_RELOCATE_POINTERS
	model->vertices += (int)model;
	model->normals += (int)model;
	model->poly_block = (int)src + model->poly_block;

	if (KeepNormals == 0)
		model->point_normals = 0;
	else
		model->point_normals += (int)model;
#else
	if (KeepNormals == 0)
		model->point_normals = 0;

	model->instance_number = -1;
#endif
	return model;
}

// [D] [T] [A]
// JERICHO: the palette row a built poly may bake. Never negative - see its definition.
static int CarPalIndexForBuild(int tpage, int city);
static void PalBakeSetsDiag(int slot);	/* JERICHO-DIAG (JERICHO_DIAG_PALBAKE) */

void buildNewCarFromModel(int index, int detail, char* polySrc, MODEL* model)
{
	// JERICHO: only an imported car's polys get the set remap. index is the resident
	// slot, so this is where import-ness is known. Set for every build, so the value
	// can never leak from one car to the next.
	int imported = (index >= 0 && GetCarModelSourceCity(index) >= 0);

	// JERICHO: the city this model is built FROM, so the palette lookups below can be
	// told rather than left to guess. GetCarPalIndex answers "which row?" by asking every
	// held city in order and taking the first hit - with three cities that is the first
	// city, so a model from the second or third was baked with the wrong city's rows and
	// rendered in its colours. -1 means "no source city": the lookup falls back to the old
	// behaviour for the host and for anything unaffected by imports.
	int srcCity = (index >= 0) ? GetCarModelSourceCity(index) : -1;

	CarSetRemapEnable(imported);

	int newNumPolys;
	int i, pass;
	ushort clut;
	u_char ptype, carid;
	u_char *polyList;
	CAR_POLY* cp;
	CAR_MODEL* car;
	MODEL* polySrcModel;

	polySrcModel = (MODEL*)polySrc;
	car = detail ? &NewCarModel[index] : &NewLowCarModel[index];
	car->imported = imported;

	if (polySrcModel == NULL || (polySrcModel->shape_flags & 0xfffffff) > 0x800000)
	{
		car->numGT3 = 0;
		car->numFT3 = 0;
		car->numB3 = 0;
		return;
	}

	model->instance_number = -1;
	polySrcModel->instance_number = -1;

	car->vlist = GET_MODEL_DATA(SVECTOR, model, vertices);
	car->nlist = GET_MODEL_DATA(SVECTOR, model, point_normals);
;
	for (pass = 0; pass < 3; pass++)
	{
		polyList = GET_RELOC_MODEL_DATA(u_char, polySrcModel, poly_block);

		if (pass == 1)
			car->pFT3 = gJerCarPolyBase + whichCP;
		else if (pass == 0)
			car->pGT3 = gJerCarPolyBase + whichCP;
		else if (pass == 2)
			car->pB3 = gJerCarPolyBase + whichCP;

		newNumPolys = whichCP;

		for (i = 0; newNumPolys < gJerCarPolyCap && i < model->num_polys; i++)
		{
			ptype = *polyList;

			cp = gJerCarPolyBase + newNumPolys;

			switch (ptype & 0x1f) 
			{
				case 0:
				case 18:
					if (pass == 2)	// F3
					{
						cp->vindices = M_INT_4R(polyList[1], polyList[2], polyList[3], 0);
						cp->originalindex = i;

						newNumPolys++;
					}
					break;
				case 1:
				case 19:
					if (pass == 2)	// F4
					{
						cp->vindices = M_INT_4R(polyList[4], polyList[5], polyList[6], 0); 
						cp->originalindex = i;

						cp++;

						cp->vindices = M_INT_4R(polyList[4], polyList[6], polyList[7], 0);
						cp->originalindex = i;

						newNumPolys += 2;
					}
					break;
				case 20:
					if (pass == 1)	// FT3
					{
						POLYFT3* pft3 = (POLYFT3*)polyList;

						// JERICHO: record the set for EVERY model, host or imported - the pool
						// needs to know which car pages any model actually names.
						CarModelSetsAdd(index, pft3->texture_set);
									
						cp->vindices = M_INT_4R(pft3->v0, pft3->v1, pft3->v2, 0);
						// JERICHO: an FT poly's clut is baked as the civ_clut INDEX, exactly like a GT
						// poly's, and resolved at draw time (plotCarPolyFT3). Baking the VALUE asked
						// texture_cluts[DST_SET], and for an IMPORTED car that index only holds the
						// real CLUTs once the car is DRAWN (the pin), so the bake read the (960,16)
						// dummy and the underside kept it forever.
						carid = CarPalIndexForBuild(pft3->texture_set, srcCity);
						clut = (carid - 1) * 6 * 32 + pft3->texture_id * 6;

						civ_clut[carid][pft3->texture_id][0] = texture_cluts[pft3->texture_set][pft3->texture_id];

						cp->clut_uv0 = M_INT_2(clut, *(ushort*)&pft3->uv0);
						cp->tpage_uv1 = M_INT_2(CAR_BAKE_TPAGE(imported, pft3->texture_set), *(ushort*)&pft3->uv1);
						cp->uv3_uv2 = *(ushort*)&pft3->uv2;
						cp->originalindex = i;

						newNumPolys++;
					}
					break;
				case 21:
					if (pass == 1)	// FT4
					{
						POLYFT4* pft4 = (POLYFT4*)polyList;

						CarModelSetsAdd(index, pft4->texture_set);

						cp->vindices = M_INT_4R(pft4->v0, pft4->v1, pft4->v2, 0);
						// JERICHO: same index bake as the FT3 case above.
						carid = CarPalIndexForBuild(pft4->texture_set, srcCity);
						clut = (carid - 1) * 6 * 32 + pft4->texture_id * 6;

						civ_clut[carid][pft4->texture_id][0] = texture_cluts[pft4->texture_set][pft4->texture_id];

						cp->clut_uv0 = M_INT_2(clut, *(ushort *)&pft4->uv0);
						cp->tpage_uv1 = M_INT_2(CAR_BAKE_TPAGE(imported, pft4->texture_set), *(ushort*)&pft4->uv1);
						cp->uv3_uv2 = *(ushort*)&pft4->uv2;
						cp->originalindex = i;

						cp++;
						
						cp->vindices = M_INT_4R(pft4->v0, pft4->v2, pft4->v3, 0);
						// JERICHO: the FT4's second triangle names its set/texid through polyList.
						carid = CarPalIndexForBuild(polyList[1], srcCity);
						clut = (carid - 1) * 6 * 32 + polyList[2] * 6;

						civ_clut[carid][polyList[2]][0] = texture_cluts[polyList[1]][polyList[2]];

						cp->clut_uv0 = M_INT_2(clut, *(ushort*)&pft4->uv0);
						cp->tpage_uv1 = M_INT_2(CAR_BAKE_TPAGE(imported, polyList[1]), *(ushort*)&pft4->uv2);
						cp->uv3_uv2 = *(ushort*)&pft4->uv3;
						cp->originalindex = i;

						newNumPolys += 2;
					}
					break;
				case 22:
					if (pass == 0) // GT3
					{
						POLYGT3* pgt3 = (POLYGT3*)polyList;

						CarModelSetsAdd(index, pgt3->texture_set);

						carid = CarPalIndexForBuild(pgt3->texture_set, srcCity);
						CarBakeRowNote(carid, index, pgt3->texture_set);
						clut = (carid - 1) * 6 * 32 + pgt3->texture_id * 6;

						civ_clut[carid][pgt3->texture_id][0] = texture_cluts[pgt3->texture_set][pgt3->texture_id];

						cp->vindices = M_INT_4R(pgt3->v0, pgt3->v1, pgt3->v2, 0);
						cp->nindices = M_INT_4R(pgt3->n0, pgt3->n1, pgt3->n2, 0);
						cp->clut_uv0 = M_INT_2(clut, *(ushort*)&pgt3->uv0);
						cp->tpage_uv1 = M_INT_2(CAR_BAKE_TPAGE(imported, pgt3->texture_set), *(ushort *)&pgt3->uv1);
						cp->uv3_uv2 = *(ushort *)&pgt3->uv2;
						cp->originalindex = i;

						newNumPolys++;
					}
					break;
				case 23:
					if (pass == 0)  // GT4
					{
						POLYGT4* pgt4 = (POLYGT4*)polyList;

						CarModelSetsAdd(index, pgt4->texture_set);

						// JERICHO: no imported special-case - GetCarPalIndex already returns a
						// second-bank row (8..15) for a page belonging to the imported city, so
						// an import reads its own palettes through exactly the host's formula.
						// A GT poly's clut_uv0 high word is a civ_clut INDEX, not a CLUT id.
						carid = CarPalIndexForBuild(pgt4->texture_set, srcCity);
						CarBakeRowNote(carid, index, pgt4->texture_set);
						clut = (carid - 1) * 6 * 32 + pgt4->texture_id * 6;

						civ_clut[carid][pgt4->texture_id][0] = texture_cluts[pgt4->texture_set][pgt4->texture_id];

						cp->vindices = M_INT_4R(pgt4->v0, pgt4->v1, pgt4->v2, 0);
						cp->nindices = M_INT_4R(pgt4->n0, pgt4->n1, pgt4->n2, 0);
						cp->clut_uv0 = M_INT_2(clut, *(ushort*)&pgt4->uv0);
						cp->tpage_uv1 = M_INT_2(CAR_BAKE_TPAGE(imported, pgt4->texture_set), *(ushort*)&pgt4->uv1);
						cp->uv3_uv2 = *(ushort*)&pgt4->uv2;
						cp->originalindex = i;

						cp++;

						cp->vindices = M_INT_4R(pgt4->v0, pgt4->v2, pgt4->v3, 0);
						cp->nindices = M_INT_4R(pgt4->n0, pgt4->n2, pgt4->n3, 0);
						cp->clut_uv0 = M_INT_2(clut, *(ushort*)&pgt4->uv0);
						cp->tpage_uv1 = M_INT_2(CAR_BAKE_TPAGE(imported, pgt4->texture_set), *(ushort *)&pgt4->uv2);
						cp->uv3_uv2 = *(ushort *)&pgt4->uv3;
						cp->originalindex = i;

						newNumPolys += 2;
					}
			}

			polyList += PolySizes[ptype & 0x1f];
		}

		if (pass == 1) 
			car->numFT3 = newNumPolys - whichCP;
		else if (pass == 0)
			car->numGT3 = newNumPolys - whichCP;
		else if (pass == 2) 
			car->numB3 = newNumPolys - whichCP;

		whichCP = newNumPolys;
	}

	// JERICHO: a build that produced NOTHING while the model has polys. The one cause in
	// practice is the poly arena being full (whichCP at MAX_CAR_POLYS): the cursor is a bump
	// allocator that a mid-match hot load advances and can never give back, so after a few
	// car changes every later model builds 0 polys - the car is INVISIBLE while its pages
	// still upload, which reads as "the textures load but the model is not there". Say it,
	// because the symptom is otherwise indistinguishable from a bad page.
	if ((car->numGT3 + car->numFT3 + car->numB3) == 0 && model->num_polys > 0)
		printInfo("JERICHO: car model build for slot %d produced 0 polys of %d (poly arena %d of %d used) - this car is not drawn\n",
			index, model->num_polys, whichCP, gJerCarPolyCap);

	PalBakeSetsDiag(index);	/* JERICHO-DIAG (JERICHO_DIAG_PALBAKE) */
}

// [D] [T]
void MangleWheelModels(void)
{
	UV_INFO tmpUV2;
	u_char tmpUV;
	u_int v0, v1, v2;
	POLYFT4*src;
	MODEL *m;
	int i, j;

	for (i = 0; i < 3; i++)
	{
		if (i == 1)
			m = gFastWheelModelPtr;
		else if (i == 2)
			m = gDamWheelModelPtr;
		else
			m = gCleanWheelModelPtr;

		// do some fuckery swaps
		src = GET_MODEL_DATA(POLYFT4, m, poly_block);
		
		v0 = *(u_int *)&src[2].v0;
		v1 = *(u_int *)&src[2].uv0;
		v2 = *(u_int *)&src[2].uv2;
		*(u_int *)src = *(u_int *)(src + 2);
		*(u_int *)&src->v0 = v0;
		*(u_int *)&src->uv0 = v1;
		*(u_int *)&src->uv2 = v2;
		src->color = src[2].color;

		v0 = *(u_int *)&src[3].v0;
		v1 = *(u_int *)&src[3].uv0;
		v2 = *(u_int *)&src[3].uv2;
		*(u_int *)(src + 1) = *(u_int *)(src + 3);
		*(u_int *)&src[1].v0 = v0;
		*(u_int *)&src[1].uv0 = v1;
		*(u_int *)&src[1].uv2 = v2;
		src[1].color = src[3].color;

		v0 = *(u_int *)&src[4].v0;
		v1 = *(u_int *)&src[4].uv0;
		v2 = *(u_int *)&src[4].uv2;
		*(u_int *)(src + 2) = *(u_int *)(src + 4);
		*(u_int *)&src[2].v0 = v0;
		*(u_int *)&src[2].uv0 = v1;
		*(u_int *)&src[2].uv2 = v2;
		src[2].color = src[4].color;

		v0 = *(u_int *)&src[6].v0;
		v1 = *(u_int *)&src[6].uv0;
		v2 = *(u_int *)&src[6].uv2;
		*(u_int *)(src + 3) = *(u_int *)(src + 6);
		*(u_int *)&src[3].v0 = v0;
		*(u_int *)&src[3].uv0 = v1;
		*(u_int *)&src[3].uv2 = v2;
		src[3].color = src[6].color;

		src[2].v2 = 23;
		src[2].v3 = 22;
		src[3].v0 = 21;
		src[3].v1 = 20;
		src[3].v2 = 19;
		src[3].v3 = 18;
		src[2].v1 = 17;
		src[2].v0 = 16;

		tmpUV = (src->uv0.u + src->uv2.u) / 2;
		src[3].uv3.u = tmpUV;
		src[3].uv2.u = tmpUV;
		src[3].uv1.u = tmpUV;
		src[3].uv0.u = tmpUV;
		src[2].uv3.u = tmpUV;
		src[2].uv2.u = tmpUV;
		src[2].uv1.u = tmpUV;
		src[2].uv0.u = tmpUV;

		tmpUV = (src->uv0.v + src->uv2.v) / 2;
		src[3].uv3.v = tmpUV;
		src[3].uv2.v = tmpUV;
		src[3].uv1.v = tmpUV;
		src[3].uv0.v = tmpUV;
		src[2].uv3.v = tmpUV;
		src[2].uv2.v = tmpUV;
		src[2].uv1.v = tmpUV;
		src[2].uv0.v = tmpUV;

		m->num_polys = 4;

		for (j = 0; j < 2; j++)
		{
			tmpUV2 = src->uv0;
			
			src->uv0 = src->uv1;
			src->uv1 = src->uv2;
			src->uv2 = src->uv3;
			src->uv3 = tmpUV2;

			src++;
		}

	}

	// HACK: Show clean model only in Rio.
	//if (GameLevel == 3) 
	//	gFastWheelModelPtr = gCleanWheelModelPtr;
}

// [D] [T]
// defined below, next to GetCarPalIndex
static int CarPalIndexInCity(int tpage, int city);

static int sPalDumpCount[CITY_COUNT];	// JERICHO-DIAG PAL: per CITY, so a guest is never crowded out

// JERICHO: how many entries the last walk put into each civ_clut row. The import's palette
// upload uses it to report rows a built model READS but the city's lump has no entries for -
// those rows stay empty and their polys draw their own page CLUT (slot 0). Nothing is
// fabricated for them (see the upload loop).
static int sJerRowWritten[CIV_CLUT_ROWS];

static void ProcessPalletLumpForRows(char *lump_ptr, int lump_size, int city, const unsigned char *rowNeeded)
{
	ushort clutValue;
	int *buffPtr;
	int texnum;
	int palette;
	u_short *clutTablePtr;
	u_short clutTable[320];
	int tpageindex;
	int total_cluts;
	int clut_number;
	int skipped = 0;
	int entriesRead = 0;		// entries walked (bounds the lump when no size was given)
	int reused = 0;			// entries that reused a CLUT already in VRAM (no new row)
	int deferred = 0;		// JERICHO: stored CLUTs the row filter left out of VRAM
	int borrowed = 0;		// JERICHO: skipped CLUTs a needed entry referred to (uploaded on demand)
	// JERICHO: WHERE this city's CLUTs go.
	//
	// The host's own palettes are the LEVEL's, so they belong in the base column. An
	// imported city's are not: uploading those there is what took the base strip from 85
	// safe free rows to 18 in a two-guest mashup, reached into the CD-icon/spool band at
	// 433..464, and left the host's other cars sharing rows with a foreign table. The pool
	// column (x960..1023, rows 512..1023) is 512 rows of space nothing stock computes, so a
	// guest city's table goes THERE - 32..38 rows each, measured.
	//
	// The cursor rather than an allocation because this walk does not know how many rows it
	// needs until it has walked the lump; it commits the rows it used at the end.
	RECT16 importClut;
	RECT16 *dst = &clutpos;
	int importRow0 = 0, importX0 = 0;

	if (CarImportCityHeld(city) && city != GameLevel)
	{
		JerLowerPoolClutCursor(&importClut);
		dst = &importClut;
		importRow0 = importClut.y;
		importX0 = importClut.x;
	}

	const int rowStart = dst->y;	// to report how many COLUMN ROWS this load consumed

	// JERICHO: the deferred-upload state (see ProcessImportedPaletteRows).
	//
	// In the filtered pass (rowNeeded != NULL) a stored CLUT whose row the imported model
	// does not draw from is NOT uploaded - that is the reclaim. But an entry can REFER to
	// an earlier CLUT by index (`clut_number`, i.e. `clutTable[n]`), and that earlier CLUT
	// may belong to a row we are skipping. Such a reference must still be given its real
	// colours, or a needed part draws from nothing. So each stored CLUT's source keeps a
	// pointer, and a reference to a skipped one uploads it on the spot.
	int* clutSrc[320];		// where each stored CLUT's 16 colours are in the lump
	u_short clutDone[320];		// the id index k was uploaded to, 0 = not uploaded
	int clutStored = 0;		// ALL stored CLUTs seen, so `clut_number` indices line up
	// The Clut id stored for each page, so an upload that has run out of budget can
	// reuse THE SAME PAGE's palette rather than the city's very first one. The old
	// fallback (`clutTable[0]`) is the "crazy colours" report: it is a palette for a
	// different texture, so the car draws with another car's colours instead of slightly
	// wrong ones. Pages are few (a city has 5-8 car page columns), so a tiny table does.
	ushort pageClut[16];
	int pageOf[16];
	int nPage = 0;
	int k;

	// JERICHO-DIAG: what the mask asked for vs what the lump actually holds, per row.
	int histRow[CIV_CLUT_ROWS];
	int histNeed[CIV_CLUT_ROWS];
	int histWrite[CIV_CLUT_ROWS];

	memset(histRow, 0, sizeof(histRow));
	memset(histNeed, 0, sizeof(histNeed));
	memset(histWrite, 0, sizeof(histWrite));

	for (k = 0; k < 16; k++)
	{
		pageClut[k] = 0;
		pageOf[k] = -1;
	}

	total_cluts = *(int*)lump_ptr;
	
	if (total_cluts == 0)
		return;

	buffPtr = (int*)(lump_ptr + 4);
	clutTablePtr = (u_short*)clutTable;

	for (;;)
	{
		int palidx, needed;	// JERICHO: the row this entry belongs to, and whether we keep it

		// BOUNDS, from whichever limit the CALLER actually gave us. The two callers differ:
		//   * the LEVEL's own lump is a pointer into the level file plus the LUMP_PALLET
		//     segment's byte size (main.c records it, texture.c passes it), so it gets the
		//     exact byte check;
		//   * a deferred IMPORT lump is a pointer+size from gCarImports, with no such
		//     contract, so it gets the same check - and if it arrives with NO size at all
		//     the header's own count is the backstop.
		//
		// `total_cluts` is NOT the record count and must never be used as one: on VEGAS it
		// is 200 while the lump holds 525 records, so treating it as the count stopped the
		// walk two-thirds of the way and left the tail's palette rows (VEGAS rows 5 and 6)
		// empty - which is what made a car whose page lives there draw its panels from
		// different fallback colours. It is a CLUT count for clutTable sizing, nothing more.
		//
		// The terminator is a lone `-1` int (4 bytes), NOT a full 16-byte record: every
		// city's lump ends that way (levpalette.py measures 4 bytes left over in each).
		// So guard just the 4 bytes needed to read *buffPtr, test the terminator, and only
		// then require the full record. The old +12 check ran before the terminator test
		// and tripped 4 bytes early on EVERY city, printing a false "the lump is mis-sized"
		// and reporting clutStored (the inline-CLUT count) as "entries read" - which is
		// what made a correct walk look like a ~2.6x stride bug. The walk always read all
		// 525 records; it just never got to see the -1.
		if (lump_size > 0)
		{
			if ((char*)buffPtr + 4 > (char*)lump_ptr + lump_size)
			{
				printInfo("cross-city: %s palettes: ran out of lump after %d entries with no terminator - the lump is truncated; stopping\n",
					LevelNames[city], entriesRead);
				break;
			}
		}
		else if (entriesRead >= total_cluts)
		{
			break;		/* no size given, and the header count is spent (conservative) */
		}

		if (*buffPtr == -1)
			break;

		// A record is four ints; require them all before reading.
		if (lump_size > 0 && (char*)buffPtr + 16 > (char*)lump_ptr + lump_size)
		{
			printInfo("cross-city: %s palettes: truncated record at entry %d - the lump is truncated; stopping\n",
				LevelNames[city], entriesRead);
			break;
		}

		palette = buffPtr[0];
		texnum = buffPtr[1];
		tpageindex = buffPtr[2];
		clut_number = buffPtr[3];
		buffPtr += 4;
		entriesRead++;

		palidx = CarPalIndexInCity(tpageindex, city);

		/* JERICHO-DIAG (JERICHO_DIAG_PAL=1): what the palette lump actually holds. The
		 * traffic cars pick ap.palette from 0..5, so a model's colour VARIETY is the
		 * number of distinct `palette` fields its (row, texnum) group carries: one field
		 * per group = one colour per model, however the draw is fixed. */
		// JERICHO-DIAG: capped PER CITY, not globally. A single 250-entry cap let the host's
		// walk fill the budget, so a GUEST city's entries - the ones a cross-city run is
		// actually about - were never printed at all. That is the failure mode VRAM.md warns
		// about: "an assertion that can be suppressed by volume is not an assertion".
		if (getenv("JERICHO_DIAG_PAL") != NULL && sPalDumpCount[city] < 250)
		{
			sPalDumpCount[city]++;
			printInfo("JERICHO-DIAG PAL: entry %d city=%d filtered=%d row=%d texnum=%d palette=%d tpage=%d clut=%d\n",
				sPalDumpCount[city], city, (rowNeeded != NULL), palidx, texnum, palette, tpageindex, clut_number);
		}

		if (palidx < 0)
		{
			// JERICHO: resolve an unclassifiable set to the SAME row the BUILD resolves it to
			// (CarPalIndexForBuild), so the row a model is baked to read is a row this walk
			// writes. The two used to disagree: the bake sent such a set to the source city's
			// OWN first row, the walk sent it to the HOST's row 0 - so an imported model read
			// rows nothing ever wrote and those polys drew colourless ("several corrupted
			// palettes" on an imported special). For the host level this is unchanged: its
			// block base is -1 and the fallback stays row 0, which is what the host's own
			// build already reads.
			int base = CarImportPaletteBlockBase(city);

			palidx = (base >= 0) ? base : 0;
		}

		needed = (rowNeeded == NULL) ? 1 : rowNeeded[palidx];

		if (rowNeeded != NULL && palidx >= 0 && palidx < CIV_CLUT_ROWS)
		{
			histRow[palidx]++;

			if (needed)
				histNeed[palidx]++;
		}

		// JERICHO: the reclaim. This CLUT belongs to a row the imported model does not
		// draw from, so it is neither uploaded nor entered into civ_clut - nothing reads
		// it. Its SOURCE is still recorded: a needed entry can refer to it by index.
		if (clut_number == -1 && !needed)
		{
			if (clutStored < 320)
			{
				clutSrc[clutStored] = buffPtr;
				clutDone[clutStored] = 0;
				clutTable[clutStored] = 0;
			}

			if (clutStored < 320)
				clutStored++;
			deferred++;
			buffPtr += 8;
			continue;		}

		if (clut_number == -1)
		{
			// store clut
			// JERICHO: an imported city's palette upload stops at CAR_CLUT_IMPORT_LIMIT so it
			// cannot eat the pin band's reserve. Past it, reuse the palette already stored
			// for THIS PAGE - the car's own page, so its colours stay its own - and only
			// fall back to the city's first palette if this page has not stored one yet.
			// The reclaim's trigger is "this upload is eating a SHARED column". For the host that
			// is the base strip past CAR_CLUT_IMPORT_LIMIT. For a guest the column is the
			// pool's OWN, so there is nothing to protect and the check becomes the literal
			// one: is there a row left at all.
			int outOfRoom = (dst == &clutpos) ? (clutpos.y > CAR_CLUT_IMPORT_LIMIT)
											 : (dst->y + 4 > JER_VRAM_TOTAL_ROWS);

			if (CarImportCityHeld(city) && city != GameLevel && outOfRoom)
			{
				int hit = -1;

				for (k = 0; k < nPage; k++)
					if (pageOf[k] == tpageindex)
						hit = k;

				clutValue = (hit >= 0) ? pageClut[hit]
					: ((clutTablePtr > clutTable) ? clutTable[0] : 0);
				skipped++;
			}
			else
			{
				int* src = buffPtr;

				LoadImage(dst, (u_long*)buffPtr);
				buffPtr += 8;

				clutValue = GetClut(dst->x, dst->y);
				IncrementClutNum(dst);

				// JERICHO: remember this CLUT's source, so a later entry that REFERS to it
				// can be served even if this one were skipped (see the reference branch).
				if (clutStored < 320)
				{
					clutSrc[clutStored] = src;
					clutDone[clutStored] = clutValue;
					clutTable[clutStored] = clutValue;
				}

				// Clamped: clutStored bounds the REFERENCE branch below, and it must not
				// grow past the table it indexes.
				if (clutStored < 320)
					clutStored++;

				*clutTablePtr++ = clutValue;

				// remember this page's first stored palette (the fallback above)
				if (nPage < 16)
				{
					int seen = -1;

					for (k = 0; k < nPage; k++)
						if (pageOf[k] == tpageindex)
							seen = k;

					if (seen < 0)
					{
						pageOf[nPage] = tpageindex;
						pageClut[nPage] = clutValue;
						nPage++;
					}
				}
			}
		}
		else
		{
			// use stored clut
			// clut_number is DATA out of the lump: bound it before it indexes the table.
			// An index past the table, or past what has actually been stored, is a lump
			// this code must not trust -- it is the second half of the +0x300 fault.
			if (clut_number < 0 || clut_number >= 320 || clut_number >= clutStored)
			{
				printInfo("cross-city: %s palettes: entry %d refers to CLUT %d, which is not stored (%d stored) - using the first palette\n",
					LevelNames[city], clutStored, clut_number, clutStored);
				clut_number = (clutStored > 0) ? 0 : -1;
			}

			clutValue = (clut_number >= 0) ? clutTable[clut_number] : 0;
			reused++;

			// JERICHO: in the filtered pass that CLUT may have been SKIPPED - it belongs to
			// a row we are not keeping. A needed entry refers to it, so it must still be
			// given its real colours: upload it now from its recorded source. Costs a row
			// only when a reference crosses rows, and without it that part paints with
			// nothing at all.
			if (clutValue == 0 && clut_number >= 0 && clut_number < clutStored)
			{
				if (clutDone[clut_number] == 0)
				{
					LoadImage(dst, (u_long*)clutSrc[clut_number]);
					clutDone[clut_number] = GetClut(dst->x, dst->y);
					IncrementClutNum(dst);
					borrowed++;
				}

				clutValue = clutDone[clut_number];
			}
		}

		if (needed)
		{
			// JERICHO: the lump's palette field is used as palette+1 into a 6-slot group,
			// where slot 0 is the page's own CLUT, so a value past 4 writes into the NEXT
			// texture_id's group. Real lumps are 0..4 (levpalette.py measures it), but a
			// malformed one would now clobber a texture_id a squeezed upload kept - so it
			// is bounded at the point of use rather than trusted.
			int slot = palette + 1;

			if (slot < 1)
				slot = 1;
			if (slot > 5)
				slot = 5;

			civ_clut[palidx][texnum][slot] = clutValue;

			// JERICHO: which rows this walk actually put something in. The uploader uses it
			// to find rows a built model READS but this city's lump has NO entries for.
			if (palidx >= 0 && palidx < CIV_CLUT_ROWS)
				sJerRowWritten[palidx]++;

			if (palidx >= 0 && palidx < CIV_CLUT_ROWS)
				histWrite[palidx]++;

			if (slot > sCivClutRowMaxSlot[palidx])
				sCivClutRowMaxSlot[palidx] = slot;
			if (texnum >= 0 && texnum < 32 && slot > sCivClutTexMaxSlot[palidx][texnum])
				sCivClutTexMaxSlot[palidx][texnum] = slot;
		}
	}

	// JERICHO: a row the built model reads but this city's lump cannot fill. The polys on it
	// then draw their OWN page CLUT (slot 0) - which is the page's real colours - rather than
	// a neighbour's. Nothing is fabricated to cover the gap. Say it here, where the mismatch
	// is known, rather than leaving it to be rediscovered as "some cars still look wrong".
	// Measured on a HAVANA-in-CHICAGO import: the model names rows 14 and 15 (its
	// special-body pair) and the lump holds 0 entries that map to either, while the 30..140
	// entries it does hold map to rows 8..13, which the model does not read.
	if (rowNeeded != NULL)
	{
		int blockBase = CarImportBankRow(city);
		int blockEnd = (blockBase < 0) ? 0 : blockBase + CIV_CLUT_BLOCK_ROWS;

		// Only this city's OWN block: the mask spans every guest's rows, so without this a
		// city would be blamed for a neighbour's rows it was never asked to fill.
		for (k = blockBase; k < blockEnd && k < CIV_CLUT_ROWS; k++)
			if (k >= 0 && rowNeeded[k] && histWrite[k] == 0)
				printInfo("cross-city: %s palettes: row %d is READ by the built model but the lump wrote nothing to it (%d of its entries map to other rows) - the row stays empty and its polys draw their own page CLUT (slot 0)\n",
					LevelNames[city], k, histRow[k]);
	}

	// JERICHO: commit the rows the guest walk actually used. IncrementClutNum wraps
	// x 960 -> 1024 to x = 960 and y++, so the ending position says how many whole rows it
	// took, plus one if it stopped mid-row.
	if (dst != &clutpos)
		JerLowerPoolClutAdvance((dst->y - importRow0) + ((dst->x > importX0) ? 1 : 0));

	// JERICHO: always report, not only when something was skipped. This is the number
	// that decides whether the CLUT column fits: the import's whole-table load is what
	// pushes the level's own layout past the font (cars.h, VRAM.md §6), so the count of
	// CLUTs and the rows they took has to be visible without a skip happening first.
	printInfo("cross-city: %s palettes: %d CLUT(s) in the lump, %d row(s) taken (rows %d -> %d), %d reusing an earlier CLUT, %d deferred by the row filter, %d of those borrowed back, %d past the row %d budget\n",
		LevelNames[city], total_cluts, dst->y - rowStart, rowStart, dst->y, reused, deferred, borrowed, skipped, CAR_CLUT_IMPORT_LIMIT);
}

// JERICHO: the deferred import palette lumps, ONE PER CITY (see
// ProcessImportedPaletteRows). A level can hold more than one city's car data now
// (models.c's gCarImports[4]), and each city's own LUMP_PALLET is deferred until
// the built model says which rows it draws from.
static char* sImpPalLump[CITY_COUNT];
static int sImpPalSize[CITY_COUNT];

// JERICHO: the host path, and the entry point every existing caller uses.
//
// An imported city's table is DEFERRED rather than loaded here. Its lump is a WHOLE
// foreign city's - 228 CLUTs, 57 column rows - and the import is ONE model that draws
// from 2 of the bank's 8 rows (measured, CarImportDumpState). Loading all 57 rows is what
// pushes the level's own layout 19 rows past the level font image (cars.h, VRAM.md §6).
// The rows to keep cannot be known here: they come from the built model's poly stream,
// which is walked long after this runs. So the lump is remembered and the real upload
// happens in ProcessImportedPaletteRows, from CarImportPin - the first point the rows
// exist.
static void ProcessPalletLumpForCity(char *lump_ptr, int lump_size, int city)
{
	// This SLOT's city, not the level's one guest city: any city the level holds
	// has its palette table deferred (models.c's CarImportCityHeld).
	if (CarImportCityHeld(city) && city != GameLevel)
	{
		sImpPalLump[city] = lump_ptr;
		sImpPalSize[city] = lump_size;

		printInfo("cross-city: %s palettes: deferred (%d CLUT(s) in the lump) - which rows to keep is not known until the model is built\n",
			LevelNames[city], (lump_ptr != NULL) ? *(int*)lump_ptr : 0);

		return;
	}

	ProcessPalletLumpForRows(lump_ptr, lump_size, city, NULL);
}

// JERICHO: upload an imported city's car palettes for ONLY the civ_clut rows the built
// model names. `rowNeeded` is indexed by civ_clut row (CIV_CLUT_ROWS entries); every
// entry that is set is uploaded, every entry that is not is neither uploaded nor entered
// into civ_clut - nothing reads it, since the model's polys only name its own rows.
//
// A CLUT that a kept entry REFERS to by index is uploaded on demand even when its own row
// was skipped (ProcessPalletLumpForRows), so a cross-row reference cannot leave a part
// pointing at nothing. Returns 1 when a deferred lump was uploaded, 0 when there was none.
int ProcessImportedPaletteRows(const unsigned char* rowNeeded)
{
	int city, uploaded = -1;

	for (city = 0; city < CITY_COUNT; city++)
	{
		int rows = 0, r, base, limit;

		if (sImpPalLump[city] == NULL)
			continue;

		// JERICHO: THIS CITY'S block, not the whole bank. The bank used to admit a single
		// city - a second was REFUSED - so a mashup's other cities got nothing while
		// their cars still read the block. A block each means the rows uploaded here and
		// the row CarPalIndexInCity hands the model are the same rows, for every city.
		base = CarImportBankRow(city);

		if (base < 0)
		{
			// The honest refusal, at the measured threshold: the column can pay for
			// CIV_CLUT_GUEST_CITIES cities' palettes, and this is one more. Refusing is
			// recoverable (this car has no colours); overflowing is not (the glyphs go).
			printInfo("cross-city: %s palettes: REFUSED - civ_clut affords %d guest cities and %s have them (rows %d..%d are the import bank)\n",
				LevelNames[city], CIV_CLUT_GUEST_CITIES, "the others",
				CIV_CLUT_IMPORT_ROW, CIV_CLUT_ROWS - 1);

			sImpPalLump[city] = NULL;
			continue;
		}

		limit = base + CIV_CLUT_BLOCK_ROWS;
		if (limit > CIV_CLUT_ROWS)
			limit = CIV_CLUT_ROWS;

		for (r = base; r < limit; r++)
			if (rowNeeded[r])
			{
				rows++;
				CarPalRowNote(r, city);
			}

		printInfo("cross-city: %s palettes: uploading for %d of its block's %d rows (civ_clut %d..%d)\n",
			LevelNames[city], rows, CIV_CLUT_BLOCK_ROWS, base, limit - 1);

		// The honest refusal, now per city: a city whose own palettes need more rows than
		// a block has loses the excess rather than a neighbour's block.
		if (rows > CIV_CLUT_BLOCK_ROWS)
			printInfo("cross-city: %s palettes: wants %d rows and a block is %d - the excess is not placed (CIV_CLUT_BLOCK_ROWS)\n",
				LevelNames[city], rows, CIV_CLUT_BLOCK_ROWS);

		// JERICHO: a row a built model READS that this city's lump has NO entries for is now
		// LEFT EMPTY - nothing is fabricated.
		//
		// A city's palette table covers the pages its OWN cars use, and an imported model can
		// read more rows than that: measured, CHICAGO's model 8 reads eight rows (civ_clut
		// 8..15) while its file carries colour variants for five.
		//
		// This block used to ALIAS those rows: it copied the first row in the block that has
		// data into every needed row that does not, six colour columns wholesale INCLUDING
		// slot 0. That is the "some panels correct, most wrong, on cars from every Driver 1
		// city" report: slot 0 is the poly's own page CLUT (written at build from
		// texture_cluts[set][texid] and re-pointed by CarImportPin), and CarClutLookup accepts
		// any non-zero slot - so a copied FOREIGN CLUT was drawn as if it were this page's
		// paint. A wrong shade that shadows the right one is worse than an honest empty row.
		//
		// Empty is already handled, and only reachable now that the bake never numbers a car
		// page 0 (so the pin takes every set a model names): CarImportPin fills slot 0 with the
		// poly's own page CLUT, CarClutVariant clamps a spawned palette to the row's real
		// coverage (none -> slot 0), and CarClutLookup falls back to the group's slot 0.
		{
			int r2;

			for (r2 = base; r2 < limit; r2++)
			{
				sJerRowWritten[r2] = 0;		// scope the count to THIS city's walk
			}

			ProcessPalletLumpForRows(sImpPalLump[city], sImpPalSize[city], city, rowNeeded);

			for (r2 = base; r2 < limit; r2++)
			{
				if (sJerRowWritten[r2] == 0 && rowNeeded[r2])
					printInfo("cross-city: %s palettes: row %d is read by a built model but this city's lump has no entries for it - left to the poly's own page CLUT (slot 0), nothing fabricated\n",
						LevelNames[city], r2);
			}
		}

		sImpPalLump[city] = NULL;
		uploaded = city;
	}

	return (uploaded >= 0);
}

// [D] [T]
void ProcessPalletLump(char *lump_ptr, int lump_size)
{
	ProcessPalletLumpForCity(lump_ptr, lump_size, GameLevel);
}

// JERICHO-HOOK: load a cross-city import's own car palettes into civ_clut.
//
// The import's models name their colours by the imported city's palette rows, so those
// palettes have to be in civ_clut for their CLUT ids to resolve. The host level runs
// first and fills rows 0..7 with its own, so an unmodified merge would paint the host's
// cars with the foreign city's colours. CarPalIndexInCity therefore puts this city's
// rows in the second bank (8..15), and ProcessPalletLumpForCity then runs exactly as it
// does for the host - same LUMP_PALLET format, same LoadImage of the colours into VRAM -
// without touching a single host row.
// JERICHO: forget a level's deferred palette work. Called from CarImportResetState so the
// upload happens once per LEVEL. Without it the lumps stay pointing at the previous
// level's palette data (and CarImportPin's sPalDone stays set, so nothing is uploaded for
// the new level at all) - a bug that only shows on the SECOND level of a session, which is
// why it went unnoticed until the lifecycle was audited.
void CarImportPaletteReset(void)
{
	int c, cleared = 0;

	for (c = 0; c < CITY_COUNT; c++)
	{
		if (sImpPalLump[c] != NULL)
			cleared++;

		sImpPalLump[c] = NULL;
		sImpPalSize[c] = 0;
	}

	CarPalRowClear();

	// JERICHO: this runs on the per-level reset path (models.c InitCarImport), and it is
	// what makes the deferred upload happen once per LEVEL. Logged because the failure it
	// fixes is invisible on a single-level run: the SECOND level used to keep CarImportPin's
	// sPalDone set and upload nothing, with the previous level's lumps still in place. If
	// this line appears once per level with the count from the level before, the lifecycle
	// is right; a missing line on level 2 is the bug back.
	printInfo("cross-city: palette state cleared for the new level (%d lump(s) were deferred by the previous one)\n", cleared);
}

void ProcessImportedPalette(void)
{
	int city;

	// JERICHO: a fresh level means a fresh palette map - the row ownership recorded
	// here is what CarPalRowReport prints at exit.
	CarPalRowClear();

	// EVERY held city, not just the first: each has its own deferred LUMP_PALLET to
	// hand to the pin (ProcessPalletLumpForCity defers it), so a second city's cars
	// are described by their OWN palettes instead of being silently coloured by the
	// first city's rows.
	for (city = 0; city < CITY_COUNT; city++)
	{
		int size = 0;
		char* pallet;

		if (!CarImportCityHeld(city) || city == GameLevel)
			continue;

		pallet = GetCarImportPalletForCity(city, &size);

		if (pallet == NULL || size <= 0)
			continue;

		ProcessPalletLumpForCity(pallet, size, city);

		printInfo("cross-city: %s car palettes applied to civ_clut rows %d..%d\n",
			LevelNames[city], CIV_CLUT_IMPORT_ROW, CIV_CLUT_ROWS - 1);
	}
}

// JERICHO: apply ONE city's imported car palettes, for a city read in MID-LEVEL (the
// hot load). ProcessImportedPalette() above is a level-start function: it clears the
// whole row map and walks every held city. This does neither and touches only `city`:
// its palette lump is deferred, and the rows themselves are still uploaded lazily on
// the first draw of a car that names them (ProcessImportedPaletteRows, from
// CarImportPin) -- exactly as for a level-load import, so a hot-loaded car's colours
// come from its own city rather than from whichever city the level already had.
// Returns 1 when a lump was deferred, 0 when there is nothing to do.
int CarImportApplyPaletteForCity(int city)
{
	int size = 0;
	char* pallet;

	if (city < 0 || city >= CITY_COUNT || city == GameLevel)
		return 0;			// the level's own palettes are already in place

	if (!CarImportCityHeld(city))
		return 0;			// no data read in for it: nothing to apply

	pallet = GetCarImportPalletForCity(city, &size);

	if (pallet == NULL || size <= 0)
		return 0;

	ProcessPalletLumpForCity(pallet, size, city);

	printInfo("cross-city: %s car palettes deferred MID-LEVEL for the hot load\n",
		LevelNames[city]);

	return 1;
}

// [D] [T]
void DrawCarObject(CAR_MODEL* car, MATRIX* matrix, VECTOR* pos, int palette, CAR_DATA* cp, int detail)
{
	VECTOR modelLocation;
	SVECTOR cog;
	int flatBlack;
	int flatColor;		// -1 = stock shading, 0 = flat black, >0 = flat colour

	// JERICHO-HOOK: body color — a module renders the body flat: black for a
	// totaled wreck, or a flat colour at full brightness (e.g. icy cyan for a
	// frozen car).
	flatBlack = 0;
	flatColor = -1;
	{
		JER_ARGS_CAR_DRAW_COLOR jer;

		jer.car = cp;
		jer.flatBlack = 0;
		jer.tintR = jer.tintG = jer.tintB = -1;
		jer_fire(JER_EVENT_CAR_DRAW_COLOR, &jer);
		flatBlack = jer.flatBlack;

		if (flatBlack)
			flatColor = 0;
		else if (jer.tintR >= 0 || jer.tintG >= 0 || jer.tintB >= 0)
		{
			int r = (jer.tintR < 0) ? 255 : (jer.tintR & 0xff);
			int g = (jer.tintG < 0) ? 255 : (jer.tintG & 0xff);
			int b = (jer.tintB < 0) ? 255 : (jer.tintB & 0xff);

			// POLY/GTE colour words are B<<16 | G<<8 | R (red in the low byte)
			flatColor = (b << 16) | (g << 8) | r;
		}
	}

	cog = cp->ap.carCos->cog;

	// [A] mini cars correct position
	if (ActiveCheats.cheat13 != 0)
	{
		cog.vx <<= 1;
		cog.vy <<= 1;
		cog.vz <<= 1;
	}

	gte_SetRotMatrix(matrix);
	gte_SetTransVector(pos);

	gte_ldv0(&cog);

	gte_rtv0tr();

	gte_stlvnl(&modelLocation);

	gte_SetTransVector(&modelLocation);

	/* JERICHO-DIAG (opt-in, JERICHO_DIAG_CARDRAW=1): what the body is about to
	 * draw - the model, whether it has geometry, and the first poly's baked
	 * texture page + CLUT. This is what tells an invisible car (no verts / null
	 * model) from a mis-textured one (bad page/clut), which the log otherwise
	 * cannot distinguish. */
	if (jerDiagCarDraw())
	{
		static int antCarDrawTick;

		if ((++antCarDrawTick % 90) == 0 && cp != NULL)
		{
			/* histogram the baked texture pages across the model's polys: a car
			 * whose body is invisible but whose mirrors/wheel-wells render is a
			 * model whose polys are split across pages, one of them bad. */
			u_int pg[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
			int cnt[4] = { 0, 0, 0, 0 };
			CAR_POLY* lists[2];
			int counts[2];
			int k, i, np = car->numGT3 + car->numFT3;

			lists[0] = car->pGT3; counts[0] = car->numGT3;
			lists[1] = car->pFT3; counts[1] = car->numFT3;

			for (k = 0; k < 2; k++)
			{
				for (i = 0; i < counts[k]; i++)
				{
					u_int p = (u_int)(lists[k][i].tpage_uv1 >> 16);
					int j;

					for (j = 0; j < 4; j++)
					{
						if (cnt[j] == 0 || pg[j] == p)
						{
							pg[j] = p;
							cnt[j]++;
							break;
						}
					}
				}
			}

			/* the resolved CLUT across the SIX palettes the spawner can pick, for the
			 * biggest page group of this model. That IS the colour variety the traffic
			 * of this model has: six distinct values = a proper palette set, a value
			 * repeated or zero = a mangled set. */
			plotCarGlobals dbgPg;
			int ci0 = 0;
			int found = 0;

			dbgPg.pciv_clut = (u_short*)&civ_clut[1];

			for (k = 0; k < 2 && !found; k++)
			{
				for (i = 0; i < counts[k]; i++)
				{
					if ((u_int)(lists[k][i].tpage_uv1 >> 16) == pg[0])
					{
						ci0 = lists[k][i].clut_uv0 >> 16;
						found = 1;
						break;
					}
				}
			}

			printInfo("JERICHO-DIAG CARDRAW: car=%d model=%d pal=%d ci=%d polys=%d pg%04x:%d raw[ci-1..ci+5] %04x %04x %04x %04x %04x %04x %04x | res0..5 %04x %04x %04x %04x %04x %04x\n",
				cp->id, cp->ap.model, palette, ci0, np, pg[0], cnt[0],
				(unsigned)dbgPg.pciv_clut[ci0 - 1], (unsigned)dbgPg.pciv_clut[ci0], (unsigned)dbgPg.pciv_clut[ci0 + 1],
				(unsigned)dbgPg.pciv_clut[ci0 + 2], (unsigned)dbgPg.pciv_clut[ci0 + 3], (unsigned)dbgPg.pciv_clut[ci0 + 4],
				(unsigned)dbgPg.pciv_clut[ci0 + 5],
				(unsigned)CarClutLookup(&dbgPg, ci0, 0), (unsigned)CarClutLookup(&dbgPg, ci0, 1),
				(unsigned)CarClutLookup(&dbgPg, ci0, 2), (unsigned)CarClutLookup(&dbgPg, ci0, 3),
				(unsigned)CarClutLookup(&dbgPg, ci0, 4), (unsigned)CarClutLookup(&dbgPg, ci0, 5));

			/* JERICHO-DIAG: the SPLIT detector - every distinct clut index this model's
			 * polys bake, with the colour it resolves to for THIS car's palette (and how
			 * many polys take it). A coherent car answers with ONE colour here; two or
			 * more means some panels are drawn from another palette, which is the
			 * "some panels one colour, some another" report. The ci shows WHICH row and
			 * texture_id each group came from, so the split can be traced to its source
			 * rather than inferred. */
			{
				int uci[24], ures[24], ucnt[24], nu = 0, s, kk, ii;

				for (kk = 0; kk < 2; kk++)
				for (ii = 0; ii < counts[kk]; ii++)
				{
					int ci = lists[kk][ii].clut_uv0 >> 16;
					int res = CarClutLookup(&dbgPg, ci, palette);

					for (s = 0; s < nu; s++)
						if (uci[s] == ci && ures[s] == res)
							break;

					if (s < nu)
						ucnt[s]++;
					else if (nu < 24)
					{
						uci[nu] = ci;
						ures[nu] = res;
						ucnt[nu] = 1;
						nu++;
					}
				}

				for (s = 0; s < nu; s++)
					printInfo("JERICHO-DIAG SPLIT: car=%d model=%d pal=%d ci=%d polys=%d -> CLUT %04x\n",
						cp->id, cp->ap.model, palette, uci[s], ucnt[s], (unsigned)ures[s]);
			}
		}
	}

	plotNewCarModel(car, palette, flatColor);
}

/* JERICHO-DIAG: the car-body draw census, off unless JERICHO_DIAG_CARDRAW=1. */
static int jerDiagCarDraw(void)
{
	static int on = -1;

	if (on < 0)
		on = (getenv("JERICHO_DIAG_CARDRAW") != NULL) ? 1 : 0;

	return on;
}

// [D] [T] [A]
void DrawCar(CAR_DATA* cp, int view)
{
	int yVal;
	int maxDamage;
	int WheelSpeed;
	int oboxLenSq;
	CAR_MODEL* CarModelPtr;
	int model;
	CVECTOR col;
	SVECTOR d;
	VECTOR pos, dist;
	VECTOR corners[4];
	MATRIX workmatrix;
	MATRIX wheelmatrix;

	D_CHECK_ERROR(cp < car_data, "Invalid car");
	
	model = cp->ap.model;

	// JERICHO: no geometry for this slot in this level (its model was never
	// loaded) -> there is nothing to draw, and the draw path dereferences the
	// model (the one shared rule, cars.h). Skip the car rather than fault. InitPlayer
	// clamps an unavailable player car to a resident slot, so this is a backstop.
	if (!JerCarSlotUsable(model))
	{
		if (jerDiagCarDraw())
			printInfo("JERICHO-DIAG CARDRAW: SKIP car=%d model=%d (no geometry)\n", cp->id, model);

		return;
	}

	// draw car lights in for InCar camera
	if (player[view].cameraView == 2 && cp->id == player[view].cameraCarId)
	{
		if (cp->ap.damage[0] < 500)
			LeftLight = 2;
		else if (cp->ap.damage[0] < 1000)
			LeftLight = 1;
		else
			LeftLight = 0;

		if (cp->ap.damage[1] < 500)
			RightLight = 2;
		else if (cp->ap.damage[1] < 1000)
			RightLight = 1;
		else
			RightLight = 0;

		if (gLightsOn == 0)
			return;

		if (lightsOnDelay[cp->id])
			return;

		col.r = 128;
		col.g = 120;
		col.b = 110;

		PlacePoolForCar(cp, &col, 1, 1);
		return;
	}

	pos.vx = cp->hd.where.t[0];
	pos.vz = cp->hd.where.t[2];
	pos.vy = -cp->hd.where.t[1];

	SetFrustrumMatrix();

	if (FrustrumCheck(&pos, 800) == -1)
		return;

	// corners for frustrum checking of big cars
	corners[0].vx = corners[1].vx = pos.vx + cp->hd.oBox.radii[0].vx;
	corners[0].vx += cp->hd.oBox.radii[2].vx;
	corners[1].vx -= cp->hd.oBox.radii[2].vx;

	corners[2].vx = corners[3].vx = pos.vx - cp->hd.oBox.radii[0].vx;
	corners[2].vx += cp->hd.oBox.radii[2].vx;
	corners[3].vx -= cp->hd.oBox.radii[2].vx;

	corners[0].vz = corners[1].vz = pos.vz + cp->hd.oBox.radii[0].vz;
	corners[0].vz += cp->hd.oBox.radii[2].vz;
	corners[1].vz -= cp->hd.oBox.radii[2].vz;

	corners[2].vz = corners[3].vz = pos.vz - cp->hd.oBox.radii[0].vz;
	corners[2].vz += cp->hd.oBox.radii[2].vz;
	corners[3].vz -= cp->hd.oBox.radii[2].vz;

	corners[3].vy = pos.vy;
	corners[2].vy = pos.vy;
	corners[1].vy = pos.vy;
	corners[0].vy = pos.vy;

	if (FrustrumCheck(corners, 0) == -1 &&
		FrustrumCheck(corners + 1, 0) == -1 &&
		FrustrumCheck(corners + 2, 0) == -1 &&
		FrustrumCheck(corners + 3, 0) == -1)
	{
		return;
	}

	d.vx = cp->hd.oBox.location.vx - camera_position.vx;
	d.vy = -camera_position.vy - cp->hd.oBox.location.vy;
	d.vz = cp->hd.oBox.location.vz - camera_position.vz;

	dist.vx = d.vx * cp->hd.oBox.radii[0].vx + d.vy * cp->hd.oBox.radii[0].vy + d.vz * cp->hd.oBox.radii[0].vz;

	if (dist.vx < 0)
		dist.vx = -dist.vx;

	dist.vy = d.vx * cp->hd.oBox.radii[1].vx + d.vy * cp->hd.oBox.radii[1].vy + d.vz * cp->hd.oBox.radii[1].vz;
	if (dist.vy < 0)
		dist.vy = -dist.vy;

	dist.vz = d.vx * cp->hd.oBox.radii[2].vx + d.vy * cp->hd.oBox.radii[2].vy + d.vz * cp->hd.oBox.radii[2].vz;
	if (dist.vz < 0)
		dist.vz = -dist.vz;

	oboxLenSq = cp->hd.oBox.length[0];

	if (dist.vx < oboxLenSq * oboxLenSq)
	{
		oboxLenSq = cp->hd.oBox.length[1];

		if (dist.vy < oboxLenSq * oboxLenSq)
		{
			oboxLenSq = cp->hd.oBox.length[2];

			if (dist.vz < oboxLenSq * oboxLenSq)
				return;
		}
	}

	pos.vx -= camera_position.vx;
	pos.vy -= camera_position.vy;
	pos.vz -= camera_position.vz;

	Apply_Inv_CameraMatrix(&pos);

	num_cars_drawn++;

	// JERICHO-HOOK: car body draw — visual pitch/roll/yaw on a render-only
	// copy of the draw matrix (physics/collision matrices are untouched).
	// By default the rotation applies to the BODY model only, and the wheels keep
	// the un-rotated draw matrix so they stay level - which is right for a body
	// lean. A module doing something that moves the WHOLE car (a knock: buck, rock,
	// squat) sets rigidWheels and gets the same matrix for the wheels, so they go
	// with the body instead of being left behind.
	{
		MATRIX bodyMatrix = cp->hd.drawCarMat;
		JER_ARGS_CAR_DRAW jerArgs;

		jerArgs.car = cp;
		jerArgs.matrix = &bodyMatrix;
		jerArgs.view = view;
		jerArgs.rigidWheels = 0;
		jer_fire(JER_EVENT_CAR_DRAW, &jerArgs);

		MulMatrix0(&inv_camera_matrix, &bodyMatrix, &workmatrix);
		MulMatrix0(&inv_camera_matrix, jerArgs.rigidWheels ? &bodyMatrix : &cp->hd.drawCarMat, &wheelmatrix);
	}

	// [A] there was mini cars cheat
	// we need full blown mini cars with physics support
	if (ActiveCheats.cheat13 != 0)
	{
		int i;
		for (i = 0; i < 3; i++)
		{
			workmatrix.m[i][0] >>= 1;
			workmatrix.m[i][1] >>= 1;
			workmatrix.m[i][2] >>= 1;
		}
	}

	// to check if car is flipped
	yVal = cp->hd.where.m[1][1];

	// LOD switching
	if (pos.vz <= CAR_LOD_SWITCH_DISTANCE && gForceLowDetailCars == 0 || cp->controlType == CONTROL_TYPE_PLAYER)
	{
		int doSmoke = 0;
		int smokeType, smokeStart, smokeEnd, black_offset;
		int flame, flameStart, flameEnd;

		WheelSpeed = cp->hd.speed * 8192;
		maxDamage = MaxPlayerDamage[0];

		if (cp->controlType == CONTROL_TYPE_PLAYER)
		{
			if(*cp->ai.padid >= 0 && *cp->ai.padid < 2)
				maxDamage = MaxPlayerDamage[*cp->ai.padid];
		}

		flame = 0;
		flameStart = 50;
		flameEnd = 100;

		if (cp->totalDamage >= maxDamage)
		{
			/* the fire is gated to a car that has almost stopped (and a reversing
			 * one wraps WheelSpeed and never burns) - see the hook below, which
			 * keeps that gate whatever a module asks for */
			if (WheelSpeed + 59999U < 119999)
				flame = 1;

			doSmoke = 2;
		}
		else
		{
			if (cp->ap.damage[0] > 2000 || cp->ap.damage[1] > 2000)
			{
				if (cp->ap.damage[0] > 3000 || cp->ap.damage[1] > 3000)
					doSmoke = 2;
				else
					doSmoke = 1;
			}
		}

		smokeType = (doSmoke == 2) ? SMOKE_BLACK : ((doSmoke == 1) ? SMOKE_WHITE : 0);
		smokeStart = 100;
		smokeEnd = (doSmoke == 2) ? 500 : 400;
		black_offset = (doSmoke == 2);

		/* JERICHO-HOOK: the damage smoke and fire decision for this car, on the
		 * values the stock rule produced above. A module may rewrite them and set
		 * handled = 1, which is how a module puts the ladder on its own rule -
		 * health rather than ap.damage per zone (JER_ARGS_CAR_DAMAGE_FX). With
		 * handled = 0 the values above are used unchanged, so a build with no
		 * module handling this emits exactly what the stock code did. */
		{
			JER_ARGS_CAR_DAMAGE_FX jerDmg;
			int cap = maxDamage;
			int dmg = cp->totalDamage;
			int hp;

			hp = (cap > 0) ? (100 - (dmg * 100) / cap) : 0;

			if (hp < 0)
				hp = 0;
			else if (hp > 100)
				hp = 100;

			jerDmg.car = cp;
			jerDmg.health = hp;
			jerDmg.smokeType = smokeType;
			jerDmg.smokeStart = smokeStart;
			jerDmg.smokeEnd = smokeEnd;
			jerDmg.flame = flame;
			jerDmg.flameStart = flameStart;
			jerDmg.flameEnd = flameEnd;
			jerDmg.handled = 0;

			jer_fire(JER_EVENT_CAR_DAMAGE_FX, &jerDmg);

			if (jerDmg.handled)
			{
				smokeType = jerDmg.smokeType;
				smokeStart = jerDmg.smokeStart;
				smokeEnd = jerDmg.smokeEnd;
				flame = jerDmg.flame;
				flameStart = jerDmg.flameStart;
				flameEnd = jerDmg.flameEnd;

				/* the offset was the stock "black sits lower" rule; a module that
				 * picks its own colour keeps it for anything that is not white */
				black_offset = (smokeType != 0 && smokeType != SMOKE_WHITE);
			}
		}

		/* the gates are the smoke pool's budget, not part of the ladder, so they
		 * apply to a module's choice too */
		if (flame && WheelSpeed + 59999U < 119999)
			AddFlamingEngineSized(cp, flameStart, flameEnd);

		if (smokeType != 0 && WheelSpeed + 399999U < 1199999)
			AddSmokingEngineTyped(cp, smokeType, smokeStart, smokeEnd, black_offset, WheelSpeed);

#if ENABLE_GAME_ENCHANCEMENTS
		AddExhaustSmoke(cp, doSmoke > 1, WheelSpeed);
#endif

		SetShadowPoints(cp, corners);
		PlaceShadowForCar(corners, 4, 10, yVal < 0 ? 0 : 2);

		ComputeCarLightingLevels(cp, 1);
		gTempCarUVPtr = gTempHDCarUVDump[cp->id];
		CarModelPtr = &NewCarModel[model];
		CarModelPtr->vlist = gTempCarVertDump[cp->id];
		CarModelPtr->nlist = gTempCarVertDump[cp->id];

		FindCarLightFade(&workmatrix);

		DrawCarObject(CarModelPtr, &workmatrix, &pos, cp->ap.palette, cp, 1);

		if (ActiveCheats.cheat13 != 0)
		{
			MulMatrix0(&inv_camera_matrix, &cp->hd.drawCarMat, &workmatrix);
		}

		DrawCarWheels(cp, &wheelmatrix, &pos, view);
	}
	else
	{
		CarModelPtr = &NewLowCarModel[model];
		CarModelPtr->nlist = gTempCarVertDump[cp->id];

		gTempCarUVPtr = gTempLDCarUVDump[cp->id];

		if (pos.vz < 8000)
		{
			SetShadowPoints(cp, corners);
			PlaceShadowForCar(corners, 0, 0, yVal < 0 ? 0 : 2);
		}

		ComputeCarLightingLevels(cp, 0);

		FindCarLightFade(&workmatrix);

		DrawCarObject(CarModelPtr, &workmatrix, &pos, cp->ap.palette, cp, 0);
	}

	TransparentObject = 0;

	if (cp->controlType == CONTROL_TYPE_PLAYER)
		PlayerCarFX(cp);
	else if (cp->controlType == CONTROL_TYPE_CIV_AI)
		CivCarFX(cp);

	if (gLightsOn && !lightsOnDelay[cp->id])
	{
		if (cp->controlType == CONTROL_TYPE_CIV_AI)
		{
			if (cp->ai.c.thrustState != CIV_AI_THRUST_STOP || (cp->ai.c.ctrlState != CIV_AI_CTRL_PARKED && cp->ai.c.ctrlState != CIV_AI_CTRL_EMPTY && cp->ai.c.ctrlState != CIV_AI_CTRL_STOP_AT_NODE))
				AddNightLights(cp);
		}
		else if (SilenceThisCar(cp->id) == 0)
			AddNightLights(cp);
	}

	if(CarHasSiren(cp->ap.model))
	{
		if ((IS_ROADBLOCK_CAR(cp) || cp->controlType == CONTROL_TYPE_PURSUER_AI) ||		// any regular cop car including roadblock
			gInGameCutsceneActive && cp->controlType == CONTROL_TYPE_CUTSCENE && force_siren[CAR_INDEX(cp)] != 0 ||		// any car with siren in cutscene
			gCurrentMissionNumber == 26 && cp->controlType == CONTROL_TYPE_CUTSCENE && cp->ap.model == SPECIAL_CAR_SLOT)				// Vegas ambulance
		{
			if (cp->ai.p.dying < 75)
				AddCopCarLight(cp);
		}
	}
}

// [D] [T]
// Which of a city's eight car-palette slots a texture page belongs to, or -1 when
// that city does not use the page as a car palette.
//
// JERICHO: the returned row is offset into civ_clut's second bank when `city` is the
// imported city, so an import's palettes land in rows 8..15 and never touch the host's
// 0..7. Both users get it consistently: ProcessPalletLumpForCity writes through it,
// and GetCarPalIndex reads through it for the model's carid.
static int CarPalIndexInCity(int tpage, int city)
{
	int i;
	int rowbase;

	if (city < 0 || city >= CITY_COUNT)
		return -1;

	rowbase = (CarImportCityHeld(city) && city != GameLevel) ? CarImportBankRow(city) : 0;

	// A city the column cannot afford has no block. Its cars then fall back to the host's
	// row 0 (and ProcessImportedPaletteRows says why), which is a wrong colour rather than
	// a write into someone else's block or into the level font.
	if (rowbase < 0)
		rowbase = 0;

	for (i = 0; i < 8; i++)
	{
		if (tpage == carTpages[city][i])
			return i + rowbase;
	}

	// JERICHO: a SPECIAL body's pages live in specTpages, not carTpages. The host level
	// gets its own pair into carTpages[6]/[7] by OVERWRITING those two entries at load
	// (texture.c:2025), and only the resident body is ever built - so without this scan a
	// specTpages page is in neither table, GetCarPalIndex answers 0, and the row is the
	// HOST's. Map the pair onto the bank's last two rows, exactly where the host's pair
	// sits (rowbase 0 -> 6/7, the import bank -> 14/15).
	// JERICHO: a SPECIAL body's pages live in specTpages, not carTpages - but this ONE row
	// mapping is for the HOST level only, and applying it to a guest is what left an imported
	// special colourless.
	//
	// The host level gets its own pair into carTpages[6]/[7] by OVERWRITING those two entries
	// at load (texture.c), so without this scan a specTpages page is in neither table,
	// GetCarPalIndex answers 0, and the row is the HOST's. Mapping it onto the bank's last two
	// rows is right for the host (rowbase 0 -> 6/7, exactly where the host's pair sits).
	//
	// For a GUEST the same mapping lands on rows base+6/base+7 - and the guest's palette lump
	// holds nothing there. Measured: HAVANA's specTpages are its own 38/39, and its deferred
	// palette lump is keyed by its CAR page numbers, resolving to base..base+5. So the guest's
	// special baked rows base+6/+7 while every entry of its lump sat five rows below, and those
	// panels drew colourless. With the scan restricted to the host, a guest's spec page falls
	// through to the unclassifiable-set path above, which resolves it to that city's own first
	// row - a row its lump DOES fill.
	if (city == GameLevel)
	{
		for (i = 0; i < 12; i++)
		{
			if (tpage == specTpages[city][i])
				return rowbase + 6 + (i & 1);
		}
	}

	return -1;
}

// JERICHO: the same lookup, for a caller that KNOWS which city the page belongs to.
//
// GetCarPalIndex answers "which city does this page belong to?" by asking every held
// city's table in order and taking the first that has the page - which, with three
// cities whose page numbers overlap, is simply the first city. That is fine while one
// city owns one shared block, and wrong the moment each city owns its own: the pin
// recorded the source city of every page it holds (sPinCity), so use that instead of a
// guess. Without this, a 3-city mashup uploads for the first city only - the other two
// blocks stay empty and their cars read whatever the first city put there.
int CarPalIndexInCityFor(int tpage, int city)
{
	if (city < 0 || city >= CITY_COUNT)
		return GetCarPalIndex(tpage);

	return CarPalIndexInCity(tpage, city);
}

// JERICHO: the palette row a BUILT poly bakes, which must never be negative.
//
// `CarPalIndexInCityFor` answers -1 for a set that is in neither the source city's
// carTpages nor its specTpages, and the bake sites then compute
// `clut = (carid - 1) * 6 * 32 + texture_id * 6` - at carid = -1 that is NEGATIVE. The poly's
// clut_uv0 high word is a civ_clut INDEX (not a CLUT id), so at draw time it reads BEFORE
// civ_clut[1] - and the line beside it WRITES `civ_clut[-1][texture_id][0]`, i.e. out of
// bounds. Build-time memory corruption, and a wrong palette on every poly naming that set:
// "broken palettes on each panel" with no other symptom.
//
// A set in neither table means the import and the model disagree about what the set IS, so
// fall back to the held-city search (GetCarPalIndex), which always answers a real row - 0 at
// worst, which is a wrong COLOUR rather than a wild write - and say so once.
static int sPalBakeMiss;
static int sPalBakeDiag;
static int sPalSetsDumping;	/* set while the set dump calls the resolver, so it does not re-enter the miss diag */

// JERICHO-DIAG (JERICHO_DIAG_PALBAKE=1): a set the classification could not place. Prints
// the RUNTIME page table for that city -- the static initialiser in texture.c is replaced
// on a guest by that city's own parsed list (CarImportFillCarTpages), so reading the table
// in the source is not reading the table in the build -- and whether the remembered palette
// lump knows the page at all. Those two answers decide whether the row is knowable from the
// lump (fixable in place) or genuinely absent (a data question).
static void PalBakeMissDiag(int tpage, int city)
{
	char buf[512];
	int i, len = 0, records = 0, pageInLump = 0, firstRec = 0, total = 0;
	char* lump;
	int* p;

	if (getenv("JERICHO_DIAG_PALBAKE") == NULL || sPalBakeDiag >= 24)
		return;

	/* The set dump calls the resolver, and the resolver calls this on a miss. Without
	 * this guard the dump would emit a miss line per unresolved set, inflating the very
	 * count it is being used to measure. */
	if (sPalSetsDumping)
		return;

	sPalBakeDiag++;

	lump = (city >= 0 && city < CITY_COUNT) ? sImpPalLump[city] : NULL;

	len += snprintf(buf + len, sizeof(buf) - len,
		"JERICHO-DIAG PALBAKE: tpage=%d city=%s(%d) lumpHeld=%d carTpages=[",
		tpage, (city >= 0 && city < CITY_COUNT) ? LevelNames[city] : "?", city,
		(lump != NULL) ? 1 : 0);

	if (city >= 0 && city < CITY_COUNT)
	{
		for (i = 0; i < 8; i++)
			len += snprintf(buf + len, sizeof(buf) - len, "%d%s",
				(int)carTpages[city][i], (i < 7) ? "," : "");
	}

	len += snprintf(buf + len, sizeof(buf) - len, "]");

	if (lump != NULL)
	{
		/* Walk with the SAME bound the real walk uses -- the lump's own byte size. The
		 * first version of this probe stopped on a lone -1 int instead, and the lump's
		 * tail is texture data, not records, so a coincidental -1 there made it read
		 * text bytes as pages. Anything that reads a foreign lump has to be bounded by
		 * the size it was given, exactly as ProcessPalletLumpForRows is. */
		int size = (city >= 0 && city < CITY_COUNT) ? sImpPalSize[city] : 0;
		int* q = (int*)(lump + 4);
		char* end = (size > 0) ? (lump + size) : NULL;
		int pages[24], np = 0;
		int k;

		total = *(int*)lump;

		for (k = 0; k < 4096; k++)
		{
			int pg, found;

			if (end != NULL)
			{
				if ((char*)q + 4 > end)
					break;

				if (*q == -1)
					break;

				if ((char*)q + 16 > end)
					break;
			}
			else if (*q == -1)
			{
				break;
			}

			records++;
			pg = q[2];

			if (pg == tpage && !pageInLump)
			{
				pageInLump = 1;
				firstRec = records;
			}

			found = 0;

			for (i = 0; i < np; i++)
				if (pages[i] == pg) { found = 1; break; }

			if (!found && np < 24)
				pages[np++] = pg;

			/* The stride is NOT fixed. An entry is four ints, and when its
			 * clut_number is -1 an INLINE CLUT of eight more ints follows
			 * (ProcessPalletLumpForRows does `buffPtr += 8` there). Stepping a flat
			 * four read that CLUT's pixels as the next pages -- which is why the
			 * first version of this probe printed text bytes as page numbers. */
			q += (q[3] == -1) ? 12 : 4;
		}

		len += snprintf(buf + len, sizeof(buf) - len,
			" lumpCluts=%d bytes=%d records=%d pageInLump=%d rec=%d lumpPages=",
			total, size, records, pageInLump, firstRec);

		for (i = 0; i < np; i++)
			len += snprintf(buf + len, sizeof(buf) - len, "%d%s",
				pages[i], (i < np - 1) ? "," : "");
	}

	printInfo("%s\n", buf);
}

static int CarPalIndexForBuild(int tpage, int city)
{
	int idx = CarPalIndexInCityFor(tpage, city);

	if (idx < 0)
	{
		// JERICHO: an unclassifiable set resolves to the SOURCE CITY'S OWN first row, not to
		// the host's row 0. Set 0 is the usual one - every city's models carry 8-16 polys
		// naming it, and no city has a car page there.
		//
		// Two reasons, and the second is a bug fix rather than a preference:
		//
		// 1. The host reads its OWN row 0 for a set it cannot classify. Applying that rule
		//    where the car actually came from is the same rule, not a new one.
		// 2. `carid` is the row in `clut = (carid-1)*6*32 + texture_id*6`, which is NEGATIVE
		//    at carid 0 - and the poly's clut_uv0 high word is a civ_clut INDEX into
		//    `&civ_clut[1]`, so a negative one reads BEFORE the array. A guest's row 0 is its
		//    block base (8/16/24), which is positive, so those polys become safe; the old
		//    `GetCarPalIndex` fallback answered 0 and left the wild index in place.
		int base = CarImportPaletteBlockBase(city);

		PalBakeMissDiag(tpage, city);	/* JERICHO-DIAG (JERICHO_DIAG_PALBAKE) */

		if (sPalBakeMiss++ < 4)
			printInfo("cross-city: set %d has no palette row in %s - baking that city's own row 0 (civ_clut %d) rather than a negative index\n",
				tpage, (city >= 0 && city < CITY_COUNT) ? LevelNames[city] : "?", (base >= 0) ? base : 0);

		idx = (base >= 0) ? base : GetCarPalIndex(tpage);
	}

	return idx;
}

// JERICHO-DIAG (JERICHO_DIAG_PALBAKE=1): what a BUILT car actually names, against the
// pages its city's table claims. This is the pair of lists that decides whether a bake
// miss is a defect or a fact: if the sets are the city's own pages, something is wrong
// with the table or the lookup; if they are a different numbering entirely (a page index
// within the model, say), then no table could classify them and the fallback is the only
// answer available at build time.
static void PalBakeSetsDiag(int slot)
{
	char buf[768];
	int i, n, city, len = 0;

	if (getenv("JERICHO_DIAG_PALBAKE") == NULL)
		return;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	city = GetCarModelSourceCity(slot);
	n = CarModelSetsCount(slot);
	sPalSetsDumping = 1;	/* the resolver below must not log misses */

	len += snprintf(buf + len, sizeof(buf) - len,
		"JERICHO-DIAG PALBAKE-SETS: slot=%d model=%d city=%s n=%d pageTable=[",
		slot, residentCarModels[slot],
		(city >= 0 && city < CITY_COUNT) ? LevelNames[city] : "?", n);

	if (city >= 0 && city < CITY_COUNT)
	{
		for (i = 0; i < 8; i++)
			len += snprintf(buf + len, sizeof(buf) - len, "%d%s",
				(int)carTpages[city][i], (i < 7) ? "," : "");
	}

	len += snprintf(buf + len, sizeof(buf) - len, "] modelNames=[");

	for (i = 0; i < n; i++)
	{
		int set = CarModelSetsGet(slot, i);

		/* own = what the model's OWN city can classify it as (-1 = it cannot);
		 * fin = the row the bake actually bakes, after the miss branch. Before the
		 * miss branch preferred the all-cities search, fin was the block base for
		 * every own=-1; now it is the search hit when a held city has the page. */
		len += snprintf(buf + len, sizeof(buf) - len, "%d(o%d/f%d)%s", set,
			CarPalIndexInCityFor(set, city), CarPalIndexForBuild(set, city),
			(i < n - 1) ? "," : "");
	}

	len += snprintf(buf + len, sizeof(buf) - len, "]");

	sPalSetsDumping = 0;

	printInfo("%s\n", buf);
}

char GetCarPalIndex(int tpage)
{
	int idx = CarPalIndexInCity(tpage, GameLevel);
	int imported;

	if (idx >= 0)
		return (char)idx;

	// JERICHO: a vehicle imported from another city brings that city's texture
	// pages with it. Its polygons look their colours up by page, and the host
	// level has no entry for a foreign page - so they all collapse to slot 0 and
	// the car is painted with the HOST's palette, which is what 'foreign palettes
	// do not load' looks like. Map through a HELD city's table instead, which is
	// where its palettes were stored. More than one city can be held.
	for (imported = 0; imported < CITY_COUNT; imported++)
	{
		if (!CarImportCityHeld(imported) || imported == GameLevel)
			continue;

		idx = CarPalIndexInCity(tpage, imported);

		if (idx >= 0)
			return (char)idx;
	}

	return 0;
}

// [D] JERICHO: THE ONE DECISION ON WHETHER A CAR SLOT CAN BE HANDED OUT.
//
// See cars.h. Every site that gives a car a model routes through this rather than
// testing the range and the mesh itself, so a new spawn path cannot be written with
// a slightly weaker test than the one next to it.
int JerCarSlotState(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return JER_CAR_SLOT_OUT_OF_RANGE;

	if (gCarCleanModelPtr[slot] == NULL)
		return JER_CAR_SLOT_NO_MESH;

	return JER_CAR_SLOT_OK;
}

int JerCarSlotUsable(int slot)
{
	return JerCarSlotState(slot) == JER_CAR_SLOT_OK;
}

const char* JerCarSlotRefusal(int slot)
{
	switch (JerCarSlotState(slot))
	{
	case JER_CAR_SLOT_OUT_OF_RANGE:	return "not a car this level can load";
	case JER_CAR_SLOT_NO_MESH:	return "no model for it in this level";
	default:			return NULL;
	}
}
