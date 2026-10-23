/*
 * fulltwist.h -- FULL TWIST: severe off-center collision tumbles.
 *
 * Shared types and the small cross-file surface. The module is deliberately
 * split by concern:
 *
 *   fulltwist.c          entry point + event wiring
 *   fulltwist_config.c   defaults, presets, ini load/save
 *   fulltwist_menu.c     pause menu
 *   fulltwist_hull.c     mesh hull + convex footprint + 3D scenery test
 *   fulltwist_impulse.c  contact/lever/torque/impulse math
 *
 * Engine dependency stays the same as before: it reads the CAR_DATA handed to
 * it by JER_EVENT_COLLISION / JER_EVENT_CAR_STEP and writes only
 * st.n.angularVelocity, st.n.linearVelocity and hd.aacc. It never edits
 * CAR_COSMETICS, handlingType, GlobalTimeStep or bcollide.c.
 */

#ifndef FULLTWIST_H
#define FULLTWIST_H

#include "jericho.h"
#include "dr2limits.h"		/* MAX_CARS, MAX_CAR_RESIDENT_MODELS */

#define FT_MOD_ID          "fulltwist"
#define FT_VERSION         "0.3.2"

/* The impact value at which the tumble rate reaches its configured maximum.
 * Measured scenery impacts sit around 22k (nudge), 78k-114k (ordinary) and
 * 290k (violent); 350k leaves headroom for the worst car-vs-car hits. */
#define FT_FULL_IMPACT     350000

/* The integrator clamps angular velocity to +/-0x800000. Keep our writes
 * inside the same envelope so a collision cannot push a component past the
 * value the engine will clamp to on the next step. */
#define FT_AV_CLAMP        0x800000

/* A level-start reset can zero the car's angular velocity after the first
 * CAR_STEP. The headless test kick therefore waits this long before firing. */
#define FT_TEST_KICK_DELAY 120

/* Debug kick launch, independent of the contact-impulse slider. */
#define FT_TEST_LAUNCH     380000

/* Rollover test: the car is rotated 90 degrees, held for one second at the
 * 30 Hz sim rate, then given a lateral impulse 8x the old test shove. */
#define FT_TEST_ROLLOVER_WAIT   30
#define FT_TEST_ROLLOVER_FORCE  1200000		/* 8 * the old 150000 shove */
#define FT_TEST_ROLLOVER_RATE   7200000		/* 8 * the old 900000 rate */

/* One cached hull per resident model slot. The mesh itself is not stored --
 * only the copied vertices (already placed in the where-local frame) and the
 * 2D convex footprint used by the contact queries. 512 is above the largest
 * car body seen in the game; a larger model is truncated and logged. */
#define FT_HULL_MAX_VERTS  512

enum
{
	FT_PRESET_CINEMATIC = 0,
	FT_PRESET_ARCADE = 1,
	FT_PRESET_OFF = 2,
	FT_PRESET_CUSTOM = 3,
	/* Appended after CUSTOM so an existing ini's `preset = 3` keeps meaning
	 * Custom; the menu cycle skips Custom explicitly. */
	FT_PRESET_FREE_PHYSICS = 4
};

