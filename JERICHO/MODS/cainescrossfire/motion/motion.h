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
 * angle to a ceiling and settles. An idle that did that would twitch and stop. So
 * the layers are procedural - sines and springs evaluated every frame - and only
 * the idle's random SPIKES are handed to the knock, where the settling machinery
 * already exists and is already tuned.
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
// Per-car state
// ---------------------------------------------------------------------------
// Its own storage rather than more fields on CD2_CAR: this is animation, CD2_CAR is
// the handling model's, and mixing them would make it hard to see which of the two
// a number belongs to later.
typedef struct CD2_MOTION_STATE
{
	// --- Layer 1: the idle fidget ---
	int phase[3];		// per-axis phase accumulators (pitch, roll, yaw)
	int bobPhase;		// the vertical bob's own
	int freq[3];		// per-axis frequencies, PSX units per frame, seeded per car
	int bobFreq;
	int driftPhase;		// the slow amplitude drift, so it never visibly loops
	unsigned int rng;	// this car's generator for the spike timing
	int spikeIn;		// frames until the next random spike
	int idleScale;		// smoothed 0..4096: full at rest, 0 at speed

	// --- Layer 2: acceleration pitch-back ---
	int accelPitch;		// the spring's position
	int accelVel;		// and its velocity
	int accelShift;		// the squat that comes with it (along the car)
	int accelBob;		// and the vertical part

	int logged;		// the class line has been written for this car
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

void cd2MotionReset(int carId);
void cd2MotionResetAll(void);

#endif /* CD2_MOTION_H */
