/*
 * motion/motion.c -- see motion/motion.h for what the layers are and why.
 *
 * This file currently owns the CLASS DATA and the per-car state: which class a car
 * is in, and the storage its layers keep between frames. The layers themselves are
 * added on top of it.
 */
#include "driver2.h"
#include "cainescrossfire.h"
#include "cainescrossfire_internal.h"	/* gCd2Car - the per-car handling state */
#include "cars.h"
#include "cosmetic.h"		/* car_cosmetics[] - the model index is an offset into it */
#include "dr2math.h"		/* RSIN - the integer sine the engine already uses */
#include "main.h"			/* FrameCnt - the sampler's clock */
#include "jericho.h"
#include "jer_math.h"		/* jer_clamp_int */

#include "motion/motion.h"
#include "knock/knock.h"	/* the slam rides the knock */
#include "ai/ai.h"		/* cd2AiIsOpponent - the racer test */

// ---------------------------------------------------------------------------
// The classes
// ---------------------------------------------------------------------------
// Read the numbers as "how the car carries its weight". A light car pitches further
// and springs back faster; a heavy one is slower to move and slower to settle, and
// that difference - not the top speed - is what makes a truck feel like a truck.
//
// The idle amplitudes are all small on purpose: a degree of pitch at a standstill
// is a car with a running engine, three degrees is a car that looks broken.
const CD2_MOTION_CLASS cd2MotionClasses[CD2_MOTION_CLASSES] =
{
	//  name      pitchMax  stiffness  damping  over%  idleP/R/Y  bob
	{ "LIGHT",    137,       400,      1400,    35,      6,  6,  3,  1 },	// ~12 deg
	{ "MEDIUM",   102,       340,      1415,    30,      5,  5,  3,  1 },	// ~9 deg
	{ "HEAVY",     68,       240,      1350,    25,      4,  4,  2,  1 },	// ~6 deg
};

// Which model is which class. -1 = work it out from the car's own numbers.
//
// Fill this in from a run: cd2MotionDump prints each model's class alongside the
// mass and power that were used, so this becomes a list of decisions rather than a
// list of guesses. Until a model is listed, the fallback below keeps it sensible.
const signed char cd2MotionModelClass[CD2_MOTION_MODEL_MAX] =
{
	-1, -1, -1, -1, -1, -1, -1, -1
};

// The fallback, derived from what a real level actually reports. Two levels were
// dumped (see the class line below) and every resident car came back like this:
//
//   mass    4096 for ALL of them (1.0 in fixed point - the engine's default, so
//           mass cannot classify anything on its own)
//   power   3000 .. 4096
//   length  351 .. 396      (colBox.vz - the collision box's length)
//   width   129 .. 145
//   wheel   49 .. 55
//   hnd     0 or 1
//
// So the pool is genuinely narrow, and the class table above is the intended lever,
// not this. The boundaries are therefore set OUTSIDE the observed range: they exist
// to catch a genuine outlier (a bus, a truck, a limo - if a level ever holds one)
// and everything ordinary comes out MEDIUM. A rule that split a 351..396 spread into
// three classes would be reading noise, and would put a whole level in one bucket
// anyway - which is exactly what the first attempt at this did.
#define CD2_MOTION_LEN_LIGHT	340	// colBox.vz at or below this is light
#define CD2_MOTION_LEN_HEAVY	430	// ...at or above this is heavy
#define CD2_MOTION_PW_LIGHT	5000	// power-to-weight (== power here), secondary
#define CD2_MOTION_PW_HEAVY	2500

static CD2_MOTION_STATE gMotion[MAX_CARS];

// ---------------------------------------------------------------------------
// Which car is in which class
// ---------------------------------------------------------------------------
// [D] [T]
int cd2MotionModelOf(int carId)
{
	const CAR_DATA* cp;
	int model;

	if (carId < 0 || carId >= MAX_CARS)
		return -1;

	cp = &car_data[carId];

	if (cp->ap.carCos == NULL)
		return -1;

	model = (int)(cp->ap.carCos - car_cosmetics);

	if (model < 0 || model >= CD2_MOTION_MODEL_MAX)
		return -1;

	return model;
}

