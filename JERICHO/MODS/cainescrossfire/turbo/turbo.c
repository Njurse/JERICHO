/*
 * turbo/turbo.c -- the turbo meter, the double-tap trigger, and what a boost
 * does to the car.
 *
 * One state per car, because the trigger is a property of a driver rather than
 * of the player: today only the player's car is ever asked about, but an AI car
 * could be given a boost tomorrow without changing any of this.
 *
 * The trigger, as specified: a drive button pressed twice in quick succession
 * (CD2_TURBO_TAP_GRACE) latches turbo ON. It holds until the meter runs out OR
 * the driver lets that button go - so a boost is something you can stop early by
 * releasing, and there is no way to accidentally leave it running. The reverse
 * button works the same way for a reverse boost.
 *
 * The meter does NOT trickle back: it comes back on the refill events only
 * (CD2_TURBO_REFILL_*), which is what makes it worth spending deliberately.
 */
#include "driver2.h"
#include "cainescrossfire.h"
#include "cainescrossfire_internal.h"	/* cd2GetStats */
#include "cars.h"
#include "main.h"		/* FrameCnt */
#include "pad.h"		/* MPAD_* */
#include "players.h"		/* player[] */
#include "overlay.h"		/* FelonyBar, COLOUR_BAND - the Turbo bar */
#include "jericho.h"

#include "turbo/turbo.h"

typedef struct CD2_TURBO_STATE
{
	int active;		// latched on right now
	int reverse;		// ...in reverse (the reverse button)
	int meter;		// frames of boost left
	int heldButton;		// the button that has to stay held (-1 = none yet)
	int tapFrame;		// frame of a first tap that opened the window (-1 = none)
	int tapButton;		// ...and which button that was
	int prevPad;		// the pad last seen for this car, for edge detection
	int inited;		// the meter has been filled once (it starts full)
	int hold;		// programmatic hold: keeps the boost on until it runs out
	int kickFrames;		// the engagement kick, counting down
	int shove;		// a one-frame impulse owed to the integrator
				// (the debug driver, and any future scripted/AI driver)
} CD2_TURBO_STATE;

static CD2_TURBO_STATE gTurbo[MAX_CARS];

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
int cd2TurboActive(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	return gTurbo[carId].active;
}

int cd2TurboReverse(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	return gTurbo[carId].active && gTurbo[carId].reverse;
}

int cd2TurboMeter(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	return gTurbo[carId].meter;
}

int cd2TurboMeterFull(void)
{
	return CD2_TURBO_METER_FRAMES;
}

// The boost itself, as a percentage the caller scales by (100 = no boost).
int cd2TurboSpeedPct(int carId)
{
	return cd2TurboActive(carId) ? CD2_TURBO_SPEED_PCT : 100;
}

int cd2TurboAccelPct(int carId)
{
	return cd2TurboActive(carId) ? CD2_TURBO_ACCEL_PCT : 100;
}

// The gearbox may rev this much further while boosting (the over-rev).
int cd2TurboRevCeiling(int carId, int ceiling)
{
	if (!cd2TurboActive(carId))
		return ceiling;

	return (int)(((long long)ceiling * (100 + CD2_TURBO_OVERREV_PCT)) / 100);
}

// ---------------------------------------------------------------------------
// refills
// ---------------------------------------------------------------------------
void cd2TurboRefill(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	gTurbo[carId].active = 0;
	gTurbo[carId].heldButton = -1;
	gTurbo[carId].tapFrame = -1;
	gTurbo[carId].meter = CD2_TURBO_METER_FRAMES;
	gTurbo[carId].inited = 1;
	gTurbo[carId].kickFrames = 0;
	gTurbo[carId].shove = 0;
}

void cd2TurboResetAll(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
		cd2TurboRefill(i);
}

