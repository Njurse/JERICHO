/*
 * jer_events.h — game-side argument structs for the JERICHO events.
 *
 * The SDK (jericho.h) stays generic; these structs carry the game's types
 * (as void* so the SDK never depends on game headers) at each hook point.
 * Modules cast the void* members back to the real types.
 *
 * Every call site in the vanilla files is a small tagged block:
 *     // JERICHO-HOOK: <what>
 *     { JER_ARGS_X a; ...; jer_fire(JER_EVENT_X, &a); }
 * With no modules registered the fire is a no-op, so the vanilla build is
 * unchanged without modules.
 */
#ifndef JERICHO_JER_EVENTS_H
#define JERICHO_JER_EVENTS_H

#include "jericho.h"


// To do: Separate CRUMPLE functions from jericho events and try to use vanilla-bound function hooks

/* JER_EVENT_COLLISION — car-car / car-world collision.
 * car0/car1: CAR_DATA* (car1 NULL for world hits); point/normal: VECTOR*;
 * howHard: impact strength; wheelMask: wheels eligible to bend (0xF = all). */
typedef struct JER_ARGS_COLLISION
{
	void* car0;
	void* car1;
	void* point;
	void* normal;
	int howHard;
	int wheelMask;
} JER_ARGS_COLLISION;

/* JER_EVENT_DENT_PASS — the vertex deformation pass.
 * car: CAR_DATA*; damage: int* (the 6 zone-damage values). */
typedef struct JER_ARGS_DENT_PASS
{
	void* car;
	void* damage;
} JER_ARGS_DENT_PASS;

/* JER_EVENT_RESET_CAR — a car's damage state is being reset. */
typedef struct JER_ARGS_RESET_CAR
{
	int carId;
} JER_ARGS_RESET_CAR;

/* JER_EVENT_GET_WHEEL_BEND — query: per-wheel bend array (SVECTOR*),
 * NULL when nothing is registered. */
typedef struct JER_ARGS_QUERY_PTR
{
	int carId;
	void* result;
} JER_ARGS_QUERY_PTR;

/* JER_EVENT_GET_WHEEL_DAMAGE — query: cumulative wheel damage 0..4096. */
typedef struct JER_ARGS_QUERY_INT
{
	int carId;
	int result;
} JER_ARGS_QUERY_INT;

/* JER_EVENT_GET_IMPACT_INFO — query: newest impact for the debug overlay.
 * point/normal: VECTOR* out; howHard: int* out. */
typedef struct JER_ARGS_IMPACT_INFO
{
	int carId;
	void* point;
	void* normal;
	void* howHard;
} JER_ARGS_IMPACT_INFO;

/* JER_EVENT_DRAW_WHEEL — wheel draw; a module may distort the per-wheel
 * vertex copy (SVECTOR*) for camber/toe. */
typedef struct JER_ARGS_DRAW_WHEEL
{
	int carId;
	int wheelnum;
	void* verts;
	int numVerts;
} JER_ARGS_DRAW_WHEEL;

/* JER_EVENT_GET_WHEEL_PARAMS — query: wheel-damage physics parameters the
 * engine needs: front/rear steering-deviation scales and the lateral scrub
 * force. All 0 = no wheel-damage influence (stock behavior). */
typedef struct JER_ARGS_WHEEL_PARAMS
{
	int carId;
	int frontScale;
	int rearScale;
	int scrubForce;
} JER_ARGS_WHEEL_PARAMS;

/* JER_EVENT_GET_BUDDHA — query: clamp totalDamage below the totaled
 * threshold for the player car (Buddha mode); 0 = disabled. */
typedef struct JER_ARGS_QUERY_FLAG
{
	int result;
} JER_ARGS_QUERY_FLAG;

/* JER_EVENT_PAUSE_MENU — pause menu shell <-> module bridge. The engine
 * keeps the Crumple Debug menu items; the module owns their state. */
