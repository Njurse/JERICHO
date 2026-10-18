/*
 * handling_probe.c — JERICHO: make "my car drives way too fast" a number.
 *
 * WHY THIS EXISTS
 * A "too fast" report has three very different causes and they look identical
 * from the driver's seat:
 *
 *   1. the CLOCK — State_GameLoop steps the sim once per two vblanks (30 Hz) but
 *      SEVEN times per frame while FastForward is on, so a leaked fast-forward
 *      makes everything run at ~7x and the car feels over-powered;
 *   2. the INPUT — cp->thrust / cp->wheel_angle come straight from the pad, and
 *      the analogue steering curve is cubic, so a stick value that is scaled
 *      wrong turns a small deflection into more than the 352 lock;
 *   3. the DATA/physics — the car's own CAR_COSMETICS record (powerRatio,
 *      traction, mass, twistRateY) or the tyre grip it resolves.
 *
 * This probe prints those three in one line per simulated second, and a verdict
 * line at the end of a -frames run, so the answer is read rather than guessed.
 * It only ever reads engine state: it writes nothing and changes no behaviour.
 *
 * HOW TO RUN IT
 *   JERICHO_HANDLING_PROBE=1  JERICHO_PADPROBE_FORCE=cross  JERICHO_dev.exe \
 *       -level chicago -frames 900
 *
 * (JERICHO_PADPROBE_FORCE holds the button at the pad layer, so the car actually
 * accelerates — see HOOKS.md "Input debugging". Use "square" when the pad is in
 * the tmb_buttons scheme.)
 *
 * HOW TO READ IT
 *   t=..s    one simulated second — 30 steps at the stock rate. If these lines
 *            arrive much faster than one per second of wall time, the problem is
 *            the clock, not the car (the verdict line says so explicitly).
 *   speed    cp->hd.speed: the engine's own speed number.
 *   wheel    cp->wheel_angle against (lock 352). Beyond the lock means the
 *            input path is over-driving the steering, not the physics.
 *   thrust   cp->thrust, the engine force the pad produced.
 *   power/   the car's CAR_COSMETICS record — the numbers everything above is
 *   traction derived from. A record that does not belong to the car being driven
 *   /mass    (cross-city import, resident-slot mix-up) shows up here first.
 *   grip     the front/rear friction scales the tyres actually resolved.
 *
 * Nothing is printed unless JERICHO_HANDLING_PROBE is set, so this is inert in a
 * normal session and in a release.
 */

#include "driver2.h"
#include "main.h"
#include "players.h"
#include "cosmetic.h"
#include "cars.h"		// car_data (the local view's car by identity)
#include "mission.h"		// residentCarModels (slot -> model number)
#include "dr2limits.h"
#include "director.h"		// FastForward / FastForwardLocked
#include "cutscene.h"		// gSkipInGameCutscene (the other path that bypasses the step gate)
#include "handling_probe.h"

#include <stdlib.h>		// getenv (JERICHO_HANDLING_PROBE)

/* the stock rate the sim is supposed to run at: two vblanks per step */
#define PROBE_STEPS_PER_SEC		30

/* the steering lock ProcessCarPad's digital path clamps to; the analogue path
 * computes its own angle, so a value past this is a signal, not a bug in here */
#define PROBE_STEER_LOCK		352

/* per-run state, reset by nothing: one process is one run */
static int probeSamples;		/* StepOneCar calls for the local player's car */
static int probeSteps;			/* StepGame calls (1 per simulated step) */
static int probeMaxSpeed;
static int probeGripFront = -1;
static int probeGripRear = -1;
static unsigned int probeTickStart;	/* PsyX_GetTicks() at the first sample */
static int probeClockStarted;

/* the load-independence window: a one-second STEP COUNT can hide a burst inside a
 * second, so the same window also tracks how many frames were DRAWN and the
 * shortest/longest gap between two steps. If the step rate were tracking render
 * rate, fps and the gap spread would move together with the count. */
static unsigned int probeLastStepTick;
static int probeMinGap = -1;	/* ms between consecutive steps, window min */
static int probeMaxGap = -1;	/* ms between consecutive steps, window max */

