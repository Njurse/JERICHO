// COMBAT D2 — Twisted Metal: Black style arcade handling for REDRIVER2.
//
// A compiled-in JERICHO deep mod. Unlike COLLISIONDEVIL (which scales the
// stock wheel/suspension sim), Combat D2 replaces the *horizontal* motion
// with a point-mass rigid body:
//
//   * velocity is controlled directly from throttle (no engine/gear/suspension)
//   * yaw is controlled directly from steering (target yaw rate, works at zero
//     speed — rotate in place); steering authority never drops during a slide
//   * the Tight Turn (Triangle / handbrake) is a separate acute-pivot
//     authority — an intentional "cheat" against realism for combat positioning
//   * brakes are fast and proportional (strong scrub at speed, smooth taper)
//   * lateral grip is high and only slips a little, so skids are brief and
//     never a loss-of-control spiral
//   * walls absorb momentum (hard stop) instead of bouncing
//
// Feel is engineered, not simulated: responsive and forgiving (TMB's north
// star), with weight/control differentiated per vehicle from the chassis
// stats so a light car is instant and a truck is slow but heavy.
//
// Vertical motion (gravity + ground lift, cp->hd.acc[1]) and roll/pitch
// (cp->hd.aacc[0]/[2]) are left to the stock code so the car still rides the
// terrain. Collision impulses are stock (mass-based "push"); our grip + drag
// make the recovery forgiving, which is the TM2 feel.

#ifndef COMBATD2_H
#define COMBATD2_H

// --------------------------- units --------------------------------------
//
// Two unit systems meet here:
//   * speed  : "game speed units" == world-units per frame == cp->hd.speed.
//              linearVelocity is that × 4096 (fixed point, ONE = 4096).
//   * yaw    : PSX angle units, 4096 == 360° (the game's native angle).
//
// The TM2 reference gives SI values (topSpeed 20 m/s, handling 120°/s, …).
// Those are quoted in the comments below; the active numbers are the game-unit
// equivalents, folded to per-frame (the sim is frame-locked) so they can be
// tuned by play exactly like the stock physics.

// angularVelocity[1] per unit of yaw rate (PSX-units/frame). Derived from the
// quaternion integrator in GlobalTimeStep (handling.c): AV = avel >> 13 is the
// half-angle in fixed point, so avel = yawRate × π × 8192.
#define CD2_AV_PER_UNIT     25736

// wheel_angle magnitude treated as full-lock steer (stock regular max = 352).
#define CD2_STEER_MAX       352

// Tight Turn (TMB): an acute forced pivot on its OWN yaw authority — NOT a
// steering amplification. Pivot direction comes from the (latched) steer
// input; the pivot bleeds a little forward speed so holding gas produces a
// short drift-slide instead of a dead stop, and at low speed it spins
// nearly in place. The trigger is selectable so it stays rebindable:
//   CD2_TIGHT_INPUT_HANDBRAKE = Triangle (engine's handbrake),
//   CD2_TIGHT_INPUT_WHEELSPIN = Circle  (engine's wheelspin/burnout bit),
//   CD2_TIGHT_INPUT_OFF       = disabled.
// (Physical buttons themselves are remapped by the engine's config.ini.)
#define CD2_TIGHT_RATE          50    // pivot yaw, PSX-units/frame (x control/4096)
#define CD2_TIGHT_ANG_MULT      1.6      // yaw angular step multiplier during a pivot
#define CD2_TIGHT_BLEED         96     // fp/frame: horizontal speed lost while pivoting (~2.3%)
#define CD2_SLIDE_BLEED         480    // fp/frame: extra scrub while tight-sliding with gas (~11.7%)
#define CD2_SLIDE_ACCEL_FRAC    1024   // fp: accel fraction allowed during a tight slide (0.25x)
#define CD2_TIGHT_STEER_MIN     16     // |wheel_angle| that (re)latches a pivot direction

enum
{
	CD2_TIGHT_INPUT_HANDBRAKE = 0,
	CD2_TIGHT_INPUT_WHEELSPIN = 1,
	CD2_TIGHT_INPUT_OFF = 2
};

#define CD2_TIGHT_ENABLED_DEFAULT   1
#define CD2_TIGHT_STRENGTH_DEFAULT  100   // 0..100 pivot authority
#define CD2_TIGHT_INPUT_DEFAULT     CD2_TIGHT_INPUT_HANDBRAKE

// TMB Classic in-car face-button layout (default ON):
//   Square-position = Gas, Circle = Brake, and the other primary face button
//   (Cross) = Tight Turn. Triangle is left unbound for the car (in TMB it is
//   rear-view / nothing combat relevant here); get-in/get-out stays on the
//   dedicated L3 exit so it is not hijacked by the remap. On foot, ped
//   controls are untouched.
#define CD2_TMB_BUTTONS_DEFAULT     1
// Some pads label the LEFT face button "X" (Xbox X / PS Square). tmb_tight
// picks which PHYSICAL button carries the Tight Turn when the TMB layout is
// on: 0 = Cross/bottom (PS "X", Xbox A), 1 = Square/left (Xbox X). Gas is on
// the other one.
#define CD2_TMB_TIGHT_DEFAULT       0

// Iconic TMB slide: while the Tight Turn is held AND the car is fast enough,
// lateral traction is suspended (~CD2_SLIDE_GRIP_FRAC of normal grip) so the
// car keeps moving along its ORIGINAL velocity vector while the pivot rotates
// the heading underneath it — you steer through the slide instead of the car
// arcing. Releasing the button (or dropping below CD2_SLIDE_MIN_SPEED) hooks
// the car back up.
#define CD2_SLIDE_MIN_SPEED  50     // speed units/frame (below: low-speed spin)
#define CD2_SLIDE_GRIP_FRAC  64     // fp: grip multiplier during the slide
                                    // (64/4096 ≈ 1.5% of normal grip)

