/*
 * jer_system.c — JERICHO runtime: module table + activation, hook registry,
 * dispatch, override slots, logging.
 *
 * Compiled into the game. Modules are NOT compiled in anymore: the loader
 * (jer_loader.c) scans JERICHO/MODS at runtime, loads each compiled
 * <id>.dll/.so and fills the module table. This file drives them: read
 * JERICHO/CONFIG/modlist.ini (status + load order), activate the enabled
 * modules just-in-time at boot, fire JER_EVENT_BOOT, and dispatch every
 * event the engine fires. Adding a mod = drop folder + compiled binary;
 * no exe rebuild.
 */
#include "jericho.h"
#include "jer_internal.h"
#include "jer_config.h"
#include "jer_pause_menu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define JER_MAX_HANDLERS 64

/* Generated registry — compiled-in modules (deep mods, auto-scanned by
 * premake). The registry TU is C++-compiled and wraps its definitions in
 * extern "C", so these declarations must match. The runtime merges these
 * with the loader's runtime DLL addons below. */
#if defined(__cplusplus)
extern "C" {
#endif
extern const JER_REGISTRY_ENTRY jer_registry_modules[];
extern const int jer_registry_module_count;
#if defined(__cplusplus)
}
#endif

typedef struct JER_HANDLER
{
	int event;
	JER_HOOK_FN fn;
	void* userdata;
	int priority;
	JER_MODULE* module;	/* owning module (NULL = engine-side handler) */
} JER_HANDLER;

static JER_HANDLER gHandlers[JER_MAX_HANDLERS];
static int gHandlerCount;
static JER_MODULE gModules[JER_MAX_MODULES];
static int gModuleCount;
static JER_MODULE* gCurrentModule;	/* module whose entry is running */
static void* gOverrideSlots[JER_OVERRIDE_SLOTS];
static JERICHO_CONTEXT gCtx;
static int gCtxBuilt;
static void (*gLogger)(const char* msg);
static char gJerichoRoot[512];		/* central JERICHO folder passed to jer_init */
static const char* gLoadOrder[JER_MAX_MODULES];	/* effective load order */
static int gLoadOrderCount;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void jerEmit(const char* msg)
{
	if (gLogger != NULL)
		gLogger(msg);
	else
		printf("%s", msg);
}

static void jerLog(const char* fmt, ...)
{
	char buf[512];
	va_list va;

	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);

	buf[sizeof(buf) - 1] = 0;
	jerEmit(buf);
}

static JER_MODULE* jerFindModule(const char* id)
{
	int i;

	for (i = 0; i < gModuleCount; i++)
	{
		if (gModules[i].id[0] != 0 && strcmp(gModules[i].id, id) == 0)
			return &gModules[i];
	}

	return NULL;
}

/* (Re)fill the module table: compiled-in modules from the generated
 * registry first, then runtime-loaded DLL addons from the loader that
 * aren't already compiled in. Called at boot and on manager reload. */
static void jerSnapshotModules(const char* rootDir)
{
	JER_MODULE loaded[JER_MAX_MODULES];
	int loadedCount;
	int i, j;

	jer_loader_unload(gModules, gModuleCount);
	gModuleCount = 0;

	/* 1) compiled-in modules (the game's registry — deep mods) */
	for (i = 0; i < jer_registry_module_count && gModuleCount < JER_MAX_MODULES; i++)
	{
		JER_MODULE* m = &gModules[gModuleCount];

		memset(m, 0, sizeof(*m));
		snprintf(m->id, sizeof(m->id), "%s", jer_registry_modules[i].id != NULL ? jer_registry_modules[i].id : "");
		snprintf(m->name, sizeof(m->name), "%s", m->id);
		m->entry = jer_registry_modules[i].entry;
		m->defaultEnabled = jer_registry_modules[i].defaultEnabled;
		m->valid = 1;
		gModuleCount++;
	}

	/* 2) runtime DLL addons not already compiled in */
	loadedCount = jer_loader_scan(rootDir, loaded, JER_MAX_MODULES);

	for (i = 0; i < loadedCount && gModuleCount < JER_MAX_MODULES; i++)
	{
		int dup = 0;

		for (j = 0; j < gModuleCount; j++)
		{
			if (strcmp(gModules[j].id, loaded[i].id) == 0)
			{
				dup = 1;
				break;
			}
		}

		if (dup)
		{
			/* already compiled in — drop the loader copy (and its handle) */
			jer_loader_unload(&loaded[i], 1);
			continue;
		}

		gModules[gModuleCount++] = loaded[i];
	}
}

