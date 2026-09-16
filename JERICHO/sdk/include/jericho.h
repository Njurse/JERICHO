/*
 * jericho.h -- JERICHO: Just-in-Time Extensible Runtime Interface for
 * Compiled Hooks & Overrides.
 *
 * A tiny, platform-neutral C/C++ API that turns the game into a HOST for
 * modules. Two module kinds exist:
 *
 *   - API addons (mod.toml declares runtime = "dll"): compiled into DLLs
 *     by JERICHO/build_mods.bat (or the in-game "Compile Mods" action) and
 *     LOADED AT RUNTIME by jer_loader.c via LoadLibrary/dlopen — the game
 *     exe is never rebuilt for them. This is the developer-facing model:
 *     write against this API, drop the folder in JERICHO/MODS, done.
 *   - Deep mods (no runtime declaration): source-level patches of game
 *     internals that read/write game globals. MSVC cannot import game
 *     data into a DLL without dllimport, so these are compiled in by
 *     premake's auto-scan (see docs/JERICHO/README.md).
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
#define JER_MAX_MODULES 32

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

/*
 * Event IDs fired by the engine at fixed hook points in the vanilla files
 * (and by modules, which may also fire custom events >= JER_EVENT_MODULE_CUSTOM).
 * The argument struct for each event is documented at the engine call sites
 * (search for "JERICHO-HOOK") and in src_rebuild/Game/C/JERICHO/docs/events.md.
 */
enum
{
	JER_EVENT_BOOT = 0,			/* fired once after all modules are activated */
	JER_EVENT_FRAME,			/* every game frame (global step) */
	JER_EVENT_PRE_SIM,			/* top of the world step (StepSim), BEFORE the
								   car-control loop reads the pads -- the one
								   spot modules may inject/replace pad input or
								   block for external input (e.g. an ML agent in
								   wait-for-input lockstep). JER_EVENT_FRAME
								   fires inside GlobalTimeStep, after the car
								   loop, so it is too late for input. */
	JER_EVENT_INIT,				/* game / damage system (re)initialisation */
	JER_EVENT_COLLISION,		/* car-car / car-world collision */
	JER_EVENT_DENT_PASS,		/* vertex deformation pass */
	JER_EVENT_RESET_CAR,		/* a car's damage state is being reset */
	JER_EVENT_DEBUG_TICK,		/* per-frame debug/tuning tick */
	JER_EVENT_GET_WHEEL_BEND,	/* query: per-wheel damage bend (result slot) */
	JER_EVENT_GET_WHEEL_DAMAGE,	/* query: cumulative wheel damage 0..4096 */
	JER_EVENT_GET_IMPACT_INFO,	/* query: newest impact for the overlay */
	JER_EVENT_GET_WHEEL_PARAMS,	/* query: wheel-damage physics params (scales + scrub) */
	JER_EVENT_GET_PHYSICS_PARAMS,	/* query: gravity + angular settling + suspension
									   rate for a car (see JER_ARGS_PHYSICS_PARAMS) */
	JER_EVENT_GET_BUDDHA,		/* query: damage-total clamp flag (Buddha mode) */
	JER_EVENT_DRAW_WHEEL,		/* wheel draw: allow visual mesh distortion */
	JER_EVENT_PAUSE_MENU,		/* pause menu draw/update */
	JER_EVENT_DRAW_OVERLAY,		/* per-frame overlay draw (menus over the world) */
	JER_EVENT_CAMERA,			/* camera position/angle adjusted by modules */
	JER_EVENT_GAME_START,		/* a level is starting (fresh/restart/next) */
	JER_EVENT_CAMERA_LOOK,		/* per-frame look handling (TurnHead) -- modules
								   may drive/suppress the free-look */
	JER_EVENT_PED_INPUT,		/* on-foot pad input before it drives the ped --
								   modules may remap to camera-relative movement */
	JER_EVENT_PED_MOVE,		/* inside AnimatePed() before the player ped's
								   position advances -- the one spot a module
								   speed write survives (runner re-arms 40/frame) */
	JER_EVENT_PED_POSE,		/* DrawTanner: between SetupTannerSkeleton and
								   newRotateBones (per-bone rotation window) */
	JER_EVENT_FRONTEND,		/* frontend take-a-ride city confirm (defer hook) */
	JER_EVENT_PED_SKELETON,		/* player ped skeleton posed for draw -- modules
								   may override bone rotations (arm holds) and
								   read joint world positions (vJPos) */
	JER_EVENT_MAP,			/* fullscreen map: INPUT claim + DRAWN cursor
								   (see JER_ARGS_MAP in jer_events.h) */
	JER_EVENT_SHUTDOWN,		/* the game is exiting (after the state loop) --
								   modules release resources (sockets, files) */

