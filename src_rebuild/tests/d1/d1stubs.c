/* d1stubs.c — stub definitions of the engine symbols jer_d1.c references,
 * so the D1 parsing library can be compiled and exercised HEADLESS (no
 * window, no SDL, no controller) against the real Driver 1 .LEV files.
 * The harness lives in tests/d1/d1test.c. */

#include <stdio.h>
#include <string.h>

/* --- types the stubs must match (mirrored from the game headers) --- */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;

struct _RECT16 { short x, y, w, h; };	/* must match libgpu.h exactly */
typedef struct _RECT16 RECT16;
typedef unsigned long u_long;

typedef struct { short x, y; } XYPAIR;

struct MODEL { int shape_flags; int flags2; };
typedef struct MODEL MODEL;

struct ROAD_MAP_LUMP_DATA
{
	int width;
	int height;
	int unitXMid;
	int unitZMid;
};
typedef struct ROAD_MAP_LUMP_DATA ROAD_MAP_LUMP_DATA;

struct PACKED_CELL_OBJECT
{
	unsigned short vx, vy, vz;
	unsigned short value;
};
typedef struct PACKED_CELL_OBJECT PACKED_CELL_OBJECT;

/* --- globals used by jer_d1.c --- */
u_short civ_clut[8][32][6];
RECT16 clutpos;
ROAD_MAP_LUMP_DATA roadMapLumpData;
int units_across_halved;
int units_down_halved;
int cells_across;
int cells_down;
int num_models_in_pack;
MODEL* modelpointers[2048];
PACKED_CELL_OBJECT* cell_objects;
static PACKED_CELL_OBJECT sCellObjects[65536];	/* test scratch */
int num_straddlers;
int cell_objects_add[5];

/* --- function stubs --- */

void d1stubs_init(void)
{
	cell_objects = sCellObjects;
}

int FileExists(char* filename)
{
	FILE* f = fopen(filename, "rb");
	if (f) fclose(f);
	return f != NULL;
}

/* libgpu.h declares LoadImage + GetClut extern "C" */
#ifdef __cplusplus
extern "C" {
#endif

int LoadImage(RECT16* rect, u_long* pixels)
{
	(void)rect; (void)pixels;	/* no-op headless */
	return 0;
}

unsigned short GetClut(int x, int y)
{
	(void)x; (void)y;
	return 0;
}

#ifdef __cplusplus
}
#endif

void IncrementClutNum(RECT16* clut)
{
	(void)clut;
}

char GetCarPalIndex(int tpage)
{
	(void)tpage;
	return 0;
}
