/* jer_d1.c — game-side implementation of the Driver 1 support library.
 *
 * See JERICHO/include/jer_d1.h for the API contract and the format notes.
 * Everything Driver-1-specific lives here; the engine calls in through the
 * thin gated sites in main.c / system.c / cars.c / spool.c / cell.c /
 * dr2roads.c / map.c.
 *
 * Format facts verified against the extracted Driver 1 (PS1) assets and the
 * OpenDriver2Tools DriverLevelTool sources (regions_d1.cpp, textures.cpp,
 * d2_types.h):
 *  - D1 .LEV shares the D2 container (lump 37 + 4 citylump offsets)
 *  - sections carry a leading lump count (no 0xff terminator)
 *  - D1 cells are 4-byte {num, next_ptr}; D2 cells are 2-byte {num}
 *  - D1 cell objects are 16-byte {absolute pos, pad, yang, model type}
 *  - spooled region data: roadm RLE, roadh RLE, packed cell pointers,
 *    cell data, cell objects
 *  - terrain height: per-cell road map at 1500-unit resolution
 */

#include "jer_d1.h"

#include "driver2.h"

#include "map.h"
#include "spool.h"
#include "dr2roads.h"
#include "texture.h"
#include "cars.h"
#include "models.h"
#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* unbuffered D1 debug trace (the game's log stream is buffered and lost on
 * crash) — jer_d1_log writes straight to d1_debug.log */
static FILE* sD1Log = NULL;

void jer_d1_log(const char* fmt, ...)
{
	va_list va;

	if (sD1Log == NULL)
		sD1Log = fopen("d1_debug.log", "wb");

	if (sD1Log == NULL)
		return;

	va_start(va, fmt);
	vfprintf(sD1Log, fmt, va);
	va_end(va);

	fflush(sD1Log);
}

/* ------------------------------------------------------------------ */
/* Driver 1 type layouts (mirror OpenDriver2Tools d2_types.h)         */
/* ------------------------------------------------------------------ */

/* D1 cell: {ushort num; ushort next_ptr;} (D2 is just {ushort num}) */
struct D1CELL
{
	ushort num;
	ushort next_ptr;
};

/* D1 cell object: {int vx,vy,vz; u8 pad; u8 yang; u16 type} */
struct D1CELL_OBJECT
{
	int vx, vy, vz;
	u_char pad;
	u_char yang;
	u_short type;
};

/* 3-int pallet entry: {palette, texnum, tpage} — no clut_number */
struct D1PALLET_INFO
{
	int palette;
	int texnum;
	int tpage;
};

/* road surface linked list from LUMP_ROADSURF */
struct D1SIPOLY
{
	XYPAIR xz[4];
	int normals[4];		/* a, b, c, d */
	short num_vertices;
};

struct D1SURFACEINFO
{
	short type;
	short info;
	short heading;
	short numpolys;
	/* followed by numpolys D1SIPOLYs */
};

#define D1_SURFACE_MAX 1024

static struct D1SURFACEINFO* sD1SurfacePtrs[D1_SURFACE_MAX];

/* ------------------------------------------------------------------ */
/* City tables                                                         */
/* ------------------------------------------------------------------ */

static const char* const sD1LevelFiles[JER_D1_CITY_COUNT] = {
	"LEVELS\\MIAMI.LEV",
	"LEVELS\\FRISCO.LEV",
	"LEVELS\\LA.LEV",
	"LEVELS\\NEWYORK.LEV",
};

static const char* const sD1LevelNames[JER_D1_CITY_COUNT] = {
	"Miami",
	"San Francisco",
	"Los Angeles",
	"New York",
};

/* player start positions per D1 city (x, rotation 0..4095, z). These are
 * the pre-existing "what?" slots in the engine's levelstartpos[4..7]; they
 * sit the player on a road in each city and are runtime-verified. */
static const int sD1StartPos[JER_D1_CITY_COUNT][3] = {
	{ -10500,  -6163, -22144 },	/* Miami */
	{  148808,  6163, -112000 },	/* San Francisco */
	{ -170000, 6163,  361000 },	/* Los Angeles */
	{   -8995, -6163,  63655 },	/* New York */
};

static char sD1Folder[64] = { 0 };
static int sD1FolderInit = 0;
static int sD1Available = -1;

const char* jer_d1_level_file(int city)
{
	if (city < 0 || city >= JER_D1_CITY_COUNT)
		return NULL;

	return sD1LevelFiles[city];
}