enum
{
	JER_PAUSE_CRUMPLE_GET_DPAD_TEXT = 0,	/* result: const char* label */
	JER_PAUSE_CRUMPLE_GET_BUDDHA_TEXT,	/* result: const char* label */
	JER_PAUSE_CRUMPLE_GET_COL_TEXT,		/* result: const char* label */
	JER_PAUSE_CRUMPLE_GET_OVERLAY_TEXT,	/* result: const char* label */
	JER_PAUSE_CRUMPLE_TOGGLE_DPAD,
	JER_PAUSE_CRUMPLE_TOGGLE_BUDDHA,
	JER_PAUSE_CRUMPLE_TOGGLE_COL,
	JER_PAUSE_CRUMPLE_TOGGLE_OVERLAY,
	JER_PAUSE_CRUMPLE_REPAIR,

	JER_PAUSE_SANDBOX_GET_LABEL,		/* result: const char* label or NULL */
	JER_PAUSE_SANDBOX_OPEN,			/* handler returns JER_RESULT_STOP if opened */

	/* fired when the player presses START (single-player): a module that
	 * replaces the pause menu (e.g. the sandbox overlay) claims the press
	 * by returning JER_RESULT_STOP, and the engine pause never opens */
	JER_PAUSE_OPEN,

	/* d2pl (Driver 2 Parallel Lines) settings bridge: one GET_LABEL and one
	 * ADJUST action, multiplexed by the item id in JER_ARGS_PAUSE_MENU.value.
	 * GET_LABEL -> result: const char* label. ADJUST -> value carries the
	 * item id, result carries the direction (-1/0/+1). */
	JER_PAUSE_D2PL_GET_LABEL,
	JER_PAUSE_D2PL_ADJUST
};

/* d2pl settings item ids (JER_ARGS_PAUSE_MENU.value for the d2pl actions).
 * Shared between the pause-menu shell (pause.c) and the d2pl module. */
enum
{
	D2PL_ITEM_SENS_X = 0,	/* view-change sensitivity (horizontal) */
	D2PL_ITEM_SENS_Y,	/* view-change sensitivity (vertical) */
	D2PL_ITEM_FOOT_DIST,	/* on-foot camera pull-in distance */
	D2PL_ITEM_FOOT_LAT,	/* on-foot shoulder offset */
	D2PL_ITEM_FOOT_HEIGHT,	/* on-foot camera height (below the body) */
	D2PL_ITEM_CAR_DIST,	/* in-car pull-in */
	D2PL_ITEM_CAR_LAT,	/* in-car lateral (% of the car's bbox width) */
	D2PL_ITEM_SHOULDER,	/* camera shoulder side */
	D2PL_ITEM_INVERT_H,	/* invert horizontal look */
	D2PL_ITEM_INVERT_V,	/* invert vertical look */
	D2PL_ITEM_FOV,		/* base FOV override (55-90 degrees) */
	D2PL_ITEM_LASER,	/* laser sight color cycle (0 = off) */
	D2PL_ITEM_CAMERA,	/* camera mod on/off (diagnostics kill-switch) */
	D2PL_ITEM_COUNT
};

/* d2pl laser-sight colors (D2PL_ITEM_LASER values) */
enum
{
	D2PL_LASER_OFF = 0,
	D2PL_LASER_RED,
	D2PL_LASER_GREEN,
	D2PL_LASER_BLUE,
	D2PL_LASER_WHITE,
	D2PL_LASER_COUNT
};

typedef struct JER_ARGS_PAUSE_MENU
{
	int action;
	void* result;	/* out: const char* for the GET_*_TEXT actions */
	int value;	/* item id for the d2pl actions (and any future multiplexed ones) */
} JER_ARGS_PAUSE_MENU;

/* JER_EVENT_CAMERA — fired at the end of InitCamera once camera_position is
 * set; modules adjust the position/angle in place (stock = no handler).
 * Set override = 1 when a module takes full control: the engine then rebuilds
 * the view matrices from the (possibly changed) camera_angle so nothing
 * downstream (BuildWorldMatrix) fights the module's values. */
