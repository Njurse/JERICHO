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
// How hard the spring pulls an angle back to level, and how much velocity is
// kept per frame. Together: how loose the car looks. Lower spring = slower
// recovery; lower damping = more wobble before it settles.
#define CD2_KNOCK_SPRING		220	// /4096 - fraction of the remaining angle
#define CD2_KNOCK_DAMPING		4030	// /4096 - velocity kept each frame

// The lift springs back a little softer, so the body comes down after the impact
// rather than snapping to the ground with it.
#define CD2_KNOCK_LIFT_SPRING		150
#define CD2_KNOCK_LIFT_DAMPING		4000

// Nothing may knock beyond this, however hard the hit: a car spinning on its
// side would look broken rather than hit.
#define CD2_KNOCK_MAX_PITCH		900	// ~79 degrees
#define CD2_KNOCK_MAX_ROLL		700
#define CD2_KNOCK_MAX_YAW		400

// The lift an impulse may ask for, and its ceiling. Small numbers: this is a
// nudge to clear geometry, not a jump.
#define CD2_KNOCK_MAX_LIFT		70
#define CD2_KNOCK_LIFT_PER_HIT		26	// world units of lift per unit of impulse

// Impulse scale, so callers can pass whatever their own units are and say how
// much it was:
//   cd2KnockAdd(carId, howHard >> CD2_KNOCK_HARD_SHIFT, ...)
#define CD2_KNOCK_HARD_SHIFT		16	// collision howHard -> impulse

typedef struct CD2_KNOCK_STATE
{
	int pitch, roll, yaw;		// current visual offsets
	int vpitch, vroll, vyaw;	// their velocities
	int lift;			// current lift
	int vlift;			// its velocity
} CD2_KNOCK_STATE;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
// Add an impulse. Any of these may be negative; 0 means "no knock on that axis".
// The lift is clamped to CD2_KNOCK_MAX_LIFT and never goes below zero.
void cd2KnockAdd(int carId, int pitch, int roll, int yaw, int lift);

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
