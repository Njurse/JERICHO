#ifndef JER_CITY_H
#define JER_CITY_H

/* jer_city — the engine's city registry, as a module sees it.
 *
 * WHY. A city index used to be a compile-time row of the engine's three tables
 * (LevelNames / LevelFiles / CosmeticFiles), which meant the engine could only
 * ever open a car-data file it had been compiled to name: LEVELS\CHICAGO.LEV,
 * JERICHO\MODS\carhacks\D1CARS\MIAMI.LEV and the rest. carhacks' cross-city import is POLICY - "resident
 * slot 5 = city 2, model 7" - and is already index-based, so a car-data bank that
 * is not one of those compiled names could not be offered at all: nothing would
 * ever open the file, whatever the config said.
 *
 * The registry closes that. JERICHO/CONFIG/cities.ini names up to 16 arbitrary
 * banks:
 *
 *     [cities]
 *     city_0 = BETTERCHI, LEVELS\BCCHI.LEV, LEVELS\BCCHI.LCF
 *     city_1 = MYBANK, MODS\banks\MB.LEV
 *
 * city_N owns city index 9 + N (the nine stock cities are 0..8: Driver 2's four,
 * then Driver 1's five car-data cities). The number is FIXED for the process, so a
 * config that names city 10 names the same bank tomorrow. Paths are given relative
 * to the game's data folder (a subfolder is fine); the colours file is optional.
 *
 * A bank is a CAR-DATA SOURCE, exactly like Driver 1's five cities: its cars can be
 * imported, and it is never played - no level, no frontend city row, no spool.
 *
 * A bank is only registered when its .LEV is actually on disk, and the list is
 * re-scanned before every car list and every level load: install the files and the
 * bank appears, delete them and it is gone, with no restart. So a module must ask
 * JerCityRegistered() rather than assume a configured bank exists, and must never
 * assume the registered rows are contiguous - a missing city_N leaves a hole.
 *
 *     for (c = JerCityFirstCustom(); c < JerCityCount(); c++)
 *     {
 *         unsigned models;
 *
 *         if (!JerCityRegistered(c))
 *             continue;
 *
 *         models = JerCityCarModels(c);
 *         if (models != 0)
 *             ... offer the models in `models` for city c ...
 *     }
 *
 * The names are the engine's own (`Jer` prefix), because that is what the exe
 * exports and what the rest of the module-side car API already calls
 * (JerLoadCarImportForCity, JerCarImportModels).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* JerCityFile kinds. */
#define JER_CITY_FILE_LEVEL		0	/* the car-data .LEV (LUMP_CAR_MODELS etc.) */
#define JER_CITY_FILE_COSMETIC		1	/* the .LCF beside it (optional) */

/* How many city rows are in play: the stock nine plus every bank row that has been
 * assigned (a configured-but-missing bank still counts, so this is the bound to
 * LOOP to - skip the rows where JerCityRegistered() is 0). Never more than 25. */
int JerCityCount(void);

/* The first row a bank can occupy (9). Rows below it are the stock cities, which
 * are always registered. */
int JerCityFirstCustom(void);

/* 1 when the row has a name and a car-data file, i.e. it is safe to use. */
int JerCityRegistered(int city);

/* The row's display name ("" when unregistered - never NULL). */
const char* JerCityName(int city);

/* The file the registry holds for that kind, as cities.ini wrote it ("" when none).
 * Relative to the game's data folder; JerCityPath is the openable form. */
const char* JerCityFile(int city, int kind);

/* Build the openable path for a registered row (data root + entry, slashes
 * normalised). Answers 1 on success, 0 when the row is unregistered or `out` is
 * too small. */
int JerCityPath(int city, char* out, int outSize, int kind);

/* 1 when that file is on disk right now. */
int JerCityFileExists(int city, int kind);

/* Register a bank from a module (normally the config does it). Answers its city
 * index - the first free bank row - or -1 when there is no free row or the .LEV is
 * not on disk. The strings are copied. A NULL `levFile` unregisters the bank of
 * that name. */
int JerCityRegister(const char* name, const char* levFile, const char* lcfFile);

/* Re-read the config and re-probe the disk. Called by the engine before every car
 * list and every level load; a module may call it too. Answers the bank count. */
int JerCityRegistryRefresh(void);

/* The models this city's car data carries, as a bitmask (bit N = model N), loading
 * the bank's car data first when it is not loaded yet. 0 means "offer no cars from
 * this city": the row is unregistered, not installed, or carries nothing.
 *
 * Frontend car-list slot order is the bit order, so a caller filling the engine's
 * own table does:
 *
 *     unsigned m = JerCityCarModels(city);
 *     for (model = 0; model < 13 && slot < 10; model++)
 *         if (m & (1u << model))
 *             carNumLookup[city][slot++] = (char)model;
 */
unsigned JerCityCarModels(int city);

#ifdef __cplusplus
}
#endif

#endif /* JER_CITY_H */