// Read once: the lever is run-wide, not a per-frame knob.
static int JerProbeOn(void)
{
	static int on = -1;

	if (on < 0)
		on = (getenv("JERICHO_HANDLING_PROBE") != NULL) ? 1 : 0;

	return on;
}

// The car this report is about: the LOCAL player's, and only that one.
//
// It has to be the local view's car by IDENTITY, not by "controlType == PLAYER":
// in a session the peer cars on this machine are player cars too, so a looser
// test sampled whichever of them StepOneCar happened to touch that step -- the
// probe's second counter then ran several times a second and its slot/model
// wandered (seen on the first pair run). MainPlayer is this machine's view, so
// its car is the only one whose numbers mean "my car".
static int JerProbeIsLocalPlayer(CAR_DATA* cp)
{
	int carId = (int)MainPlayer.playerCarId;

	return cp != NULL && carId >= 0 && carId < MAX_CARS &&
		cp == &car_data[carId] &&
		cp->controlType == CONTROL_TYPE_PLAYER &&
		cp->ai.padid != NULL && *cp->ai.padid >= 0 && *cp->ai.padid < 2;
}

static void JerProbeStartClock(void)
{
	if (!probeClockStarted)
	{
		probeClockStarted = 1;
		probeTickStart = PsyX_GetTicks();
	}
}

void JerHandleProbeStep(CAR_DATA* cp)
{
	CAR_COSMETICS* cos;
	int slot, model;

	if (!JerProbeOn() || !JerProbeIsLocalPlayer(cp))
		return;

	JerProbeStartClock();

	if (cp->hd.speed > probeMaxSpeed)
		probeMaxSpeed = cp->hd.speed;

	probeSamples++;

	/* one line per simulated second (every 30th car sample) */
	if ((probeSamples % PROBE_STEPS_PER_SEC) != 1)
		return;

	slot = cp->ap.model;
	model = (slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS) ? residentCarModels[slot] : -1;

	cos = cp->ap.carCos;

	if (cos == NULL && slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS)
		cos = &car_cosmetics[slot];

	printInfo("[JER-PROBE] t=%ds speed=%d wheel=%d/%d thrust=%d hndType=%d slot=%d model=%d "
			  "power=%d traction=%d mass=%d twistY=%d grip=%d/%d handbrake=%d ff=%d lock=%d skip=%d\n",
		probeSamples / PROBE_STEPS_PER_SEC,
		(int)cp->hd.speed, (int)cp->wheel_angle, PROBE_STEER_LOCK, (int)cp->thrust,
		(int)cp->hndType, slot, model,
		(cos != NULL) ? (int)cos->powerRatio : -1,
		(cos != NULL) ? (int)cos->traction : -1,
		(cos != NULL) ? (int)cos->mass : -1,
		(cos != NULL) ? (int)cos->twistRateY : -1,
		probeGripFront, probeGripRear,
		(int)cp->handbrake,
		FastForward, FastForwardLocked, PsyX_GetFrameSkip());
}

void JerHandleProbeStepGame(void)
{
	unsigned int now;
	int gap;

	if (!JerProbeOn())
		return;

	JerProbeStartClock();

	now = PsyX_GetTicks();

	/* gap since the previous step: the WALL-CLOCK spread is the direct test of
	 * "is the step rate tied to the renderer" -- a gate that follows render rate
	 * shows a wide min/max spread that moves with fps. */
	if (probeLastStepTick != 0)
	{
		gap = (int)(now - probeLastStepTick);

		if (probeMinGap < 0 || gap < probeMinGap)
			probeMinGap = gap;
		if (gap > probeMaxGap)
			probeMaxGap = gap;
	}

	probeLastStepTick = now;
	probeSteps++;
}

/* Once per frame. One line per REAL second with the measured step rate, plus a line
 * whenever any of the three clock flags changes.
 *
 * This is the instrument for the other half of the report -- "the fast forward and
 * normal speed flip every 1-3 seconds". The per-simulated-second lines above cannot
 * show that, because if the sim runs at two rates its own seconds are not wall
 * seconds. A flip has to be timed against the real clock, and it needs a NAME:
 * FastForward (7 steps/frame), the lock, or PsyX's frame skip. Whichever of the
 * three moves here is the cause; if NONE moves and the rate still flips, the step
 * gate itself is unstable -- which is why it now runs on wall-clock milliseconds. */