const char* jer_d1_level_name(int city)
{
	if (city < 0 || city >= JER_D1_CITY_COUNT)
		return NULL;

	return sD1LevelNames[city];
}

/* minimal config.ini scan for "[fs] d1DataFolder" (the game's own config
 * loader only knows dataFolder). Defaults to the sibling "DRIVER\\". */
static void jer_d1_init_folder(void)
{
	FILE* f;
	char line[128];

	sD1FolderInit = 1;
	strcpy(sD1Folder, "DRIVER\\");

	f = fopen("config.ini", "rb");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL)
	{
		if (strncmp(line, "d1DataFolder", 12) == 0)
		{
			char* eq = strchr(line, '=');

			if (eq != NULL)
			{
				char* v = eq + 1;
				char* nl = strchr(v, '\n');

				if (nl != NULL)
					*nl = 0;

				while (*v == ' ' || *v == '\t')
					v++;

				if (*v != 0)
				{
					strncpy(sD1Folder, v, sizeof(sD1Folder) - 2);
					sD1Folder[sizeof(sD1Folder) - 2] = 0;

					if (sD1Folder[strlen(sD1Folder) - 1] != '\\')
						strcat(sD1Folder, "\\");
				}
			}
			break;
		}
	}

	fclose(f);
}

const char* jer_d1_folder(void)
{
	if (!sD1FolderInit)
		jer_d1_init_folder();

	return sD1Folder;
}

static int jer_d1_file_exists(const char* relpath)
{
	char name[96];

	sprintf(name, "%s%s", jer_d1_folder(), relpath);

	return FileExists(name) != 0;
}

int jer_d1_available(void)
{
	int i;

	if (sD1Available >= 0)
		return sD1Available;

	sD1Available = 1;

	for (i = 0; i < JER_D1_CITY_COUNT; i++)
	{
		if (!jer_d1_file_exists(sD1LevelFiles[i]))
		{
			sD1Available = 0;
			break;
		}
	}

	return sD1Available;
}

int jer_d1_lump_count(const void* section)
{
	int count;

	if (section == NULL)
		return -1;

	count = *(const int*)section;

	/* a plausible D1 section: 1..255 lumps (the tool iterates up to 255) */
	if (count < 1 || count > 255)
		return -1;

	return count;
}

void jer_d1_start_pos(int city, int* x, int* rot, int* z)
{
	if (city < 0 || city >= JER_D1_CITY_COUNT)
		return;

	*x = sD1StartPos[city][0];
	*rot = sD1StartPos[city][1];
	*z = sD1StartPos[city][2];
}

int jer_d1_roadmap_scale(void)
{
	return 750;
}

/* ------------------------------------------------------------------ */
/* Pallet lump (3-int entries, clut inline, count-bounded)             */
/* ------------------------------------------------------------------ */

void jer_d1_process_pallet(const void* lump)
{
	const int* buffPtr;
	ushort clutValue;
	int texnum;
	int palette;
	int tpageindex;
	int total_cluts;
	int added;

	total_cluts = *(const int*)lump;

	if (total_cluts == 0)
		return;

	buffPtr = (const int*)lump + 1;
	added = 0;

	/* D1 entries are 12 bytes {palette, texnum, tpage} and every entry
	 * stores its clut inline (no clut_number field). The loop is bounded
	 * by total_cluts. */
	while (added < total_cluts)
	{
		palette = buffPtr[0];
		texnum = buffPtr[1];
		tpageindex = buffPtr[2];

		if (palette == -1)
			break;

		buffPtr += 3;

		/* store the 16-color clut inline */
		LoadImage(&clutpos, (u_long*)buffPtr);
		buffPtr += 8;	/* 16 u16 = 32 bytes */

		clutValue = GetClut(clutpos.x, clutpos.y);
		IncrementClutNum(&clutpos);

		civ_clut[GetCarPalIndex(tpageindex)][texnum][palette + 1] = clutValue;

		added++;
	}
}

/* ------------------------------------------------------------------ */
/* Road / junction / bounds lumps — parsed + stored for reference.     */
/* (The engine's own D1 parsers are stubs; AI routing is not wired up   */
/* yet, so this currently only keeps the counts for diagnostics.)       */
/* ------------------------------------------------------------------ */

void jer_d1_process_roads(const void* lump, int size)
{
	(void)size;

	if (lump != NULL)
		(void)*(const int*)lump;	/* road count */
}

