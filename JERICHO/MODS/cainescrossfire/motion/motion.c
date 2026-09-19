/*
 * motion/motion.c -- see motion/motion.h for what the layers are and why.
 *
 * This file currently owns the CLASS DATA and the per-car state: which class a car
 * is in, and the storage its layers keep between frames. The layers themselves are
 * added on top of it.
 */
#include "driver2.h"
#include "cainescrossfire.h"
#include "cars.h"
#include "cosmetic.h"		/* car_cosmetics[] - the model index is an offset into it */
#include "dr2math.h"		/* RSIN - the integer sine the engine already uses */
#include "main.h"			/* FrameCnt - the sampler's clock */
#include "jericho.h"
#include "jer_math.h"		/* jer_clamp_int */

#include "motion/motion.h"
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
	{ "LIGHT",    137,       300,      1500,    35,     11, 11,  6,  2 },	// ~12 deg
	{ "MEDIUM",   102,       240,      1450,    30,      9,  9,  5,  2 },	// ~9 deg
	{ "HEAVY",     68,       170,      1400,    25,      7,  7,  4,  1 },	// ~6 deg
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

	/* splitmix32. A plain LCG's output is good enough for phases but NOT for a
	 * small modulo taken from a subsequence of it: the spike timer consumes two
	 * draws per spike, so the axis was being picked from every other output, and
	 * every other output of an LCG is a weaker LCG. That produced fourteen jolts in
	 * a row on pitch and none on the other two axes. A real finalizer fixes it at
	 * the source rather than at each use. */
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

	st->driftPhase = (int)(cd2MotionNext(&st->rng) & 4095);
	st->spikeIn = CD2_IDLE_SPIKE_MIN + (int)(cd2MotionNext(&st->rng) % (unsigned)(CD2_IDLE_SPIKE_MAX - CD2_IDLE_SPIKE_MIN + 1));
	st->idleScale = 4096;
	st->inited = 1;
}

// The impulse that asks the knock for a given ANGLE. Sizing spikes as angles rather
// than as raw impulses is what keeps them stable when the knock's decay is tuned:
// the number below means "about 1.4 degrees" and stays meaning that.
// [D] [T]
static int cd2SpikeImpulse(int angle)
{
	int mag = CD2_KNOCK_IMPULSE_TO(angle < 0 ? -angle : angle);

	return (angle < 0) ? -mag : mag;
}

// The spikes: every 0.8 to 2.5 seconds a car at rest takes one sudden jolt on one
// axis, and the knock carries it up and settles it back. Nothing here animates the
// jolt itself - it is a kick, and the kick machinery exists.
// [D] [T]
static void cd2IdleSpike(int carId, CD2_MOTION_STATE* st)
{
	int r, pitch = 0, roll = 0, yaw = 0;

	/* a car at rest takes them; as the idle fades out with speed so does the
	 * jolting, or a car at 100mph would keep hiccuping */
	if (st->idleScale < 2048)
		return;

	if (--st->spikeIn > 0)
		return;

	r = (int)cd2MotionNext(&st->rng);

	/* one axis takes it, and the sign is a coin toss: an idle that always jolted
	 * the same way would read as a mechanism rather than an engine */
	switch (r % 3)
	{
	case 0:
		pitch = (r & 4) ? CD2_IDLE_SPIKE_PITCH : -CD2_IDLE_SPIKE_PITCH;
		break;
	case 1:
		roll = (r & 4) ? CD2_IDLE_SPIKE_ROLL : -CD2_IDLE_SPIKE_ROLL;
		break;
	default:
		yaw = (r & 4) ? CD2_IDLE_SPIKE_YAW : -CD2_IDLE_SPIKE_YAW;
		break;
	}

	/* scaled by how present the idle is, so the jolt fades with everything else */
	pitch = (pitch * st->idleScale) >> 12;
	roll = (roll * st->idleScale) >> 12;
	yaw = (yaw * st->idleScale) >> 12;

	cd2KnockAdd(carId, cd2SpikeImpulse(pitch), cd2SpikeImpulse(roll), cd2SpikeImpulse(yaw), 0, 0);

	st->spikeIn = CD2_IDLE_SPIKE_MIN + (int)(cd2MotionNext(&st->rng) % (unsigned)(CD2_IDLE_SPIKE_MAX - CD2_IDLE_SPIKE_MIN + 1));
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

	st->lastBob = (int)(((long long)((cd2IdleWave(st->bobWave, st->step, envelope) * cls->idleBob) >> 12) * st->idleScale) >> 12);
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

	/* the racer set only. Traffic and parked cars keep whatever the physics gave
	 * them: a queue of traffic shuddering in unison looks like a bug, not a world. */
	if (!cd2MotionIsRacer(carId))
		return;

	st = &gMotion[carId];

	if (!st->inited)
		cd2MotionSeed(carId);

	cls = cd2MotionClassOf(carId);

	cd2IdleApply(st, cls);
	cd2IdleSpike(carId, st);

	o->pitch += st->lastPitch;
	o->roll += st->lastRoll;
	o->yaw += st->lastYaw;
	o->bob += st->lastBob;
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

	jer_log("[cainescrossfire] idle car=%d pitch=%d roll=%d yaw=%d bob=%d scale=%d class=%s\n",
		carId, st->lastPitch, st->lastRoll, st->lastYaw, st->lastBob, st->idleScale, cls->name);
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
			cd2MotionDumpIdle(i);
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
