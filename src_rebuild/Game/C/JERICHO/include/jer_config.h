#ifndef JER_CONFIG_H
#define JER_CONFIG_H

/* ------------------------------------------------------------------
 * jer_config.h — lightweight persistent configuration for JERICHO
 * modules.
 *
 * Each module gets its own text file in <mods>/config/<modid>.ini:
 *
 *     # Driver 2 Parallel Lines (d2pl)
 *     sens_x = 64
 *     invert_h = 1
 *
 * The API is a tiny key=value store. Files are read lazily on the
 * first access to a module and rewritten immediately on every set,
 * so values survive restarts with no explicit flush step.
 *
 * PC/emscripten builds persist to disk; on PSX the API is a no-op
 * that always returns the provided default.
 *
 * All functions are thread-free / single-threaded like the rest of
 * the JERICHO runtime (called from the game loop).
 * ------------------------------------------------------------------ */

/* Initialise the config store; modsDir is the directory containing
 * modlist.json ("" or NULL keeps the store inert). Called by jer_init,
 * so modules never need to call this. */
int jer_config_init(const char* modsDir);

/* Where config files live, relative to the mods dir. */
#define JER_CONFIG_PATH "config"

/* The runtime is compiled as C inside JERICHO.lib while the game + modules
 * are C++ — keep the linkage consistent (same pattern as jericho.h). */
#ifdef __cplusplus
extern "C" {
#endif

/* Integer value (default returned when missing or on PSX). */
int jer_config_get_int(const char* mod, const char* key, int def);

/* Persist an integer value. */
void jer_config_set_int(const char* mod, const char* key, int value);

/* Boolean value: stored as 0/1. */
int jer_config_get_bool(const char* mod, const char* key, int def);

/* Persist a boolean value. */
void jer_config_set_bool(const char* mod, const char* key, int value);

/* String value. The returned pointer is valid until the next set call for
 * the same slot (or until the config is re-inited). NOTE: the fallback for
 * missing keys is one shared static buffer, so a get_str of a *different*
 * missing key overwrites it — copy the string if you keep it around. */
const char* jer_config_get_str(const char* mod, const char* key, const char* def);

/* Persist a string value (truncated to the store's slot size). */
void jer_config_set_str(const char* mod, const char* key, const char* value);

/* Delete a module's entire profile (file + cache). The next set_* call
 * regenerates it from scratch — use to flush stale configs when defaults
 * change. No-op on PSX. */
void jer_config_clear(const char* mod);

#ifdef __cplusplus
}
#endif

#endif /* JER_CONFIG_H */
