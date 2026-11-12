/*
 * directorbox.c — JERICHO Director Box.
 *
 * A camera-only addon, switched on from one row in the in-game pause menu
 * (Pause -> JERICHO Addons -> DIRECTOR BOX). While it is on, the camera cuts
 * between framed static shots of whatever the engine's chase camera is
 * chasing — the player's car, or Tanner on foot — two moving shots (a slow
 * orbit and a rising crane), and now and then a random roadside tripod that
 * stays put while its aim pans with the subject.
 *
 * Design notes (the reasons, not just the what):
 *
 *  - The takeover goes through JER_EVENT_CAMERA and sets override = 1
 *    (src_rebuild/Game/C/camera.c:195-216). The engine then rebuilds the view
 *    matrices from our camera_angle, and the camera_position we write is the
 *    one the renderer, the frustum culling and the region streaming use.
 *    That hook only fires while events.cameraEvent == NULL, so a scripted
 *    cutscene camera still wins and the director camera resumes afterwards.
 *
 *  - It registers at priority 1000 (lower runs first, jer_system.c:343), so it
 *    is the LAST writer of camera_position/camera_angle and wins over any other
 *    module that nudges the chase camera.
 *
 *  - It is SDK-ONLY (jericho.h and friends; no game headers, no engine
 *    symbols). A module DLL that imports engine symbols is bound to the exe
 *    name baked into its import library (JERICHO.exe in the SDK copy,
 *    JERICHO_dev.exe in the dev tree), so it would fail to load under any
 *    other build; calling nothing but the context's function pointers and the
 *    host API table keeps this addon loadable everywhere. The cost is that the
 *    two engine structs it writes through are mirrored below instead of
 *    included.
 *
 *  - The host API's car table already exposes map_height() and cell_empty(),
 *    which is what makes a real roadside tripod possible without compiling
 *    into the game: the sampled vantage can be grounded and kept out of
 *    scenery. No FOV/zoom is available through the SDK, so framing is done with
 *    distance and height only.
 *
 *  - The part that is easy to get wrong is the height, because y is INVERTED in
 *    this engine's camera/world frame: a SMALLER y is HIGHER. Measured, not
 *    guessed: in one frame of a stock Chicago run the car's basePos[1] was 114
 *    while the engine's own chase camera sat at y = -305 — a chase camera looks
 *    slightly down at the car, so it is ~419 units above it at a LOWER y. The
 *    directed shots therefore use `basePos[1] - height` (debugorbit proved
 *    this; `-basePos[1] + height` puts the camera under the road). Horizontal
 *    placement and the aim are the engine's own:
 *      * camera sits at basePos + (sin(a), cos(a)) * distance, as the chase
 *        camera does (camera.c:646-648);
 *      * camera_angle.vy = -(a + 2048) is the engine's own yaw for a camera at
 *        that offset (camera.c:619, and through ratan2, camera.c:621);
 *      * camera_angle.vx is the pitch in PSX units, positive down (the chase
 *        camera's vx = 25, camera.c:542).
 *    The tripod's y is derived from the sampled ground instead: Ant Farm's
 *    working form is `-MapHeight(x,z) - height`, which is why the map_height
 *    query returns render-space ground for a world-space point.
 *
 *  - A shot's height is relative to the SUBJECT, not to the ground under the
 *    camera. A shallow shot over a bank, an embankment or a raised junction can
 *    therefore end up below the local surface even when the subject-relative
 *    math is right. EVERY placement is clamped with MapHeight(x,z) afterwards,
 *    so the lens is at least DB_GROUND_CLEARANCE above the terrain under it.
 *    This is what keeps the low and wide shots above the road on a slope.
 *
 *  - The offset normally follows the subject's live heading. At the cut, one
 *    shot in DB_DECOUPLE_ONE_IN freezes the heading instead: the camera holds a
 *    static world angle while the car rotates under it, and only the aim keeps
 *    tracking. That is the "static offset" variant of the directed and moving
 *    shots, and the log reports it as basis=world.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_host_api.h"
#include "jer_math.h"
#include "jer_pause_menu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
typedef struct DB_VEC { int vx, vy, vz, pad; } DB_VEC;	/* VECTOR */
typedef struct DB_ANG { short vx, vy, vz, pad; } DB_ANG;	/* SVECTOR */

#define DB_ID			"directorbox"

/* callable only from the hooks below; the context is saved at entry */
#define DB_LOG(st, ...)	do { if ((st) != NULL && (st)->ctx != NULL) (st)->ctx->jer_log((st)->ctx, __VA_ARGS__); } while (0)

/* --------------------------------------------------------------------------
 * The game steps its logic at a fixed 30 Hz, so time and camera ticks are the
 * same clock and the cut timer cannot drift.
 * ------------------------------------------------------------------------ */
#define DB_FPS			30
#define DB_TURN			4096	/* PSX angle units per full turn */
#define DB_PI			3.14159265358979323846

#define DB_DEG2PSX(d)		((int)(((d) * DB_TURN) / 360))
#define DB_PSX2RAD(p)		((p) * (2.0 * DB_PI) / DB_TURN)

/* config re-read cadence, in camera ticks (~1 s) */
#define DB_CFG_EVERY		30