/* ------------------------------------------------------------------ */
/* MOD_CONTEXT function pointers (called by modules)                   */
/* ------------------------------------------------------------------ */

static void jerCtxRegisterHook(JERICHO_CONTEXT* ctx, int event, JER_HOOK_FN fn, void* userdata, int priority)
{
	int i;

	(void)ctx;

	if (fn == NULL || gHandlerCount >= JER_MAX_HANDLERS)
		return;

	/* insert keeping (event, priority) order — lower priority runs first */
	for (i = gHandlerCount; i > 0; i--)
	{
		if (gHandlers[i - 1].event < event ||
			(gHandlers[i - 1].event == event && gHandlers[i - 1].priority <= priority))
		{
			break;
		}

		gHandlers[i] = gHandlers[i - 1];
	}

	gHandlers[i].event = event;
	gHandlers[i].fn = fn;
	gHandlers[i].userdata = userdata;
	gHandlers[i].priority = priority;
	gHandlers[i].module = gCurrentModule;
	gHandlerCount++;
}

static int jerCtxFire(JERICHO_CONTEXT* ctx, int event, void* args)
{
	int result = JER_RESULT_CONTINUE;
	int i;

	(void)ctx;

	for (i = 0; i < gHandlerCount; i++)
	{
		if (gHandlers[i].event != event)
			continue;

		/* skip handlers owned by modules that failed validation */
		if (gHandlers[i].module != NULL && !gHandlers[i].module->valid)
			continue;

		result = gHandlers[i].fn(gHandlers[i].userdata, args);

		if (result == JER_RESULT_STOP)
			break;
	}

	return result;
}

static void jerCtxOverride(JERICHO_CONTEXT* ctx, int slot, void* fn)
{
	(void)ctx;

	if (slot < 0 || slot >= JER_OVERRIDE_SLOTS)
		return;

	gOverrideSlots[slot] = fn;
}

static void jerCtxLog(JERICHO_CONTEXT* ctx, const char* fmt, ...)
{
	char buf[512];
	va_list va;

	(void)ctx;

	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);

	buf[sizeof(buf) - 1] = 0;
	jerEmit(buf);
}

static void jerCtxRegisterModule(JERICHO_CONTEXT* ctx,
	const char* id, const char* name, const char* version,
	const char* author, const char* description, const char* deps,
	int sdkVersion)
{
	JER_MODULE* m;

	(void)ctx;

	m = jerFindModule(id);

	if (m == NULL)
	{
		jerLog("[jericho] warning: module \"%s\" registered itself but is not in the runtime table\n", id);
		return;
	}

	/* copy into the owned buffers: the strings live in the DLL image and
	 * would dangle after a reload unloads the binary */
	if (name != NULL)
		snprintf(m->name, sizeof(m->name), "%s", name);

	if (version != NULL)
		snprintf(m->version, sizeof(m->version), "%s", version);

	if (author != NULL)
		snprintf(m->author, sizeof(m->author), "%s", author);

	if (description != NULL)
		snprintf(m->description, sizeof(m->description), "%s", description);

	if (deps != NULL)
		snprintf(m->deps, sizeof(m->deps), "%s", deps);

	m->sdkVersion = sdkVersion;
	m->metadataSet = 1;
}

/* ------------------------------------------------------------------ */
/* Boot inventory                                                      */
/* ------------------------------------------------------------------ */

