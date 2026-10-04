/*
 * debugorbit.c — JERICHO Debug Orbit Camera.
 *
 * A camera-only addon. The moment a level starts it takes the camera over and
 * orbits the player (their car, or Tanner on foot) at a fixed radius and
 * elevation: a short static hold at a 3/4 front view, then a slow full turn.
 * That is what inspecting a model needs — both sides, the wheels, the roof and
 * the shadow, all without touching the pad.
 *
 * Design notes (the reasons, not just the what):
 *
 *  - The takeover goes through JER_EVENT_CAMERA and sets override = 1
 *    (src_rebuild/Game/C/camera.c:195-216). The engine then rebuilds the view
 *    matrices from our camera_angle, and the camera_position we write is the
 *    one the renderer, the frustum culling and the region streaming use.
 *    That hook only fires while events.cameraEvent == NULL, so a scripted
 *    cutscene camera still wins and the orbit resumes after it.
 *
 *  - It registers at priority 1000 (lower runs first, jer_system.c:343), so it
 *    is the LAST writer of camera_position/camera_angle and wins over any other
 *    module that nudges the chase camera.
 *
 *  - It is SDK-ONLY (jericho.h and friends; no game headers, no engine
 *    symbols). A module DLL that imports engine symbols is bound to the exe
 *    name baked into its import library (JERICHO.exe in the SDK copy,
 *    JERICHO_dev.exe in the dev tree), so it would fail to load under any
 *    other build; calling nothing but the context's function pointers keeps
 *    this addon loadable everywhere. The cost is that the two engine structs
 *    it writes through are mirrored below instead of included.
 *
 *  - Gameplay is left alone: no pad input is suppressed. An untouched car sits
 *    still and the orbit is a clean turntable; a driven one is simply watched.
 *
 *  - The part that is easy to get wrong is the height, because y is INVERTED in
 *    this engine's camera/world frame: a SMALLER y is HIGHER. Measured, not
 *    guessed: in one frame of a stock Chicago run the car's basePos[1] was 114
 *    while the engine's own chase camera sat at y = -305 — a chase camera looks
 *    slightly down at the car, so it is ~419 units above it at a LOWER y. The
 *    first version of this module used `-basePos[1] + height` (the shape of
 *    PlaceCameraFollowCar's own `carheight - basePos[1]`, camera.c:650) and the
 *    camera spawned well under the road; `basePos[1] - height` is the fix.
 *    Horizontal placement and the aim are not affected:
 *      * camera sits at basePos + (sin(a), cos(a)) * distance, as the chase
 *        camera does (camera.c:646-648);
 *      * camera_angle.vy = -(a + 2048) is the engine's own yaw for a camera at
 *        that offset (camera.c:619, and through ratan2, camera.c:621);
 *      * camera_angle.vx is the pitch in PSX units, positive down (the chase
 *        camera's vx = 25, camera.c:542).
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_math.h"

#if defined(_WIN32)
#include <windows.h>	/* GetModuleHandle/GetProcAddress - the host's own exports */
#endif

#include <math.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * The two engine structs we write through, mirrored (SDK-only).
 *
 * Both are the engine's own, handed to us as void* by JER_ARGS_CAMERA:
 *   cameraPosition -> VECTOR*  (PsyCross/include/psx/libgte.h: int vx,vy,vz,pad)
 *   cameraAngle    -> SVECTOR* (same header: short vx,vy,vz,pad)
 *   basePos        -> LONGVECTOR4 (x, y, z — int[4])
 * They are laid out here rather than included so this module links no engine
 * symbol at all.
 * ------------------------------------------------------------------------ */
typedef struct DBG_VEC { int vx, vy, vz, pad; } DBG_VEC;	/* VECTOR */
typedef struct DBG_ANG { short vx, vy, vz, pad; } DBG_ANG;	/* SVECTOR */

#define DBG_ID			"debugorbit"

/* callable only from the hooks below; the context is saved at entry */
#define DBG_LOG(st, ...)	do { if ((st)->ctx != NULL) (st)->ctx->jer_log((st)->ctx, __VA_ARGS__); } while (0)

/* --------------------------------------------------------------------------
 * Tunables — JERICHO/CONFIG/debugorbit.ini, re-read live.
 *
 * The defaults are the approved framing: distance 2100 (halved from the 4200
 * inspection distance testmode picked; the stock chase camera sits ~850 behind a
 * car), 30 degrees below horizontal (the spec's 25-35 band), 40 deg/s (the spec's
 * 30-45, i.e. a 9 s turn), a 2.5 s static hold (the spec's 2-3 s) and a start
 * angle 45 degrees off the subject's centreline (the spec's 3/4 front view).
 * The height is derived, so halving the distance halves it too: 2100 * tan 30
 * = 1212 above the subject, instead of 2425 at 4200.
 * ------------------------------------------------------------------------ */