/* shot timing */
#define DB_DEF_SHOT_MS		15000
#define DB_MIN_SHOT_MS		5000
#define DB_MAX_SHOT_MS		60000

/* config defaults */
#define DB_DEF_TRIPOD		1

/* tripod sampling */
#define DB_TRIPOD_TRIES		24	/* same-region candidates first */
#define DB_TRIPOD_TRIES_ANY	16	/* then candidates anywhere loaded */
#define DB_TRIPOD_MIN_DIST	1200
#define DB_TRIPOD_MAX_DIST	2200
#define DB_TRIPOD_MIN_HEIGHT	550
#define DB_TRIPOD_MAX_HEIGHT	700
#define DB_TRIPOD_CLEAR_RADIUS	80	/* keep the lens out of scenery */
#define DB_TRIPOD_AZ_MIN	120	/* ahead/side arc, degrees from the subject */
#define DB_TRIPOD_AZ_MAX	240

/* on-foot framing: a smaller subject needs a tighter shot */
#define DB_PED_SCALE		70

/* --------------------------------------------------------------------------
 * Directed shot table.
 *
 * azimuth is relative to the subject's heading: 0 = directly behind, 180 =
 * directly ahead, +90/-90 = the flanks. distance/height are in engine world
 * units (the stock chase camera sits ~850 behind a car); on foot they are
 * scaled to DB_PED_SCALE.
 * ------------------------------------------------------------------------ */
typedef struct DB_SHOT
{
	const char* name;
	int azimuth;		/* degrees from the subject's heading */
	int distance;		/* world units from the subject */
	int height;		/* world units above the subject */
} DB_SHOT;

static const DB_SHOT kShots[] =
{
	{ "CHASE",       0, 1150,  400 },
	{ "FRONT 3/4", 150, 1050,  300 },
	{ "SIDE",       90, 1350,  220 },
	{ "LOW",        35,  650,  180 },
	{ "HIGH WIDE", 200, 2100, 1500 },
	{ "TAIL",      -30,  950,  200 },
};

#define DB_SHOT_COUNT	((int)(sizeof(kShots) / sizeof(kShots[0])))
#define DB_SHOT_ORBIT	DB_SHOT_COUNT		/* moving: a 180-degree orbit */
#define DB_SHOT_CRANE	(DB_SHOT_COUNT + 1)	/* moving: rise and pull back */
#define DB_BAG_BASE	(DB_SHOT_COUNT + 2)	/* static + moving shots */
#define DB_SHOT_TRIPOD	DB_BAG_BASE		/* the extra bag slot */
#define DB_BAG_MAX	(DB_BAG_BASE + 1)

/* every shot is kept at least this far above the ground under the camera */
#define DB_GROUND_CLEARANCE	300

/* moving shot parameters: degrees from the subject's heading, world units */
#define DB_ORBIT_START_AZ	60
#define DB_ORBIT_SWEEP		-180
#define DB_ORBIT_DIST		1450
#define DB_ORBIT_HEIGHT		450

#define DB_CRANE_START_AZ	0
#define DB_CRANE_END_AZ		160
#define DB_CRANE_START_DIST	950
#define DB_CRANE_END_DIST	2300
#define DB_CRANE_START_HEIGHT	180
#define DB_CRANE_END_HEIGHT	1400

/* chance that a shot's offset is captured in world space at the cut instead
 * of following the subject's live heading: 1 in N. The car then rotates under
 * a static camera angle while the aim still tracks it. */
#define DB_DECOUPLE_ONE_IN	3

/* --------------------------------------------------------------------------
 * State.
 * ------------------------------------------------------------------------ */
typedef struct DB_CFG
{
	int enabled;		/* the camera switch, written by the pause-menu row */
	int shotMs;		/* milliseconds per shot */
	int tripod;		/* include the random tripod slot in the bag */
} DB_CFG;

typedef struct DB_STATE
{
	JERICHO_CONTEXT* ctx;
	const JER_HOST_API* api;

	DB_CFG cfg;

	int cfgCountdown;	/* camera ticks until the next live config re-read */
	int logCountdown;	/* camera ticks until the next diagnostic line */

	int active;		/* we overrode the camera last frame */
	int ticks;		/* camera ticks into the current shot */
	int bag[DB_BAG_MAX];	/* 0..5 static, 6 orbit, 7 crane, 8 tripod */
	int bagCount;		/* DB_BAG_BASE, plus 1 when the tripod is on */
	int bagPos;		/* index into bag[] */
	int bagCycle;		/* completed passes, for the log */
	int clampLift;		/* units the ground clamp raised the camera this frame */
	int shotBaseDir;	/* heading captured at the cut, for a decoupled shot */
	int decoupled;		/* this shot uses shotBaseDir, not the live heading */

	/* the current tripod: 0 = not sampled, 1 = placed, -1 = failed for this
	 * shot (do not retry every frame) */
	int tripodSet;
	int tripodX, tripodY, tripodZ;	/* camera position, render y */
	int tripodGround;		/* map height at the vantage */
	int tripodDistance;		/* sampled distance from the subject */
	int tripodHeight;		/* sampled height above the ground */
	int tripodAttempts;		/* candidates tried before one passed */

	unsigned int rng;	/* module-local LCG */
	int rngInit;
} DB_STATE;