// Friction inside a slide is intentionally tiny: the pivot bleed was dropped to
// ~2.3%/frame and rolling drag is skipped entirely while sliding, so an off-gas
// sharp turn carries its speed (TMB "continues moving in the original velocity
// direction").

// Hook-up sharpness: the first CD2_HOOKUP_FRAMES after a slide ends use a
// strong fixed grip (below 4096 so it can never overshoot), snapping the
// velocity back onto the heading like TMB's clean, crisp recovery.
#define CD2_HOOKUP_FRAMES    2
#define CD2_HOOKUP_GRIP      3800   // fp (~93% of lateral velocity removed/frame)

// --------------------------- default stats -------------------------------
//
// Speed defaults (world-units/frame):
#define CD2_TOP_SPEED       360   // speed-units/frame (~top gear; highway limit is 138)
#define CD2_REVERSE_SPEED   360    // ≈ 33% of top
#define CD2_ACCEL           2     // speed-units/frame² (0→top in ~1s)
#define CD2_BRAKE           8    // PEAK brake decel, speed-units/frame², applied
                                  // proportionally (strong at speed, taper near 0)
#define CD2_BRAKE_FLOOR     1024  // fp: fraction of peak brake kept at standstill
                                  // (1024/4096 = 25%) so stopping is never asymptotic
#define CD2_REVERSE_ACCEL_FRAC 2048 // fp: reverse accel = brake x this (2048/4096 = 0.5)
#define CD2_DRAG            36    // fixed point /frame: 48/4096 ≈ 1.2%/frame (coast)

// Yaw defaults (PSX-units/frame; 4096 = 360°):
#define CD2_HANDLING        25    // max yaw rate  (≈ 120°/s at 30 fps)
#define CD2_ANGULAR_ACCEL   50    // yaw accel toward target (≈ 360°/s²)

// Grip default (fixed point /frame; 1800/4096 ≈ 0.44/frame ≈ 13 s⁻¹ — TMB keeps
// skids short and sparse, so base grip is high and only drops a little):
#define CD2_GRIP            1800
#define CD2_SLIP_REDUCTION  3     // /10 → max 30% grip drop at full slip
                                   // (skids are brief, never a loss-of-control spiral)

// Visual: lateral velocity (speed units) → body roll (PSX angle units).
#define CD2_ROLL_GAIN       2     // roll = -latVel * gain, clamped below
#define CD2_BODY_MAX_ROLL   24    // ~2° lean (TMB: weight felt, not exaggerated)
#define CD2_ROLL_LERP       2     // exponential settle divisor

// Camera FOV pull (same trick as COLLISIONDEVIL): scr_z reduction at speed.
#define CD2_FOV_REF_SPEED   120

// Wall restitution scale (0..4096; 4096 = stock bounce). 700/4096 ≈ 17% kept,
// i.e. walls absorb ~83% of the car's momentum on impact — TMB's hard stop,
// with just enough carry to keep wall-scraping from feeling frozen.
#define CD2_WALL_KEEP       700

// Per-vehicle variety references (typical values in this data set):
#define CD2_REF_PW          4096   // typical powerRatio/mass ratio (4096/4096)
#define CD2_REF_MASS        4096   // typical car mass (fixed-point scale)
#define CD2_FOV_PULL_SCRZ   60

// presets
enum
{
	CD2_PRESET_DEFAULT = 0,   // TM2 baseline
	CD2_PRESET_TURBO = 1,     // faster + stiffer
	CD2_PRESET_DRIFTY = 2,    // looser + more agile
	CD2_PRESET_CUSTOM = 3,    // sliders were hand-tuned
	CD2_PRESET_COUNT = 4
};

// --------------------------- state ---------------------------------------

typedef struct CD2_STATS
{
	int topSpeed;      // speed-units/frame
	int reverseSpeed;  // speed-units/frame
	int accel;         // speed-units/frame²
	int brake;         // speed-units/frame²
	int drag;          // fixed point /frame
	int handling;      // yaw, PSX-units/frame
	int angularAccel;  // yaw, PSX-units/frame²
	int grip;          // fixed point /frame
	int control;       // steering/pivot authority, fixed point (4096 = average car)
} CD2_STATS;

typedef struct CD2_CONFIG
{
	int enabled;
	int topSpeed;
	int accel;
	int brake;         // peak brake decel (speed-units/frame²)
	int handling;
	int grip;
	int preset;        // CD2_PRESET_*
	int fovPull;       // 0..100
	int tightTurn;     // 0/1 master toggle
	int tightStrength; // 0..100 pivot authority
	int tightInput;    // CD2_TIGHT_INPUT_*
	int tmbButtons;    // 0/1: TMB in-car button layout (Square gas, Circle brake)
	int tmbTight;      // 0/1: which face button is Tight Turn (0=Cross/bottom, 1=Square/left)
	int debugLog;      // 0/1: log player-car input/velocity telemetry to REDRIVER2.log
} CD2_CONFIG;

typedef struct CD2_CAR
{
	int yawRate;       // current yaw rate, PSX-units/frame (signed)
	int slip;          // lateral velocity, speed units (signed), for visuals
	int roll;          // smoothed body roll, PSX angle units
	int throttle;      // +1/-1/0 raw throttle captured at CAR_STEP (see note)
	int pivotDir;      // latched tight-turn direction +1/-1/0
	int slideTicks;    // hook-up frames remaining after a traction-suspended slide
} CD2_CAR;

extern CD2_CONFIG gCd2Cfg;

#endif /* COMBATD2_H */