typedef struct FULLTWIST_CONFIG
{
	int enabled;
	int intensity;		/* 0..200 % */
	int minImpact;		/* raw howHard/strikeVel threshold */
	int offCenter;		/* 0..200 % lever offset threshold */
	int maxRate;		/* raw angular-velocity impulse at full severity */
	int groundKick;		/* 0..200 % of the impact tumble, once per landing */
	int wallSnag;		/* 0..100 % tangential velocity removed on a wall hit */
	int damping;		/* 0..100 % extra roll/pitch damping while tumbling */
	int window;			/* frames the tumble response stays active */
	int trip;			/* 0..100 % of the impulse redirected into roll/pitch */
	int launch;			/* 0..200 % upward pop on a severe grounded hit */
	int rolloverDamping;	/* 0..100 % extra damping while on the side/roof */
	int rolloverCatch;		/* 0..100 % uprighting torque on the ground */
	int spinRetention;		/* 0..100 % offset of the stock angular damping */
	int freeSpin;			/* 1 = remove the stock angular damping while active */
	int angularDamping;		/* 0..128: stock damping used for free-spin cars */
	int cancelExtraDamping;	/* 0..100 % of the lift (-avel/8) damping to cancel */
	int keepSpinAtRest;		/* 1 = keep hd.speed off zero while free-spinning */
	int freeSpinWindow;		/* frames the free-spin state stays active */
	int holdCamera;			/* 1 = freeze the chase camera heading while tumbling */
	int hullContact;		/* 1 = use the hull contact for the scenery lever */
	int hullFootprint;		/* 1 = use the projected hull footprint as a gate */
	int sceneryMode;		/* 0 = heuristic assist, 1 = hull-led assist */
	int wallBounce;			/* 0..100 % wall restitution (100 = stock) */
	int carCarImpulse;		/* 0..100 % of the stock car-car impulse */
	int carCarLaunch;		/* 0..200 % car-car contact impulse */
	int carCarTumble;		/* 0..200 % car-car roll/pitch assist */
	int bodyKick;			/* 0..200 body-landing assist strength */
	int scenery3d;		/* 1 = 3D hull/scenery override of the 2D check */
	int playerOnly;		/* 1 = only player car / cars hit by the player */
	int debugLog;		/* 1 = recorder lines in the JERICHO log */
	int testKick;		/* 1 = apply one deterministic kick at level start */
	int testCrash;		/* 1 = spawn an oncoming car and collide head-on */
	int testSmash;		/* 1 = spawn a car beside the player and shove them together */
	int testRollover;	/* 1 = apply the rollover test impulse */
	int testRolloverSide;	/* -1/1: side the rollover test pushes toward */
	int preset;
} FULLTWIST_CONFIG;

typedef struct FULLTWIST_HULL
{
	const void* model;		/* gCarCleanModelPtr[slot] this was built from */
	int slot;				/* resident model slot */
	int count;				/* copied mesh vertices (<= FT_HULL_MAX_VERTS) */
	int ready;				/* 1 = a complete hull is cached for this model */
	int modelVerts;			/* mdl->num_vertices when built */
	int hullCount;			/* points in the 2D convex footprint */
	SVECTOR verts[FT_HULL_MAX_VERTS];		/* where-local, about -cog */
	SVECTOR footprint[FT_HULL_MAX_VERTS];	/* XZ convex hull, y = 0 */
	int minX, maxX;
	int minY, maxY;
	int minZ, maxZ;
	int hx, hy, hz;			/* half extents about the body centre */
	int cogX, cogY, cogZ;	/* cosmetics cog used for the placement */
} FULLTWIST_HULL;

typedef struct FULLTWIST_CAR
{
	unsigned int latchFrame;	/* frame whose strongest impact is already applied */
	int latchHard;				/* howHard of that strongest impact */
	int tumbleFrames;			/* remaining frames of the response window */
	int settleFrames;			/* frames since the tumble started */
	int tumbleStrength;			/* 0..4096 severity of the strongest impact */
	int tumbleRollShare;		/* -4096..4096 local-Z direction */
	int tumblePitchShare;		/* -4096..4096 local-X direction */
	int tumbleRate;				/* raw rate of the strongest impact */
	int landed;					/* ground kick already used this tumble */
	int airborne;				/* previous step had no compressed wheel */
	int airFrames;				/* consecutive airborne steps */
	int upsideFrames;			/* consecutive steps with m[1][1] < 100 */
	int tumbled;				/* fulltwist has started a tumble on this car */
	int bodyContact;			/* lowest hull vertex is on the road */
	unsigned int bodyContactFrame;	/* frame of the last body-contact impulse */
	unsigned int wallSnagFrame;	/* frame of the last world-wall snag */
	int freeSpinFrames;			/* remaining frames of the free-spin state */
	unsigned int wallHitFrame;	/* frame of the last scenery hit (restitution) */
	int contactMask;			/* last wheel susCompression mask */
	int logTick;				/* log throttle */
	int rolloverCount;			/* up-vector sign crossings in the current test */
	int upSign;				/* sign of the last up-vector sample */
	int peakAv;				/* largest |angularVelocity| component seen */
} FULLTWIST_CAR;