static DB_STATE gDirectorBox;

/* --------------------------------------------------------------------------
 * Small angle / random helpers.
 * PSX angles are 0..4095 per turn; ratan2 is scaled the same way.
 * ------------------------------------------------------------------------ */
static int dbWrapAngle(int a)
{
	a &= (DB_TURN - 1);

	if (a < 0)
		a += DB_TURN;

	return a;
}

/* PSX angle -> whole degrees (for the log; the maths stays in PSX units) */
static int dbDegrees(int psx)
{
	return (dbWrapAngle(psx) * 360) / DB_TURN;
}

/* atan2(dx, dz) in PSX units, matching the engine's ratan2 convention */
static int dbRatan2(int dx, int dz)
{
	double a = atan2((double)dx, (double)dz);

	return dbWrapAngle((int)lround(a * (double)DB_TURN / (2.0 * DB_PI)));
}

/*
 * A module-local LCG, seeded once per run. Random2() is a pure function of the
 * engine frame counter, so it returns the same value for every call in one
 * frame; a shot sampler that used it would produce correlated picks. The seed
 * is drawn from the wall clock and the state address so two launches do not
 * play the same sequence, and it is kept in the module so a later sample does
 * not depend on the engine's counter.
 */
static unsigned int dbRand(DB_STATE* st)
{
	if (!st->rngInit)
	{
		unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(size_t)st;

		if (seed == 0)
			seed = 0x9E3779B9u;

		st->rng = seed;
		st->rngInit = 1;
	}

	st->rng = st->rng * 1103515245u + 12345u;

	return (st->rng >> 16) & 0x7fffu;
}

/* --------------------------------------------------------------------------
 * Config, through the stable host API (no engine import).
 * ------------------------------------------------------------------------ */
static void dbReadConfig(DB_STATE* st)
{
	const JER_HOST_CONFIG_API* c = (st->api != NULL) ? st->api->config : NULL;

	if (c == NULL || c->get_int == NULL || c->get_bool == NULL)
	{
		/* no config surface: the safe defaults still leave the module idle */
		st->cfg.enabled = 0;
		st->cfg.shotMs = DB_DEF_SHOT_MS;
		st->cfg.tripod = DB_DEF_TRIPOD;
		return;
	}

	st->cfg.enabled = c->get_bool(DB_ID, "enabled", 0) ? 1 : 0;
	st->cfg.shotMs = jer_clamp_int(c->get_int(DB_ID, "shot_ms", DB_DEF_SHOT_MS),
		DB_MIN_SHOT_MS, DB_MAX_SHOT_MS);
	st->cfg.tripod = c->get_bool(DB_ID, "tripod", DB_DEF_TRIPOD) ? 1 : 0;
}

/*
 * Write the resolved values once at boot so CONFIG/directorbox.ini
 * materialises with every knob visible and editable (the debugorbit pattern).
 * Reading first means a value the player edited is written back unchanged.
 */
static void dbSaveConfig(DB_STATE* st)
{
	const JER_HOST_CONFIG_API* c = (st->api != NULL) ? st->api->config : NULL;

	if (c == NULL)
		return;

	if (c->set_bool != NULL)
		c->set_bool(DB_ID, "enabled", st->cfg.enabled ? 1 : 0);

	if (c->set_int != NULL)
		c->set_int(DB_ID, "shot_ms", st->cfg.shotMs);

	if (c->set_bool != NULL)
		c->set_bool(DB_ID, "tripod", st->cfg.tripod ? 1 : 0);
}

static void dbLogOptions(DB_STATE* st, const char* when)
{
	DB_LOG(st, "[directorbox] options (%s): %s, shot_ms=%d, tripod=%s\n",
		when, st->cfg.enabled ? "ENABLED" : "idle", st->cfg.shotMs,
		st->cfg.tripod ? "on" : "off");
}

/* --------------------------------------------------------------------------
 * The shuffled shot bag.
 *
 * A bag — rather than a random pick per cut — guarantees that every pass shows
 * all six static framings, both moving shots, and the tripod once, in a
 * different order each time. The tripod's vantage is re-sampled whenever its
 * slot comes up.
 * ------------------------------------------------------------------------ */
static int dbBagCount(const DB_STATE* st)
{
	return DB_BAG_BASE + (st->cfg.tripod ? 1 : 0);
}

static void dbShuffleBag(DB_STATE* st)
{
	int i;
	int n = dbBagCount(st);

	for (i = 0; i < n; i++)
		st->bag[i] = i;

	for (i = n - 1; i > 0; i--)
	{
		int j = (int)(dbRand(st) % (unsigned int)(i + 1));
		int t = st->bag[i];

		st->bag[i] = st->bag[j];
		st->bag[j] = t;
	}

	st->bagCount = n;
	st->bagPos = 0;
	st->ticks = 0;
	st->tripodSet = 0;
	st->bagCycle++;
}

static int dbShotTicks(const DB_STATE* st)
{
	int t = (st->cfg.shotMs * DB_FPS) / 1000;

	return (t < 1) ? 1 : t;
}