#define DBG_DEF_DISTANCE	2100	/* half the 4200 inspection distance -- and so half the height */
#define DBG_DEF_ELEVATION	30	/* degrees below horizontal */
#define DBG_DEF_SPEED		40	/* degrees per second */
#define DBG_DEF_HOLD_MS		2500	/* static hold before the orbit starts */
#define DBG_DEF_START_ANGLE	45	/* degrees from the subject's centreline */
#define DBG_DEF_DIR		1	/* orbit direction */

#define DBG_MIN_DISTANCE	200
#define DBG_MAX_DISTANCE	20000
#define DBG_MIN_ELEVATION	5
#define DBG_MAX_ELEVATION	80
#define DBG_MAX_SPEED		360
#define DBG_MAX_HOLD_MS		60000

/* the game steps its logic at a fixed 30 Hz, so time and ticks are the same
 * clock and the orbit cannot drift */
#define DBG_FPS			30
#define DBG_TURN		4096	/* PSX angle units per full turn */
#define DBG_PI			3.14159265358979323846

#define DBG_DEG2PSX(d)		((int)((d) * DBG_TURN / 360))
#define DBG_PSX2RAD(p)		((p) * (2.0 * DBG_PI) / DBG_TURN)

/* how often the config is re-read, in in-game frames (~1 s) */
#define DBG_CFG_EVERY		30

/* --------------------------------------------------------------------------
 * State.
 * ------------------------------------------------------------------------ */
typedef struct DBG_CFG
{
	int enabled;		/* the module is armed */
	int distance;		/* orbit radius, world units (same scale as the chase camera) */
	int elevation;		/* degrees below horizontal */
	int speed;		/* degrees per second (signed: the knob for the orbit direction too) */
	int holdMs;		/* the static hold before the orbit starts, milliseconds */
	int startAngle;		/* degrees from the subject's centreline (0 = behind it) */
	int dir;		/* +1 / -1: which way the orbit turns */
} DBG_CFG;

typedef struct DBG_STATE
{
	JERICHO_CONTEXT* ctx;

	DBG_CFG cfg;

	int captured;		/* the start yaw was taken for this level */
	int ticks;		/* in-game camera ticks since the capture */
	int yawBase;		/* the captured start yaw, PSX units */
	int revLogged;		/* the first full revolution was reported */
	int phase;		/* DBG_PHASE_HOLD / DBG_PHASE_ORBIT (last tick) */
	int logCountdown;	/* frames until the next diagnostic line */
	int diagEvery;		/* diagnostic cadence, frames */
	int cfgCountdown;	/* frames until the next live config re-read */
} DBG_STATE;

static DBG_STATE gDbg;

/* --------------------------------------------------------------------------
 * Small angle helpers. PSX angles are 0..4095 per turn; the engine's own
 * convention (LIBGTE.C ratan2) is atan2 scaled the same way.
 * ------------------------------------------------------------------------ */
#define DBG_PHASE_HOLD		0
#define DBG_PHASE_ORBIT		1

static int dbgWrapAngle(int a)
{
	a &= (DBG_TURN - 1);

	if (a < 0)
		a += DBG_TURN;

	return a;
}

/* PSX angle -> whole degrees (for the log; the maths stays in PSX units) */
static int dbgDegrees(int psx)
{
	return (dbgWrapAngle(psx) * 360) / DBG_TURN;
}

/* --------------------------------------------------------------------------
 * Config, without importing the host.
 *
 * jer_config_* live in the host exe (JERICHO.lib). Calling them directly would
 * put JERICHO.exe in this DLL's import table — the name baked into the import
 * library — so the DLL would refuse to load inside any other build, and the
 * only symptom is the loader's "enabled but no compiled binary found". They are
 * therefore resolved from the RUNNING host at entry (GetModuleHandle(NULL) +
 * GetProcAddress) instead. Same storage (JERICHO/CONFIG/<id>.ini), same live
 * re-read, and the addon keeps loading under any exe name. If the lookups fail
 * the defaults below are used, so the camera still works.
 * ------------------------------------------------------------------------ */
typedef int  (*DBG_CFG_GET_INT)(const char* mod, const char* key, int def);
typedef void (*DBG_CFG_SET_INT)(const char* mod, const char* key, int value);
typedef int  (*DBG_CFG_GET_BOOL)(const char* mod, const char* key, int def);
typedef void (*DBG_CFG_SET_BOOL)(const char* mod, const char* key, int value);