// ---------------------------------------------------------------------------
// the trigger
//
// Called once per frame per car from the pad hook, with the pad word the module
// sees for that car. Everything is edge-detected here so callers just hand over
// the pad.
// ---------------------------------------------------------------------------
void cd2TurboPad(int carId, int pad)
{
	CD2_TURBO_STATE* st;
	int gasDown, brakeDown, gasHit, brakeHit, button, hit;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	st = &gTurbo[carId];

	if (!gCd2Cfg.enabled)
		return;

	/* starts full: the meter is filled the first time we see this car, and after
	 * that only by cd2TurboRefill() - it never trickles back up */
	if (!st->inited)
	{
		st->inited = 1;
		st->meter = CD2_TURBO_METER_FRAMES;
	}

	// the drive button and the reverse button
	gasDown = (pad & MPAD_CROSS) ? 1 : 0;
	brakeDown = (pad & MPAD_SQUARE) ? 1 : 0;

	gasHit = (gasDown && !(st->prevPad & MPAD_CROSS)) ? 1 : 0;
	brakeHit = (brakeDown && !(st->prevPad & MPAD_SQUARE)) ? 1 : 0;

	if (st->active)
	{
		/* hold it or lose it: the driver releasing that button ends the boost.
		 * A programmatic hold (st->hold) stands in for a held button. */
		if (!st->hold && (st->reverse ? brakeDown : gasDown) == 0)
		{
			st->active = 0;
			st->heldButton = -1;
			st->tapFrame = -1;
		}
		else if (st->meter > 0)
		{
			st->meter--;

			if (st->kickFrames > 0)
				st->kickFrames--;
		}
		else
		{
			// spent: the boost stops, and has to be earned again
			st->active = 0;
			st->heldButton = -1;
			st->tapFrame = -1;
			st->hold = 0;
		}
	}
	else
	{
		// a double tap on either drive button
		button = gasHit ? MPAD_CROSS : (brakeHit ? MPAD_SQUARE : 0);
		hit = gasHit || brakeHit;

		if (hit)
		{
			if (st->tapFrame >= 0 && st->tapButton == button &&
				(FrameCnt - st->tapFrame) <= CD2_TURBO_TAP_GRACE && st->meter > 0)
			{
				st->active = 1;
				st->reverse = (button == MPAD_SQUARE) ? 1 : 0;
				st->heldButton = button;
				st->tapFrame = -1;

				/* the kick: a shove owed to the integrator, and a nose-up pitch
				 * that settles over CD2_TURBO_KICK_FRAMES (the far wheels lifting) */
				st->shove = 1;
				st->kickFrames = CD2_TURBO_KICK_FRAMES;
			}
			else
			{
				// first tap: remember it, and wait for its partner
				st->tapFrame = FrameCnt;
				st->tapButton = button;
			}
		}
		else if (st->tapFrame >= 0 && (FrameCnt - st->tapFrame) > CD2_TURBO_TAP_GRACE)
		{
			st->tapFrame = -1;	// the window closed with no second tap
		}
	}

	st->prevPad = pad;
}

// ---------------------------------------------------------------------------
// the kick
//
// Two halves, both specified: one shove when turbo engages (a percentage of top
// speed, so the car is actually pushed and not just allowed to go faster), and a
// nose-up pitch that lifts the front like a hard stop lifting the far wheels,
// settling over CD2_TURBO_KICK_FRAMES. While the boost runs a lighter version of
// the same pitch keeps the body feeling shoved along.
// ---------------------------------------------------------------------------
int cd2TurboTakeShove(int carId)
{
	if (carId < 0 || carId >= MAX_CARS || !gTurbo[carId].shove)
		return 0;

	gTurbo[carId].shove = 0;
	return CD2_TURBO_KICK_FORCE_PCT;
}

// The pitch to add to the car's body this frame, in the same units as the roll.
int cd2TurboKickPitch(int carId)
{
	int limit, pitch;

	if (carId < 0 || carId >= MAX_CARS || !gTurbo[carId].active)
		return 0;

	limit = gCd2Cfg.rollLimit;

	if (limit <= 0)
		return 0;

	/* the engagement kick, decaying to nothing over its frames */
	pitch = 0;
	if (gTurbo[carId].kickFrames > 0)
		pitch = (limit * CD2_TURBO_KICK_PITCH_PCT * gTurbo[carId].kickFrames)
			/ (100 * CD2_TURBO_KICK_FRAMES);

	/* plus the lighter sustained shove that makes it read as being pushed */
	pitch += (limit * CD2_TURBO_SHAKE_PCT) / 100;

	return pitch;
}