static const char* dbShotName(int shot)
{
	if (shot == DB_SHOT_TRIPOD)
		return "TRIPOD";

	if (shot == DB_SHOT_ORBIT)
		return "ORBIT";

	if (shot == DB_SHOT_CRANE)
		return "CRANE";

	if (shot >= 0 && shot < DB_SHOT_COUNT)
		return kShots[shot].name;

	return "?";
}

/*
 * The heading a shot's offset is measured from. Normally it follows the car's
 * live facing, so a chase/side/crane shot keeps its relation to the car. A
 * decoupled shot freezes the heading at the cut instead, so the car can rotate
 * under a static camera angle; the aim still tracks it.
 */
static int dbShotHeading(const DB_STATE* st, const JER_ARGS_CAMERA* a)
{
	return st->decoupled ? st->shotBaseDir : a->baseDir;
}

/* --------------------------------------------------------------------------
 * Camera placement.
 *
 * The camera is placed in world space and then aimed at the subject, so the
 * directed shots and the fixed tripod share one aim path. The aim is what
 * makes the tripod "pan": its position never changes, but its yaw and pitch
 * are recomputed every frame from the moving basePos.
 * ------------------------------------------------------------------------ */
static void dbAimAtSubject(JER_ARGS_CAMERA* a, int camX, int camY, int camZ)
{
	DB_ANG* ang = (DB_ANG*)a->cameraAngle;
	int* base = (int*)a->basePos;
	int dx = camX - base[0];
	int dz = camZ - base[2];
	int above = base[1] - camY;	/* positive when the camera is above the subject */
	double horiz = sqrt((double)dx * (double)dx + (double)dz * (double)dz);
	int pitchDeg;

	if (horiz < 1.0)
		horiz = 1.0;

	pitchDeg = (int)lround(atan2((double)above, horiz) * 180.0 / DB_PI);

	ang->vx = (short)DB_DEG2PSX(pitchDeg);	/* positive = looking down */
	ang->vy = (short)dbWrapAngle(-(dbRatan2(dx, dz) + 2048));
	ang->vz = 0;
}

/*
 * How far into the current shot we are, 0..1. The ticks are the same 30 Hz
 * clock the cut timer uses, so a moving shot is deterministic and cannot
 * drift over a long pass.
 */
static double dbProgress(const DB_STATE* st)
{
	int total = dbShotTicks(st);
	double p = (total > 0) ? (double)st->ticks / (double)total : 0.0;

	if (p < 0.0)
		p = 0.0;

	if (p > 1.0)
		p = 1.0;

	return p;
}

static int dbGroundAt(const DB_STATE* st, int x, int z)
{
	const JER_HOST_CAR_API* car = (st->api != NULL) ? st->api->car : NULL;
	JER_VEC query;

	if (car == NULL || car->map_height == NULL)
		return 0;

	query.x = x;
	query.y = 0;
	query.z = z;
	query.pad = 0;

	return car->map_height(&query);
}

/*
 * Keep the lens above the terrain.
 *
 * The shot tables place the camera relative to the SUBJECT, which says nothing
 * about the ground a thousand units away: a shallow shot over a bank, an
 * embankment or a raised junction can end up under the local surface. The
 * engine's ground plane is MapHeight(x,z) and its render y is -MapHeight; y is
 * inverted, so raising the camera means making y SMALLER. Returns how far the
 * camera was raised (0 when it was already clear).
 */
static int dbClampAboveGround(DB_STATE* st, DB_VEC* cam)
{
	int ground = dbGroundAt(st, cam->vx, cam->vz);
	int ceilY;

	if (ground == 0)
		return 0;

	ceilY = -ground - DB_GROUND_CLEARANCE;

	if (cam->vy > ceilY)
	{
		int lift = cam->vy - ceilY;

		cam->vy = ceilY;
		return lift;
	}

	return 0;
}

/*
 * Ground-clamp first, then aim: the clamp moves the camera vertically, so the
 * pitch has to be computed from the final position.
 */
static void dbFinishPlacement(DB_STATE* st, JER_ARGS_CAMERA* a, DB_VEC* cam)
{
	st->clampLift = dbClampAboveGround(st, cam);
	dbAimAtSubject(a, cam->vx, cam->vy, cam->vz);
}

/*
 * A directed shot: place the camera on the shot's ring around the subject,
 * then aim back at it. `baseDir` is the chased object's facing; azimuth 0 is
 * directly behind it, exactly the chase camera's own offset.
 */
static void dbPlaceDirected(DB_STATE* st, JER_ARGS_CAMERA* a, const DB_SHOT* shot)
{
	DB_VEC* cam = (DB_VEC*)a->cameraPosition;
	int* base = (int*)a->basePos;
	int scale = a->inCar ? 100 : DB_PED_SCALE;
	int dist = shot->distance * scale / 100;
	int height = shot->height * scale / 100;
	int yaw = dbWrapAngle(dbShotHeading(st, a) + DB_DEG2PSX(shot->azimuth));
	double yawRad = DB_PSX2RAD(yaw);

	cam->vx = base[0] + (int)lround((double)dist * sin(yawRad));
	cam->vy = base[1] - height;	/* Y is INVERTED here: a smaller y is HIGHER */
	cam->vz = base[2] + (int)lround((double)dist * cos(yawRad));

	dbFinishPlacement(st, a, cam);
}

