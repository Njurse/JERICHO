/*
 * jericho.h — JERICHO: Just-in-Time Extensible Runtime Interface for
 * Compiled Hooks & Overrides.
 *
 * A tiny, platform-neutral C API that turns the game into a HOST for
 * compiled-in modules ("packages"). Modules are built from source with the
 * same toolchain as the game itself — there is NO runtime loading, no
 * dlopen/LoadLibrary, no platform-specific code anywhere. Whatever lives in
 * mods/ at build time is linked straight into the binary, and the JERICHO
 * runtime activates modules just-in-time at boot according to
 * mods/modlist.json.
 *
 * Why compile-in? Three properties fall out of it:
 *   - Deterministic security: audit the module source -> build it with your
 *     own toolchain -> the bytes you run are the bytes you read.
 *   - Platform agnosticism: no loader code to port to Windows/Linux/emscripten.
 *   - Minimal intrusion: the vanilla game files only get single inert
 *     jer_fire(...) lines at the hook points; with no handlers registered,
 *     those are no-ops, so upstream merges stay viable.
 *
 * ABI: this header is the contract. Bump JERICHO_SDK_VERSION on any change
 * that breaks module compatibility; the runtime refuses mismatched modules
 * with a clear reason.
 */
#ifndef JERICHO_JERICHO_H
#define JERICHO_JERICHO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JERICHO_BUILD_VERSION
#define JERICHO_BUILD_VERSION "1.0.0"
#endif

#define JERICHO_SDK_VERSION 1

/* Maximum number of compiled-in modules (registry + modlist capacity). */
#define JER_MAX_MODULES 16

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

/*
 * Event IDs fired by the engine at fixed hook points in the vanilla files
 * (and by modules, which may also fire custom events >= JER_EVENT_MODULE_CUSTOM).
 * The argument struct for each event is documented at the engine call sites
 * (search for "JERICHO-HOOK") and in JERICHO/docs/events.md.
 */
enum
{
	JER_EVENT_BOOT = 0,			/* fired once after all modules are activated */
	JER_EVENT_FRAME,			/* every game frame (global step) */
	JER_EVENT_INIT,				/* game / damage system (re)initialisation */
	JER_EVENT_COLLISION,		/* car-car / car-world collision */
	JER_EVENT_DENT_PASS,		/* vertex deformation pass */
	JER_EVENT_RESET_CAR,		/* a car's damage state is being reset */
	JER_EVENT_DEBUG_TICK,		/* per-frame debug/tuning tick */
	JER_EVENT_GET_WHEEL_BEND,	/* query: per-wheel damage bend (result slot) */
	JER_EVENT_GET_WHEEL_DAMAGE,	/* query: cumulative wheel damage 0..4096 */
	JER_EVENT_GET_IMPACT_INFO,	/* query: newest impact for the overlay */
	JER_EVENT_GET_WHEEL_PARAMS,	/* query: wheel-damage physics params (scales + scrub) */
	JER_EVENT_GET_BUDDHA,		/* query: damage-total clamp flag (Buddha mode) */
	JER_EVENT_DRAW_WHEEL,		/* wheel draw: allow visual mesh distortion */
	JER_EVENT_PAUSE_MENU,		/* pause menu draw/update */
	JER_EVENT_DRAW_OVERLAY,		/* per-frame overlay draw (menus over the world) */

	JER_EVENT_MODULE_CUSTOM = 1000	/* modules define custom ids from here */
};

/* Common return values from hook handlers. */
enum
{
	JER_RESULT_CONTINUE = 0,	/* keep going: let the next handler run */
	JER_RESULT_STOP = 1			/* stop dispatching to further handlers */
};

/* ------------------------------------------------------------------ */
/* Hook handlers                                                       */
/* ------------------------------------------------------------------ */

/*
 * A hook handler. `args` points to the event's argument struct; for query
 * events the struct carries a result slot the handler fills in. Return
 * JER_RESULT_CONTINUE or JER_RESULT_STOP.
 */
typedef int (*JER_HOOK_FN)(void* userdata, void* args);

/*
 * Function-pointer override slots. The engine keeps a table of swappable
 * pointers (default = the builtin behavior); a module may replace any slot
 * via MOD_CONTEXT::jer_override to take over whole behaviors, and call the
 * previous pointer if it saved it. Slot meaning is defined by the engine
 * (see JERICHO/docs/overrides.md).
 */
enum
{
	JER_OVERRIDE_SLOTS = 8,

	/* Slot meanings (engine-defined): */
	JER_OVERRIDE_SLOT_SIM = 0	/* the world step (StepSim) — modules may
								   time-scale or replace it */
};