// ---------------------------------------------------------------------------
// the Turbo bar
//
// The Felony bar is replaced, not supplemented: the module owns FelonyBar's
// position, colour and tag, and the engine draws it in the usual place. That
// keeps the whole thing out of the engine's render path and means the bar lands
// exactly where the player already looks for one.
//
// The crime value behind it (main.c's per-frame assignment) is deliberately left
// alone - it still feeds the felony checks, it just is not what the bar shows.
// ---------------------------------------------------------------------------
static COLOUR_BAND sBarWhite[1] = { { { 255, 255, 255, 0 }, 0, 0 } };
static COLOUR_BAND sBarRed[1]   = { { { 255, 0, 0, 0 }, 0, 0 } };
static int sBarPulse;

// Called once per frame, at the overlay draw (the last word before the engine
// renders it, so the crime assignment above cannot overwrite it).
void cd2TurboBarTick(int carId)
{
	if (carId < 0 || carId >= MAX_CARS || !gCd2Cfg.enabled)
		return;

	sBarPulse++;

	FelonyBar.tag = "Turbo";
	FelonyBar.active = 1;
	FelonyBar.max = (u_short)CD2_TURBO_METER_FRAMES;
	FelonyBar.position = (u_short)gTurbo[carId].meter;

	/* white while it is simply there; pulsing white <-> red at the specified rate
	 * while the boost is being spent */
	if (gTurbo[carId].active && ((sBarPulse / CD2_TURBO_BAR_PULSE_FRAMES) & 1))
		FelonyBar.pColourBand = sBarRed;
	else
		FelonyBar.pColourBand = sBarWhite;
}

/* the draw-overlay hook: the bar's last word before the engine renders it */
int cd2TurboOnDrawOverlay(void* ud, void* args)
{
	(void)ud;
	(void)args;

	cd2TurboBarTick((player[0].playerCarId >= 0) ? player[0].playerCarId : 0);
	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// the debug driver's view of it
// ---------------------------------------------------------------------------
void cd2TurboDump(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	/* the numbers the boost actually produces, not just the flag: the sim caps
	 * speed and acceleration at these, so a boost is visible in them */
	{
		CAR_DATA* cp = &car_data[carId];
		CD2_STATS s = cd2GetStats(cp);

		jer_log("[cainescrossfire] turbo car=%d %s%s meter=%d/%d (%.1fs left) topSpeed=%d accel=%d bar=%d/%d %s\n",
			carId,
			gTurbo[carId].active ? "ACTIVE" : "idle",
			(gTurbo[carId].active && gTurbo[carId].reverse) ? " reverse" : "",
			gTurbo[carId].meter, CD2_TURBO_METER_FRAMES,
			(float)gTurbo[carId].meter / 30.0f,
			s.topSpeed, s.accel,
			FelonyBar.position, FelonyBar.max,
			(FelonyBar.pColourBand == sBarRed) ? "red" : "white");
	}
}

// Force it on/off, for a headless check (the debug driver's turbo step).
void cd2TurboForce(int carId, int on)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	if (on)
	{
		if (gTurbo[carId].meter <= 0)
			gTurbo[carId].meter = CD2_TURBO_METER_FRAMES;

		gTurbo[carId].active = 1;
		gTurbo[carId].reverse = 0;
		gTurbo[carId].hold = 1;		/* keep it on so the meter can be watched */
		gTurbo[carId].shove = 1;
		gTurbo[carId].kickFrames = CD2_TURBO_KICK_FRAMES;
	}
	else
	{
		gTurbo[carId].active = 0;
		gTurbo[carId].heldButton = -1;
		gTurbo[carId].hold = 0;
	}

	cd2TurboDump(carId);
}
