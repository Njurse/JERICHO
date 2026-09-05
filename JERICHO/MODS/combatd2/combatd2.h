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


// ==============================================================
// UNITS & ENGINE-WRITE CONSTANTS
// ==============================================================

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


// ==============================================================
// INPUT: TMB LAYOUT + TIGHT TURN TRIGGER (who presses what)
// ==============================================================

// Tight Turn (TMB): an acute forced pivot DERIVED from the car's normal turn
// authority — pivot rate = normal handling x CD2_TIGHT_MULT, so it scales
// with the car's own steering stat/control instead of a fixed extra spin.
// Pivot direction comes from the (latched) steer input; the pivot bleeds a
// little forward speed so holding gas produces a short drift-slide instead
// of a dead stop, and at low speed it spins nearly in place. The trigger is
// selectable so it stays rebindable:
//   CD2_TIGHT_INPUT_HANDBRAKE = Triangle (engine's handbrake),
//   CD2_TIGHT_INPUT_WHEELSPIN = Circle  (engine's wheelspin/burnout bit),
//   CD2_TIGHT_INPUT_OFF       = disabled.
// (Physical buttons themselves are remapped by the engine's config.ini.)
#define CD2_TIGHT_MULT          8192   // fp: pivot = normal handling x this (2.0x)
#define CD2_TIGHT_ANG_MULT      1.6    // yaw angular step multiplier during a pivot
#define CD2_TIGHT_BLEED         96     // fp/frame: horizontal speed lost while pivoting (~2.3%)
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

// TMB Classic in-car face-button layout (default ON). It is implemented as
// a JER_EVENT_CAR_PAD override, NOT a rebinding of physical buttons: the
// module writes the car's pedal state itself and the engine skips its stock
// face-button assignment, so the original binds never double-fire.
//   Square-position = Gas, Circle = Brake, and the other primary face button
//   (Cross) = Tight Turn. Triangle is left unbound for the car (in TMB it is
//   rear-view / nothing combat relevant here); get-in/get-out stays on the
//   dedicated L3 exit. On foot, ped controls are untouched.
#define CD2_TMB_BUTTONS_DEFAULT     1
// Some pads label the LEFT face button "X" (Xbox X / PS Square). tmb_tight
// picks which PHYSICAL button carries the Tight Turn when the TMB layout is
// on: 0 = Cross/bottom (PS "X", Xbox A), 1 = Square/left (Xbox X). Gas is on
// the other one.
#define CD2_TMB_TIGHT_DEFAULT       0
// thrust magnitude the override writes for gas (- for brake); only the SIGN
// matters to the combatd2 torque model, but the magnitude feeds the stock
// wheel/pitch pass, so it should sit near the stock accel force (~power*4915).
#define CD2_TMB_THRUST              4915


// ================ GRIP, SLIDE & RECOVERY ========================

// Grip default (fixed point /frame; 1800/4096 ≈ 0.44/frame ≈ 13 s⁻¹ — TMB keeps
// skids short and sparse, so base grip is high and only drops a little):
#define CD2_GRIP            400
#define CD2_SLIP_REDUCTION  6     // /10 → max 60% grip drop at full slip
                                    // (never past 6: a drop over 100% makes
                                    // grip negative = instant blow-up)

// Iconic TMB slide: while the Tight Turn is held AND the car is fast enough,
// traction is only PARTIALLY released — the car keeps real friction
// (CD2_SLIDE_GRIP_FRAC of full grip), so the skid itself scrubs speed and
// arcs under the pivot like a stock friction skid. The heavy grip overrides
// are walked back: combatd2 nudges the grip multiplier; it does not switch
// the skids off.
#define CD2_SLIDE_MIN_SPEED  150     // speed units/frame (below: low-speed spin)
#define CD2_SLIDE_GRIP_FRAC  900     // fp: grip multiplier during the slide
                                    // (900/4096 ≈ 22% of normal grip)
// Big-skid ice lock: once the slide is sliding SIDEWAYS harder than
// CD2_SKID_LOCK_LAT, the car stops arcing toward the nose entirely and keeps
// its ORIGINAL momentum line — grip drops to CD2_SKID_LOCK_GRIP so the
// velocity is never scrubbed toward the rotated heading (pure TMB ice drift).
#define CD2_SKID_LOCK_LAT    40      // speed units: lateral velocity threshold
#define CD2_SKID_LOCK_GRIP   96      // fp: grip multiplier once locked (~2.3%)


#define CD2_SLIDE_BLEED         32     // fp/frame: bleed while tight-sliding (~0.8%).
                                        // Tight Turn is NOT a brake: on ice the car
                                        // keeps its momentum and sheds speed slowly
                                        // (~half speed every 1.5s), forgiving to steer.