/*
 * ORBIT — a moving shot: the camera carries the framing around the subject
 * through 180 degrees, from the rear quarter to the front quarter, at a
 * constant radius and height. The aim is recomputed every frame, so the
 * subject stays centred while the world sweeps past behind it.
 */
static void dbPlaceOrbit(DB_STATE* st, JER_ARGS_CAMERA* a)
{
	DB_VEC* cam = (DB_VEC*)a->cameraPosition;
	int* base = (int*)a->basePos;
	int scale = a->inCar ? 100 : DB_PED_SCALE;
	int dist = DB_ORBIT_DIST * scale / 100;
	int height = DB_ORBIT_HEIGHT * scale / 100;
	int az = DB_ORBIT_START_AZ + (int)lround((double)DB_ORBIT_SWEEP * dbProgress(st));
	int yaw = dbWrapAngle(dbShotHeading(st, a) + DB_DEG2PSX(az));
	double yawRad = DB_PSX2RAD(yaw);

	cam->vx = base[0] + (int)lround((double)dist * sin(yawRad));
	cam->vy = base[1] - height;
	cam->vz = base[2] + (int)lround((double)dist * cos(yawRad));

	dbFinishPlacement(st, a, cam);
}

/*
 * CRANE — a moving shot: starts low and close behind the subject, then rises
 * while pulling back and swinging round to the front quarter. Smoothstep over
 * the shot keeps the start and end gentle; the aim tracks the car throughout.
 */
static void dbPlaceCrane(DB_STATE* st, JER_ARGS_CAMERA* a)
{
	DB_VEC* cam = (DB_VEC*)a->cameraPosition;
	int* base = (int*)a->basePos;
	int scale = a->inCar ? 100 : DB_PED_SCALE;
	double p = dbProgress(st);
	double e = p * p * (3.0 - 2.0 * p);
	int az = DB_CRANE_START_AZ + (int)lround((double)(DB_CRANE_END_AZ - DB_CRANE_START_AZ) * e);
	int dist = DB_CRANE_START_DIST + (int)lround((double)(DB_CRANE_END_DIST - DB_CRANE_START_DIST) * e);
	int height = DB_CRANE_START_HEIGHT + (int)lround((double)(DB_CRANE_END_HEIGHT - DB_CRANE_START_HEIGHT) * e);
	int yaw;
	double yawRad;

	dist = dist * scale / 100;
	height = height * scale / 100;
	yaw = dbWrapAngle(dbShotHeading(st, a) + DB_DEG2PSX(az));
	yawRad = DB_PSX2RAD(yaw);

	cam->vx = base[0] + (int)lround((double)dist * sin(yawRad));
	cam->vy = base[1] - height;
	cam->vz = base[2] + (int)lround((double)dist * cos(yawRad));

	dbFinishPlacement(st, a, cam);
}

/* --------------------------------------------------------------------------
 * The random roadside tripod.
 *
 * Sampled from the subject's current heading into the front/side arc, so the
 * shot usually sees them approaching or passing. Each candidate is grounded
 * with map_height() and tested with cell_empty(); a point inside a wall or
 * over an unloaded region is rejected. The first 24 tries are restricted to
 * the subject's own map region (the area the engine is already streaming),
 * then 16 more are allowed anywhere that has ground.
 * ------------------------------------------------------------------------ */
static int dbTryTripodCandidate(DB_STATE* st, JER_ARGS_CAMERA* a, int requireRegion,
	int* outX, int* outY, int* outZ, int* outGround, int* outDist, int* outHeight)
{
	const JER_HOST_CAR_API* car = (st->api != NULL) ? st->api->car : NULL;
	const JER_HOST_WORLD_API* world = (st->api != NULL) ? st->api->world : NULL;
	int* base = (int*)a->basePos;
	int baseRegion = -1;
	int scale = a->inCar ? 100 : DB_PED_SCALE;
	int azDeg, dist, height, yaw;
	int x, z, ground, camY;
	double yawRad;
	JER_VEC query;
	JER_VEC clear;

	if (car == NULL || car->map_height == NULL || car->cell_empty == NULL)
		return 0;

	if (world != NULL && world->region_of != NULL)
		baseRegion = world->region_of(base[0], base[2]);

	azDeg = DB_TRIPOD_AZ_MIN + (int)(dbRand(st) % (unsigned int)(DB_TRIPOD_AZ_MAX - DB_TRIPOD_AZ_MIN + 1));
	dist = (DB_TRIPOD_MIN_DIST + (int)(dbRand(st) % (unsigned int)(DB_TRIPOD_MAX_DIST - DB_TRIPOD_MIN_DIST + 1))) * scale / 100;
	height = (DB_TRIPOD_MIN_HEIGHT + (int)(dbRand(st) % (unsigned int)(DB_TRIPOD_MAX_HEIGHT - DB_TRIPOD_MIN_HEIGHT + 1))) * scale / 100;

	yaw = dbWrapAngle(a->baseDir + DB_DEG2PSX(azDeg));
	yawRad = DB_PSX2RAD(yaw);

	x = base[0] + (int)lround((double)dist * sin(yawRad));
	z = base[2] + (int)lround((double)dist * cos(yawRad));

	/* MapHeight only needs x/z; a 0 answer means no loaded ground plane */
	query.x = x;
	query.y = 0;
	query.z = z;
	query.pad = 0;

	ground = car->map_height(&query);

	if (ground == 0)
		return 0;

	if (requireRegion && baseRegion >= 0 && world != NULL && world->region_of != NULL)
	{
		if (world->region_of(x, z) != baseRegion)
			return 0;
	}

	camY = -ground - height;

	/* CellEmpty wants the engine's positive-down world y */
	clear.x = x;
	clear.y = -camY;
	clear.z = z;
	clear.pad = 0;

	if (!car->cell_empty(&clear, DB_TRIPOD_CLEAR_RADIUS))
		return 0;

	*outX = x;
	*outY = camY;
	*outZ = z;
	*outGround = ground;
	*outDist = dist;
	*outHeight = height;

	return 1;
}