// [D] [T]
static int cd2MotionDeriveClass(const CAR_COSMETICS* cos)
{
	int len = cos->colBox.vz;
	int mass = cos->mass;
	int power = cos->powerRatio;
	int pw;

	/* size first: it is the one field that actually differs across a level's cars */
	if (len > 0 && len <= CD2_MOTION_LEN_LIGHT)
		return CD2_MOTION_LIGHT;

	if (len >= CD2_MOTION_LEN_HEAVY)
		return CD2_MOTION_HEAVY;

	/* a car with no mass is not a car; fall back to the middle rather than divide */
	if (mass <= 0)
		return CD2_MOTION_MEDIUM;

	pw = (power * 4096) / mass;

	if (pw >= CD2_MOTION_PW_LIGHT)
		return CD2_MOTION_LIGHT;

	if (pw <= CD2_MOTION_PW_HEAVY)
		return CD2_MOTION_HEAVY;

	return CD2_MOTION_MEDIUM;
}

// [D] [T]
int cd2MotionClassIndex(int carId)
{
	int model;

	if (carId < 0 || carId >= MAX_CARS)
		return CD2_MOTION_MEDIUM;

	model = cd2MotionModelOf(carId);

	if (model >= 0 && cd2MotionModelClass[model] >= 0 &&
		cd2MotionModelClass[model] < CD2_MOTION_CLASSES)
	{
		return cd2MotionModelClass[model];
	}

	if (car_data[carId].ap.carCos != NULL)
		return cd2MotionDeriveClass(car_data[carId].ap.carCos);

	return CD2_MOTION_MEDIUM;
}

// [D] [T]
const CD2_MOTION_CLASS* cd2MotionClassOf(int carId)
{
	return &cd2MotionClasses[cd2MotionClassIndex(carId)];
}

// [D] [T]
CD2_MOTION_STATE* cd2MotionStateOf(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return NULL;

	return &gMotion[carId];
}

// ---------------------------------------------------------------------------
// The racer set
// ---------------------------------------------------------------------------
// The player, and the cars cainescrossfire's own AI is driving. This is the test the
// AI already uses to find the cars in a race (ai/opponent.c), so traffic, parked
// cars and the world's police are out by construction rather than by a filter that
// has to remember to exclude them.
// [D] [T]
int cd2MotionIsRacer(int carId)
{
	const CAR_DATA* cp;

	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	cp = &car_data[carId];

	if (cp->controlType == CONTROL_TYPE_PLAYER)
		return 1;

	return cd2AiIsOpponent(cp);
}

// ---------------------------------------------------------------------------
// The class line
// ---------------------------------------------------------------------------
// Written once per car per run. It is deliberately verbose: the model index, what
// the table said about it, and the mass and power the fallback used - because this
// is the evidence cd2MotionModelClass above gets filled from.
// [D] [T]
void cd2MotionDump(int carId)
{
	CD2_MOTION_STATE* st;
	const CAR_COSMETICS* cos;
	const CD2_MOTION_CLASS* cls;
	int model, listed, pw;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	st = &gMotion[carId];

	if (st->logged)
		return;

	cos = car_data[carId].ap.carCos;
	model = cd2MotionModelOf(carId);
	listed = (model >= 0 && cd2MotionModelClass[model] >= 0);
	cls = cd2MotionClassOf(carId);
	pw = (cos != NULL && cos->mass > 0) ? (cos->powerRatio * 4096) / cos->mass : 0;

	st->logged = 1;

	/* deliberately verbose: this line is the evidence cd2MotionModelClass above
	 * gets filled from, so it prints every field that could discriminate a car -
	 * and the collision box, because that is the one that actually did. */
	jer_log("[cainescrossfire] motion car=%d model=%d slot=%d class=%s (%s) racer=%d mass=%d power=%d pw=%d hnd=%d len=%d wide=%d wheel=%d\n",
		carId, model, car_data[carId].ap.model, cls->name, listed ? "listed" : "derived",
		cd2MotionIsRacer(carId),
		cos != NULL ? cos->mass : 0, cos != NULL ? cos->powerRatio : 0, pw,
		car_data[carId].hndType,
		cos != NULL ? cos->colBox.vz : 0, cos != NULL ? cos->colBox.vx : 0,
		cos != NULL ? cos->wheelSize : 0);
}