	JER_EVENT_CAR_PAD,		/* inside ProcessCarPad before the stock pedal
				   assignment: module may take over thrust/handbrake/
				   wheelspin (handled) so original binds do not double-fire
				   (see JER_ARGS_CAR_PAD) */
	JER_EVENT_CAR_ENGINE,		/* end of ProcessCarPad: transform thrust and
								   steering (see JER_ARGS_CAR_ENGINE) */
	JER_EVENT_CAR_FRICTION,		/* end of GetFrictionScalesDriver1: transform
								   front/rear grip (see JER_ARGS_CAR_FRICTION) */
	JER_EVENT_CAR_STEP,		/* top of StepOneCar: observe car state
								   (see JER_ARGS_CAR_STEP) */
	JER_EVENT_CAR_TORQUE,		/* after torque->angular accel: inject yaw
								   (see JER_ARGS_CAR_TORQUE) */
	JER_EVENT_CAR_DRAW,		/* car body draw: visual pitch/roll/yaw on the
								   render matrix (see JER_ARGS_CAR_DRAW) */
	JER_EVENT_CAR_DRAW_COLOR,	/* car body color: module forces a flat black
								   wreck (see JER_ARGS_CAR_DRAW_COLOR) */
	JER_EVENT_GET_WALL_RESTITUTION,	/* query: wall/scenery hit restitution
									   scale 0..4096 (4096 = stock bounce) */

	JER_EVENT_CAR_GEARBOX,		/* inside GetEngineRevs (gamesnd.c): retune
				   the gear/rev model per car (see JER_ARGS_CAR_GEARBOX) */
	JER_EVENT_CAR_ENGINE_SOUND,	/* SoundTasks: scale/offset the player's
				   rev + idle engine channel pitch/volume */

	JER_EVENT_CAR_REVS,		/* top of ControlCarRevs (gamesnd.c): scale
				   how fast engine pitch slews to its target revs
				   (see JER_ARGS_CAR_REVS) */

	JER_EVENT_LEVEL_LAUNCH,		/* end of State_GameStart, after the pending
				   level/gametype/player count/mission number are
				   finalised but before the level loads -- modules
				   may rewrite them (e.g. swap a take-a-ride
				   mission for its multiplayer-map variant) */

	JER_EVENT_DRAW_WORLD,		/* mid-render world pass: fired inside
				   RenderGame2 after DrawAllTheCars, while the
				   camera matrices (inv_camera_matrix, ...) are
				   live -- modules draw world-space extras
				   (projectiles, pickups) into the real OT */

	JER_EVENT_GET_DAMAGE_SCALE,	/* query: scale 0..4096 (4096 = stock) applied
				   to the damage a car takes from hitting solid
				   scenery (buildings/walls), fired in DamageCar
				   (bcollide.c) before ApplyDamage (see
				   JER_ARGS_DAMAGE_SCALE) */

	JER_EVENT_CAR_VS_CAR,		/* fired in DamageCar3D (bcollide.c) when two
				   cars collide, before ApplyDamage: a module
				   may retune the damage two cars exchange,
				   e.g. give a non-player car the player damage
				   model or scale the exchange (see
				   JER_ARGS_CAR_VS_CAR) */

	JER_EVENT_DRAW_MAP,		/* fired while the overhead/fullscreen map is
				   drawn (overmap.c), after the player blip:
				   a module plots its own markers with
				   DrawTargetBlip/DrawPlayerDot using the same
				   flags (see JER_ARGS_DRAW_MAP) */