static int dbEnsureTripod(DB_STATE* st, JER_ARGS_CAMERA* a)
{
	int attempt;
	int x = 0, y = 0, z = 0, ground = 0, dist = 0, height = 0;

	if (st->tripodSet > 0)
		return 1;

	if (st->tripodSet < 0)
		return 0;

	for (attempt = 0; attempt < DB_TRIPOD_TRIES; attempt++)
	{
		if (dbTryTripodCandidate(st, a, 1, &x, &y, &z, &ground, &dist, &height))
		{
			st->tripodX = x;
			st->tripodY = y;
			st->tripodZ = z;
			st->tripodGround = ground;
			st->tripodDistance = dist;
			st->tripodHeight = height;
			st->tripodAttempts = attempt + 1;
			st->tripodSet = 1;
			return 1;
		}
	}

	for (attempt = 0; attempt < DB_TRIPOD_TRIES_ANY; attempt++)
	{
		if (dbTryTripodCandidate(st, a, 0, &x, &y, &z, &ground, &dist, &height))
		{
			st->tripodX = x;
			st->tripodY = y;
			st->tripodZ = z;
			st->tripodGround = ground;
			st->tripodDistance = dist;
			st->tripodHeight = height;
			st->tripodAttempts = DB_TRIPOD_TRIES + attempt + 1;
			st->tripodSet = 1;
			return 1;
		}
	}

	st->tripodAttempts = DB_TRIPOD_TRIES + DB_TRIPOD_TRIES_ANY;
	st->tripodSet = -1;

	DB_LOG(st, "[directorbox] tripod: no clear vantage after %d attempts - using CHASE for this shot\n",
		st->tripodAttempts);

	return 0;
}

static void dbPlaceTripod(DB_STATE* st, JER_ARGS_CAMERA* a)
{
	DB_VEC* cam = (DB_VEC*)a->cameraPosition;

	cam->vx = st->tripodX;
	cam->vy = st->tripodY;
	cam->vz = st->tripodZ;

	dbFinishPlacement(st, a, cam);
}

/* --------------------------------------------------------------------------
 * Diagnostics: a run has to be checkable from the log alone.
 * ------------------------------------------------------------------------ */
static void dbLogShot(DB_STATE* st, JER_ARGS_CAMERA* a, int shot)
{
	DB_VEC* cam = (DB_VEC*)a->cameraPosition;
	DB_ANG* ang = (DB_ANG*)a->cameraAngle;
	int* base = (int*)a->basePos;
	int dx = cam->vx - base[0];
	int dz = cam->vz - base[2];
	int horiz = (int)lround(sqrt((double)dx * (double)dx + (double)dz * (double)dz));
	int above = base[1] - cam->vy;
	int ground = dbGroundAt(st, cam->vx, cam->vz);
	const char* basis = st->decoupled ? "world" : "car";
	int dir = dbDegrees(a->baseDir);

	if (shot == DB_SHOT_TRIPOD)
	{
		DB_LOG(st, "[directorbox] shot %d/%d TRIPOD vantage=(%d,%d) ground=%d height=%d dist=%d attempts=%d gnd=%d clamp=%d basis=world dir=%d%s\n",
			st->bagPos + 1, st->bagCount, st->tripodX, st->tripodZ, st->tripodGround,
			st->tripodHeight, st->tripodDistance, st->tripodAttempts,
			ground, st->clampLift, dir, a->inCar ? "" : " (on foot)");
	}
	else
	{
		DB_LOG(st, "[directorbox] shot %d/%d %s pct=%d: cam=(%d,%d,%d) target=(%d,%d,%d) "
			"look=(%d,%d) ang=(%d,%d,%d) gnd=%d clamp=%d basis=%s dir=%d%s\n",
			st->bagPos + 1, st->bagCount, dbShotName(shot),
			(int)lround(dbProgress(st) * 100.0),
			cam->vx, cam->vy, cam->vz, base[0], base[1], base[2], horiz, above,
			(int)ang->vx, (int)ang->vy, (int)ang->vz,
			ground, st->clampLift, basis, dir,
			a->inCar ? "" : " (on foot)");
	}
}