static DBG_CFG_GET_INT  gCfgGetInt;
static DBG_CFG_SET_INT  gCfgSetInt;
static DBG_CFG_GET_BOOL gCfgGetBool;
static DBG_CFG_SET_BOOL gCfgSetBool;

#if defined(_WIN32)
static void dbgResolveConfigApi(void)
{
	HMODULE host = GetModuleHandleA(NULL);

	if (host == NULL)
		return;

	gCfgGetInt = (DBG_CFG_GET_INT)(void*)GetProcAddress(host, "jer_config_get_int");
	gCfgSetInt = (DBG_CFG_SET_INT)(void*)GetProcAddress(host, "jer_config_set_int");
	gCfgGetBool = (DBG_CFG_GET_BOOL)(void*)GetProcAddress(host, "jer_config_get_bool");
	gCfgSetBool = (DBG_CFG_SET_BOOL)(void*)GetProcAddress(host, "jer_config_set_bool");
}
#else
static void dbgResolveConfigApi(void)
{
	/* no runtime loading here: the defaults apply */
}
#endif

static int dbgCfgGetInt(const char* key, int def)
{
	return (gCfgGetInt != NULL) ? gCfgGetInt(DBG_ID, key, def) : def;
}

static int dbgCfgGetBool(const char* key, int def)
{
	if (gCfgGetBool != NULL)
		return gCfgGetBool(DBG_ID, key, def);

	return dbgCfgGetInt(key, def);
}

static void dbgCfgSetInt(const char* key, int value)
{
	if (gCfgSetInt != NULL)
		gCfgSetInt(DBG_ID, key, value);
}

static void dbgCfgSetBool(const char* key, int value)
{
	if (gCfgSetBool != NULL)
		gCfgSetBool(DBG_ID, key, value ? 1 : 0);
}

/* --------------------------------------------------------------------------
 * Config.
 * ------------------------------------------------------------------------ */

/*
 * Read the knobs and clamp them, so a hand-edited ini cannot put the camera
 * under the ground or make the orbit unreadable.
 */
static void dbgReadConfig(DBG_STATE* st)
{
	DBG_CFG* c = &st->cfg;

	c->enabled = dbgCfgGetBool("enabled", 1) ? 1 : 0;

	c->distance = jer_clamp_int(dbgCfgGetInt("distance", DBG_DEF_DISTANCE),
		DBG_MIN_DISTANCE, DBG_MAX_DISTANCE);
	c->elevation = jer_clamp_int(dbgCfgGetInt("elevation", DBG_DEF_ELEVATION),
		DBG_MIN_ELEVATION, DBG_MAX_ELEVATION);
	c->speed = jer_clamp_int(dbgCfgGetInt("speed", DBG_DEF_SPEED),
		-DBG_MAX_SPEED, DBG_MAX_SPEED);
	c->holdMs = jer_clamp_int(dbgCfgGetInt("hold_ms", DBG_DEF_HOLD_MS),
		0, DBG_MAX_HOLD_MS);
	c->startAngle = dbgCfgGetInt("start_angle", DBG_DEF_START_ANGLE) % 360;
	c->dir = (dbgCfgGetInt("orbit_dir", DBG_DEF_DIR) < 0) ? -1 : 1;
}

/*
 * Write the resolved values back once at boot so CONFIG/debugorbit.ini
 * materialises with every knob visible and editable (the aidriver/testmode
 * pattern). Reading first means a value the player edited is written back
 * unchanged; it is deliberately NOT re-saved later, or a live edit would be
 * clobbered by the periodic re-read.
 */
static void dbgSaveConfig(DBG_STATE* st)
{
	DBG_CFG* c = &st->cfg;

	dbgCfgSetBool("enabled", c->enabled);
	dbgCfgSetInt("distance", c->distance);
	dbgCfgSetInt("elevation", c->elevation);
	dbgCfgSetInt("speed", c->speed);
	dbgCfgSetInt("hold_ms", c->holdMs);
	dbgCfgSetInt("start_angle", c->startAngle);
	dbgCfgSetInt("orbit_dir", c->dir);
}

/* One line naming every resolved knob, so a run is checkable from the log. */
static void dbgLogOptions(DBG_STATE* st, const char* when)
{
	DBG_CFG* c = &st->cfg;

	DBG_LOG(st, "[debugorbit] options (%s): %s, distance=%d elevation=%d deg speed=%d deg/s "
		"hold=%d ms start_angle=%d deg dir=%+d\n",
		when, c->enabled ? "ENABLED" : "idle",
		c->distance, c->elevation, c->speed, c->holdMs, c->startAngle, c->dir);
}