	JER_EVENT_EXPLOSION_SPAWN,	/* an explosion slot was armed
				   (AddExplosion, job_fx.c): a module may
				   attach a parametric FX profile -- resize
				   (speed/hscale/rscale), tint, spin rate,
				   collision on/off -- and rewrite `type`
				   (see JER_ARGS_EXPLOSION_SPAWN) */
	JER_EVENT_EXPLOSION_DRAW,	/* an explosion is drawn (DrawExplosion,
				   job_fx.c): a module may tint/spin the
				   stock bang, or set override to draw its
				   own mesh (see JER_ARGS_EXPLOSION_DRAW) */
	JER_EVENT_EXPLOSION_COLLIDE,	/* query: may this explosion push/damage
				   this car, and at what box scale
				   (ExplosionCollisionCheck, bomberman.c).
				   result 0 = visual only
				   (see JER_ARGS_EXPLOSION_COLLIDE) */

	JER_EVENT_CAR_AVAILABILITY,	/* query: fired while the frontend builds a
					   level's car list (CarSelectScreen). A module
					   sets result = 1 to offer the normally-locked
					   extra vehicles (fire truck / buses / truck) for
					   that level, lifting the progression gate and the
					   single-player restriction. 0 = stock. */

	JER_EVENT_CAR_DATA_SOURCE,	/* query: fired at the start of a level's car
					   model processing, before any CARMODEL_*
					   file is read. A module sets sourceLevel to
					   another city (0..3) so this level loads that
					   city's vehicles; -1 = the level's own city. */

	/* ---- Multiplayer (the mp module drives these) ---- */
	JER_EVENT_MP_FRONTEND,		/* the frontend is about to enter a
					   multiplayer menu point (the main-menu
					   "Multiplayer" entry or the multiplayer
					   gamemode screen): a module may claim it
					   (claimed) and run its own menu
					   (see JER_ARGS_MP_FRONTEND) */
	JER_EVENT_NET_INPUT,		/* per player car per frame, before the pad
					   drives it: a module may substitute a remote
					   player's input (see JER_ARGS_NET_INPUT) */
	JER_EVENT_NET_CAR_STATE,	/* per player car per frame: capture (read)
					   or apply (write) a synced transform for the
					   host state-resync fallback
					   (see JER_ARGS_NET_CAR_STATE) */
	JER_EVENT_NET_PLAYERS,		/* query: enumerate the local player slots
					   (pad ids) so a module can map them to
					   network peers (see JER_ARGS_NET_PLAYERS) */
	JER_EVENT_NET_RECV,		/* the addon net bridge delivered an inbound
					   channel payload (see JER_ARGS_NET_RECV and
					   jer_net.h) */
	JER_EVENT_NET_SPAWN,		/* fired when a level's player cars are
					   about to be created: a network module adds
					   the remote players here
					   (see JER_ARGS_NET_SPAWN) */

	JER_EVENT_CMDLINE,		/* fired once after the engine parsed its own
					   command line, so a module can pick up its own
					   shortcuts (see JER_ARGS_CMDLINE) */

	JER_EVENT_PED_DRAW,		/* ped body colour: a module forces a flat black
					   (a burning/bailed-out ped) or a full-brightness
					   tint on a ped the skeleton path is drawing
					   (see JER_ARGS_PED_DRAW) */

	JER_EVENT_MODULE_CUSTOM = 1000	/* modules define custom ids from here */};

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
 * (see the "Override slots" section of src_rebuild/Game/C/JERICHO/docs/events.md).
 */
enum
{
	JER_OVERRIDE_SLOTS = 8,