#define CD2_SLIDE_ACCEL_FRAC    1024   // fp: accel fraction allowed during a tight slide (0.25x)
// Velocity recovery after a slide ends: the old "hookup" scrubbed the lateral
// component at ~93%/frame, which killed the car's speed whenever the pivot had
// rotated the heading away from the travel direction (it stopped dead). TMB
// releases instead CARRY the momentum: for CD2_RECOVER_FRAMES after letting
// off the Tight Turn, the velocity is rotated back onto the heading while its
// magnitude is conserved, then grip returns GRADUALLY over CD2_GRIP_RAMP_FRAMES
// (never a snap back to full grip).
#define CD2_RECOVER_FRAMES    8     // frames of magnitude-preserving recovery
#define CD2_RECOVER_RATE      256   // fp: fraction of the heading gap closed per
                                    // recovery frame (256/4096 = 6.25%)
#define CD2_GRIP_RAMP_FRAMES  10    // frames to ramp grip back up after recovery

// --------------------------- default stats -------------------------------
//
// Speed defaults (world-units/frame):
#define CD2_TOP_SPEED       25   // speed-units/frame (raw slider value; the
                                    // physics top is CD2_SPEED_SCALE x this)
#define CD2_SPEED_SCALE     2048  // fp: effective top speed multiplier (0.5x =
                                    // ~130 at the default slider value)
#define CD2_REVERSE_SPEED   -CD2_TOP_SPEED    // The vehicles drive just as fast backwards in Twisted Metal as they do forwards
#define CD2_ACCEL           4     // speed-units/frame² (0→top in ~1s)
#define CD2_BRAKE           10    // PEAK brake decel, speed-units/frame², applied
                                  // proportionally (strong at speed, taper near 0)
#define CD2_BRAKE_FLOOR     1024  // fp: fraction of peak brake kept at standstill
                                  // (1024/4096 = 25%) so stopping is never asymptotic
#define CD2_REVERSE_ACCEL_FRAC 2048 // fp: reverse accel = brake x this (2048/4096 = 0.5)
#define CD2_DRAG            36    // fixed point /frame: 48/4096 ≈ 1.2%/frame (coast)

// Yaw defaults (PSX-units/frame; 4096 = 360°):
#define CD2_HANDLING        20    // max yaw rate  (≈ 120°/s at 30 fps)
#define CD2_ANGULAR_ACCEL   15    // yaw accel toward target (≈ 360°/s²)
#define CD2_YAW_SPEED_FALLOFF 0   // fp: TM2 speed-sensitive yaw falloff. 0 = off;
                                  // ~1200-1600 turns down yaw authority as speed
                                  // rises so the car can't spin out at top speed.
#define CD2_YAW_DECAY       2048  // fp: centering step multiplier while the yaw
                                  // returns to center (2048 = 2x) — the car stops
                                  // spinning promptly instead of carrying rotation




// ==============================================================
// SUSPENSION, GRAVITY & ANGULAR SETTLING  (JER_EVENT_GET_PHYSICS_PARAMS)
// ==============================================================
// These feed the engine's StepOneCar via JER_EVENT_GET_PHYSICS_PARAMS. Stock
// constants are: gravity -7456, angular damping 128, spring rate 230, spring
// damping 100 (wheelforces.c). Higher angularDamping settles pitch/roll/yaw
// faster (less floaty). Higher springRate/springDamping = tighter, less
// bouncy suspension. Values here are compile-time tunables.
#define CD2_GRAVITY           -7456   // vertical accel (D1 used -10922)
#define CD2_ANGULAR_DAMPING   256     // stock 128; 256 = 2x faster angular settle
#define CD2_SPRING_RATE       320     // stock 230 (stiffer)
#define CD2_SPRING_DAMPING    160     // stock 100 (less bounce)

// ==============================================================
// VISUALS: BODY ROLL, CAMERA & FOV
// ==============================================================

// Visual: lateral velocity (speed units) → body roll (PSX angle units).
#define CD2_ROLL_GAIN       160     // roll = -latVel * gain, clamped below
#define CD2_BODY_MAX_ROLL   370    // ~2° lean (TMB: weight felt, not exaggerated)
#define CD2_ROLL_LERP       4     // exponential settle divisor

// Camera FOV pull (same trick as COLLISIONDEVIL): scr_z reduction at speed.
#define CD2_FOV_PULL_SCRZ   60
#define CD2_FOV_REF_SPEED   120

// TMB-style chase framing (applied every frame to the player's main chase
// cam, cameraView 0): after the engine places the camera it is nudged CLOSER
// to the car on the ground plane and eased LOWER toward the car's base. Both
// are relative fractions of the gap (fixed point 4096) so they can never
// overshoot into the car; 0 disables that axis. 4096 = keep the stock frame.
#define CD2_CAM_PULL        350   // fp: fraction of the gap to the car closed
#define CD2_CAM_LOW         0      // fp: fraction of the height gap closed (0 = keep stock height)


// ==============================================================
// COLLISION: WALLS ABSORB MOMENTUM
// ==============================================================

// Wall collision: combatd2 returns this as the wall restitution, but the
// engine now applies it to the INTO-wall (normal) component only: 0 = walls
// fully absorb the impact and the car keeps scraping tangentially along the
// wall (TM2 "collision forgiveness"), 4096 = stock outward bounce + spin.
#define CD2_WALL_KEEP       256