/* --------------------------------------------------------------------------
 * Hooks.
 * ------------------------------------------------------------------------ */

/*
 * JER_EVENT_GAME_START — a level is starting (fresh launch, restart or the next
 * mission). Drop the capture so the orbit re-aims from the new subject and its
 * heading, and start the hold again.
 */
static int dbgOnGameStart(void* ud, void* args)
{
	DBG_STATE* st = (DBG_STATE*)ud;

	(void)args;

	dbgReadConfig(st);

	st->captured = 0;
	st->ticks = 0;
	st->revLogged = 0;
	st->logCountdown = 0;
	st->cfgCountdown = DBG_CFG_EVERY;

	dbgLogOptions(st, "level start");

	return JER_RESULT_CONTINUE;
}

/*
 * JER_EVENT_CAMERA — fired at the end of InitCamera with the engine's own
 * placement for this frame. This is where the orbit replaces it.
 */
static int dbgOnCamera(void* ud, void* args)
{
	DBG_STATE* st = (DBG_STATE*)ud;
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	DBG_VEC* cam;
	DBG_ANG* ang;
	int* target;
	int stockX, stockY, stockZ;
	int past; 		/* frames past the hold: < 0 while holding */
	int orbitPsx;		/* how far into the turn we are, PSX units */
	int yaw;		/* the offset direction: where the camera sits around the subject */
	int dx, dz, height;
	double yawRad, elevRad;

	if (st == NULL || a == NULL)
		return JER_RESULT_CONTINUE;

	/* live re-read: a hand-edited ini is picked up within ~1 s, and turning
	 * `enabled` off hands the camera straight back to the engine */
	if (--st->cfgCountdown <= 0)
	{
		st->cfgCountdown = DBG_CFG_EVERY;
		dbgReadConfig(st);
	}

	if (!st->cfg.enabled)
		return JER_RESULT_CONTINUE;

	if (a->cameraPosition == NULL || a->cameraAngle == NULL || a->basePos == NULL)
		return JER_RESULT_CONTINUE;

	cam = (DBG_VEC*)a->cameraPosition;
	ang = (DBG_ANG*)a->cameraAngle;
	target = (int*)a->basePos;

	/* the engine's own placement for this frame, read before it is replaced:
	 * the diagnostic line uses it to prove the y sign (stock is always above
	 * the subject) */
	stockX = cam->vx;
	stockY = cam->vy;
	stockZ = cam->vz;

	/* The start yaw is taken once per level, from the subject's own heading, so
	 * the hold is always the same 3/4 view of that subject and the orbit is
	 * anchored to the world rather than following the car around. */
	if (!st->captured)
	{
		st->yawBase = dbgWrapAngle(a->baseDir + DBG_DEG2PSX(st->cfg.startAngle));
		st->ticks = 0;
		st->revLogged = 0;
		st->logCountdown = 0;
		st->phase = DBG_PHASE_HOLD;
		st->captured = 1;

		DBG_LOG(st, "[debugorbit] hold: subject heading %d deg, camera at %d deg (start_angle %+d), %d ms, then %d deg/s at radius %d and %d deg down\n",
			dbgDegrees(a->baseDir), dbgDegrees(st->yawBase), st->cfg.startAngle,
			st->cfg.holdMs, st->cfg.speed, st->cfg.distance, st->cfg.elevation);
	}

	/* The hold is a tick count and the turn is a pure function of it — no
	 * accumulated angle — so a live speed change re-aims smoothly instead of
	 * jumping, and the orbit cannot drift over a long run. */
	past = st->ticks - (st->cfg.holdMs * DBG_FPS) / 1000;
	orbitPsx = (past <= 0) ? 0
		: (int)(((long long)past * st->cfg.speed * DBG_TURN) / (360LL * DBG_FPS));
	yaw = dbgWrapAngle(st->yawBase + st->cfg.dir * orbitPsx);

	if (past > 0 && st->phase == DBG_PHASE_HOLD)
	{
		st->phase = DBG_PHASE_ORBIT;
		DBG_LOG(st, "[debugorbit] orbit: hold done after %d frames, yaw starts at %d deg\n",
			st->ticks, dbgDegrees(yaw));
	}

	/* the subject sits at basePos; the camera orbits it at a constant radius
	 * and a constant elevation, exactly the way the chase camera places itself
	 * (camera.c:646-648) — the offset is (sin(a), cos(a)) * distance */
	yawRad = DBG_PSX2RAD(yaw);
	elevRad = (double)st->cfg.elevation * DBG_PI / 180.0;

	dx = (int)lround((double)st->cfg.distance * sin(yawRad));
	dz = (int)lround((double)st->cfg.distance * cos(yawRad));
	height = (int)lround((double)st->cfg.distance * tan(elevRad));

	cam->vx = target[0] + dx;
	cam->vy = target[1] - height;	/* Y is INVERTED here: a smaller y is HIGHER (see the notes above) */
	cam->vz = target[2] + dz;

	/* aim: the engine's own yaw for a camera at that offset (camera.c:619/621),
	 * and the elevation as the downward pitch (positive = down, like the chase
	 * camera's vx = 25) */
	ang->vx = (short)DBG_DEG2PSX(st->cfg.elevation);
	ang->vy = (short)dbgWrapAngle(-(yaw + 2048));
	ang->vz = 0;

	/* full control: the engine rebuilds the view matrices from camera_angle and
	 * stops fighting our transform (camera.c:214-215) */
	a->override = 1;

	/* ------------------------------------------------------------------
	 * Diagnostics: the run has to be checkable from the log alone.
	 * ------------------------------------------------------------------ */
	if (orbitPsx >= DBG_TURN && !st->revLogged)
	{
		st->revLogged = 1;
		DBG_LOG(st, "[debugorbit] first full orbit after %d frames (%.1f s at %d Hz)\n",
			past, (double)past / DBG_FPS, DBG_FPS);
	}

	if (++st->logCountdown >= st->diagEvery)
	{
		st->logCountdown = 0;

		DBG_LOG(st, "[debugorbit] t=%d %s yaw=%d deg cam=(%d,%d,%d) ang=(%d,%d,%d) target=(%d,%d,%d) stockCam=(%d,%d,%d)\n",
			st->ticks, (st->phase == DBG_PHASE_ORBIT) ? "orbit" : "hold", dbgDegrees(yaw),
			cam->vx, cam->vy, cam->vz,
			(int)ang->vx, (int)ang->vy, (int)ang->vz,
			target[0], target[1], target[2],
			stockX, stockY, stockZ);
	}

	st->ticks++;

	return JER_RESULT_CONTINUE;
}