/* Human-readable names for the engine-defined events (diagnostics). */
static const char* jerEventName(int event)
{
	switch (event)
	{
	case JER_EVENT_BOOT:			return "BOOT";
	case JER_EVENT_FRAME:			return "FRAME";
	case JER_EVENT_PRE_SIM:			return "PRE_SIM";
	case JER_EVENT_INIT:			return "INIT";
	case JER_EVENT_COLLISION:		return "COLLISION";
	case JER_EVENT_DENT_PASS:		return "DENT_PASS";
	case JER_EVENT_RESET_CAR:		return "RESET_CAR";
	case JER_EVENT_DEBUG_TICK:		return "DEBUG_TICK";
	case JER_EVENT_GET_WHEEL_BEND:	return "GET_WHEEL_BEND";
	case JER_EVENT_GET_WHEEL_DAMAGE:return "GET_WHEEL_DAMAGE";
	case JER_EVENT_GET_IMPACT_INFO:	return "GET_IMPACT_INFO";
	case JER_EVENT_GET_WHEEL_PARAMS:return "GET_WHEEL_PARAMS";
	case JER_EVENT_GET_BUDDHA:		return "GET_BUDDHA";
	case JER_EVENT_DRAW_WHEEL:		return "DRAW_WHEEL";
	case JER_EVENT_PAUSE_MENU:		return "PAUSE_MENU";
	case JER_EVENT_DRAW_OVERLAY:	return "DRAW_OVERLAY";
	case JER_EVENT_CAMERA:			return "CAMERA";
	case JER_EVENT_GAME_START:		return "GAME_START";
	case JER_EVENT_CAMERA_LOOK:		return "CAMERA_LOOK";
	case JER_EVENT_PED_INPUT:		return "PED_INPUT";
	case JER_EVENT_PED_MOVE:		return "PED_MOVE";
	case JER_EVENT_PED_POSE:		return "PED_POSE";
	case JER_EVENT_FRONTEND:		return "FRONTEND";
	case JER_EVENT_PED_SKELETON:	return "PED_SKELETON";
	case JER_EVENT_SHUTDOWN:		return "SHUTDOWN";
	default:
		if (event >= JER_EVENT_MODULE_CUSTOM)
		{
			static char customName[24];

			snprintf(customName, sizeof(customName), "CUSTOM(%d)", event);
			return customName;
		}

		return "UNKNOWN";
	}
}

/*
 * Emit the boot diagnostics: one line per compiled-in module (state + reason)
 * and one per registered hook handler. Re-emitted whenever modules are
 * (re)activated, so a manager reload also re-reports what is actually live.
 */
static void jerLogInventory(void)
{
	int i;

	jerLog("[jericho] --- module inventory (%d loaded) ---\n", gModuleCount);

	for (i = 0; i < gModuleCount; i++)
	{
		JER_MODULE* m = &gModules[i];
		const char* state;

		if (!m->enabled)
			state = "disabled";
		else if (!m->activated)
			state = "not-activated";
		else if (!m->valid)
			state = "INVALID";
		else
			state = "active";

		jerLog("[jericho]   %-10s v%-6s enabled=%d state=%-14s author=\"%s\" deps=\"%s\"\n",
			m->id,
			m->version[0] != 0 ? m->version : "?",
			m->enabled,
			state,
			m->author[0] != 0 ? m->author : "?",
			m->deps[0] != 0 ? m->deps : "-");
	}

	jerLog("[jericho] --- hook inventory (%d handler(s)) ---\n", gHandlerCount);

	for (i = 0; i < gHandlerCount; i++)
	{
		JER_HANDLER* h = &gHandlers[i];

		jerLog("[jericho]   event=%-20s module=%-10s priority=%d\n",
			jerEventName(h->event),
			h->module != NULL ? h->module->id : "(engine)",
			h->priority);
	}
}

/* ------------------------------------------------------------------ */
/* Activation                                                          */
/* ------------------------------------------------------------------ */