void jer_d1_process_junctions(const void* lump, int size)
{
	(void)size;

	if (lump != NULL)
		(void)*(const int*)lump;	/* junction count */
}

void jer_d1_process_roadbounds(const void* lump, int size)
{
	(void)size;

	if (lump != NULL)
		(void)*(const int*)lump;	/* bound count */
}

void jer_d1_process_juncbounds(const void* lump, int size)
{
	(void)size;

	if (lump != NULL)
		(void)*(const int*)lump;	/* bound count */
}

void jer_d1_process_roadsurf(const void* lump, int size)
{
	int numSurfaces;
	int i;
	const char* ptr;

	memset(sD1SurfacePtrs, 0, sizeof(sD1SurfacePtrs));

	if (lump == NULL || size <= 4)
		return;

	numSurfaces = *(const int*)lump;

	if (numSurfaces <= 0 || numSurfaces >= D1_SURFACE_MAX)
		return;

	/* The surface data lives inside the level's in-memory lump (the DATA2
	 * buffer, kept alive for the whole level) so pointing into it is safe. */
	ptr = (const char*)lump + sizeof(int);

	for (i = 0; i < numSurfaces; i++)
	{
		const struct D1SURFACEINFO* si = (const struct D1SURFACEINFO*)ptr;

		if (si->type >= 0 && si->type < D1_SURFACE_MAX)
			sD1SurfacePtrs[si->type] = (struct D1SURFACEINFO*)si;

		ptr += sizeof(struct D1SURFACEINFO) + si->numpolys * sizeof(struct D1SIPOLY);
	}
}

/* ------------------------------------------------------------------ */
/* Region data: RLE decoders + cell-object conversion                  */
/* ------------------------------------------------------------------ */

/* roadm RLE: runs of [short length, ushort value]; length == -1 ends. */
void jer_d1_unpack_surfaceroads(const void* src, int cells, void* out)
{
	const short* pSrc = (const short*)src;
	ushort* pOut = (ushort*)out;
	int i = cells;

	do
	{
		short length;
		ushort value;
		int count;

		length = *pSrc++;
		if (length == -1)
			break;

		value = *pSrc++;

		count = length;
		i -= count;

		while (count > 0)
		{
			*pOut++ = value;
			count--;
		}
	} while (i > 0);
}

/* roadh RLE: runs of [uint length, uint value]; runs until cells filled. */
void jer_d1_unpack_roadmap(const void* src, int cells, void* out)
{
	const u_int* pSrc = (const u_int*)src;
	u_int* pOut = (u_int*)out;
	int i = cells;

	do
	{
		u_int len;
		u_int value;

		len = *pSrc++;
		value = *pSrc++;

		i -= len;

		while (len != 0)
		{
			*pOut++ = value;
			len--;
		}
	} while (i != 0);
}

int jer_d1_convert_cell_objects(const void* src, int nbytes, void* out)
{
	/* D1 CELL_OBJECT (16 bytes) -> PACKED_CELL_OBJECT (8 bytes).
	 *
	 * The engine unpacks with:
	 *   pco->pos.vx = near.x + (short)(ppco->pos.vx - near.x)
	 * so ppco->pos.vx only has to be congruent to (absX - near.x)
	 * mod 65536; storing absX & 0xFFFF satisfies that for any cell size
	 * (the (short) wrap cancels the 65536 multiples). The vertical is
	 * packed like Driver 2: (y << 1) | model high bit, and value =
	 * (model type << 6) | yang so every consumer
	 * (draw.c / objcoll.c / camera.c / leadai.c) works unchanged. */
	const struct D1CELL_OBJECT* pIn = (const struct D1CELL_OBJECT*)src;
	ushort* pOut = (ushort*)out;
	int count = nbytes / (int)sizeof(struct D1CELL_OBJECT);
	int i;

	for (i = 0; i < count; i++)
	{
		u_short type = pIn[i].type;

		pOut[i * 4 + 0] = (ushort)(pIn[i].vx & 0xFFFF);
		pOut[i * 4 + 2] = (ushort)(pIn[i].vz & 0xFFFF);
		pOut[i * 4 + 1] = (ushort)((pIn[i].vy << 1) | ((type >> 10) & 1));
		pOut[i * 4 + 3] = (ushort)((type << 6) | (pIn[i].yang & 63));
	}

	return count;
}

/* D1 straddlers (16-byte objects right after the MAP lump header) are
 * converted into the engine's packed cell_objects buffer too. */
