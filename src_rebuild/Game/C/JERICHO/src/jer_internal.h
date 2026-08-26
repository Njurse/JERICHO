/*
 * jer_internal.h — shared between jer_system.c, jer_manager.c and
 * jer_loader.c. Not part of the public SDK surface (modules only see
 * jericho.h).
 */
#ifndef JERICHO_JER_INTERNAL_H
#define JERICHO_JER_INTERNAL_H

#include "jericho.h"
#include "jer_config.h"

/*
 * Layout of the central JERICHO framework folder (the path passed to
 * jer_init — next to the game's data files at runtime, and mirrored there
 * from the repo's JERICHO/ by premake's postbuild step):
 *
 *   JERICHO/
 *     MODS/<id>/      installed module folders: mod.toml + <id>.dll (or .so)
 *     CONFIG/modlist.ini   module status + load order (this file)
 *     CONFIG/<id>.ini      per-module settings (jer_config store)
 *
 * Modules are loaded at runtime (not compiled in): the loader scans
 * MODS/, loads each compiled <id>.dll/.so and resolves its entry symbol.
 * A folder without a compiled binary is still listed (so the frontend can
 * show "not compiled"), but is skipped at activation.
 */
#define JER_MODLIST_PATH "modlist.ini"

/* Shared whitespace trimmer (in place; returns the new start). Used by the
 * modlist parser, the mod.toml parser and the ini config loader. */
char* jerTrim(char* s);

/* Parsed JERICHO/CONFIG/modlist.ini state (module id + enabled flag, in
 * file order = load order). */
typedef struct JER_MODLIST_ITEM
{
	char id[40];
	int enabled;
} JER_MODLIST_ITEM;

typedef struct JER_MODLIST_STATE
{
	int count;
	JER_MODLIST_ITEM items[JER_MAX_MODULES];
} JER_MODLIST_STATE;

/*
 * One runtime module. Strings are OWNED buffers (filled from mod.toml by
 * the loader, then possibly overridden by the module's own
 * ctx->jer_register_module during activation) — never pointers into the
 * DLL image, so unloading/reloading a DLL can't leave dangling metadata.
 */
typedef struct JER_MODULE
{
	char id[40];
	char name[64];
	char version[16];
	char author[64];
	char description[128];
	char deps[128];
	void* handle;			/* DLL/.so handle (NULL when not compiled) */
	JER_MODULE_ENTRY entry;	/* resolved entry point (NULL when not compiled) */
	int defaultEnabled;		/* mod.toml default-enabled (used when modlist omits) */
	int enabled;			/* effective enable flag (modlist wins) */
	int activated;			/* entry ran this cycle */
	int metadataSet;		/* module registered metadata via ctx */
	int sdkVersion;			/* from ctx->jer_register_module */
	int valid;				/* passes SDK + dependency validation */
} JER_MODULE;

/* ------------------------------------------------------------------ */
/* Loader (jer_loader.c)                                               */
/* ------------------------------------------------------------------ */

/*
 * Scan <root>/MODS and fill table (up to max entries) with every installed
 * module, loading each compiled binary and resolving its entry symbol.
 * Metadata comes from each mod.toml; missing binaries yield entry=NULL
 * (module listed but not activatable). Returns the number of modules.
 */
int jer_loader_scan(const char* rootDir, JER_MODULE* table, int max);

/* Unload every loaded binary (FreeLibrary/dlclose). Call before rescan. */
void jer_loader_unload(JER_MODULE* table, int count);

/* ------------------------------------------------------------------ */
/* Manager (jer_manager.c)                                             */
/* ------------------------------------------------------------------ */

/*
 * Read <root>/CONFIG/modlist.ini into st. Returns 0 on success (a missing
 * file yields an empty state, i.e. "everything enabled by default"). Returns
 * -1 on a parse error.
 */
int jer_manager_read(const char* rootDir, JER_MODLIST_STATE* st);

/* Write st out to <root>/CONFIG/modlist.ini. Returns 0 on success. */
int jer_manager_write(const char* rootDir, const JER_MODLIST_STATE* st);

#endif /* JERICHO_JER_INTERNAL_H */
