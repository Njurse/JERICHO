/*
 * jer_internal.h — shared between jer_system.c and jer_manager.c.
 * Not part of the public SDK surface (modules only see jericho.h).
 */
#ifndef JERICHO_JER_INTERNAL_H
#define JERICHO_JER_INTERNAL_H

#include "jericho.h"

#define JER_MODLIST_PATH "modlist.json"

/* Parsed mods/modlist.json state (module id + enabled flag, in list order). */
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
 * Read modsDir/modlist.json into st. Returns 0 on success (a missing file
 * yields an empty state, i.e. "everything enabled by default"). Returns -1
 * on a parse error.
 */
int jer_manager_read(const char* modsDir, JER_MODLIST_STATE* st);

/* Write st out to modsDir/modlist.json. Returns 0 on success. */
int jer_manager_write(const char* modsDir, const JER_MODLIST_STATE* st);

#endif /* JERICHO_JER_INTERNAL_H */