// ---------------------------------------------------------------------------
// Layer 1: the idle fidget
// ---------------------------------------------------------------------------
// A tiny local LCG. The engine's Random2 ignores its argument and is a pure function
// of the frame counter, so every car would get the same sequence - exactly the trap
// the AI hit (see project memory, combatd2-ai-seeding). cd2AiRunSeed() supplies the
// per-run entropy and is pinned by -seed, so a replay is exact; the car id decides
// which car within the run.
// [D] [T]
static unsigned int cd2MotionNext(unsigned int* s)
{
	unsigned int x;

	/* splitmix32. A plain LCG is good enough for phases, but the phase and detune draws
	 * take small modulos of it, and small modulos of an LCG's low bits are exactly where
	 * an LCG is weakest - cars ended up sharing periods. A real finalizer fixes it at the
	 * source rather than at each use. */
	*s += 0x9E3779B9u;

	x = *s;
	x = (x ^ (x >> 16)) * 0x21F0AAADu;
	x = (x ^ (x >> 15)) * 0x735A2D97u;

	return x ^ (x >> 15);
}

// [D] [T]
void cd2MotionSeed(int carId)
{
	static const int base[CD2_IDLE_WAVES] = CD2_IDLE_FREQ;
	static const int bobBase[CD2_IDLE_WAVES] = CD2_IDLE_BOB_FREQ;
	CD2_MOTION_STATE* st;
	unsigned int s;
	int axis, w;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	st = &gMotion[carId];

	s = cd2AiRunSeed() ^ (((unsigned int)carId + 1u) * 2654435761u);
	s ^= s >> 15;
	s *= 2246822519u;
	s ^= s >> 13;

	st->rng = s | 1;

	/* every wave starts somewhere different, so no two axes and no two cars are in
	 * step at frame one */
	for (axis = 0; axis < CD2_IDLE_AXES; axis++)
	{
		for (w = 0; w < CD2_IDLE_WAVES; w++)
			st->wave[axis][w] = (int)(cd2MotionNext(&st->rng) & 4095);
	}

	for (w = 0; w < CD2_IDLE_WAVES; w++)
		st->bobWave[w] = (int)(cd2MotionNext(&st->rng) & 4095);

	/* and the frequencies are detuned, so even two cars seeded alike drift apart */
	for (w = 0; w < CD2_IDLE_WAVES; w++)
	{
		int jitter = (int)(cd2MotionNext(&st->rng) % (unsigned)(CD2_IDLE_DETUNE * 2 + 1)) - CD2_IDLE_DETUNE;

		st->step[w] = base[w] + (base[w] * jitter) / 4096;

		if (st->step[w] == 0)
			st->step[w] = base[w];
	}

	/* and the bob gets its own, faster set, detuned the same way */
	for (w = 0; w < CD2_IDLE_WAVES; w++)
	{
		int jitter = (int)(cd2MotionNext(&st->rng) % (unsigned)(CD2_IDLE_DETUNE * 2 + 1)) - CD2_IDLE_DETUNE;

		st->bobStep[w] = bobBase[w] + (bobBase[w] * jitter) / 4096;

		if (st->bobStep[w] == 0)
			st->bobStep[w] = bobBase[w];
	}

	st->driftPhase = (int)(cd2MotionNext(&st->rng) & 4095);
	st->idleScale = 4096;
	st->inited = 1;
}

// The three waves of one axis, summed and scaled. Never steps: the output is stored
// so a dump can read it without changing what was drawn.
// [D] [T]
static int cd2IdleWave(int* phases, const int* steps, int envelope)
{
	static const int weight[CD2_IDLE_WAVES] = CD2_IDLE_WEIGHT;
	int sum = 0;
	int w;

	for (w = 0; w < CD2_IDLE_WAVES; w++)
	{
		sum += (weight[w] * RSIN(phases[w])) >> 12;
		phases[w] = (phases[w] + steps[w]) & 4095;
	}

	/* the envelope is applied to the sum as a whole: the layer breathes, and the
	 * ratio between the three waves never changes */
	return (int)(((long long)sum * envelope) >> 12);
}

// [D] [T]
static void cd2IdleApply(CD2_MOTION_STATE* st, const CD2_MOTION_CLASS* cls)
{
	int amp[CD2_IDLE_AXES];
	int envelope;
	int axis;

	/* the slow envelope: between half and full amplitude, over about four seconds */
	envelope = 3072 + (int)(((long long)RSIN(st->driftPhase) * CD2_IDLE_DRIFT_DEPTH) >> 12);
	st->driftPhase = (st->driftPhase + CD2_IDLE_DRIFT_FREQ) & 4095;

	amp[0] = cls->idlePitch;
	amp[1] = cls->idleRoll;
	amp[2] = cls->idleYaw;

	for (axis = 0; axis < CD2_IDLE_AXES; axis++)
	{
		int value = (int)(((long long)cd2IdleWave(st->wave[axis], st->step, envelope) * amp[axis]) >> 12);

		value = (int)(((long long)value * st->idleScale) >> 12);

		if (axis == 0)
			st->lastPitch = value;
		else if (axis == 1)
			st->lastRoll = value;
		else
			st->lastYaw = value;
	}

	st->lastBob = (int)(((long long)((cd2IdleWave(st->bobWave, st->bobStep, envelope) * cls->idleBob) >> 12) * st->idleScale) >> 12);
}

