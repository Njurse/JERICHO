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

// JERICHO: civ_clut has two banks of 8 car-palette rows - 0..7 the host level's own,
// 8..15 a cross-city import's - so a foreign car can be painted from its own city's
// palettes without overwriting the host's. See the comment on civ_clut in cars.c.
#define CIV_CLUT_ROWS		16
#define CIV_CLUT_IMPORT_ROW	8

// ---------------------------------------------------------------------------
// The CLUT column's budget, measured (tools/vrammap.py + the JERICHO_PAL_DIAG readings
// in texture.c print it step by step on every level load):
//
//   after the host's palettes            y=304    (48 rows)
//   after an import's palettes           y=361    (57 rows - the WHOLE foreign table)
//   after the level's page CLUTs         y=445    (84 rows)
//   after the streamed-slot walk         y=485    (40 rows, 8 per streamed slot)
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
#define CAR_CLUT_IMPORT_LIMIT	476

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

/* Cross-city car data: which city's LEVELS\<CITY> folder the CARMODEL_* files
 * (.MDL/.COS/.DEN) are read from. -1 = the level's own city (stock). A module
 * sets it (see JER_EVENT_CAR_DATA_SOURCE) so a level can load another city's
 * vehicles. GetCarDataFolder() is the single place that resolves it, so all
 * three loaders agree on the folder. */
extern int gCarDataSourceLevel;
extern const char* GetCarDataFolder(void);

#endif
