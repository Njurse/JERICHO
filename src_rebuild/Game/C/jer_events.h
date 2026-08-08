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
	JER_PAUSE_SANDBOX_OPEN			/* handler returns JER_RESULT_STOP if opened */
};

typedef struct JER_ARGS_PAUSE_MENU
{
	int action;
	void* result;	/* out: const char* for the GET_*_TEXT actions */
} JER_ARGS_PAUSE_MENU;

/* JER_EVENT_CAMERA — fired at the end of InitCamera once camera_position is
 * set; modules adjust the position/angle in place (stock = no handler). */
typedef struct JER_ARGS_CAMERA
{
	void* player;		/* PLAYER* whose camera was just updated */
	void* cameraPosition;	/* VECTOR* (camera_position) — modules may move it */
	void* cameraAngle;	/* SVECTOR* (camera_angle) — modules may re-aim it */
	int cameraView;		/* the player's camera view */
} JER_ARGS_CAMERA;

#endif /* JERICHO_JER_EVENTS_H */