/*
 * JER_EVENT_SHUTDOWN — the game is going away; nothing to hand back (the camera
 * is re-placed by the engine every frame), but say so in the log.
 */
static int dbgOnShutdown(void* ud, void* args)
{
	DBG_STATE* st = (DBG_STATE*)ud;

	(void)args;

	DBG_LOG(st, "[debugorbit] shutdown (no camera state to restore)\n");

	return JER_RESULT_CONTINUE;
}

/* --------------------------------------------------------------------------
 * Entry.
 * ------------------------------------------------------------------------ */
JER_MODULE_ENTRY(jer_module_debugorbit_entry)(JERICHO_CONTEXT* ctx)
{
	DBG_STATE* st = &gDbg;

	memset(st, 0, sizeof(*st));
	st->ctx = ctx;
	st->diagEvery = DBG_CFG_EVERY;	/* ~1 s at the 30 Hz logic rate */
	st->cfgCountdown = DBG_CFG_EVERY;

	dbgResolveConfigApi();
	dbgReadConfig(st);
	dbgSaveConfig(st);

	ctx->jer_register_module(ctx,
		DBG_ID,
		"Debug Orbit Camera",
		"0.1.0",
		"Jaret Ludvik",
		"Takes the camera over at level start and orbits the player at a fixed "
		"radius and elevation (a short static hold, then a slow 360).",
		"",
		JERICHO_SDK_VERSION);

	/* priority 1000: lower runs first, so the orbit is the last word on the
	 * camera this frame and no other module's chase-cam nudge survives it */
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, dbgOnGameStart, st, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, dbgOnCamera, st, 1000);
	ctx->jer_register_hook(ctx, JER_EVENT_SHUTDOWN, dbgOnShutdown, st, 0);

	dbgLogOptions(st, "boot");

	if (gCfgGetInt == NULL)
		DBG_LOG(st, "[debugorbit] warning: the host's jer_config exports were not found - "
			"using built-in defaults (no CONFIG/debugorbit.ini this run)\n");

	if (!st->cfg.enabled)
		DBG_LOG(st, "[debugorbit] idle: set enabled = 1 in JERICHO/CONFIG/debugorbit.ini to arm it\n");

	DBG_LOG(st, "[debugorbit] registered (SDK v%d)\n", ctx->sdkVersion);
}
