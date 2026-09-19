#ifndef CD2_MOTION_H
#define CD2_MOTION_H

/*
 * motion/motion.h -- the procedural visual motion of a car: the layers that make it
 * look like it has weight and a running engine, and the per-class tuning they read.
 *
 * Everything here ends up as a CD2_VISUAL_OFFSET (knock/knock.h) and is applied by
 * cd2VisualApply to the car's RENDER matrix. Nothing in this file, and nothing that
 * reads it, may reach the handling model - the whole system is a costume on the
 * same physics.
 *
 * THE LAYERS
 *   Layer 1  the idle fidget   always on, scaled away by speed. Not a kick: a
 *                              continuous, slightly irregular shudder.
 *   Layer 2  pitch-back        the nose coming up under power, and the bob back
 *                              through neutral when the throttle drops.
 *   (further layers - brake dive, airborne, landing - land here as they come.)
 *
 * A LAYER IS NOT A KICK. The knock (knock/knock.c) is an impulse: it carries an
 * angle to a ceiling and settles, which is what an impact should do. The layers are
 * the opposite - procedural, sines and springs evaluated every frame, so they can be
 * HELD for as long as the car is doing whatever drives them.
 *
 * Every layer is gated on the RACER SET: the player and the AI opponents. Traffic
 * and parked cars keep whatever the physics gave them, because a queue of traffic
 * shuddering in unison looks like a bug rather than a world.
 */

#include "driver2.h"		/* MAX_CAR_RESIDENT_MODELS */
#include "knock/knock.h"	/* CD2_VISUAL_OFFSET - what a layer produces */

// ---------------------------------------------------------------------------
// Vehicle classes
// ---------------------------------------------------------------------------
// Feel is per class, not per car: a light car pitches more and springs faster than
// a truck, which is most of what makes the two read differently. Which class a car
// is in comes from cd2MotionModelClass below, keyed by its MODEL, with a derived
// fallback so a model the table has not been told about still behaves sensibly.
#define CD2_MOTION_LIGHT	0
#define CD2_MOTION_MEDIUM	1
#define CD2_MOTION_HEAVY	2
#define CD2_MOTION_CLASSES	3

// The resident car models a level can hold. A level only loads its own pool, so a
// model INDEX means "the nth model this level loaded" - which is why the table is
// filled from a real run's log (each model's mass and power are printed with it)
// rather than guessed from a list of car names.
#define CD2_MOTION_MODEL_MAX	MAX_CAR_RESIDENT_MODELS

typedef struct CD2_MOTION_CLASS
{
	const char* name;

	// --- Layer 2: pitch-back under power ---
	int pitchMax;		// ceiling for the nose, PSX angle units (4096 = a turn)
	int stiffness;		// /4096 - spring pull per unit of error, per frame
	int damping;		// /4096 - velocity kept per frame
	int overshootPct;	// % past neutral the release is allowed to swing

	// --- Layer 1: the idle fidget ---
	int idlePitch;		// amplitudes at a standstill, PSX angle units
	int idleRoll;
	int idleYaw;
	int idleBob;		// world units of vertical bob
} CD2_MOTION_CLASS;

// The three classes. PROVISIONAL: stiffness and damping are a starting point and
// are calibrated against a real run in the phase that adds Layer 2 - the dump says
// what they actually did, so they get tuned on evidence rather than on vibes.
extern const CD2_MOTION_CLASS cd2MotionClasses[CD2_MOTION_CLASSES];

// Which model is which class. -1 = not listed, derive it from the car's own mass
// and power (and say so in the log). Filled from the dump of a real run.
extern const signed char cd2MotionModelClass[CD2_MOTION_MODEL_MAX];

// ---------------------------------------------------------------------------
// Layer 1: the idle fidget
// ---------------------------------------------------------------------------
// Three sine waves per axis, at frequencies that do not share a period. That is the
// whole trick: one sine is a wobble you can see repeating, and three that never line
// up again inside a human attention span read as an engine idling. They are summed
// with weights that total 4096, so the sum can never exceed the class amplitude and
// the ceilings cannot be breached by layering.
//
// In PSX angle units per frame at 30Hz: 1.2Hz = 164, 1.7Hz = 232, 2.3Hz = 314.
// Raised by about half again (to roughly 1.9, 2.7 and 3.7Hz) so the idle reads as a
// fast tremble rather than a slow sway, and the class amplitudes halved to match: the
// spec is "almost nothing, but quick", and a slow deep motion at any depth reads as a
// boat rather than an engine.
#define CD2_IDLE_AXES		3	// pitch, roll, yaw
#define CD2_IDLE_WAVES		3
#define CD2_IDLE_FREQ		{ 262, 371, 502 }
#define CD2_IDLE_WEIGHT		{ 2048, 1229, 819 }	/* 0.5, 0.3, 0.2 */

// A per-car frequency detune, +/- this much, so two cars never share a period even
// if they share a phase. Without it a pair of cars shudders in step, which is the
// tell that it is a generator rather than an engine.
#define CD2_IDLE_DETUNE		24		// /4096