// ---------------------------------------------------------------------------
// Layer 2: pitch-back under power
// ---------------------------------------------------------------------------
// What the car is doing, recorded at the PHYSICS rate rather than at the draw rate:
// the delta has to be a real per-step change in speed, not however often this car
// happened to be drawn.
// [D] [T]
void cd2MotionStep(int carId)
{
	CD2_MOTION_STATE* st;
	int speed;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	if (!cd2MotionIsRacer(carId))
		return;

	st = &gMotion[carId];

	if (!st->inited)
	{
		/* no delta for the first step: there is nothing to compare against, and the
		 * car has not moved yet */
		st->prevSpeed = car_data[carId].hd.speed;
		return;
	}

	speed = car_data[carId].hd.speed;

	st->delta = speed - st->prevSpeed;
	st->prevSpeed = speed;
	st->speed = speed;
	/* Which way the car is actually going. hd.speed is a magnitude, so this is the only
	 * thing in the state that knows: the thrust says what the driver asked for, which is
	 * not the same question. */
	st->travel = (gCd2Car[carId].fwdSpeed < 0) ? -1 : 1;
	st->throttle = jer_clamp_int(gCd2Car[carId].throttle, -1, 1);
}

// [D] [T]
static void cd2AccelApply(int carId, CD2_MOTION_STATE* st, const CD2_MOTION_CLASS* cls)
{
	int target = 0, accel;

	/* The sustained half: while the car is under power, so is the pitch - but scaled by
	 * SPEED, because lifting the nose is something a car does when it has the power to
	 * lift itself, not something it does while crawling. Below CD2_MOTION_SPEED_FLOOR
	 * there is no sustained pitch at all; by CD2_MOTION_SPEED_FULL it is at full
	 * amplitude. That rule is from the source material and it is also what keeps gentle
	 * driving flat: at walking pace this term is zero whatever the throttle is doing.
	 * Reversing is the same power pointed the other way and gets the opposite pitch at
	 * half amplitude - backing up is a different manoeuvre, not a faster one. */
	{
		int speed = st->speed;
		int scale;

		if (speed < 0)
			speed = -speed;

		if (speed <= CD2_MOTION_SPEED_FLOOR)
			scale = 0;
		else if (speed >= CD2_MOTION_SPEED_FULL)
			scale = 4096;
		else
			scale = ((speed - CD2_MOTION_SPEED_FLOOR) * 4096) / (CD2_MOTION_SPEED_FULL - CD2_MOTION_SPEED_FLOOR);

		/* Sustained: POWER, and not the pedal alone. Forward power lifts the nose
		 * whichever way the car happens to be pointing - a forward launch always lifts
		 * it. Reverse power (the same pedal as braking) lifts the tail, and only when the
		 * car is genuinely travelling backwards. Braking while travelling forwards gets
		 * no sustained term at all: its whole effect is the transient dive below. */
		if (st->throttle > 0)
			target = (cls->pitchMax * scale) >> 12;
		else if (st->throttle < 0 && st->travel < 0)
			target = -(((cls->pitchMax / 2) * scale) >> 12);
	}

	/* The transient half: the speed delta, which is what actually moves the car.
	 * Braking is a negative delta, so the same rule that lifts the nose on power
	 * takes it down on the brakes - that is the stoppie, and it needs no case of its
	 * own. A turbo lurches for the same reason, because a turbo IS a large positive
	 * delta rather than a flag this has to know about. */
	{
		int d = st->delta * CD2_MOTION_DELTA_GAIN;

		/* The sign belongs to the DIRECTION OF TRAVEL, not to the pedal. Keying it off
		 * the thrust was the bug behind "the forward/backwards reads inverted": braking
		 * hard while travelling forwards looked like reverse, so the nose LIFTED under
		 * the brakes instead of diving. Speeding up digs the far end in - a positive
		 * delta when travelling forwards, and the same delta negated when travelling
		 * backwards, at half the amplitude because a reverse dive is a gentler thing. */
		if (st->travel < 0)
			d = -((d * CD2_MOTION_REVERSE_PCT) / 100);

		/* The floor. A delta too small to produce CD2_MOTION_DELTA_FLOOR of angle is
		 * not an event, it is the integrator breathing - and applied continuously it
		 * tilts the car for no reason the player can see, which is the "it tilts when
		 * nothing happened" family of reports. Below the floor this term is exactly
		 * zero, so a tilt means something occurred. */
		if (d < CD2_MOTION_DELTA_FLOOR && d > -CD2_MOTION_DELTA_FLOOR)
			d = 0;

		target += d;
	}

	target = jer_clamp_int(target, -cls->pitchMax, cls->pitchMax);

	/* accel = (target - pos) * stiffness - vel * damping, in /4096 fixed point. The
	 * class decides how fast it gets there and how much it overshoots. */
	{
		int err = target - st->accelPitch;
		int stiff = cls->stiffness;

		/* Is the body being pushed further from level, or coming back? Compression is
		 * the stiff half: a car digs in fast and climbs back reluctantly, which is the
		 * asymmetry that reads as weight. Tested on the SIGN of the error against the
		 * sign of the position rather than on the throttle, so it is the body's motion
		 * being classified - a launch, a dive and a landing all compress, and every
		 * return is a rebound. */
		if (err != 0 && (st->accelPitch == 0 ? 1 : ((st->accelPitch > 0) == (err > 0))))
			stiff = (cls->stiffness * CD2_MOTION_COMPRESS_PCT) / 100;

		accel = ((err * stiff) >> 12) - (st->accelVel * cls->damping >> 12);
	}

	st->accelVel += accel;
	st->accelPitch += st->accelVel;

	/* The rebound's bound, and what overshootPct was always for: it was declared,
	 * initialised in all three classes and never read. Returning to level may carry the
	 * body PAST level once - that is the snap the source material describes - but no
	 * further than the class allows, so the crossing is a movement rather than the start
	 * of a wobble. */
	{
		/* Two bounds, because the two halves need different ones. Rising, the body may
		 * pass the class ceiling by overshootPct - a wheelie arrives at the top of its arc
		 * and goes a little beyond, which is the "overshoot on arrival" the source material
		 * describes. Returning, it may only pass LEVEL by overshootPct, since that is the
		 * counter-rock before the motion dies. Without the rising bound the arrival is
		 * unbounded and the 4x compression makes it enormous: measured at 144 against a
		 * 102 ceiling before this existed. */
		int limit = (target == 0) ? (cls->pitchMax * cls->overshootPct) / 100
			: cls->pitchMax + (cls->pitchMax * cls->overshootPct) / 100;

		if (st->accelPitch > limit)
			st->accelPitch = limit;
		else if (st->accelPitch < -limit)
			st->accelPitch = -limit;
	}

	/* THE SLAM. It fires when the body CROSSES level, judged against how far out this
	 * movement has been - not against the previous frame, which is what the first version
	 * did and it could never fire: at the crossing the previous frame is about 2 units by
	 * definition, so a test of "was it out at 35 the frame before" is never true. The peak
	 * is remembered instead, and cleared as the crossing is taken, so the slam happens once
	 * per movement rather than once per frame of the return.
	 *
	 * No airborne work is needed: the spring arriving back at level IS the far end of the
	 * car coming down. */
	{
		int mag = st->accelPitch < 0 ? -st->accelPitch : st->accelPitch;

		if (mag > st->accelPeak)
			st->accelPeak = mag;

		/* The crossing itself: the sign of the pitch changed since the last frame. That is
		 * what "arrives back at level" means, and it happens exactly once per movement. */
		if ((st->accelPeak >= CD2_MOTION_SLAM_MIN) &&
			((st->accelPrev > 0 && st->accelPitch <= 0) || (st->accelPrev < 0 && st->accelPitch >= 0)))
		{
			int wheelie = (st->accelPrev > 0);
			int impulse = wheelie ? -CD2_KNOCK_IMPULSE_TO(CD2_MOTION_SLAM_IMPULSE) : CD2_KNOCK_IMPULSE_TO(CD2_MOTION_SLAM_IMPULSE);
			int shift = wheelie ? CD2_MOTION_SLAM_SHIFT : -CD2_MOTION_SLAM_SHIFT;

			jer_log("[cainescrossfire] slam car=%d peak=%d (a %s ending)\n",
				carId, st->accelPeak, wheelie ? "wheelie" : "stoppie");

			cd2KnockAdd(carId, impulse, 0, 0, 0, shift);

			st->accelPeak = 0;	/* one slam per movement */
		}

		st->accelPrev = st->accelPitch;
	}

	/* a movement that has settled flat is over: the next one starts its own peak */
	if (target == 0 && st->accelPitch == 0 && st->accelVel == 0)
		st->accelPeak = 0;
}