	/* Slot meanings (engine-defined): */
	JER_OVERRIDE_SLOT_SIM = 0	/* the world step (StepSim) -- modules may
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
 * boot, in modlist.ini load order, only for enabled modules. Inside, the
 * module registers its metadata and its hook handlers.
 */
typedef void (*JER_MODULE_ENTRY)(JERICHO_CONTEXT* ctx);

/*
 * Declare a module entry point with the right C/C++ linkage and export
 * visibility. Modules are compiled as C++ (like the game) into a DLL/.so;
 * when built as a loadable module (JERICHO_MODULE_BUILD defined — see the
 * SDK build script and premake5_mods.lua) the symbol is exported so the
 * host's loader (jer_loader.c) can resolve it with GetProcAddress/dlsym:
 *
 *     extern "C" __declspec(dllexport) void jer_module_<id>_entry(JERICHO_CONTEXT* ctx) { ... }
 *
 * This macro expands to exactly that in C++, and to a plain definition in C.
 */
#if defined(__cplusplus)
	#if defined(_WIN32) && defined(JERICHO_MODULE_BUILD)
		#define JER_MODULE_ENTRY(name) extern "C" __declspec(dllexport) void name
	#elif defined(JERICHO_MODULE_BUILD) && (defined(__GNUC__) || defined(__clang__))
		#define JER_MODULE_ENTRY(name) extern "C" __attribute__((visibility("default"))) void name
	#else
		#define JER_MODULE_ENTRY(name) extern "C" void name
	#endif
#else
	#if defined(_WIN32) && defined(JERICHO_MODULE_BUILD)
		#define JER_MODULE_ENTRY(name) __declspec(dllexport) void name
	#elif defined(JERICHO_MODULE_BUILD) && (defined(__GNUC__) || defined(__clang__))
		#define JER_MODULE_ENTRY(name) __attribute__((visibility("default"))) void name
	#else
		#define JER_MODULE_ENTRY(name) void name
	#endif
#endif

/* One row of the generated module registry. */
typedef struct JER_REGISTRY_ENTRY
{
	const char* id;
	JER_MODULE_ENTRY entry;
	int defaultEnabled;	/* enabled by default when modlist.ini omits it */
} JER_REGISTRY_ENTRY;

/* ------------------------------------------------------------------ */
/* Host-side API (called by the engine, not by modules)                */
/* ------------------------------------------------------------------ */

/*
 * Initialise JERICHO: read <root>/CONFIG/modlist.ini, activate the enabled
 * compiled-in modules in load order, then fire JER_EVENT_BOOT. rootDir is
 * the central JERICHO folder (the game passes "<dataFolder>JERICHO"). Safe
 * to call once per game boot; call again after modules were toggled to
 * re-apply.
 */
void jer_init(const char* rootDir);

/* Fire an event through the runtime (the engine's thin entry point). */
int jer_fire(int event, void* args);

/* Log through the JERICHO logger (defaults to printf). */
void jer_log(const char* fmt, ...);

/* Raise a short-lived error notice. It is drawn in a gentle red down the
 * LEFT of the screen for about five seconds -- in the frontend and in
 * game. Use it for anything the player needs to be told: bad
 * command-line arguments, a failed join, a lost connection, ... */
int jer_error(const char* fmt, ...);

/* The live error notices, oldest first -- the ENGINE asks for these and
 * prints them itself (it owns the text primitives). */
int         jer_error_count(void);
const char* jer_error_at(int index);

/* Set a custom logger (NULL restores the default printf). */
void jer_set_logger(void (*fn)(const char* msg));

/* Replace a function-pointer override slot (engine-side; NULL = restore). */
void jer_override(int slot, void* fn);

/* Get the current function pointer for an override slot (engine-side). */
void* jer_get_override(int slot);

/*
 * Module manager (jer_manager.c): enable/disable and load-order state,
 * persisted to <root>/CONFIG/modlist.ini. Toggling needs no rebuild.
 */

/* Re-read modlist.ini and re-activate modules (used after toggles). */
void jer_manager_reload(const char* rootDir);

/* Set a module's enabled state; persists to modlist.ini. Returns 0 on ok. */
int jer_manager_set_enabled(const char* rootDir, const char* id, int enabled);

/* Move a module up/down in the load order; persists. Returns 0 on ok. */
int jer_manager_move(const char* rootDir, const char* id, int direction);

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

/* The central JERICHO folder passed to jer_init (used by the manager). */
const char* jer_root_dir(void);

/*
 * Host-side: compile every installed runtime "dll" addon into a loadable
 * module using the game's own toolchain (JERICHO/build_mods.bat). The game
 * exe is never rebuilt. Returns 1 on success. Windows only (the loader
 * that consumes the result is Windows/Linux; on other platforms this is a
 * no-op returning 0). Used by the in-game "Compile Mods" action.
 */
int jer_compile_mods(void);

#ifdef __cplusplus
}
#endif

#endif /* JERICHO_JERICHO_H */