static void dbLogDiag(DB_STATE* st, JER_ARGS_CAMERA* a, int shot)
{
	DB_VEC* cam = (DB_VEC*)a->cameraPosition;
	DB_ANG* ang = (DB_ANG*)a->cameraAngle;
	int* base = (int*)a->basePos;
	int dx = cam->vx - base[0];
	int dz = cam->vz - base[2];
	int horiz = (int)lround(sqrt((double)dx * (double)dx + (double)dz * (double)dz));
	int above = base[1] - cam->vy;
	int ground = dbGroundAt(st, cam->vx, cam->vz);
	const char* basis = (shot == DB_SHOT_TRIPOD || st->decoupled) ? "world" : "car";
	int dir = dbDegrees(a->baseDir);

	DB_LOG(st, "[directorbox] t=%d %s pct=%d cam=(%d,%d,%d) target=(%d,%d,%d) "
		"look=(%d,%d) ang=(%d,%d,%d) gnd=%d clamp=%d basis=%s dir=%d\n",
		st->ticks, dbShotName(shot), (int)lround(dbProgress(st) * 100.0),
		cam->vx, cam->vy, cam->vz, base[0], base[1], base[2], horiz, above,
		(int)ang->vx, (int)ang->vy, (int)ang->vz, ground, st->clampLift, basis, dir);
}

/* --------------------------------------------------------------------------
 * Hooks.
 * ------------------------------------------------------------------------ */

/*
 * JER_EVENT_GAME_START — a level is starting (fresh launch, restart or the next
 * mission). Rebuild the bag so the first cut of the new level is a fresh pass.
 */
static int dbOnGameStart(void* ud, void* args)
{
	DB_STATE* st = (DB_STATE*)ud;

	(void)args;

	if (st == NULL)
		return JER_RESULT_CONTINUE;

	dbReadConfig(st);

	st->active = 0;
	st->tripodSet = 0;
	st->cfgCountdown = DB_CFG_EVERY;
	st->logCountdown = 0;
	dbShuffleBag(st);

	DB_LOG(st, "[directorbox] level start: %s\n",
		st->cfg.enabled ? "armed for the next camera frame" : "idle");

	return JER_RESULT_CONTINUE;
}

/*
 * JER_EVENT_CAMERA — fired at the end of InitCamera with the engine's own
 * placement for this frame. This is where the director camera replaces it.
 */
static int dbOnCamera(void* ud, void* args)
{
	DB_STATE* st = (DB_STATE*)ud;
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	int shot;
	int cut = 0;

	if (st == NULL || a == NULL)
		return JER_RESULT_CONTINUE;

	/* re-read the config surface: the pause-menu row writes through it, and
	 * turning `enabled` off hands the camera straight back to the engine.
	 * (The host caches a hand-edited ini for the run.) */
	if (--st->cfgCountdown <= 0)
	{
		st->cfgCountdown = DB_CFG_EVERY;
		dbReadConfig(st);
	}

	if (!st->cfg.enabled)
	{
		if (st->active)
		{
			st->active = 0;
			st->tripodSet = 0;
			DB_LOG(st, "[directorbox] idle: stock camera restored\n");
		}

		return JER_RESULT_CONTINUE;
	}

	if (a->cameraPosition == NULL || a->cameraAngle == NULL || a->basePos == NULL)
		return JER_RESULT_CONTINUE;

	if (!st->active)
	{
		dbShuffleBag(st);
		st->active = 1;
		st->logCountdown = 0;

		DB_LOG(st, "[directorbox] armed: %d-shot bag (%s), shot %d ms\n",
			st->bagCount, st->cfg.tripod ? "6 static + 2 moving + tripod" : "6 static + 2 moving",
			st->cfg.shotMs);
	}

	/* the tripod slot may have been enabled or disabled: rebuild the bag */
	if (st->bagCount != dbBagCount(st))
		dbShuffleBag(st);

	if (st->ticks >= dbShotTicks(st))
	{
		st->bagPos++;

		if (st->bagPos >= st->bagCount)
		{
			dbShuffleBag(st);
			DB_LOG(st, "[directorbox] new pass: %d shots reshuffled\n", st->bagCount);
		}
		else
		{
			st->ticks = 0;
			st->tripodSet = 0;
		}

		cut = 1;
		st->logCountdown = 0;
	}

	shot = (st->bagPos >= 0 && st->bagPos < st->bagCount) ? st->bag[st->bagPos] : 0;

	/* freeze or follow the heading for this shot */
	if (cut || st->ticks == 0)
	{
		st->shotBaseDir = a->baseDir;
		st->decoupled = (dbRand(st) % DB_DECOUPLE_ONE_IN) == 0;
	}

	if (shot == DB_SHOT_TRIPOD && !st->cfg.tripod)
		shot = 0;	/* tripod was switched off between cuts */

	if (shot == DB_SHOT_TRIPOD)
	{
		if (!dbEnsureTripod(st, a))
			shot = 0;
	}

	st->clampLift = 0;

	if (shot == DB_SHOT_TRIPOD)
		dbPlaceTripod(st, a);
	else if (shot == DB_SHOT_ORBIT)
		dbPlaceOrbit(st, a);
	else if (shot == DB_SHOT_CRANE)
		dbPlaceCrane(st, a);
	else
		dbPlaceDirected(st, a, &kShots[shot]);

	/* full control: the engine rebuilds the view matrices from camera_angle */
	a->override = 1;

	if (cut || st->ticks == 0)
		dbLogShot(st, a, shot);

	if (++st->logCountdown >= DB_CFG_EVERY)
	{
		st->logCountdown = 0;
		dbLogDiag(st, a, shot);
	}

	st->ticks++;

	return JER_RESULT_CONTINUE;
}