/* Opaque host context handed to every module. */
typedef struct JERICHO_CONTEXT JERICHO_CONTEXT;

struct JERICHO_CONTEXT
{
	/* SDK version this host implements; modules should check it. */
	int sdkVersion;

	/* Register a handler for an event. priority: lower runs first. */
	void (*jer_register_hook)(JERICHO_CONTEXT* ctx, int event, JER_HOOK_FN fn, void* userdata, int priority);

	/* Fire an event to all registered handlers. Returns the last result. */
	int (*jer_fire)(JERICHO_CONTEXT* ctx, int event, void* args);

	/* Replace (or restore with NULL) an engine function-pointer slot. */
	void (*jer_override)(JERICHO_CONTEXT* ctx, int slot, void* fn);

	/* Log through the engine's console/log (printf-style). */
	void (*jer_log)(JERICHO_CONTEXT* ctx, const char* fmt, ...);

	/* Register this module's metadata (id must match the build id). */
	void (*jer_register_module)(JERICHO_CONTEXT* ctx,
		const char* id, const char* name, const char* version,
		const char* author, const char* description, const char* deps,
		int sdkVersion);
};

/*
 * Module entry point. Every compiled-in module exports
 *     void jer_module_<id>_entry(JERICHO_CONTEXT* ctx);
 * which the generated registry (Game/C/JERICHO/gen/jer_registry.c) calls at
 * boot, in modlist.json load order, only for enabled modules. Inside, the
 * module registers its metadata and its hook handlers.
 */
typedef void (*JER_MODULE_ENTRY)(JERICHO_CONTEXT* ctx);

/*
 * Declare a module entry point with the right C/C++ linkage. Modules are
 * compiled as C++ (like the game); the registry declares them extern "C",
 * so the definition must match:
 *
 *     extern "C" void jer_module_<id>_entry(JERICHO_CONTEXT* ctx) { ... }
 *
 * This macro expands to exactly that in C++, and to a plain definition in C.
 */
#if defined(__cplusplus)
#define JER_MODULE_ENTRY(name) extern "C" void name
#else
#define JER_MODULE_ENTRY(name) void name
#endif

/* One row of the generated module registry. */
typedef struct JER_REGISTRY_ENTRY
{
	const char* id;
	JER_MODULE_ENTRY entry;
	int defaultEnabled;	/* enabled by default when modlist.json omits it */
} JER_REGISTRY_ENTRY;

/* ------------------------------------------------------------------ */
/* Host-side API (called by the engine, not by modules)                */
/* ------------------------------------------------------------------ */

/*
 * Initialise JERICHO: read modsDir/modlist.json, activate the enabled
 * compiled-in modules in load order, then fire JER_EVENT_BOOT. Safe to call
 * once per game boot; call again after modules were toggled to re-apply.
 */
void jer_init(const char* modsDir);

/* Fire an event through the runtime (the engine's thin entry point). */
int jer_fire(int event, void* args);

/* Log through the JERICHO logger (defaults to printf). */
void jer_log(const char* fmt, ...);

/* Set a custom logger (NULL restores the default printf). */
void jer_set_logger(void (*fn)(const char* msg));

/* Replace a function-pointer override slot (engine-side; NULL = restore). */
void jer_override(int slot, void* fn);

/* Get the current function pointer for an override slot (engine-side). */
void* jer_get_override(int slot);

/*
 * Module manager (jer_manager.c): enable/disable and load-order state,
 * persisted to modsDir/modlist.json. Toggling needs no rebuild.
 */

/* Re-read modlist.json and re-activate modules (used after toggles). */
void jer_manager_reload(const char* modsDir);

/* Set a module's enabled state; persists to modlist.json. Returns 0 on ok. */
int jer_manager_set_enabled(const char* modsDir, const char* id, int enabled);

/* Move a module up/down in the load order; persists. Returns 0 on ok. */
int jer_manager_move(const char* modsDir, const char* id, int direction);

/* ------------------------------------------------------------------ */
/* Module inventory (for the frontend Mods manager)                    */
/* ------------------------------------------------------------------ */

/* One compiled-in module's info (id/name/version + current enabled state). */
typedef struct JER_MODULE_INFO
{
	const char* id;
	const char* name;
	const char* version;
	int enabled;
} JER_MODULE_INFO;

/* Number of compiled-in modules (the generated registry). */
int jer_module_count(void);

/* Fill up to max entries with the compiled-in modules. Returns count. */
int jer_module_list(JER_MODULE_INFO* out, int max);

/* The mods dir passed to jer_init (used by the manager functions). */
const char* jer_mods_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* JERICHO_JERICHO_H */
