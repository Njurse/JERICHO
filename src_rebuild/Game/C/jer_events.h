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
 * vertex copy (SVECTOR*) for camber/toe, or set hide=1 to skip drawing this
 * wheel entirely (e.g. a totaled wreck with the wheels blown off). */
typedef struct JER_ARGS_DRAW_WHEEL
{
	int carId;
	int wheelnum;
	void* verts;
	int numVerts;
	int hide;		/* out: set 1 to skip drawing this wheel */
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

/* JER_EVENT_GET_PHYSICS_PARAMS — query: per-car physics tuning, fired once at
 * the top of StepOneCar (wheelforces.c) before gravity/suspension/angular
 * settling run. All fields are in/out, prefilled with the stock constants so
 * a module edits in place; no handler = stock. */
typedef struct JER_ARGS_PHYSICS_PARAMS
{
	void* car;			/* CAR_DATA* */
	int gravity;		/* in/out: vertical accel (stock -7456; D1 -10922) */
	int angularDamping;	/* in/out: angular velocity damping factor (stock 128;
						   higher settles pitch/roll/yaw faster) */
	int springRate;		/* in/out: suspension spring constant (stock 230) */
	int springDamping;	/* in/out: suspension damper constant (stock 100) */
} JER_ARGS_PHYSICS_PARAMS;

/* JER_EVENT_GET_BUDDHA — query: clamp totalDamage below the totaled
 * threshold for the player car (Buddha mode); 0 = disabled. */
typedef struct JER_ARGS_QUERY_FLAG
{
	int result;
} JER_ARGS_QUERY_FLAG;

/* JER_EVENT_GET_WALL_RESTITUTION — query: restitution scale for a car hitting
 * building/scenery geometry, fired in CarBuildingCollision (bcollide.c) once
 * a hit is detected. result is 0..4096 (4096 = stock bounce); a module that
 * wants TMB-style "walls absorb momentum" (hard stop, little/no bounce)
 * returns a low value. No handler = stock. */
typedef struct JER_ARGS_WALL_RESTITUTION
{
	void* car;		/* CAR_DATA* that hit the building */
	int result;		/* out: restitution scale 0..4096, default 4096 */
} JER_ARGS_WALL_RESTITUTION;

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
	D2PL_ITEM_JOYSTICK,	/* right-stick orbit/look control on/off */
	D2PL_ITEM_FOOT_DIST,	/* on-foot camera pull-in distance */
	D2PL_ITEM_FOOT_LAT,	/* on-foot shoulder offset */
	D2PL_ITEM_FOOT_HEIGHT,	/* on-foot camera height (below the body) */
	D2PL_ITEM_CAR_DIST,	/* in-car pull-in */
	D2PL_ITEM_CAR_LAT,	/* in-car lateral (% of the car's bbox width) */
	D2PL_ITEM_CAR_HEIGHT,	/* in-car camera height offset (taller cars) */
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

/* JER_EVENT_MAP — fullscreen map hooks. The engine fires it twice while the
 * in-game map (gShowMap) is up; modules return JER_RESULT_STOP on the INPUT
 * action to claim the pad (e.g. a teleport cursor), and draw their cursor on
 * the DRAWN action (after the map, so it lands on top).
 *   action JER_MAP_ACTION_INPUT: value = current pad bits (caller-dependent:
 *     Pads[0].direct in overmap.c, dirnew in pause.c — handlers should read
 *     Pads[0] directly instead of relying on it);
 *   action JER_MAP_ACTION_DRAWN: value = 0 (draw into the display buffer). */
#define JER_MAP_ACTION_INPUT 0
#define JER_MAP_ACTION_DRAWN 1

typedef struct JER_ARGS_MAP
{
	int action;	/* JER_MAP_ACTION_* */
	void* result;	/* unused */
	int value;	/* pad bits on INPUT */
} JER_ARGS_MAP;

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

/* JER_EVENT_PED_MOVE — fired inside AnimatePed() for the player ped,
 * right before its position is advanced by pPed->speed. This is the ONE
 * point where a module write to pPed->speed survives: ProcessTannerPad
 * AND the runner state (PedUserRunner) re-arm speed = MAXRUNSPEED every
 * frame, and the position advance uses the final value. A module may
 * scale the run speed here (e.g. analog deflection magnitude, lerped for
 * stand-start momentum). */
typedef struct JER_ARGS_PED_MOVE
{
	void* ped;		/* LPPEDESTRIAN whose position is about to move */
	int padId;		/* the pad driving this ped (>= 0 = player) */
} JER_ARGS_PED_MOVE;

/* JER_EVENT_PED_POSE — fired in DrawTanner() between SetupTannerSkeleton
 * and newRotateBones for the player ped (TANNER_MODEL + padId >= 0).
 * Modules mutate the per-bone ROTATION here: each Skel[i].pvRotation
 * points into the raw motion frame bytes, and newRotateBones reads them
 * immediately after the hook — a write made here is the last word on that
 * frame's bone rotation (PED_SKELETON phase-0 remains the POSITION
 * channel). Use the jer_anim helpers (jer_anim.h) to resolve bones. */
typedef struct JER_ARGS_PED_POSE
{
	void* ped;		/* LPPEDESTRIAN being drawn */
	void* skel;		/* BONE* Skel[] (index with JER_LIMB_*) */
} JER_ARGS_PED_POSE;

/* JER_EVENT_FRONTEND — fired in the frontend's take-a-ride city-select
 * confirm (CutSceneCitySelectScreen, after GameLevel = currCity). A module
 * may set defer = 1 to take over the start (its own menu runs over the
 * frozen frontend); otherwise it may rewrite gameLevel/gameType/numPlayers
 * and the stock flow continues. */
typedef struct JER_ARGS_FRONTEND
{
	int gameLevel;		/* in/out: the pending level */
	int gameType;		/* in/out: the pending gametype */
	int numPlayers;		/* in/out */
	int defer;		/* out: 1 = the module handles the start itself */
} JER_ARGS_FRONTEND;

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

/* JER_EVENT_CAR_PAD — fired inside ProcessCarPad right before the stock
 * face-button assignment (handbrake/wheelspin/thrust). A module may take
 * over the car's pedal semantics: set handled = 1 and write cp->thrust,
 * cp->handbrake and cp->wheelspin directly — the stock binds are then
 * SKIPPED for that car this frame, so a physical button never double-fires
 * its original action. pad may also be rewritten in/out for pure input
 * transforms. Engine steering (wheel_angle) is not affected by handled.
 * live = 1 only while this is genuine live player input (not AI/lead/
 * cutscene/replay pad, and not the clamped locked-car state). */
typedef struct JER_ARGS_CAR_PAD
{
	void* car;		/* CAR_DATA* being controlled */
	int pad;		/* in/out: CAR_PAD_* action bits for this car */
	int padSteer;		/* in/out: analog steering input (-128..127) */
	int useAnalogue;	/* in/out: 1 = analog steering (analogue stick) */
	int live;		/* in: 1 = live player pad (override allowed) */
	int handled;		/* out: set 1 to skip the stock pedal assignment */
} JER_ARGS_CAR_PAD;

/* JER_EVENT_CAR_GEARBOX — fired inside GetEngineRevs (gamesnd.c) once per
 * active car per frame, right before the gear is selected from wheel speed.
 * Fields default to the stock gear-table row for this car (units match the
 * stock table: ws = wheel_speed>>11). A module may rewrite the four gears to
 * retune the rev model (e.g. shorter gears, and a tall top gear whose ratio
 * levels the pitch at the car's top speed instead of revving away) and/or set
 * revCeiling to clamp the returned revs. wheelSpeed/thrust/type are inputs. */
typedef struct JER_ARGS_CAR_GEARBOX
{
	void* car;		/* CAR_DATA* */
	int type;		/* in: stock gear-table row (0/1) in use */
	int wheelSpeed;		/* in: scaled wheel speed ws (wheel_speed>>11) */
	int thrust;		/* in: accel state (cp->thrust) */
	int lowIdleWs[4];	/* in/out per gear: downshift point while coasting */
	int lowWs[4];		/* in/out per gear: downshift point while accelerating */
	int hiWs[4];		/* in/out per gear: upshift point */
	int ratioAc[4];		/* in/out: revs per ws while accelerating */
	int ratioIdle[4];	/* in/out: revs per ws while coasting */
	int revCeiling;		/* in/out: clamp returned revs when > 0 */
} JER_ARGS_CAR_GEARBOX;

/* JER_EVENT_CAR_ENGINE_SOUND — fired in SoundTasks (gamesnd.c) once per
 * player's car, right before the rev and idle engine channels are placed.
 * pitch values are the SPU pitches about to be used (4096 = normal speed);
 * volume is in PSX volume units (negative; -10000 = silent). A module may
 * scale/offset them for engine-audio tuning. */
typedef struct JER_ARGS_CAR_ENGINE_SOUND
{
	void* car;		/* CAR_DATA* of the player's car */
	int playerId;		/* in: player index (0/1) driving this car */
	int revPitch;		/* in/out: rev channel pitch */
	int revVolume;		/* in/out: rev channel volume */
	int idlePitch;		/* in/out: idle channel pitch */
	int idleVolume;		/* in/out: idle channel volume */
} JER_ARGS_CAR_ENGINE_SOUND;

/* JER_EVENT_CAR_REVS — fired at the top of ControlCarRevs (gamesnd.c) once
 * per active car per frame. A module may change how fast the engine pitch
 * slews toward its target revs: revRise is the maximum the pitch can climb
 * per frame, revDrop the maximum it can fall (stock file constants:
 * maxrevrise = 1600, maxrevdrop = 1440). No handler = exactly stock. */
typedef struct JER_ARGS_CAR_REVS
{
	void* car;		/* CAR_DATA* whose revs are being slewed */
	int revRise;		/* in/out: max revs gained per frame */
	int revDrop;		/* in/out: max revs lost per frame */
} JER_ARGS_CAR_REVS;

/* JER_EVENT_CAR_ENGINE — fired at the end of ProcessCarPad, after the engine
 * force (thrust) and steering (wheel_angle) are computed. A module scales
 * them in place to overclock acceleration / widen steering. handbrake/
 * wheelspin are read-only input state. */
typedef struct JER_ARGS_CAR_ENGINE
{
	void* car;		/* CAR_DATA* */
	int thrust;		/* in/out: engine force (fixed point) */
	int wheelAngle;		/* in/out: steering angle (fixed point) */
	int handbrake;		/* in: 1 while the handbrake is held */
	int wheelspin;		/* in: 1 while burnout/wheelspin is active */
	int speed;		/* in: cp->hd.speed */
	int wheelSpeed;		/* in: cp->hd.wheel_speed */
} JER_ARGS_CAR_ENGINE;

/* JER_EVENT_CAR_FRICTION — fired at the end of GetFrictionScalesDriver1.
 * A module scales the front/rear friction scales in place (e.g. drop rear
 * grip to induce oversteer/drift). */
typedef struct JER_ARGS_CAR_FRICTION
{
	void* car;		/* CAR_DATA* */
	int frontFS;		/* in/out: front friction scale */
	int rearFS;		/* in/out: rear friction scale */
} JER_ARGS_CAR_FRICTION;

/* JER_EVENT_CAR_STEP — fired at the top of StepOneCar once per car per
 * physics frame. Observation only; a module reads state for drift detection
 * and G-force capture. */
typedef struct JER_ARGS_CAR_STEP
{
	void* car;		/* CAR_DATA* */
	int speed;		/* in: cp->hd.speed */
	int velX;		/* in: linear velocity X (fixed point) */
	int velZ;		/* in: linear velocity Z (fixed point) */
	int avelY;		/* in: angular velocity Y (fixed point) */
} JER_ARGS_CAR_STEP;

/* JER_EVENT_CAR_TORQUE — fired after ConvertTorqueToAngularAcceleration.
 * A module adds yaw torque (angular acceleration about Y) for drift kicks. */
typedef struct JER_ARGS_CAR_TORQUE
{
	void* car;		/* CAR_DATA* */
	int yawTorque;		/* in/out: added to cp->hd.aacc[1] */
} JER_ARGS_CAR_TORQUE;

/* JER_EVENT_CAR_DRAW — fired in DrawCar before the body matrix is built.
 * A module rotates the render-only matrix (pitch/roll/yaw) without touching
 * physics or collision. */
typedef struct JER_ARGS_CAR_DRAW
{
	void* car;		/* CAR_DATA* */
	void* matrix;		/* MATRIX* — render matrix to rotate in place */
	int view;		/* in: camera view */
} JER_ARGS_CAR_DRAW;

/* JER_EVENT_CAR_DRAW_COLOR — fired in DrawCarObject before the body model is
 * plotted. A module may render the body flat:
 *   flatBlack = 1 -> flat solid black (a totaled / burned-out wreck);
 *   tintR/G/B >= 0 -> a flat body colour at full brightness, e.g. an icy cyan
 *                     for a frozen car (overrides the model's shading).
 * Tint is ignored when flatBlack is set. */
typedef struct JER_ARGS_CAR_DRAW_COLOR
{
	void* car;		/* CAR_DATA* */
	int flatBlack;		/* out: 1 = draw the body flat black */
	int tintR;		/* out: -1 = unset; else a flat body colour 0..255 */
	int tintG;
	int tintB;
} JER_ARGS_CAR_DRAW_COLOR;

/* JER_EVENT_LEVEL_LAUNCH — fired at the end of State_GameStart after the
 * pending level/gametype/player count/mission number are finalised but
 * before the level is loaded. All fields are in/out: a module rewrites them
 * to redirect the launch (e.g. bump a take-a-ride mission number into the
 * multiplayer-map range so gMultiplayerLevels ends up set). */
typedef struct JER_ARGS_LEVEL_LAUNCH
{
	int gameLevel;		/* in/out: pending level (GameLevel) */
	int gameType;		/* in/out: pending gametype (GAMETYPE) */
	int numPlayers;		/* in/out: player count */
	int missionNumber;	/* in/out: computed mission (gCurrentMissionNumber) */
} JER_ARGS_LEVEL_LAUNCH;

/* JER_EVENT_GET_DAMAGE_SCALE — query: scale (0..4096; 4096 = stock) applied to
 * the damage a car takes from hitting solid scenery (buildings/walls), fired in
 * DamageCar (bcollide.c) just before ApplyDamage. A module returning a lower
 * value softens scenery hits. No handler = stock (4096). */
typedef struct JER_ARGS_DAMAGE_SCALE
{
	void* car;	/* CAR_DATA* */
	int result;	/* in/out: damage scale, 4096 = stock */
} JER_ARGS_DAMAGE_SCALE;

/* JER_EVENT_CAR_VS_CAR — fired in DamageCar3D (bcollide.c) when two cars
 * collide, right before ApplyDamage. `value` is the stock damage this car would
 * take; `playerValue` is what a player-controlled car would take for the SAME
 * impact (the non-player stock branch applies a harsher multiplier, which is
 * what traffic/civ cars get). A module may set `value` — e.g. give an owned
 * opponent the player model, or scale all car-to-car damage. No handler =
 * stock. */
typedef struct JER_ARGS_CAR_VS_CAR
{
	void* car;		/* CAR_DATA* taking the damage */
	void* other;		/* CAR_DATA* it collided with */
	int strikeVel;		/* impact velocity term (post-scale) */
	int region;		/* 0..5 damage region */
	int value;		/* in/out: damage to apply */
	int playerValue;	/* in: damage a player car would take */
} JER_ARGS_CAR_VS_CAR;

/* JER_EVENT_DRAW_MAP — fired from overmap.c while the overhead map (and the
 * fullscreen map) is being drawn, right after the player's own blip. Plot extra
 * markers with DrawTargetBlip(pos, r, g, b, flags) using the same `flags` value
 * so they land in the right place; `fullscreen` distinguishes the two. */
typedef struct JER_ARGS_DRAW_MAP
{
	int flags;		/* flags the player blip was drawn with */
	int fullscreen;		/* 1 = fullscreen map, 0 = overhead map */
} JER_ARGS_DRAW_MAP;

/* JER_EVENT_EXPLOSION_SPAWN — an explosion slot was just armed in
 * AddExplosion (job_fx.c). The engine seeds the stock values first; a module
 * may override ANY of them and/or rewrite `type`. A custom `type` id
 * (>= 1000) is free for modules to define; the engine has no size defaults for
 * an unknown type, so a module MUST fill in speed/hscale/rscale for one.
 *   fxId        : module profile id (out; the engine does not interpret it)
 *   type        : in/out ExplosionType — rewrite to a stock bang
 *                 (BIG_BANG / LITTLE_BANG / HEY_MOMMA) to keep the stock
 *                 sound + collision branches
 *   speed       : time units added per frame (bigger = shorter life)
 *   hscale      : vertical mesh scale (1024 small / 4096 big / 16384 huge)
 *   rscale      : radial mesh scale
 *   tintR/G/B   : -1 = stock colour, else a 0..255 per-channel tint
 *   yawRate     : extra spin, PSX angle units per frame (0 = none)
 *   collide     : 1 = stock car push/damage, 0 = visual only
 *   colScale    : fixed point collision-box scale (4096 = stock) */
typedef struct JER_ARGS_EXPLOSION_SPAWN
{
	void* pos;	/* VECTOR* (world) — read only */
	int type;	/* in/out: ExplosionType */
	int fxId;	/* out: module profile id */
	int speed;	/* in/out */
	int hscale;	/* in/out */
	int rscale;	/* in/out */
	int tintR;	/* in/out: -1 = stock colour */
	int tintG;
	int tintB;
	int yawRate;	/* in/out */
	int collide;	/* in/out */
	int colScale;	/* in/out */
} JER_ARGS_EXPLOSION_SPAWN;

/* JER_EVENT_EXPLOSION_DRAW — fired once per explosion per frame from
 * DrawAllExplosions (job_fx.c), with the live camera matrices set. A module
 * may tint/spin the stock hemisphere, or set `override` to 1 and draw its own
 * effect (the engine then skips its stock mesh for this explosion). */
typedef struct JER_ARGS_EXPLOSION_DRAW
{
	int time;	/* 0..0xfff life (in) */
	void* pos;	/* VECTOR* (world) — read only */
	int hscale;	/* in/out */
	int rscale;	/* in/out */
	int tintR;	/* in/out: -1 = stock colour */
	int tintG;
	int tintB;
	int yaw;	/* in/out: extra spin applied this draw (PSX units) */
	int fxId;	/* in: module profile id */
	int override;	/* out: 1 = module drew its own, skip the stock mesh */
} JER_ARGS_EXPLOSION_DRAW;

/* JER_EVENT_EXPLOSION_COLLIDE — query fired in ExplosionCollisionCheck
 * (bomberman.c) for every (car, explosion) pair. `result` defaults to 1 (the
 * stock push/damage); colScale scales the collision box (fixed point,
 * 4096 = stock). Set result = 0 for a visual-only explosion, or a smaller
 * colScale for a tighter blast. */
typedef struct JER_ARGS_EXPLOSION_COLLIDE
{
	void* car;	/* CAR_DATA* being tested */
	void* explosion;	/* EXOBJECT* */
	int result;	/* in/out: 1 = apply the stock push/damage */
	int colScale;	/* in/out: collision-box scale, 4096 = stock */
} JER_ARGS_EXPLOSION_COLLIDE;

#endif /* JERICHO_JER_EVENTS_H */