/*
 * JER_EVENT_SHUTDOWN — the game is going away; nothing to hand back (the camera
 * is re-placed by the engine every frame), but say so in the log.
 */
static int dbOnShutdown(void* ud, void* args)
{
	DB_STATE* st = (DB_STATE*)ud;

	(void)args;

	if (st != NULL)
	{
		st->active = 0;
		DB_LOG(st, "[directorbox] shutdown (no camera state to restore)\n");
	}

	return JER_RESULT_CONTINUE;
}

/* --------------------------------------------------------------------------
 * Pause menu.
 *
 * One row, registered through the host UI table (the runtime-DLL-safe route;
 * the engine-side jer_pause_menu_register() is not in the host table). The row
 * writes `enabled` through the config table and returns QUIT_CONTINUE so the
 * pause closes on the press and the director cut is visible immediately.
 * ------------------------------------------------------------------------ */
static void dbMenuLabel(void* ud, char* out, int max)
{
	DB_STATE* st = (DB_STATE*)ud;

	if (out == NULL || max <= 0)
		return;

	snprintf(out, (size_t)max, "Director camera: %s",
		(st != NULL && st->cfg.enabled) ? "ON" : "OFF");
}

static int dbMenuToggle(void* ud, int direction)
{
	DB_STATE* st = (DB_STATE*)ud;

	(void)direction;

	if (st == NULL)
		return JER_PAUSE_QUIT_NONE;

	st->cfg.enabled = st->cfg.enabled ? 0 : 1;
	dbSaveConfig(st);

	if (st->cfg.enabled)
	{
		dbShuffleBag(st);
		st->active = 0;
		st->logCountdown = 0;
		DB_LOG(st, "[directorbox] menu: ON - unpausing into the director camera\n");
	}
	else
	{
		st->active = 0;
		st->tripodSet = 0;
		DB_LOG(st, "[directorbox] menu: OFF - stock camera restored\n");
	}

	return JER_PAUSE_QUIT_CONTINUE;
}

static const JER_PAUSE_MENU_ITEM kDirectorBoxItems[] =
{
	{ NULL, dbMenuLabel, dbMenuToggle, &gDirectorBox, NULL, 0 },
};

static const JER_PAUSE_MENU kDirectorBoxMenu =
{
	"DIRECTOR BOX",
	kDirectorBoxItems,
	(int)(sizeof(kDirectorBoxItems) / sizeof(kDirectorBoxItems[0])),
	"directorbox.main"
};

/* --------------------------------------------------------------------------
 * Entry.
 * ------------------------------------------------------------------------ */
JER_MODULE_ENTRY(jer_module_directorbox_entry)(JERICHO_CONTEXT* ctx)
{
	DB_STATE* st = &gDirectorBox;

	memset(st, 0, sizeof(*st));
	st->ctx = ctx;
	st->api = jerHostApiGet();
	st->cfgCountdown = DB_CFG_EVERY;
	st->logCountdown = 0;

	dbReadConfig(st);
	dbSaveConfig(st);
	dbShuffleBag(st);

	ctx->jer_register_module(ctx,
		DB_ID,
		"Director Box",
		"0.1.0",
		"Jaret Ludvik",
		"A cinematic director camera toggled from the pause menu: cuts between "
		"static chase/front/side/low/high/tail framings, two moving shots, and a "
		"randomly placed roadside tripod, with some angles decoupled from the car "
		"so it rotates under a static view.",
		"",
		JERICHO_SDK_VERSION);

	/* priority 1000: lower runs first, so the director camera is the last word
	 * on the camera this frame and no other module's chase-cam nudge survives
	 * it */
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, dbOnGameStart, st, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, dbOnCamera, st, 1000);
	ctx->jer_register_hook(ctx, JER_EVENT_SHUTDOWN, dbOnShutdown, st, 0);

	if (st->api != NULL && st->api->ui != NULL && st->api->ui->pause_menu_register != NULL)
	{
		st->api->ui->pause_menu_register(&kDirectorBoxMenu);
		DB_LOG(st, "[directorbox] pause menu registered: DIRECTOR BOX -> directorbox.main\n");
	}
	else
	{
		DB_LOG(st, "[directorbox] warning: host UI API unavailable - pause menu not registered\n");
	}

	dbLogOptions(st, "boot");

	if (!st->cfg.enabled)
		DB_LOG(st, "[directorbox] idle: enable the addon in Options -> JERICHO, then flip "
			"Director camera in the pause menu\n");

	DB_LOG(st, "[directorbox] registered (SDK v%d)\n", ctx->sdkVersion);
}