typedef struct FULLTWIST_STATE
{
	JERICHO_CONTEXT* ctx;
	unsigned int frame;
	int collisionsThisFrame;
	int testKickPending;
	int testKickDone;
	int testKickScheduled;
	unsigned int testKickAtFrame;
	int testCrashPending;
	int testCrashDone;
	int testCrashScheduled;
	unsigned int testCrashAtFrame;
	int testSmashPending;
	int testSmashDone;
	int testSmashScheduled;
	unsigned int testSmashAtFrame;
	int testRolloverPending;
	int testRolloverDone;
	int testRolloverScheduled;
	unsigned int testRolloverAtFrame;
	int testRolloverHolding;
	unsigned int testRolloverImpulseAtFrame;
	FULLTWIST_CONFIG cfg;
	int hullsDirty;			/* a resident model changed since the last build */
	int hullsBuilt;			/* the level-start prebuild has run */
	FULLTWIST_CAR cars[MAX_CARS];
	FULLTWIST_HULL hulls[MAX_CAR_RESIDENT_MODELS];
} FULLTWIST_STATE;

extern FULLTWIST_STATE gFullTwist;

#define FT_LOG(...) \
	do { if (gFullTwist.ctx != NULL) gFullTwist.ctx->jer_log(gFullTwist.ctx, __VA_ARGS__); } while (0)

/* fulltwist_config.c */
void ftClampConfig(void);
void ftLoadConfig(void);
void ftApplyPreset(void);
void ftSaveConfig(void);

/* fulltwist_menu.c */
void ftRegisterMenu(void);

/* fulltwist_impulse.c */
int ftIsPlayerCar(const CAR_DATA* cp);
int ftIsTumbleCar(const CAR_DATA* cp);
int ftWheelMask(const CAR_DATA* cp);
int ftFreeSpinActive(const CAR_DATA* cp);
void ftAddLocalRate(CAR_DATA* cp, int roll, int pitch);
void ftGetAxes(const CAR_DATA* cp, int* ax, int* ay, int* az);
int ftDotLocal(int x, int y, int z, const int* axis);
void ftHandleCar(CAR_DATA* car, CAR_DATA* other,
				 const VECTOR* point, const VECTOR* normal,
				 int hard, int impulseSign);

/* fulltwist_test.c */
void ftTestTick(CAR_DATA* cp);

/* fulltwist_hull.c */
FULLTWIST_HULL* ftHullForCar(const CAR_DATA* cp);
void ftHullBuildAll(void);
int ftHullSupport(const FULLTWIST_HULL* hull, int nx, int ny, int nz, SVECTOR* out);
int ftHullContact(const FULLTWIST_HULL* hull, int px, int py, int pz,
				  int nx, int ny, int nz, SVECTOR* out);
int ftHullLowest(const FULLTWIST_HULL* hull, const CAR_DATA* cp,
				 VECTOR* out, int* groundY, SVECTOR* localOut);
int ftHullWorldSpan(const FULLTWIST_HULL* hull, const CAR_DATA* cp,
					int* minY, int* maxY);
int ftHullContactWorld(const FULLTWIST_HULL* hull, const CAR_DATA* cp,
					   int px, int py, int pz, int nx, int ny, int nz,
					   VECTOR* out);
int ftHullFootprintOverlapsBuilding(const FULLTWIST_HULL* hull, const CAR_DATA* cp,
									const BUILDING_BOX* building, int margin);
void ftOnSceneryCollision(JER_ARGS_SCENERY_COLLISION* args);

#endif /* FULLTWIST_H */