static void jerActivateModules(const char* rootDir)
{
	JER_MODLIST_STATE modlist;
	const char* order[JER_MAX_MODULES];
	int orderCount = 0;
	int active = 0;
	int i, j;

	/*
	 * Resolve the load order: modlist.ini entries first (in file order),
	 * then any modules not mentioned (enabled by default, in scan
	 * order). Modules listed with enabled=0 are skipped.
	 */
	if (jer_manager_read(rootDir, &modlist) != 0)
	{
		jerLog("[jericho] warning: could not read %s/%s/modlist.ini — defaulting to all-enabled\n", rootDir, JER_CONFIG_PATH);
		memset(&modlist, 0, sizeof(modlist));	/* error: treat as empty state */
	}

	/* module-provided pause menus re-register during activation: clear the
	 * registry first so a Mods-menu reload doesn't accumulate duplicates */
	jer_pause_menu_reset();

	/* apply the modlist enable flags to the module table */
	for (i = 0; i < modlist.count; i++)
	{
		JER_MODULE* m = jerFindModule(modlist.items[i].id);

		if (m == NULL)
		{
			jerLog("[jericho] modlist references unknown module \"%s\" (not installed)\n", modlist.items[i].id);
			continue;
		}

		m->enabled = modlist.items[i].enabled;

		if (m->enabled && orderCount < JER_MAX_MODULES)
			order[orderCount++] = m->id;
	}

	/* unlisted modules default to their mod.toml defaultEnabled */
	for (i = 0; i < gModuleCount; i++)
	{
		int listed = 0;

		for (j = 0; j < modlist.count; j++)
		{
			if (gModules[i].id[0] != 0 && strcmp(gModules[i].id, modlist.items[j].id) == 0)
			{
				listed = 1;
				break;
			}
		}

		if (!listed && orderCount < JER_MAX_MODULES)
		{
			gModules[i].enabled = gModules[i].defaultEnabled;

			if (gModules[i].enabled)
				order[orderCount++] = gModules[i].id;
		}
	}

	/* activate in order */
	for (i = 0; i < orderCount; i++)
	{
		JER_MODULE* m = jerFindModule(order[i]);

		if (m == NULL || !m->enabled)
			continue;

		if (m->entry == NULL)
		{
			jerLog("[jericho] \"%s\" enabled but no compiled binary found (compile it first)\n", order[i]);
			continue;
		}

		jerLog("[jericho] activating \"%s\"\n", order[i]);
		gCurrentModule = m;
		m->entry(&gCtx);
		gCurrentModule = NULL;
		m->activated = 1;
		active++;
	}

	/* validate: SDK-version match + dependency resolution. Modules that
	 * fail are marked invalid; their handlers are skipped at dispatch and
	 * they are reported with a clear reason. */
	for (i = 0; i < gModuleCount; i++)
	{
		JER_MODULE* m = &gModules[i];

		if (!m->activated)
			continue;

		if (m->sdkVersion != 0 && m->sdkVersion != JERICHO_SDK_VERSION)
		{
			jerLog("[jericho] module \"%s\" needs SDK v%d but host is v%d — DISABLED\n",
				m->id, m->sdkVersion, JERICHO_SDK_VERSION);
			m->valid = 0;
			continue;
		}

		if (m->deps[0] != 0)
		{
			char dep[40];
			const char* p = m->deps;
			int missing = 0;

			while (*p != 0 && !missing)
			{
				const char* comma = strchr(p, ',');
				int len = comma != NULL ? (int)(comma - p) : (int)strlen(p);

				if (len > 0 && len < (int)sizeof(dep))
				{
					JER_MODULE* depModule;

					memcpy(dep, p, (size_t)len);
					dep[len] = 0;

					depModule = jerFindModule(dep);

					if (depModule == NULL || !depModule->enabled || !depModule->activated)
						missing = 1;
				}

				p = comma != NULL ? comma + 1 : p + strlen(p);
			}

			if (missing)
			{
				jerLog("[jericho] module \"%s\" DISABLED: missing dependency (\"%s\")\n", m->id, m->deps);
				m->valid = 0;
				continue;
			}
		}
	}

	jerLog("[jericho] %d module(s) active (SDK v%d)\n", active, JERICHO_SDK_VERSION);

	/* record the effective load order for the manager UI */
	gLoadOrderCount = 0;

	for (i = 0; i < orderCount && gLoadOrderCount < JER_MAX_MODULES; i++)
	{
		JER_MODULE* m = jerFindModule(order[i]);

		if (m != NULL && m->enabled)
			gLoadOrder[gLoadOrderCount++] = m->id;
	}

	/* boot diagnostics: what is built in, what is live, and which hooks
	 * are registered (re-emitted on manager reload too) */
	jerLogInventory();
}

/* ------------------------------------------------------------------ */
/* Public host-side API                                                */
/* ------------------------------------------------------------------ */