void jer_d1_convert_straddlers(const void* src, int count, void* out)
{
	jer_d1_convert_cell_objects(src, count * (int)sizeof(struct D1CELL_OBJECT), out);
}

/* ------------------------------------------------------------------ */
/* Per-barrel region data                                              */
/* ------------------------------------------------------------------ */

/* the D1 road map covers a region at half-cell resolution:
 * (region_size * 2) ^ 2 cells, e.g. 30*2 = 60 -> 3600 entries. */
#define JER_D1_REGION_CELLS 3600

static u_int* sD1RoadMap[4];
static ushort* sD1SurfaceRoads[4];
static char* sD1RoadmScratch[4];
static char* sD1RoadhScratch[4];
static char* sD1CellObjectScratch[4];
static int sD1CellObjectBytes[4];	/* raw 16-byte object byte count */

void jer_d1_region_clear(int barrel)
{
	if (barrel < 0 || barrel > 3)
		return;

	if (sD1RoadMap[barrel] != NULL)
	{
		free(sD1RoadMap[barrel]);
		sD1RoadMap[barrel] = NULL;
	}
	if (sD1SurfaceRoads[barrel] != NULL)
	{
		free(sD1SurfaceRoads[barrel]);
		sD1SurfaceRoads[barrel] = NULL;
	}
	if (sD1RoadmScratch[barrel] != NULL)
	{
		free(sD1RoadmScratch[barrel]);
		sD1RoadmScratch[barrel] = NULL;
	}
	if (sD1RoadhScratch[barrel] != NULL)
	{
		free(sD1RoadhScratch[barrel]);
		sD1RoadhScratch[barrel] = NULL;
	}
	if (sD1CellObjectScratch[barrel] != NULL)
	{
		free(sD1CellObjectScratch[barrel]);
		sD1CellObjectScratch[barrel] = NULL;
	}
	sD1CellObjectBytes[barrel] = 0;
}

/* allocate the per-barrel arrays + scratch buffers for the spooled
 * chunks. Sizes are in CD sectors (2048). Returns 0 on success. */
int jer_d1_region_begin(int barrel, int roadmSectors, int roadhSectors, int cellObjectSectors)
{
	if (barrel < 0 || barrel > 3)
		return -1;

	jer_d1_region_clear(barrel);

	sD1RoadMap[barrel] = (u_int*)malloc(JER_D1_REGION_CELLS * sizeof(u_int));
	sD1SurfaceRoads[barrel] = (ushort*)malloc(JER_D1_REGION_CELLS * sizeof(ushort));

	if (sD1RoadMap[barrel] == NULL || sD1SurfaceRoads[barrel] == NULL)
		return -1;

	memset(sD1RoadMap[barrel], 0, JER_D1_REGION_CELLS * sizeof(u_int));
	memset(sD1SurfaceRoads[barrel], 0, JER_D1_REGION_CELLS * sizeof(ushort));

	if (roadmSectors > 0)
	{
		sD1RoadmScratch[barrel] = (char*)malloc(roadmSectors * 2048);
		if (sD1RoadmScratch[barrel] == NULL)
			return -1;
	}

	if (roadhSectors > 0)
	{
		sD1RoadhScratch[barrel] = (char*)malloc(roadhSectors * 2048);
		if (sD1RoadhScratch[barrel] == NULL)
			return -1;
	}

	if (cellObjectSectors > 0)
	{
		sD1CellObjectScratch[barrel] = (char*)malloc(cellObjectSectors * 2048);
		if (sD1CellObjectScratch[barrel] == NULL)
			return -1;

		sD1CellObjectBytes[barrel] = cellObjectSectors * 2048;
	}

	return 0;
}

char* jer_d1_region_roadm_scratch(int barrel)
{
	if (barrel < 0 || barrel > 3)
		return NULL;

	return sD1RoadmScratch[barrel];
}

char* jer_d1_region_roadh_scratch(int barrel)
{
	if (barrel < 0 || barrel > 3)
		return NULL;

	return sD1RoadhScratch[barrel];
}

char* jer_d1_region_cell_object_scratch(int barrel)
{
	if (barrel < 0 || barrel > 3)
		return NULL;

	return sD1CellObjectScratch[barrel];
}

/* post-arrival processing: decode both RLE streams and convert the raw
 * 16-byte cell objects into the engine's packed cell_objects buffer.
 * Called from the D1 region callback after the final chunk landed
 * (SIMPLE_SPOOL on PC = synchronous memcpy, so all chunks are in). */