// ----------------------- engine audio / gearbox ---------------------------
//
// combatd2 drives speed directly, so the stock rev model (gamesnd geard)
// keeps climbing with cp->hd.wheel_speed and would wind far past a sane
// redline. While a player drives we retune the gear table via
// JER_EVENT_CAR_GEARBOX: short, snappy lower gears, and a TALL top gear whose
// ratio levels the pitch at the car's combatd2 top speed — revs then cap at
// CD2_REV_CEILING no matter how fast the point-mass model goes.
//   ws (rev-model speed) = car speed x CD2_WS_PER_SPEED/4096
//   (wheel_speed is ~4096x speed and GetEngineRevs shifts it >> 11 => 2x)
// Gear boundaries are fractions of the car's per-vehicle top speed:
//   gear0..1 | gear1..2 | gear2..3 top out at those fractions of top speed
//   (the LAST gear tops out at 1.0 = top speed), and each lower gear hits
//   CD2_GEAR_SHIFT_REVS just before the upshift so shifts sound punchy.
#define CD2_GEAR_AUTO         1     // 0/1: retune player cars' gear tables
#define CD2_WS_PER_SPEED      8192/5  // fp: ws per 1.0 speed unit (8192/4096 = 2x)
#define CD2_GEAR_1_FRAC       500   // fp: gear0 tops here (≈ 12% of top speed)
#define CD2_GEAR_2_FRAC       1100  // fp: gear1 tops here (≈ 27%)
#define CD2_GEAR_3_FRAC       1750  // fp: gear2 tops here (≈ 43%)
#define CD2_GEAR_SHIFT_REVS   13500 // revs at the top of gears 0..2 (pitch peak)
#define CD2_REV_CEILING       15000 // top-gear revs AT top speed; hard rev clamp
#define CD2_GEAR_DOWN_FRAC    3686  // fp: downshift point = prev gear top x this
                                    // (3686/4096 ≈ 0.9; hysteresis vs the upshift)
// Engine pitch is hd.revs, slewed every frame toward a target set by the
// gearbox above (CD2_GEAR_SHIFT_REVS / CD2_REV_CEILING choose WHERE the
// pitch sits). Two independent knobs shape the feel:
//   * CD2_REV_RISE_SCALE / CD2_REV_DROP_SCALE change HOW FAST the pitch gets
//     there. They multiply the engine's per-frame slew caps (stock
//     maxrevrise = 1600, maxrevdrop = 1440 in gamesnd.c, exposed to modules
//     by JER_EVENT_CAR_REVS). 4096 = stock lag. Raise to make the engine
//     snap to redline and fall hard on shifts / let-off (≈ 2-4x =
//     8192-16384); below 4096 is lazier.
//   * The CD2_SND_* knobs adjust the two engine channels (rev + idle) at the
//     mixer (JER_EVENT_CAR_ENGINE_SOUND). Pitch is SPU pitch (4096 = normal
//     playback rate, 8192 = twice as fast). Volume is PSX attenuation:
//     0 = loudest, -10000 = silent; the stock rev channel sits around -5500
//     at full rev and fades toward -10000. To make it LOUDER, move the
//     volume toward 0: add a POSITIVE CD2_SND_*_BIAS and/or raise the gain
//     above 4096 (gain divides the remaining attenuation: 8192 ≈ twice as
//     loud). Clamps keep everything in [-10000, 0].
#define CD2_REV_RISE_SCALE    6144  // fp: rev rise slew multiplier (~1.5x stock)
#define CD2_REV_DROP_SCALE    4096  // fp: rev fall slew multiplier (1.0x stock)
#define CD2_SND_PITCH_SCALE   4096  // fp: rev+idle pitch multiplier (1.0)
#define CD2_SND_PITCH_BIAS    1024  // additive rev-channel pitch (faster spin-up)
#define CD2_SND_IDLE_PITCH_BIAS 512 // additive idle-channel pitch
#define CD2_SND_REV_GAIN      4096  // fp: rev loudness gain (>4096 = louder)
#define CD2_SND_REV_BIAS      1800  // rev volume bias toward 0 (louder)
#define CD2_SND_IDLE_GAIN     4096  // fp: idle loudness gain (>4096 = louder)
#define CD2_SND_IDLE_BIAS     1800  // idle volume bias toward 0 (louder)
#define CD2_SND_MIN_VOL      -10000  // clamp floor for the scaled volumes
#define CD2_SND_MAX_VOL      0      // clamp ceiling


// ==============================================================
// PER-VEHICLE SPREAD (weight/control)
// ==============================================================

// Per-vehicle variety references (typical values in this data set):
#define CD2_REF_PW          4096   // typical powerRatio/mass ratio (4096/4096)
#define CD2_REF_MASS        4096   // typical car mass (fixed-point scale)


// ==============================================================
// PRESETS / CONFIG / STATE
// ==============================================================

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
	int slideTicks;    // recovery frames remaining after a slide ends
} CD2_CAR;

extern CD2_CONFIG gCd2Cfg;

// Exported for the presentation module (combatd2media):
int cd2CarTopSpeed(void* cp);

#endif /* COMBATD2_H */