// The squat, derived from the spring's own position so it cannot drift out of step
// with the angle that caused it: nose up, weight back, body sitting down on the rear.
// [D] [T]
static void cd2AccelSquat(CD2_MOTION_STATE* st)
{
	st->accelShift = -(st->accelPitch * CD2_MOTION_SQUAT_SHIFT) >> 12;
	st->accelBob = -(st->accelPitch * CD2_MOTION_SQUAT_BOB) >> 12;
}

// ---------------------------------------------------------------------------
// Composing, and the one clamp
// ---------------------------------------------------------------------------
// One place builds the offset the car is drawn with: the knock's contribution, plus
// the layers' - and then it is clamped, which is the only arithmetic in this system
// that exists purely to stop two subsystems adding up badly.
//
// The rule is deliberate: the knock always lands IN FULL, and a layer may put its own
// ceiling on top of that, but not more. Without it, a knock arriving during a
// wheelie would stack two full-amplitude rotations and the car would look like it was
// standing on its nose. The impact cannot be damped by whatever the driving layer
// happens to be doing, which is what "the knock wins" means here.
// [D] [T]
void cd2MotionCompose(int carId, CD2_VISUAL_OFFSET* o)
{
	const CD2_KNOCK_STATE* k = cd2KnockOf(carId);
	const CD2_MOTION_CLASS* cls = cd2MotionClassOf(carId);
	int pitchMax, rollMax, yawMax;

	if (o == NULL || carId < 0 || carId >= MAX_CARS)
		return;

	/* off entirely (config `motion`, or CC_MOTION=0 for a run): hand back exactly what
	 * the knock did before any of this existed, so the master switch is a true off */
	if (!cd2MotionEnabled())
	{
		o->pitch = k->pitch;
		o->roll = k->roll;
		o->yaw = k->yaw;
		o->bob = k->lift;
		o->shift = k->shift;
		return;
	}

	o->pitch = k->pitch;
	o->roll = k->roll;
	o->yaw = k->yaw;
	o->bob = k->lift;			/* the knock's lift is never negative */
	o->shift = k->shift;

	cd2MotionApply(carId, o);

	pitchMax = cls->pitchMax + CD2_KNOCK_MAX_PITCH;
	rollMax = cls->idleRoll + CD2_KNOCK_MAX_ROLL;
	yawMax = cls->idleYaw + CD2_KNOCK_MAX_YAW;

	o->pitch = jer_clamp_int(o->pitch, -pitchMax, pitchMax);
	o->roll = jer_clamp_int(o->roll, -rollMax, rollMax);
	o->yaw = jer_clamp_int(o->yaw, -yawMax, yawMax);
	o->shift = jer_clamp_int(o->shift, -CD2_MOTION_MAX_SHIFT, CD2_MOTION_MAX_SHIFT);
	o->bob = jer_clamp_int(o->bob, -CD2_MOTION_MAX_BOB, CD2_MOTION_MAX_BOB);

	/* remembered, so a dump can show what the car actually got rather than what each
	 * layer separately wanted */
	{
		CD2_MOTION_STATE* st = cd2MotionStateOf(carId);

		if (st != NULL)
		{
			st->compPitch = o->pitch;
			st->compRoll = o->roll;
			st->compYaw = o->yaw;
			st->compBob = o->bob;
			st->compShift = o->shift;
		}
	}
}