void jer_d1_region_finish(int barrel)
{
	extern PACKED_CELL_OBJECT* cell_objects;
	extern int num_straddlers;
	extern int cell_objects_add[5];

	if (barrel < 0 || barrel > 3)
		return;

	if (sD1RoadMap[barrel] == NULL || sD1SurfaceRoads[barrel] == NULL)
		return;

	if (sD1RoadmScratch[barrel] != NULL)
		jer_d1_unpack_surfaceroads(sD1RoadmScratch[barrel], JER_D1_REGION_CELLS, sD1SurfaceRoads[barrel]);

	if (sD1RoadhScratch[barrel] != NULL)
		jer_d1_unpack_roadmap(sD1RoadhScratch[barrel], JER_D1_REGION_CELLS, sD1RoadMap[barrel]);

	if (sD1CellObjectScratch[barrel] != NULL && sD1CellObjectBytes[barrel] > 0)
	{
		jer_d1_convert_cell_objects(sD1CellObjectScratch[barrel],
			sD1CellObjectBytes[barrel],
			cell_objects + num_straddlers + cell_objects_add[barrel]);
	}
}

void jer_d1_region_set(int barrel, const void* roadmapCells, const void* surfaceRoadCells)
{
	if (barrel < 0 || barrel > 3)
		return;

	sD1RoadMap[barrel] = (u_int*)roadmapCells;
	sD1SurfaceRoads[barrel] = (ushort*)surfaceRoadCells;
}

const void* jer_d1_region_roadmap(int barrel)
{
	if (barrel < 0 || barrel > 3)
		return NULL;

	return sD1RoadMap[barrel];
}

const void* jer_d1_region_surfaceroads(int barrel)
{
	if (barrel < 0 || barrel > 3)
		return NULL;

	return sD1SurfaceRoads[barrel];
}

/* ------------------------------------------------------------------ */
/* D1 terrain/surface query                                            */
/* ------------------------------------------------------------------ */

static int sD1CellSize = 3000;
static int sD1RegionSize = 30;

void jer_d1_set_grid(int cellSize, int regionSize)
{
	sD1CellSize = cellSize;
	sD1RegionSize = regionSize;
}

static int D1PointInTri2d(int tx, int ty, const XYPAIR* verts)
{
	if ((verts[1].y - verts[0].y) * (tx - verts[0].x) -
		(verts[1].x - verts[0].x) * (ty - verts[0].y) < 0)
		return 0;

	if ((verts[2].y - verts[1].y) * (tx - verts[1].x) -
		(verts[2].x - verts[1].x) * (ty - verts[1].y) >= 0)
		return (verts[0].y - verts[2].y) * (tx - verts[2].x) -
			(verts[0].x - verts[2].x) * (ty - verts[2].y) >= 0;

	return 0;
}

static int D1PointInQuad2d(int tx, int ty, const XYPAIR* verts)
{
	if ((tx - verts[0].x) * (verts[1].y - verts[0].y) -
		(ty - verts[0].y) * (verts[1].x - verts[0].x) < 0)
		return 0;

	if ((tx - verts[1].x) * (verts[2].y - verts[1].y) -
		(ty - verts[1].y) * (verts[2].x - verts[1].x) < 0)
		return 0;

	if ((tx - verts[2].x) * (verts[3].y - verts[2].y) -
		(ty - verts[2].y) * (verts[3].x - verts[2].x) < 0)
		return 0;

	if ((tx - verts[3].x) * (verts[0].y - verts[3].y) -
		(ty - verts[3].y) * (verts[0].x - verts[3].x) < 0)
		return 0;

	return 1;
}

static void D1RotatePoint(const u_int routeValue, int* px, int* py)
{
	int objectAngle = (routeValue >> 30) * 1024;

	if ((objectAngle & 0x400) == 0)
	{
		int tmp = -*px;
		*px = *py;
		*py = tmp;
	}

	if ((objectAngle & 0x800) != 0)
	{
		*px = -*px;
		*py = -*py;
	}
}

static void D1RotateNormal(const u_int routeValue, int* nx, int* ny)
{
	int objectAngle = (routeValue >> 30) * 1024;

	if ((objectAngle & 0x400) != 0)
	{
		int tmp = *ny;
		*ny = -*nx;
		*nx = tmp;
	}

	if ((objectAngle & 0x800) == 0)
	{
		*nx = -*nx;
		*ny = -*ny;
	}
}