typedef struct JER_ARGS_CAMERA
{
	void* player;		/* PLAYER* whose camera was just updated */
	void* cameraPosition;	/* VECTOR* (camera_position) — modules may move it */
	void* cameraAngle;	/* SVECTOR* (camera_angle) — modules may re-aim it */
	int cameraView;		/* the player's camera view */
	int override;		/* out: 1 = rebuild the view matrices from our values */

	/* chase-cam inputs the engine computed this frame — a module that sets
	 * override can use these to run its own full chase-cam math instead of
	 * poking at the stock result */
	void* basePos;		/* LONGVECTOR4* (x,y,z) — where the camera chases */
	int baseDir;		/* the chased object's facing (0..4095) */
	int carSpeed;		/* the chased car's hd.wheel_speed — raw fixed point,
				   FIXEDH() it to get world units (0 on foot) */
	int inCar;		/* 1 when chasing a car, 0 on foot */
} JER_ARGS_CAMERA;

/* JER_EVENT_CAMERA_LOOK — fired at the top of TurnHead() every frame the
 * chase/bumper camera is about to apply look input. A module may drive the
 * look by writing PLAYER.headTarget (smoothed by the engine via headPos) and
 * may set suppress to skip the stock L2/R2 look-left/right/back handling
 * (e.g. while aiming). stickX/stickY are the right-stick analog, signed.
 * gripOrbit = 1 takes over the camera orbit: the engine skips its own
 * "settle back behind the player" lerp on lp->cameraAngle so a module can
 * orbit the camera freely (GTA-style) and only ease it back once released. */
typedef struct JER_ARGS_CAMERA_LOOK
{
	void* player;		/* PLAYER* (write lp->headTarget / headTimer) */
	int paddCamera;		/* in: current camera pad bits (L2/R2/L3 look) */
	int stickX;		/* in: right-stick X analog, signed -128..127 */
	int stickY;		/* in: right-stick Y analog, signed -128..127 */
	int suppress;		/* out: nonzero = skip stock L2/R2 look handling */
	int gripOrbit;		/* out: 1 = module owns the camera orbit (skip the
				   engine's settle-back lerp this frame) */
} JER_ARGS_CAMERA_LOOK;

/* JER_EVENT_PED_INPUT — fired in the ped-input loop right before
 * ProcessTannerPad(), after the engine synthesized D-pad bits from the left
 * stick. A module may rewrite pad (in/out) to implement camera-relative
 * movement: combine stickX/stickY with cameraYaw to produce movement. */
typedef struct JER_ARGS_PED_INPUT
{
	void* player;		/* PLAYER* whose pad is being processed */
	int pad;		/* in/out: tannerPad bits (TANNER_PAD_*) */
	int stickX;		/* in: left-stick X analog, signed -128..127 */
	int stickY;		/* in: left-stick Y analog, signed -128..127 */
	int cameraYaw;		/* in: current camera facing, 0..4095 (PSX angle) */
} JER_ARGS_PED_INPUT;

/* JER_EVENT_PED_SKELETON — fired while the player ped is being drawn
 * (newShowTanner), after the skeleton was posed from motion data. A module
 * may override bone rotations in skel (BONE*) to force poses (e.g. an arm
 * holding a weapon) and may read jointPos (SVECTOR* vJPos[NUM_BONES]) for
 * the world offsets of joints such as RHAND (add playerPos for world pos).
 * phase 0 = before the joint positions are accumulated (pose overrides land
 * here, in skel[i].vCurrPos); phase 1 = after accumulation (hand positions
 * are valid, draw the weapon mesh). shadow = 1 while drawing the ped's
 * shadow (modules usually skip extra meshes then). All pointers are the
 * engine's own structures cast to void*. */
typedef struct JER_ARGS_PED_SKELETON
{
	void* ped;		/* LPPEDESTRIAN being drawn */
	void* skel;		/* BONE* Skel[NUM_BONES] — writeable rotations/positions */
	void* jointPos;		/* SVECTOR* vJPos[NUM_BONES] — absolute joint offsets */
	void* playerPos;	/* VECTOR* model origin in world space */
	void* cameraPos;	/* VECTOR* camera_position */
	int phase;		/* 0 = pre-accumulation (pose), 1 = post (draw) */
	int shadow;		/* 1 while the ped's shadow is being drawn */
} JER_ARGS_PED_SKELETON;

#endif /* JERICHO_JER_EVENTS_H */