// ---------------------------------------------------------------------------
// The frame, and the dump
// ---------------------------------------------------------------------------
// [D] [T]
void cd2MotionApply(int carId, CD2_VISUAL_OFFSET* o)
{
	CD2_MOTION_STATE* st;
	const CD2_MOTION_CLASS* cls;

	if (o == NULL || carId < 0 || carId >= MAX_CARS)
		return;

	if (!cd2MotionEnabled())
		return;

	/* the racer set only. Traffic and parked cars keep whatever the physics gave
	 * them: a queue of traffic shuddering in unison looks like a bug, not a world. */
	if (!cd2MotionIsRacer(carId))
		return;

	st = &gMotion[carId];

	if (!st->inited)
		cd2MotionSeed(carId);

	cls = cd2MotionClassOf(carId);

	/* How present the idle is. It is all there is at a standstill, and by the time a
	 * car is properly moving the motion layers take over and it must not fight them,
	 * so it fades between two speeds. The fade is smoothed, or pulling away would
	 * switch it off with a visible step. */
	{
		int speed = car_data[carId].hd.speed;
		int target;

		if (speed <= CD2_IDLE_SPEED_FULL)
			target = 4096;
		else if (speed >= CD2_IDLE_SPEED_ZERO)
			target = 0;
		else
			target = 4096 - ((speed - CD2_IDLE_SPEED_FULL) * 4096) / (CD2_IDLE_SPEED_ZERO - CD2_IDLE_SPEED_FULL);

		st->idleScale = jer_lerp_int(st->idleScale, target, CD2_IDLE_SCALE_LERP);
	}

	if (gCd2Cfg.motionIdle)
		cd2IdleApply(st, cls);

	if (gCd2Cfg.motionAccel)
	{
		cd2AccelApply(carId, st, cls);
		cd2AccelSquat(st);
	}

	o->pitch += st->lastPitch;
	o->roll += st->lastRoll;
	o->yaw += st->lastYaw;
	o->bob += st->lastBob;

	o->pitch += st->accelPitch;
	o->shift += st->accelShift;
	o->bob += st->accelBob;
}

