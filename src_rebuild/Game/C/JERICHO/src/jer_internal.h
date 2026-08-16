/*
 * jer_internal.h — shared between jer_system.c and jer_manager.c.
 * Not part of the public SDK surface (modules only see jericho.h).
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
 *     MODS/<id>/      installed module sources (compiled in at build time)
 *     CONFIG/modlist.ini   module status + load order (this file)
 *     CONFIG/<id>.ini      per-module settings (jer_config store)
 */
#define JER_MODLIST_PATH "modlist.ini"

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
 * Read <root>/CONFIG/modlist.ini into st. Returns 0 on success (a missing
 * file yields an empty state, i.e. "everything enabled by default"). Returns
 * -1 on a parse error.
 */
int jer_manager_read(const char* rootDir, JER_MODLIST_STATE* st);

/* Write st out to <root>/CONFIG/modlist.ini. Returns 0 on success. */
int jer_manager_write(const char* rootDir, const JER_MODLIST_STATE* st);

#endif /* JERICHO_JER_INTERNAL_H */