void jer_init(const char* rootDir)
{
	if (!gCtxBuilt)
	{
		gCtx.sdkVersion = JERICHO_SDK_VERSION;
		gCtx.jer_register_hook = jerCtxRegisterHook;
		gCtx.jer_fire = jerCtxFire;
		gCtx.jer_override = jerCtxOverride;
		gCtx.jer_log = jerCtxLog;
		gCtx.jer_register_module = jerCtxRegisterModule;
		gCtxBuilt = 1;
	}

	if (rootDir != NULL)
	{
		snprintf(gJerichoRoot, sizeof(gJerichoRoot), "%s", rootDir);
	}
	else
	{
		gJerichoRoot[0] = 0;
	}

	jerLog("== JERICHO v%d (build %s) == Just-in-Time Extensible Runtime Interface for Compiled Hooks & Overrides\n", JERICHO_SDK_VERSION, JERICHO_BUILD_VERSION);

	/* persistent module config lives under <root>/CONFIG/ */
	jer_config_init(rootDir);

	jerSnapshotModules(rootDir);
	jerActivateModules(rootDir);

	jer_fire(JER_EVENT_BOOT, NULL);
}

int jer_fire(int event, void* args)
{
	if (!gCtxBuilt || gHandlerCount == 0)
		return JER_RESULT_CONTINUE;

	return jerCtxFire(&gCtx, event, args);
}

void jer_log(const char* fmt, ...)
{
	char buf[512];
	va_list va;

	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);

	buf[sizeof(buf) - 1] = 0;
	jerEmit(buf);
}

void jer_set_logger(void (*fn)(const char* msg))
{
	gLogger = fn;
}

void jer_override(int slot, void* fn)
{
	jerCtxOverride(&gCtx, slot, fn);
}

void* jer_get_override(int slot)
{
	if (slot < 0 || slot >= JER_OVERRIDE_SLOTS)
		return NULL;

	return gOverrideSlots[slot];
}

/* Re-read JERICHO/CONFIG/modlist.ini and re-activate (used after toggles). */
void jer_manager_reload(const char* rootDir)
{
	/* deactivate by clearing handlers; also drop all override slots so a
	 * module disabled from the Mods menu can't leave its override behind */
	gHandlerCount = 0;
	memset(gOverrideSlots, 0, sizeof(gOverrideSlots));

	jerSnapshotModules(rootDir);
	jerActivateModules(rootDir);
}

/* ------------------------------------------------------------------ */
/* Module inventory                                                    */
/* ------------------------------------------------------------------ */

int jer_module_count(void)
{
	/* all compiled-in modules (the full manager list, disabled included) */
	return gModuleCount;
}

int jer_module_list(JER_MODULE_INFO* out, int max)
{
	JER_MODLIST_STATE modlist;
	int n = 0;
	int i, j;

	memset(&modlist, 0, sizeof(modlist));

	/* the manager shows the FULL list in modlist.ini order (disabled
	 * modules included, so they can be re-enabled), with unlisted modules
	 * appended per their default-enabled state */
	if (jer_manager_read(gJerichoRoot, &modlist) == 0)
	{
		for (i = 0; i < modlist.count && n < max; i++)
		{
			JER_MODULE* m = jerFindModule(modlist.items[i].id);

			if (m == NULL)
				continue;

			out[n].id = m->id;
			out[n].name = m->name != NULL ? m->name : m->id;
			out[n].version = m->version[0] != 0 ? m->version : "?";
			out[n].enabled = modlist.items[i].enabled && m->valid;
			n++;
		}
	}

	for (i = 0; i < gModuleCount && n < max; i++)
	{
		int listed = 0;

		for (j = 0; j < modlist.count; j++)
		{
			if (gModules[i].id[0] != 0 && strcmp(gModules[i].id, modlist.items[j].id) == 0)
			{
				listed = 1;
				break;
			}
		}

		if (!listed)
		{
			out[n].id = gModules[i].id;
			out[n].name = gModules[i].name != NULL ? gModules[i].name : gModules[i].id;
			out[n].version = gModules[i].version[0] != 0 ? gModules[i].version : "?";
			out[n].enabled = gModules[i].defaultEnabled && gModules[i].valid;
			n++;
		}
	}

	return n;
}

/* The central JERICHO folder passed to jer_init ("JERICHO" in the game's
 * call site — contains MODS/ and CONFIG/). */
const char* jer_root_dir(void)
{
	return gJerichoRoot[0] != 0 ? gJerichoRoot : "JERICHO";
}
