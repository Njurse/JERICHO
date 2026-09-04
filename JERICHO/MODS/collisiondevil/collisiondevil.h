// COLLISIONDEVIL — arcade handling overhaul for REDRIVER2 (JERICHO module).
//
// A compiled-in JERICHO deep mod. It listens to the five car-handling events
// added by this package (JER_EVENT_CAR_*) and re-shapes the stock physics
// with a handful of macro sliders:
//
//   Arcade Aggression  0..1  -> engine force (acceleration + emergent top speed)
//   Drift Eagerness    0..1  -> rear-grip drop + yaw kick (brake-tap powerslides)
//   Boost Intensity    0..1  -> extra engine multiplier
//   Visual Drama       0..1  -> render-only body roll/pitch/yaw + FOV pull
//
// Golden rule: DERIVE, DON'T INVENT. Every number scales an existing chassis
// stat (car_cosmetics[].powerRatio / traction / twistRateY, cp->wheel_angle,
// the frontFS/rearFS friction scales) — there are no per-car tuning tables.
//
// The visual pass never touches physics: drama rotates a render-only copy of
// cp->hd.drawCarMat (see the JER_EVENT_CAR_DRAW call site in cars.c).

#ifndef COLLISIONDEVIL_H
#define COLLISIONDEVIL_H

// --------------------------- tunables ------------------------------------

// Drift entry: minimum steering magnitude (PSX angle units) to count as
// "steering into" a brake-tap drift, and the minimum speed (cp->hd.speed,
// world units) before a drift can start.
#define CD_STEER_MIN        48
#define CD_DRIFT_MIN_SPEED  12

// Drift grip blend: exponential approach divisor (higher = slower in/out).
#define CD_BLEND_LERP       4

// Visual smoothing divisor (higher = slower body roll/pitch/yaw settle).
#define CD_VISUAL_LERP      2

// Grip drop: fraction of the REAR friction removed at full eagerness
// (4096 = 100%). 2253/4096 ~= 0.55 -> rear grip ~0.45x — the manifesto's
// "Drift Grip = base_friction * 0.45".
#define CD_GRIP_DROP_FRAC   2458

// Yaw kick: yaw angular-acceleration added per frame during a drift, derived
// from the car's yaw inertia (twistRateY). kick = twistRateY * SCALE / 2 at
// full drift blend + full eagerness. 16 -> ~8000 units/frame for a mid car.
#define CD_YAW_KICK_SCALE   28

// Visual drama magnitudes (PSX angle units; 4096 = 360 deg, ~11.4 units/deg).
#define CD_DRAMA_ROLL_SHIFT 2     // |wheel_angle| * drama >> 2 (~8 deg max)
#define CD_DRAMA_PITCH_BASE 60    // nose up/down angle at full drama + speed
#define CD_DRAMA_YAW_SHIFT  4     // drift blend * drama >> 4 (~14 deg max)
#define CD_DRAMA_REF_SPEED  60    // speed (world units) at which drama saturates
// Side-slip -> body roll gain: lateral velocity (fixed point) >> shift gives
// a PSX-angle-unit lean. 9 => ~10 m/s side slip leans ~7 deg.
#define CD_SLIP_SHIFT       9

// Brake force scale while braking/reversing (4096 = stock, 2458 ~= 0.6x) —
// a gentler brake that lets the car rotate into a drift instead of stopping.
#define CD_BRAKE_SOFTEN     2458

// FOV pull: scr_z reduction (from gCameraDefaultScrZ = 256) at full pull+speed.
#define CD_FOV_PULL_SCRZ    80

// presets
enum
{
	CD_PRESET_BURNOUT = 0,   // high aggression + eagerness, mid drama
	CD_PRESET_PARADISE = 1,  // mid aggression, high drama
	CD_PRESET_CUSTOM = 2
};

// --------------------------- state ---------------------------------------

typedef struct COLLISIONDEVIL_CONFIG
{
	int enabled;
	int aggression;   // 0..100
	int eagerness;    // 0..100
	int drama;        // 0..100
	int boost;        // 0..100
	int preset;       // CD_PRESET_*
	int fovPull;      // 0..100
} COLLISIONDEVIL_CONFIG;

typedef struct COLLISIONDEVIL_DRIFT
{
	int active;
	int direction;    // +1 / -1
	int blend;        // 0..4096
} COLLISIONDEVIL_DRIFT;

typedef struct COLLISIONDEVIL_VISUAL
{
	int roll;         // smoothed PSX angle units
	int pitch;
	int yaw;
} COLLISIONDEVIL_VISUAL;

extern COLLISIONDEVIL_CONFIG gCdCfg;

#endif /* COLLISIONDEVIL_H */