// Set by CC_MOTION_LOG=<frames> for a run - a run-only override, never saved, like
// CC_OPPONENTS. 0 (or unset) means off.
static int gMotionLogEvery = -1;

// [D] [T]
int cd2MotionLogEvery(void)
{
	if (gMotionLogEvery < 0)
	{
		const char* env = getenv("CC_MOTION_LOG");

		gMotionLogEvery = 0;

		if (env != NULL && env[0] != 0)
		{
			int v = atoi(env);

			if (v > 0)
				gMotionLogEvery = v;
		}
	}

	return gMotionLogEvery;
}

// [D] [T]
void cd2MotionDumpIdle(int carId)
{
	CD2_MOTION_STATE* st;
	const CD2_MOTION_CLASS* cls;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	st = &gMotion[carId];

	if (!st->inited)
		return;

	cls = cd2MotionClassOf(carId);

	jer_log("[cainescrossfire] idle car=%d pitch=%d roll=%d yaw=%d bob=%d scale=%d speed=%d class=%s\n",
		carId, st->lastPitch, st->lastRoll, st->lastYaw, st->lastBob, st->idleScale,
		car_data[carId].hd.speed, cls->name);
}

// The same sample for Layer 2: the spring's position, what drove it, and the squat it
// produced. Kept separate from the idle line because they are tuned separately.
// [D] [T]
void cd2MotionDumpAccel(int carId)
{
	CD2_MOTION_STATE* st;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	st = &gMotion[carId];

	if (!st->inited)
		return;

	jer_log("[cainescrossfire] accel car=%d pitch=%d vel=%d delta=%d thr=%d shift=%d bob=%d speed=%d class=%s\n",
		carId, st->accelPitch, st->accelVel, st->delta, st->throttle, st->accelShift, st->accelBob,
		car_data[carId].hd.speed, cd2MotionClassOf(carId)->name);

	/* what the renderer actually got, clamp included */
	jer_log("[cainescrossfire] composed car=%d pitch=%d roll=%d yaw=%d bob=%d shift=%d\n",
		carId, st->compPitch, st->compRoll, st->compYaw, st->compBob, st->compShift);
}

// [D] [T]
void cd2MotionSample(void)
{
	int every = cd2MotionLogEvery();
	int i;

	if (every <= 0 || (FrameCnt % every) != 0)
		return;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (gMotion[i].inited && cd2MotionIsRacer(i))
		{
			cd2MotionDumpIdle(i);
			cd2MotionDumpAccel(i);
		}
	}
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------
// [D] [T]
void cd2MotionReset(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	memset(&gMotion[carId], 0, sizeof(gMotion[carId]));
}

// [D] [T]
void cd2MotionResetAll(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
		cd2MotionReset(i);
}