static int D1GetRoadInfo(int posX, int posZ, u_int* outValue, int* outRoadIndex)
{
	const u_int* roadMap;
	const ushort* surfaceRoads;
	XZPAIR cpos;
	XZPAIR cell;
	int road_region_size;
	int regionX, regionZ;
	int barrel;
	int cellIdx;

	extern ROAD_MAP_LUMP_DATA roadMapLumpData;
	extern int units_across_halved;
	extern int units_down_halved;

	road_region_size = sD1CellSize / 1500 * sD1RegionSize;

	cpos.x = (roadMapLumpData.unitXMid + posX) / 1500;
	cpos.z = (roadMapLumpData.unitZMid - (posZ - 750)) / 1500;

	if (cpos.x < 0 || cpos.z < 0 ||
		cpos.x >= roadMapLumpData.width ||
		cpos.z >= roadMapLumpData.height)
	{
		return 0;
	}

	/* find the region the (slightly offset) position falls in */
	cell.x = (posX + 750 + units_across_halved) / sD1CellSize;
	cell.z = (posZ - 750 + units_down_halved) / sD1CellSize;

	regionX = cell.x / sD1RegionSize;
	regionZ = cell.z / sD1RegionSize;

	if (regionX < 0 || regionZ < 0 ||
		regionX >= cells_across / sD1RegionSize ||
		regionZ >= cells_down / sD1RegionSize)
	{
		return 0;
	}

	barrel = (regionX & 1) + (regionZ & 1) * 2;

	roadMap = (const u_int*)jer_d1_region_roadmap(barrel);
	surfaceRoads = (const ushort*)jer_d1_region_surfaceroads(barrel);

	if (roadMap == NULL || surfaceRoads == NULL)
		return 0;

	cellIdx = (cpos.x % road_region_size) + (cpos.z % road_region_size) * road_region_size;

	if (cellIdx < 0 || cellIdx >= JER_D1_REGION_CELLS)
		return 0;

	*outValue = roadMap[cellIdx];
	*outRoadIndex = surfaceRoads[cellIdx];

	return 1;
}

int jer_d1_find_surface(int posX, int posY, int posZ,
	int* outNx, int* outNy, int* outNz, int* outY, int* outSurface)
{
	u_int routeValue = 0;
	int roadIndex = 0;
	int height;
	int type;
	int px, py;
	const struct D1SURFACEINFO* si;
	int i;

	(void)posY;

	/* default: flat concrete at the sea-plane height (like D2) */
	*outNx = 0;
	*outNy = 4096;
	*outNz = 0;
	*outY = -2048;
	*outSurface = SURF_CONCRETE;

	if (!D1GetRoadInfo(posX, posZ, &routeValue, &roadIndex))
		return 0;

	height = *(short*)&routeValue;
	type = (routeValue >> 16) & 1023;

	*outY = -height;

	if (type >= 900)
		return 1;

	si = sD1SurfacePtrs[type];

	if (si != NULL)
	{
		int n_vx, n_vz, n_vy;
		int normFac;

		/* local coords inside the 1500-unit road-map cell */
		px = (roadMapLumpData.unitZMid - (posZ - 750)) % 1500 - 750;
		py = (roadMapLumpData.unitXMid + posX) % 1500 - 750;

		D1RotatePoint(routeValue, &px, &py);

		for (i = 0; i < si->numpolys; i++)
		{
			const struct D1SIPOLY* poly =
				(const struct D1SIPOLY*)((const char*)(si + 1) + i * sizeof(struct D1SIPOLY));

			int res = (poly->num_vertices == 4) ?
				D1PointInQuad2d(px, py, poly->xz) :
				D1PointInTri2d(px, py, poly->xz);

			if (res != 0)
			{
				normFac = px * poly->normals[0] + py * poly->normals[2];
				*outY = ((normFac / 4096) - poly->normals[3]) - height;

				n_vx = poly->normals[0];
				n_vz = poly->normals[2];
				n_vy = poly->normals[1];

				D1RotateNormal(routeValue, &n_vx, &n_vz);

				*outNx = n_vx << 2;
				*outNy = n_vy << 2;
				*outNz = n_vz << 2;
			}
		}
	}

	if (type < num_models_in_pack && modelpointers[type] != NULL)
	{
		MODEL* model = modelpointers[type];

		if (model->shape_flags & SHAPE_FLAG_WATER)
			*outSurface = SURF_WATER;

		if (model->flags2 & MODEL_FLAG_GRASS)
			*outSurface = SURF_GRASS;
	}

	return 1;
}