// The vertical bob runs on its own, FASTER frequencies than the axes. An idling
// engine trembles quickly and shallowly; a slow deep rise and fall reads as a boat,
// which is what the first pass at this looked like.
// 3.9Hz = 533, 5.2Hz = 717, 6.6Hz = 902.
//
// Note the units: the bob is applied as a whole world unit (m->t[1] += bob), so 1 is
// the smallest vertical step the matrix can express. At that size the bob is a square
// wave, which is why it reads as a tremble rather than as motion, and why the idle's
// visible depth lives in the ANGLES above instead. Going shallower than this means
// removing the vertical, not tuning it.
#define CD2_IDLE_BOB_FREQ	{ 533, 717, 902 }

// The idle is all there is at a standstill, and by the time a car is properly moving
// the motion layers take over and it must not fight them - so it fades out between
// these two speeds rather than switching off. Smoothing (a lerp toward the target)
// keeps the fade from being visible as a step when a car pulls away.
#define CD2_IDLE_SPEED_FULL	30	// at or below: the idle is fully present
#define CD2_IDLE_SPEED_ZERO	150	// at or above: not present at all
#define CD2_IDLE_SCALE_LERP	2

// The slow envelope: the whole shudder breathes between half and full amplitude over
// about four seconds, so a car sitting still never looks like it is looping.
#define CD2_IDLE_DRIFT_FREQ	18		/* ~0.13Hz */
#define CD2_IDLE_DRIFT_DEPTH	1024		/* +/- this on a 3072 base */

// ---------------------------------------------------------------------------
// Layer 2: pitch-back under power, and the squat that goes with it
// ---------------------------------------------------------------------------
// A real spring-damper (accel = (target - pos) * stiffness - vel * damping), per
// class, because this layer is HELD: the nose stays up while the throttle is on and
// only comes back when it is released. The knock's two-phase motion cannot hold
// anything - it carries an angle up and settles it, which is why an impact can use it
// and this layer cannot.
//
// The target has two parts and they do different jobs:
//   - the THRUST, which is sustained: while the car is under power, so is the pitch
//   - the DELTA, the change in speed this step, which is transient: it is what makes
//     the nose snap up as a car pulls away and what takes it down under braking. A
//     hard stop from speed dips hard because a hard stop IS a large negative delta,
//     and a turbo lurches because a turbo is a large positive one - neither needed a
//     special case, which is the point of driving this from the delta rather than
//     from "is the turbo on".
//
// Two things about the inputs, both measured rather than assumed:
//   - the module's throttle is +1/-1/0 (the thrust applied at CAR_STEP), NOT a 0..255
//     analogue. Treating it as an analogue made the sustained half round to zero and
//     left the delta carrying everything - which looked fine right up until the
//     question "why does the nose drop at cruising speed".
//   - cp->hd.speed is a MAGNITUDE, so it cannot say which way the car is going. The
//     direction comes from the thrust's sign instead.
#define CD2_MOTION_DELTA_GAIN	5	// speed units per step -> angle, per step

// The transient term's floor, in PSX angle units after the gain above: a speed change
// that would tilt the car less than this produced no event, so it produces no pitch. This
// is what makes a gentle drive exactly flat - without it the integrator's ordinary
// breathing is summed into a permanent tilt.
#define CD2_MOTION_DELTA_FLOOR	5	// ~0.44 degrees

// The sustained term's speed window, in speed units per step. Below the floor the car
// has no wheelie at all however hard the throttle is; above the full mark the pitch is
// at the class maximum. Speeds in this model run to about 275, so 25 is a crawl and 180
// is properly moving.
#define CD2_MOTION_SPEED_FLOOR	25
#define CD2_MOTION_SPEED_FULL	180

// Compression against rebound. The spring is asymmetric: while the body is being pushed
// further from level - the nose rising under power, the nose diving under the brakes -
// it uses the class stiffness multiplied by this. Coming back it uses the class stiffness
// unmodified, which is why a car digs in hard and then climbs back reluctantly. The source
// material puts the ratio at 3-5x and this is the single global knob for it.
//
// Note that only the STIFFNESS splits. The damping stays high in both directions on
// purpose: softening the rebound's damping as well would make the return underdamped,
// which is a car that keeps rocking - the floatiness this is here to remove.
#define CD2_MOTION_COMPRESS_PCT	400	// 4x

// A car in reverse has the same delta sign for the opposite reason, and should not
// pitch as hard - it is a different manoeuvre, not a faster one.
#define CD2_MOTION_REVERSE_PCT	50	// % of the amplitude when the car is going backwards

// The squat: as the nose comes up, the weight goes back over the rear wheels and the
// body sits down slightly. Both are derived from the spring's own position, so they
// cannot drift out of step with the angle that caused them.
#define CD2_MOTION_SQUAT_SHIFT	1000	// units of weight shift per unit of pitch, /4096
#define CD2_MOTION_SQUAT_BOB	900	// ...and of downward body movement, /4096

// The composed offset's ceilings - the knock's budget plus the layer's own, never
// more. See cd2MotionCompose for why this exists at all.
#define CD2_MOTION_MAX_SHIFT	100	// the knock's 55, plus room for the squat
#define CD2_MOTION_MAX_BOB	80

