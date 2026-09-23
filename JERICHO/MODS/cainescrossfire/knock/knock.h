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
 * is then carried there by that velocity, which decays each frame, and once the
 * velocity is spent the angle eases straight back to level. Setting the angle
 * instead is what made the first version read as stiff: it was a step, not a
 * movement. A spring/damper pair was tried first and read as WOBBLING - a spring
 * between an angle and zero is an oscillator - which is why the motion is these
 * two phases and not a spring. The two rates are the dial for how firm the car
 * looks; the four-way detail is on the tunables below.
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
#define CD2_KNOCK_DECAY		600	// /4096 - velocity kept per frame in phase 1
#define CD2_KNOCK_SETTLE	1700	// /4096 - fraction of the angle eased out per frame

// A hard speed change stiffens the return. The settle above is a fixed fraction per
// frame; these add to it in proportion to |delta|, the car's own speed change this step,
// so a violent event comes back with authority and a small one cannot snap the body
// around. The cap is deliberately well short of 4096: at 4096 the whole angle is removed
// in a single frame, which is the snap this file has already been bitten by once.
#define CD2_KNOCK_SETTLE_PER_FORCE	1664
// Sized against the IMPULSE that started the knock (recorded in the state by cd2KnockAdd),
// not against the car's speed change. That was tried first and abandoned on measurement:
// hd.speed does not move during a turbo engagement, so the delta was exactly 0 through
// the very event the rule exists for, and the rate stayed pinned at its base. The impulse
// is always known, always nonzero, and is already "how hard was that" in one number.
// WATCH THE SCALE, because this is easy to kill by accident: extra is
// force * CD2_KNOCK_SETTLE_PER_FORCE with NO division, so the ceiling lands at
// force = CD2_KNOCK_SETTLE_EXTRA_MAX / CD2_KNOCK_SETTLE_PER_FORCE. At the 1664 below that
// is an impulse of 0.36 - i.e. EVERY knock saturates, the rate is a flat base + cap, and
// the "harder hits return harder" gradation this describes is OFF. It was written to
// reach its cap at an impulse of about 50, which is what 24 was for.
//
// At 24 with a cap of 1200: a full-ceiling knock - CD2_KNOCK_IMPULSE_TO(57) - lands the
// rate at its stiffest, so a light graze barely changes it while anything near the
// ceiling gets the stiff return. Measured on a turbo engagement against the cap and base.
#define CD2_KNOCK_SETTLE_EXTRA_MAX	600	// base 1700 + this = 2300, about 56% per frame

// ...and the settle is bounded: whatever the curve has left is snapped away after
// this many frames (9 = 0.3s at 30Hz). An exponential approaches zero forever, so
// without a deadline "eased back" would take about a second and a half to look
// finished.
//
// Jaret: the knock should be AGGRESSIVE and then settle AGGRESSIVELY. Both rates moved
// the same way: a lower decay kills the velocity sooner, so the angle arrives as a
// punch rather than a push, and a higher settle sheds the angle faster, so it is
// already still by the time you look. The impulse sizing above absorbed the first
// change on its own - that is exactly what CD2_KNOCK_IMPULSE_TO is for, and why
// changing a rate does not silently soften a hit.
#define CD2_KNOCK_SETTLE_FRAMES	9

// What counts as "arrived" when the settle closes out: below this the angle is not worth
// a frame, above it the deadline keeps easing rather than wiping. In PSX angle units, so
// 2 is about a fifth of a degree.
#define CD2_KNOCK_IDLE(v)		((v) > -2 && (v) < 2)

// The impulse that exactly carries an axis to its ceiling:
//   displacement = impulse / (1 - decay/4096)
// A caller wanting a knock that ARRIVES at the limit (a wheelie, a hard hit) uses
// this and adds whatever margin it wants; a caller wanting a light one uses less.
// Sizing an impulse by hand is how the first attempt ended up with a wheelie that
// barely lifted when the rate changed.
#define CD2_KNOCK_IMPULSE_TO(maxAngle)	((maxAngle) * (4096 - CD2_KNOCK_DECAY) / 4096)

// The lift settles on a softer pair, so the body comes down after the impact
// rather than snapping to the ground with it.
#define CD2_KNOCK_LIFT_DECAY		1500
#define CD2_KNOCK_LIFT_SETTLE		2600	// very fast: the body slams back down

// Nothing may knock beyond this, however hard the hit: a car spinning on its
// side would look broken rather than hit.
// The ceilings, and they are deliberately tiny - about 5 degrees on the pitch,
// less on the others. A knock is not a rotation that happens to be fast: a real
// lean of the model reads as the car tipping over, and past a few degrees it looks
// like a flip however brief it is. The motion is carried by the TRANSFORM instead
// (the shift along the car below, the lift, and the pivot), with the angle as the
// accent on top - which is what makes it read as the car rocking rather than
// rotating on the spot.
#define CD2_KNOCK_MAX_PITCH		57	// ~5 degrees
#define CD2_KNOCK_MAX_ROLL		45	// ~4 degrees
#define CD2_KNOCK_MAX_YAW		30

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

// How far from the model's origin the car's axles are, for the pivot below. A
// wheelie turns about the REAR axle, a stoppie about the front one, so the nose (or
// the tail) rises instead of the whole car spinning around a point in its middle.
#define CD2_KNOCK_PIVOT_DIST		240
#define CD2_KNOCK_SHIFT_DECAY		3200
#define CD2_KNOCK_SHIFT_SETTLE		2300

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
	int force;			// how hard the impulse that started this was, for the rate
} CD2_KNOCK_STATE;

// ---------------------------------------------------------------------------
// The transform
// ---------------------------------------------------------------------------
// Everything that moves a car's RENDER matrix goes through one compositor, so the
// pivot, the shift and the rotations exist once rather than once per feature.
//
// THE CONVENTION, stated once here so no reader has to re-derive it from the
// matrix maths in knock.c:
//
//   pitch   +  the FRONT lifts (nose up);  -  the REAR lifts (nose down)
//   roll    +  ...about the car's own forward axle (see the note below)
//   yaw     +  ...about the car's own up
//   bob     +  up, - down
//   shift   +  FORWARD along the car's own nose
//
// ...and all three angles turn about the CAR'S OWN axes, not the world's. That is
// the whole point: a knock has to read the same whichever way the car is pointing.
// The engine's rotation helpers pre-multiply (they rotate about the fixed world
// axis), which is only correct when the car faces world +Z - using them is the bug
// that made "positive pitch" dive the nose facing one way and lift it facing the
// other. See cd2VisualApply for the arithmetic and the measured symptoms.
//
// An offset is a plain sum: each layer fills one of these and they are added
// together before anything is applied. Angles are PSX units (4096 = full turn) and
// the translations are world units.
typedef struct CD2_VISUAL_OFFSET
{
	int pitch, roll, yaw;		// angles on the three axes
	int bob;			// vertical translation (+ up, - down)
	int shift;			// along the car's own forward axis (+ = forward)
} CD2_VISUAL_OFFSET;

// Apply a composed offset to a render matrix: translate, then rotate. Render-only
// by construction - it is handed the car-draw matrix and nothing else, so it has
// no route to the handling model.
void cd2VisualApply(void* matrix, const CD2_VISUAL_OFFSET* o);

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
// physics. A thin wrapper over cd2VisualApply.
void cd2KnockApply(void* matrix, int carId);

// The state, for a dump or a caller that wants to know.
const CD2_KNOCK_STATE* cd2KnockOf(int carId);

void cd2KnockReset(int carId);
void cd2KnockResetAll(void);

#endif /* KNOCK_H */
