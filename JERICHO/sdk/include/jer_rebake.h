#ifndef JER_REBAKE_H
#define JER_REBAKE_H

/* jer_rebake -- the one-shot "re-bake your Driver 1 cars?" prompt.
 *
 * It asks on the main menu when the bake is older than the tooling, waits for
 * the mod store's startup update question, and can be re-armed after a mod
 * install brings newer tooling. jer_modstore.c calls the re-arm; FEmain.c calls
 * the tick. */

void jer_rebake_frontend_tick(int onMainMenu);

/* Let the next quiet main-menu frame ask again (one-shot per run otherwise). */
void jer_rebake_rearm(void);

/* What Yes on those questions does: run BAKE_D1_CARS --from-game and exit so
 * the script can relaunch the game. Options -> JERICHO -> Bake calls this too. */
void jer_rebake_start(void);

/* ---- where the baked Driver 1 cars are (jer_d1cars.c) ----
 *
 * The bake is found from the LEV files, not the BAKE_VERSION stamp alone, in
 * JERICHO/MODS/carhacks/D1CARS, then the optional CONFIG/jericho.ini key
 * d1cars_path (unset by default; never written), then the legacy DRIVER/D1CARS,
 * then <sibling install>/JERICHO/MODS/carhacks/D1CARS beside this one. The
 * newest stamped bake wins, then the most valid LEVs, then the earliest place.
 * The engine loads from the folder chosen (system.c gD1DataFolder) and the
 * prompt above asks about its version. Runtime-side; not in the module host
 * table. */

#define JER_D1CARS_PATH_KEY	"d1cars_path"
#define JER_D1CARS_ROOT_MAX	160	/* system.c gD1DataFolder */

#ifdef __cplusplus
extern "C" {
#endif

/* Look again; answers the chosen folder's valid LEV count (0 = none found).
 * The other calls scan once on first use. */
int jer_d1cars_scan(void);

/* The folder HOLDING D1CARS, engine style: backslashes, trailing '\\',
 * relative to the game folder unless d1cars_path was absolute.
 * "JERICHO\\MODS\\carhacks\\" when nothing was found. */
const char* jer_d1cars_root(void);

/* The chosen bake's BAKE_VERSION: -1 = none found, 0 = valid but unstamped. */
int jer_d1cars_baked_version(void);

/* 1 when the chosen folder holds a valid <city>.LEV ("MIAMI", "BETAMIAMI"...). */
int jer_d1cars_has(const char* city);

/* 1 when path is a level file the car importer can read and that carries at
 * least one car model (models.c JerLoadCarImportFromFile's own checks). */
int jer_d1cars_lev_valid(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* JER_REBAKE_H */