// ---------------------------------------------------------------------------
// The jolts that used to live here
// ---------------------------------------------------------------------------
// Every 0.8 to 2.5 seconds a car at rest took a sudden kick on one random axis, handed
// to the knock as an impulse. On paper it was the "occasional random impulse spike" the
// idle was specified with. On screen it was the single most noticeable thing in the
// layer and it read as the car snapping: measured as instantaneous 10-14 unit jumps
// (about a degree) at 24-92 frame intervals, which is the cadence of the timer that
// drove them. They are gone. The continuous fidget below is the whole of the idle now,
// and if an idle kick is ever wanted again it belongs right here as a cd2KnockAdd.

// ---------------------------------------------------------------------------
// Per-car state
// ---------------------------------------------------------------------------
// Its own storage rather than more fields on CD2_CAR: this is animation, CD2_CAR is
// the handling model's, and mixing them would make it hard to see which of the two
// a number belongs to later.
typedef struct CD2_MOTION_STATE
{
	// --- Layer 1: the idle fidget ---
	int wave[CD2_IDLE_AXES][CD2_IDLE_WAVES];	// phase accumulators
	int bobWave[CD2_IDLE_WAVES];			// the vertical bob's own
	int step[CD2_IDLE_WAVES];			// per-car frequencies, detuned
	int bobStep[CD2_IDLE_WAVES];			// and the bob's, which run faster
	int driftPhase;					// the slow amplitude envelope
	unsigned int rng;				// this car's generator: the phase and frequency offsets
	int idleScale;					// smoothed 0..4096: full at rest, 0 at speed

	// --- Layer 2: acceleration pitch-back ---
	int prevSpeed;		// last step's speed, for the delta
	int speed;			// this step's speed (a magnitude), for the speed-scaled pitch
	int travel;			// +1 travelling forwards, -1 backwards - the SIGNED direction
	int delta;		// change in speed over the last step (signed)
	int throttle;		// the thrust applied this step: -1, 0 or +1 (NOT an analogue)
	int accelPitch;		// the spring's position
	int accelVel;		// and its velocity
	int accelShift;		// the squat that comes with it (along the car)
	int accelBob;		// and the vertical part

	int inited;		// phases seeded
	int logged;		// the class line has been written for this car

	int lastPitch, lastRoll, lastYaw, lastBob;	// the idle's last output, for a dump

	// the composed offset as it was actually handed to the renderer, recorded so a
	// dump can show what the car got rather than what each layer wanted
	int compPitch, compRoll, compYaw, compBob, compShift;
} CD2_MOTION_STATE;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
// Which class a car ended up in, and the state behind it.
int cd2MotionClassIndex(int carId);
const CD2_MOTION_CLASS* cd2MotionClassOf(int carId);
CD2_MOTION_STATE* cd2MotionStateOf(int carId);

// The model index the class table is keyed on (cp->ap.carCos, as an index), or -1.
int cd2MotionModelOf(int carId);

// Is this car one the layers may move? The player and the AI opponents, resolved
// from the engine's own idea of a car rather than guessed from control types, so
// traffic and parked cars are out by construction.
int cd2MotionIsRacer(int carId);

// One line per car, written once: the class it resolved to, the model it keyed on,
// and the mass and power that decided it. This is what the table gets filled from.
void cd2MotionDump(int carId);

// Build the offset a car is drawn with: the knock's contribution plus the layers',
// in one place, clamped so two subsystems can never stack into something absurd. The
// knock always lands in full; a layer may add its own ceiling on top of that, no more.
void cd2MotionCompose(int carId, CD2_VISUAL_OFFSET* o);

// The layers, evaluated for one car and ADDED to an offset the caller already has
// (the knock's). Called once per frame per car from the car-draw path, which is
// render-only, so nothing here can reach the handling model. Advances the phases,
// runs the springs, and gates the whole thing on the racer set.
void cd2MotionApply(int carId, CD2_VISUAL_OFFSET* o);

// Record what the car is doing this step - its speed, the change since the last step,
// and the throttle. Called from the car-step hook (the physics rate), NOT from the
// draw, so the delta is a real delta rather than however often a car happened to be
// drawn.
void cd2MotionStep(int carId);

// Seed this car's phases. Called on first use; deterministic in the run seed, so a
// replay with the same -seed shudders identically.
void cd2MotionSeed(int carId);

// One line sampling the idle's current output. Only worth having with a log, and it
// is how the amplitudes and the "not a visible loop" claim are checked.
void cd2MotionDumpIdle(int carId);
void cd2MotionDumpAccel(int carId);

// How often to sample the idle, from CC_MOTION_LOG=<frames> (a run-only override,
// never saved - the same pattern as CC_OPPONENTS). 0 or unset means off. And the
// sampler itself, called once per frame from the debug tick.
int cd2MotionLogEvery(void);
void cd2MotionSample(void);

void cd2MotionReset(int carId);
void cd2MotionResetAll(void);

#endif /* CD2_MOTION_H */