void JerHandleProbeFrame(void)
{
	static unsigned int lastLine;
	static int lastSteps;
	static int lastFf = -1, lastLock = -1, lastSkip = -1, lastCut = -1;
	unsigned int now;
	int steps, ff, lock, skip, cut;

	if (!JerProbeOn())
		return;

	JerProbeStartClock();

	now = PsyX_GetTicks();

	ff = FastForward;
	lock = FastForwardLocked;
	skip = PsyX_GetFrameSkip();
	cut = gSkipInGameCutscene;

	if (ff != lastFf || lock != lastLock || skip != lastSkip || cut != lastCut)
	{
		printInfo("[JER-PROBE] clock change at %ums: ff %d->%d lock %d->%d skip %d->%d "
				  "skipcutscene %d->%d (steps so far %d)\n",
			now - probeTickStart, lastFf, ff, lastLock, lock, lastSkip, skip, lastCut, cut,
			probeSteps);

		lastFf = ff;
		lastLock = lock;
		lastSkip = skip;
		lastCut = cut;
	}

	if ((unsigned int)(now - lastLine) >= 1000)
	{
		steps = probeSteps - lastSteps;

		/* The gap spread is what separates "the gate caps the rate" from "the rate
		 * follows the renderer": a render-coupled gate shows a wide, lurching min/max
		 * gap, while a wall-clock gate holds a steady ~33 ms. A per-second step COUNT
		 * alone cannot show a burst inside the second; this can.
		 *
		 * There is deliberately no fps field: this engine presents one frame per
		 * GATED step (the state machine itself spins millions of times a second and
		 * returns immediately when the gate refuses), so frames-presented and steps
		 * are the same number and an fps column would only restate the rate. */
		printInfo("[JER-PROBE] rate=%d steps/s gap=%d..%dms "
				  "(ff=%d lock=%d skip=%d skipcutscene=%d)\n",
			steps, probeMinGap, probeMaxGap, ff, lock, skip, cut);

		lastLine = now;
		lastSteps = probeSteps;

		probeMinGap = -1;
		probeMaxGap = -1;
	}
}

void JerHandleProbeGrip(CAR_DATA* cp, int frontFS, int rearFS)
{
	if (!JerProbeOn() || !JerProbeIsLocalPlayer(cp))
		return;

	probeGripFront = frontFS;
	probeGripRear = rearFS;
}

void JerHandleProbeSummary(void)
{
	unsigned int elapsedMs, perSec;

	if (!JerProbeOn())
		return;

	JerProbeStartClock();

	elapsedMs = PsyX_GetTicks() - probeTickStart;
	perSec = (elapsedMs >= 100) ? (unsigned int)(((unsigned long long)probeSteps * 1000) / elapsedMs) : 0;

	// The clock is PsyX_GetTicks (real milliseconds), NOT the vblank counter: while
	// the frame skip is on the vblank counter advances extra times per frame, so a
	// rate derived from it reads LOW in exactly the case this has to catch.
	// 30 = stock. ~210 = FastForward (main.c: cnt = 7). Above 30 with ff=0 is the
	// PsyX frame-skip path (skip=1), which Psyx_SetFastForwardLocked now gates.
	printInfo("JERICHO-PROBE: steps=%u ms=%u steps_per_sec=%u max_speed=%d ff=%d lock=%d skip=%d verdict=%s\n",
		(unsigned int)probeSteps, elapsedMs, perSec, probeMaxSpeed, FastForward, FastForwardLocked,
		PsyX_GetFrameSkip(),
		(perSec > (unsigned int)(PROBE_STEPS_PER_SEC + PROBE_STEPS_PER_SEC / 2)) ? "CLOCK_FAST"
			: (perSec == 0) ? "NO_STEPS" : "RATE_OK");
}
