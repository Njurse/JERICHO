#ifndef CARS_H
#define CARS_H


#define CAR_INDEX(cp)  (int)(cp-car_data)

#define IS_ROADBLOCK_CAR(cp) (cp->controlType == CONTROL_TYPE_CIV_AI && (cp->controlFlags & CONTROL_FLAG_COP_SLEEPING))

// PHYSICS
extern CAR_DATA car_data[MAX_CARS + 2];	// all cars + Tanner cbox + Camera cbox

// active cars
extern CAR_DATA* active_car_list[MAX_CARS];
extern unsigned char lightsOnDelay[MAX_CARS];

extern CAR_MODEL NewCarModel[MAX_CAR_RESIDENT_MODELS];
extern CAR_MODEL NewLowCarModel[MAX_CAR_RESIDENT_MODELS];

extern MODEL* gCarLowModelPtr[MAX_CAR_RESIDENT_MODELS];
extern MODEL* gCarDamModelPtr[MAX_CAR_RESIDENT_MODELS];
extern MODEL* gCarCleanModelPtr[MAX_CAR_RESIDENT_MODELS];

extern int whichCP;		// car poly counter
extern int baseSpecCP;	// special car poly counter

extern SVECTOR gTempCarVertDump[MAX_CARS][MAX_DENTING_VERTS];
extern DENTUVS gTempHDCarUVDump[MAX_CARS][MAX_DENTING_UVS];
extern DENTUVS gTempLDCarUVDump[MAX_CARS][MAX_DENTING_LOD_UVS];

extern MODEL *gHubcapModelPtr;
extern MODEL *gCleanWheelModelPtr;
extern MODEL *gFastWheelModelPtr;
extern MODEL *gDamWheelModelPtr;

extern short FrontWheelRotation[MAX_CARS];
extern short BackWheelRotation[MAX_CARS];

extern char LeftLight;
extern char RightLight;

// JERICHO: civ_clut is a table of 8-row car-palette BLOCKS. Rows 0..7 are the host
// level's own; each guest city gets a block of its own after that, so a foreign car is
// painted from its own city's palettes without overwriting anyone else's. See the
// comment on civ_clut in cars.c.
//
// The block height is 8 and cannot be narrower: CarPalIndexInCity returns `i + rowbase`
// where `i` is the index into carTpages[city][0..7], so a city's rows span its whole
// block even though only the rows its polgyons name carry data (measured: 2 of them).
// Hence 8 + 8 per guest city - three guests fit in 32.
#define CIV_CLUT_ROWS		32
#define CIV_CLUT_IMPORT_ROW	8
#define CIV_CLUT_BLOCK_ROWS	8			// the height of one city's block (carTpages' range)

// ---------------------------------------------------------------------------
// The CLUT column's budget, measured (tools/vrammap.py + the JERICHO_PAL_DIAG readings
// in texture.c print it step by step on every level load):
//
//   after the host's palettes            y=304    (48 rows)
//   after an import's palettes           y=361    (57 rows - the WHOLE foreign table)
//   after the level's page CLUTs         y=445    (84 rows)
//   after the streamed-slot walk         y=485    (40 rows, 8 per streamed slot)
//
// JERICHO: the middle line is no longer what happens. The import's table is now DEFERRED
// to CarImportPin and uploaded for only the rows the built model names (2 of the bank's
// 8), so it costs 38 rows instead of 57 and the streamed-slot walk then ends at 466
// (RIO -> Havana, exactly fitting) or 470 (CHICAGO -> Vegas, 4 rows over) instead of 485.
// See ProcessPalletLumpForRows / ProcessImportedPaletteRows, and VRAM.md 6.1.
//
// and the level font image is `(960,466) 64x46` (pres.c:584) - the full width of the
// column for rows 466..511. So the CLUT-safe area is 256..465 (210 rows) and the layout
// needs 229: an import pushes the level's own CLUTs 19 rows into the font, and the pin
// band (which starts at clutpos+4 = 489) lands inside it too. Measured: the HUD font and
// the imported car's palettes overwrite each other every frame.
//
// The reserve below stops the import's palette upload from making it worse, and the
// overflow reuses the palette stored for the SAME PAGE (see ProcessPalletLumpForCity) so
// a squeezed import keeps its own colours rather than another car's. Freeing the 19+
// rows the column is short needs one of the static consumers packed - the level's page
// CLUTs (84 rows for 12 pages) or the streamed-slot walk (40 rows) - see VRAM.md §6.
//
// JERICHO: CAR_CLUT_IMPORT_LIMIT was 476, which is INSIDE the level font image
// (960,466) 64x46 (pres.c:584) - so the limit permitted the import to write rows
// 466..475, i.e. straight over the glyphs, and the pin band (which forced itself to a
// floor of y=480) landed in the font as well. Both are the same bug: a literal ceiling
// where the layout's actual ceiling is "the last row above the font image". The
// constants below name it, and everything that bounds a CLUT row uses them instead of a
// literal - CAR_CLUT_IMPORT_LIMIT is now the font's last safe row, not an arbitrary one.
#define CD2_CLUT_SAFE_FIRST	256		// the CLUT column starts here (LoadPermanentTPages)
#define CD2_CLUT_SAFE_LAST	465		// the last row above the level font image (466..511)
#define CAR_CLUT_IMPORT_LIMIT	CD2_CLUT_SAFE_LAST

// JERICHO: the bottom half of VRAM is a JERICHO-owned pool - JerLowerPoolPageAlloc /
// JerLowerPoolClutAlloc in texture.c, and carhacks/docs/VRAM.md. VRAM is 1024 rows tall
// in this build (PsyCross, PsyX_render.h), and the base game only ever addresses rows
// 0..511: the display buffers at (0,0) and (0,256), the page-slot walk tpagepos[] (Y in
// {0,256}), the sky, and the level font. Rows 512..1023 are therefore space NO stock
// path touches - which is the whole point: an import placed there cannot take a world
// page's rectangle (crosscheck INV1) or a host palette's CLUT row (INV2), because it is
// not competing for them in the first place.
#define JER_VRAM_TOTAL_ROWS	1024	// = PsyX VRAM_HEIGHT
#define JER_VRAM_HALF_Y		512		// the lower half pool's first row (rows 512..1023); tpage Y bit 9

extern u_short civ_clut[CIV_CLUT_ROWS][32][6];

extern void DrawCar(CAR_DATA *cp, int view); // 0x000210B8

#ifndef PSX
// [A] loads car model from file
char* LoadCarModelFromFile(char* dest, int modelNumber, int type);
#endif

extern MODEL* GetCarModel(char *src, char **dest, int KeepNormals); // 0x00065134
extern void startBuildNewCars(int isSpecial); // 0x00022860
extern void buildNewCarFromModel(int index, int detail, char* polySrc, MODEL* model); // 0x00022960

extern void MangleWheelModels(); // 0x000230C8

extern char GetCarPalIndex(int tpage); // 0x00023390
extern int CarPalIndexInCityFor(int tpage, int city); // JERICHO: the same, given the city (cars.c)
extern void CarImportPaletteReset(void); // JERICHO: forget a level's deferred palette work (cars.c)

/* Cross-city car data: which city's LEVELS\<CITY> folder the CARMODEL_* files
 * (.MDL/.COS/.DEN) are read from. -1 = the level's own city (stock). A module
 * sets it (see JER_EVENT_CAR_DATA_SOURCE) so a level can load another city's
 * vehicles. GetCarDataFolder() is the single place that resolves it, so all
 * three loaders agree on the folder. */
extern int gCarDataSourceLevel;
extern const char* GetCarDataFolder(void);

#endif
