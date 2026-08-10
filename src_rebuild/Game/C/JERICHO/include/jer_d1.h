#ifndef JERICHO_JER_D1_H
#define JERICHO_JER_D1_H

/* jer_d1 — Driver 1 (PS1) asset support library.
 *
 * Driver 1 .LEV files share Driver 2's container (LUMPDESC header + the 4
 * citylump offsets: loadtime / tpages / inmem / spooled) but differ inside:
 *   - each loadtime/inmem section begins with a 32-bit LUMP COUNT
 *     (Driver 2 sections are terminated by a 0xff lump instead);
 *   - the pallet lump entries are 3-int {palette, texnum, tpage}
 *     (Driver 2 adds a 4th clut_number field);
 *   - spooled region data is ordered roadm-RLE, roadh-RLE, packed cell
 *     pointers, linked CELL_DATA (4-byte, next_ptr chained), then 16-byte
 *     CELL_OBJECTs with ABSOLUTE positions and a raw model-type field;
 *   - terrain height comes from the region road map (1500-unit grid), not
 *     the Driver 2 PVS/BSP heightmap;
 *   - roads/junctions/bounds use the old Driver 1 formats.
 *
 * Everything D1-specific lives here (the game-side implementation is in
 * Game/C/jer_d1.c, which sees the real engine types); the engine only
 * carries thin gated call sites and the driver1 mod uses the API for the
 * take-a-ride city page.
 *
 * The header stays free of game headers (same rule as jer_menu/jer_npc);
 * the game-side impl does the casting.
 */
#ifdef __cplusplus
extern "C" {
#endif

#define JER_D1_CITY_COUNT 4

/* city indices follow the take-a-ride page order:
 * 0 = Miami, 1 = San Francisco, 2 = Los Angeles, 3 = New York */

/* "LEVELS\\MIAMI.LEV" etc. (relative to the D1 data folder) */
const char* jer_d1_level_file(int city);
/* display names: "Miami", "San Francisco", "Los Angeles", "New York" */
const char* jer_d1_level_name(int city);
/* the D1 data folder ("DRIVER\\" unless config.ini [fs] d1DataFolder says
 * otherwise). Cached after the first call. */
const char* jer_d1_folder(void);
/* 1 when all four D1 city files exist (drives the menu availability gate). */
int jer_d1_available(void);

/* D1 LEV section walking: returns the leading 32-bit lump count of a
 * loadtime/inmem section, or -1 if it does not look like a D1 section. */
int jer_d1_lump_count(const void* section);

/* unbuffered D1 trace (survives crashes; goes to d1_debug.log) */
void jer_d1_log(const char* fmt, ...);

/* D1 car-cosmetics compatibility: fills a 160-byte D2-format
 * CAR_COSMETICS profile (D2 fields D1 lacks get sane defaults so D1
 * traffic cars have real collision boxes, mass, suspension and wheel
 * data instead of all-zero structs). dest must hold 160 bytes. */
#define JER_D1_CAR_COSMETICS_SIZE 160
void jer_d1_default_car_cosmetics(void* dest);

/* D1 car/ped palette lump (3-int entries, clut inline) -> engine civ_clut */
void jer_d1_process_pallet(const void* lump);

/* D1 road/junction/bounds lumps — parsed and stored for reference (the
 * engine's own D1 parsers are stubs; driving-game AI is not wired up yet). */
void jer_d1_process_roads(const void* lump, int size);
void jer_d1_process_junctions(const void* lump, int size);
void jer_d1_process_roadbounds(const void* lump, int size);
void jer_d1_process_juncbounds(const void* lump, int size);
void jer_d1_process_roadsurf(const void* lump, int size);

/* D1 spooled region helpers (called from LoadRegionData's D1 branch):
 *  - jer_d1_unpack_surfaceroads: roadm RLE ([len, value] ushort pairs,
 *    len == -1 terminates) -> a per-cell ushort array
 *  - jer_d1_unpack_roadmap: roadh RLE ([len, value] uint pairs, run until
 *    cells filled) -> a per-cell uint array (height/type/angle bits)
 *  - jer_d1_convert_cell_objects: 16-byte CELL_OBJECTs (absolute pos,
 *    raw model type) -> the engine's 8-byte PACKED_CELL_OBJECT format
 *    (relative u16 pos, value = (type << 6) | yang) so every existing
 *    consumer (draw, collision, camera) works unchanged. Returns the number
 *    of objects converted. */
void jer_d1_unpack_surfaceroads(const void* src, int cells, void* out);
void jer_d1_unpack_roadmap(const void* src, int cells, void* out);
int  jer_d1_convert_cell_objects(const void* src, int nbytes, void* out);
/* D1 straddler objects (right after the MAP lump header) are 16-byte
 * CELL_OBJECTs too; convert count of them into the engine's packed buffer. */
void jer_d1_convert_straddlers(const void* src, int count, void* out);

/* per-barrel D1 region spooling. LoadRegionData's D1 branch calls
 * region_begin (sizes in CD sectors), requests the roadm/roadh/cell-object
 * chunks into the scratch buffers, then region_finish decodes the RLE
 * streams and converts the cell objects once everything has landed. */
int  jer_d1_region_begin(int barrel, int roadmSectors, int roadhSectors, int cellObjectSectors);
void jer_d1_region_clear(int barrel);
void jer_d1_region_finish(int barrel);
char* jer_d1_region_roadm_scratch(int barrel);
char* jer_d1_region_roadh_scratch(int barrel);
char* jer_d1_region_cell_object_scratch(int barrel);

/* per-barrel D1 region data (barrels 0..3) set by LoadRegionData; used by
 * the D1 surface query. Both stay valid until the next level loads. */
void jer_d1_region_set(int barrel, const void* roadmapCells, const void* surfaceRoadCells);
const void* jer_d1_region_roadmap(int barrel);      /* uint per cell or NULL */
const void* jer_d1_region_surfaceroads(int barrel); /* ushort per cell or NULL */

/* set the D1 map grid (called from ProcessMapLump when gDriver1Level) */
void jer_d1_set_grid(int cellSize, int regionSize);

/* D1 terrain/surface query — the Driver 1 equivalent of FindSurfaceD2.
 * Ported from the OpenDriver2Tools Driver1LevelMap::FindSurface. Fills the
 * out params like FindSurfaceD2: outY = ground height (PSX y-down
 * convention), outN = surface normal, outSurface = SURF_* type.
 * Returns 0 on success (a plane was found). */
int jer_d1_find_surface(int posX, int posY, int posZ,
	int* outNx, int* outNy, int* outNz, int* outY, int* outSurface);

/* D1 road-map unit scale (750-unit half-cells vs Driver 2's 512). */
int jer_d1_roadmap_scale(void);

/* D1 player start positions per city: [x, rotation(0..4095), z]. */
void jer_d1_start_pos(int city, int* x, int* rot, int* z);

#ifdef __cplusplus
}
#endif

#endif /* JERICHO_JER_D1_H */
