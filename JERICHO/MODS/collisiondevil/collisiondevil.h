// COLLISIONDEVIL — new handling model (Paradise-oriented).
// Drift is a self-limiting state: mild rear grip loss + short initiation yaw
// that fades and is countered once the car is already rotating.
// No continuous thruster, no inverted damping, hard floors on grip and yaw rate.

#ifndef COLLISIONDEVIL_H
#define COLLISIONDEVIL_H

// --------------------------- tunables ------------------------------------

#define CD_STEER_MIN            16
#define CD_DRIFT_MIN_SPEED      8

// How fast the drift intensity approaches its target (higher = slower)
#define CD_INTENSITY_LERP       6

// Rear grip reduction at full intensity (4096 = 100 %).
// 1200 → rear stays ≥ ~0.70×. Enough to step out, still self-righting.
#define CD_GRIP_DROP_FRAC       1200
#define CD_GRIP_FLOOR           1800   // absolute minimum rearFS scale (≈0.44× of original)

// Yaw initiation scale (derived from twistRateY). Small on purpose.
#define CD_YAW_INIT_SCALE       6
// How quickly the initiation fades once the car is rotating (higher = faster fade)
#define CD_YAW_FADE_SHIFT       3
// Hard cap on added yaw torque (fraction of twistRateY)
#define CD_YAW_MAX_FRAC         900    // ~0.22× twistRateY

// Mild damping soften only (>>5 ≈ 0.03, same order as stock -avel*128/4096)
#define CD_DAMP_SHIFT           5

// Brake soften while *not* in deep drift. Once intensity is high we leave
// brake closer to stock so speed bleeds and the slide cannot feed forever.
#define CD_BRAKE_SOFTEN         3600   // ~0.88×

// Speed gates
#define CD_YAW_LOW_SPEED        6
#define CD_YAW_HIGH_SPEED       28
#define CD_YAW_HIGH_FLOOR       800    // almost no initiation at high speed

// Handbrake multipliers (sharper but still finite)
#define CD_HB_GRIP_MULT         1.4f
#define CD_HB_YAW_MULT          1.5f
#define CD_HB_DURATION_FRAMES   18     // after this many frames the handbrake bonus decays

// Visual (unchanged philosophy: render-only)
#define CD_VISUAL_LERP          2
#define CD_DRAMA_ROLL_SHIFT     3
#define CD_DRAMA_PITCH_BASE     7
#define CD_DRAMA_REF_SPEED      90
#define CD_SLIP_SHIFT           10
#define CD_BODY_MAX_ROLL        42
#define CD_BODY_MAX_PITCH       28
#define CD_FOV_PULL_SCRZ        190
#define CD_CRASH_SPIN_SHIFT     2

enum {
    CD_PRESET_BURNOUT = 0,
    CD_PRESET_PARADISE = 1,
    CD_PRESET_CUSTOM = 2
};

// --------------------------- state ---------------------------------------

typedef struct COLLISIONDEVIL_CONFIG {
    int enabled;
    int aggression;
    int eagerness;
    int drama;
    int boost;
    int preset;
    int fovPull;
} COLLISIONDEVIL_CONFIG;

typedef struct COLLISIONDEVIL_DRIFT {
    int intensity;          // 0..4096 smoothed target
    int direction;          // +1 / -1
    int handbrake;          // 1 if currently handbrake-driven
    int hbFrames;           // frames handbrake has been held this slide
    int lastYawRate;        // previous frame |angularVelocity[1]| for fade
} COLLISIONDEVIL_DRIFT;

typedef struct COLLISIONDEVIL_VISUAL {
    int roll;
    int pitch;
    int yaw;
} COLLISIONDEVIL_VISUAL;

extern COLLISIONDEVIL_CONFIG gCdCfg;

#endif