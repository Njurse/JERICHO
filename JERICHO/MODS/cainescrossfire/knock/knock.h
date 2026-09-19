#ifndef KNOCK_H
#define KNOCK_H

/*
 * knock/knock.h -- the vehicle knock: bucking and rocking a car for effect,
 * without touching the physics.
 *
 * A knock is a rotation (and a small lift) applied to the car's RENDER matrix
 * only, so the whole car - wheels included - rolls with it while the handling
 * model carries on exactly as before. Hitting something should shove and sound
 * like it hurt without the physics having to agree about it.
 *
 * The lift is there so a car knocked up onto its nose does not sink into whatever
 * it is standing on: the knock can raise the body slightly, never lower it.
 *
 * HOW IT MOVES - an impulse sets a VELOCITY, never an angle directly. The angle
 * is then carried there by a spring, so it eases in, overshoots a little and
 * settles. Setting the angle instead is what made the first version read as
 * stiff: it was a step, not a movement. Everything here is smoothed the same way
 * the car's own body roll is (jer_lerp_int), and the spring/damper pair is the
 * dial for how loose the car looks.
 *
 * Angles are PSX angle units (4096 = a full turn), lengths are world units, and
 * the velocities are per frame at 30Hz.
 */

// ---------------------------------------------------------------------------
// tunables
// ---------------------------------------------------------------------------
// The motion is in two phases, and neither of them oscillates:
//
//   1. the impulse phase - a knock's velocity carries the angle up, decaying each
//      frame, so a big impulse reaches the ceiling fast (that is the wheelie: the
//      front comes up hard) and a small one barely moves.
//   2. the settle phase - once the velocity is spent, the angle interpolates
//      straight back to level.
//
// A spring/damper pair was tried first and read as wobbling: a spring between an
// angle and zero is an oscillator, so the car rocked back and forth instead of
// doing one firm movement and settling. These two rates are what replaced it.
#define CD2_KNOCK_DECAY		1704	// /4096 - velocity kept per frame in phase 1
#define CD2_KNOCK_SETTLE	1100	// /4096 - fraction of the angle eased out per frame

// ...and the settle is bounded: whatever the curve has left is snapped away after
// this many frames (15 = 0.5s at 30Hz). An exponential approaches zero forever, so
// without a deadline "eased back" would take about a second and a half to look
// finished.
#define CD2_KNOCK_SETTLE_FRAMES	15

// The impulse that exactly carries an axis to its ceiling:
//   displacement = impulse / (1 - decay/4096)
// A caller wanting a knock that ARRIVES at the limit (a wheelie, a hard hit) uses
// this and adds whatever margin it wants; a caller wanting a light one uses less.
// Sizing an impulse by hand is how the first attempt ended up with a wheelie that
// barely lifted when the rate changed.
#define CD2_KNOCK_IMPULSE_TO(maxAngle)	((maxAngle) * (4096 - CD2_KNOCK_DECAY) / 4096)

// The lift settles on a softer pair, so the body comes down after the impact
// rather than snapping to the ground with it.
#define CD2_KNOCK_LIFT_DECAY		2200
#define CD2_KNOCK_LIFT_SETTLE		800

// Nothing may knock beyond this, however hard the hit: a car spinning on its
// side would look broken rather than hit.
#define CD2_KNOCK_MAX_PITCH		240	// ~21 degrees - the ceiling the turbo wheelie uses
#define CD2_KNOCK_MAX_ROLL		180
#define CD2_KNOCK_MAX_YAW		110

// The lift an impulse may ask for, and its ceiling. Small numbers: this is a
// nudge to clear geometry, not a jump.
#define CD2_KNOCK_MAX_LIFT		70
#define CD2_KNOCK_LIFT_PER_HIT		26	// world units of lift per unit of impulse

// ---------------------------------------------------------------------------
// The weight shift
// ---------------------------------------------------------------------------
// A knock may also SHIFT the body along the car, which is what makes it read as
// weight moving rather than the car simply rotating on the spot: a wheelie puts the
// weight back over the rear wheels, a hard frontal hit throws it forward, and a
// boost leaning back looks like the car squatting. It is a translation of the whole
// body (and, once the wheel hook carries a position, the wheels with it), clamped
// so the car never visibly separates from its own wheels.
#define CD2_KNOCK_MAX_SHIFT		55	// world units, fore or aft
#define CD2_KNOCK_SHIFT_DECAY		2200
#define CD2_KNOCK_SHIFT_SETTLE		800

// Impulse scale, so callers can pass whatever their own units are and say how
// much it was:
//   cd2KnockAdd(carId, howHard >> CD2_KNOCK_HARD_SHIFT, ...)
#define CD2_KNOCK_HARD_SHIFT		16	// collision howHard -> impulse

// Below this an impulse is not worth having: cars rub against each other and
// against walls constantly, and a knock for every graze is a permanent buzz
// rather than a reaction. And however often they touch, one car may only be
// knocked every this many frames.
#define CD2_KNOCK_MIN_IMPULSE		3
#define CD2_KNOCK_COOLDOWN		6	// 0.2s at 30Hz

typedef struct CD2_KNOCK_STATE
{
	int pitch, roll, yaw;		// current visual offsets
	int vpitch, vroll, vyaw;	// their velocities
	int lift;			// current lift
	int vlift;			// its velocity
	int shift;			// current weight shift along the car (+ = forward)
	int vshift;			// its velocity
	int settleFrames;		// frames left before the settle is snapped shut
} CD2_KNOCK_STATE;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
// Add an impulse. Any of these may be negative; 0 means "no knock on that axis".
// The lift is clamped to CD2_KNOCK_MAX_LIFT and never goes below zero; the shift
// is clamped to +/-CD2_KNOCK_MAX_SHIFT and + is forward along the car.
void cd2KnockAdd(int carId, int pitch, int roll, int yaw, int lift, int shift);

// Spring everything back toward level. Once per frame per car.
void cd2KnockTick(int carId);

// Apply the current knock to a car's render matrix (rotate, and lift the body).
// Called from the car-draw path, which is render-only, so this cannot reach the
// physics.
void cd2KnockApply(void* matrix, int carId);

// The state, for a dump or a caller that wants to know.
const CD2_KNOCK_STATE* cd2KnockOf(int carId);

void cd2KnockReset(int carId);
void cd2KnockResetAll(void);

#endif /* KNOCK_H */
