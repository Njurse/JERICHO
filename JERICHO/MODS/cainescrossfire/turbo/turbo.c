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
#include "pad.h"		/* CAR_PAD_* (which ARE MPAD_*), MPAD_* */
#include "players.h"		/* player[] */
#include "overlay.h"		/* FelonyBar, COLOUR_BAND - the Turbo bar */
#include "jericho.h"

#include "turbo/turbo.h"
#include "knock/knock.h"		/* the wheelie when it engages (CD2_KNOCK_MAX_PITCH) */

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
	int shove;
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

// The rev ceiling to use while boosting: CD2_TURBO_REV_FREE_PCT of the way from
// the normal ceiling to a free-revving engine. Beyond the normal maximum, which is
// the point - the note has to carry past it to be heard doing it - but not all the
// way, so it does not scream.
int cd2TurboRevCeiling(int carId, int ceiling)
{
	if (!cd2TurboActive(carId) || ceiling >= CD2_REV_FULL_REVS)
		return ceiling;

	int free = ceiling + (int)(((long long)(CD2_REV_FULL_REVS - ceiling) * CD2_TURBO_REV_FREE_PCT) / 100);

	/* and then that figure is raised again, deliberately PAST the airborne revs: the note
	 * is meant to scream above anything the car can reach on its own. */
	return (int)(((long long)free * CD2_TURBO_REV_EXTRA_PCT) / 100);
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

	/* the refill is a rare event, so it is worth a line: it is the only thing
	 * that gives a spent meter back */
	jer_log("[cainescrossfire] turbo: meter refilled (car=%d)\n", carId);
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
// The kick that goes with a boost, forward or reverse. Shared rather than written twice,
// because it WAS written twice and only the pad path got the reverse mirror: a scripted
// reverse boost (cd2TurboForce(carId, 2)) still performed the forward gesture. A reverse
// launch is the mirror of a forward one - nose DOWN, weight FORWARD - and its lift is
// zero rather than negative, because the knock has no downward lift to give (knock.h).
// [D] [T]
static void cd2TurboKick(int carId, int reverse)
{
	int mag = (CD2_KNOCK_IMPULSE_TO(CD2_KNOCK_MAX_PITCH) * CD2_TURBO_KICK_KNOCK_PCT) / 100;

	if (reverse)
		cd2KnockAdd(carId, -mag, 0, 0, 0, CD2_TURBO_KICK_SHIFT);
	else
		cd2KnockAdd(carId, mag, 0, 0, 1, -CD2_TURBO_KICK_SHIFT);
}

// [D] [T]
void cd2TurboPad(int carId, int pad)
{
	CD2_TURBO_STATE* st;
	int gasDown, brakeDown, gasHit, brakeHit, button, hit;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	st = &gTurbo[carId];

	if (!gCd2Cfg.enabled)
	{
		st->prevPad = pad;	/* keep the edge state honest while it is off */
		return;
	}

	/* starts full: the meter is filled the first time we see this car, and after
	 * that only by cd2TurboRefill() - it never trickles back up */
	if (!st->inited)
	{
		st->inited = 1;
		st->meter = CD2_TURBO_METER_FRAMES;
	}

	// the drive button and the reverse button, named the way the ENGINE names them
	// (pad.h's ECarPads) rather than as raw bits: the mask the engine hands us is
	// already remapped, so this follows the player's own button config.
	gasDown = (pad & CAR_PAD_ACCEL) ? 1 : 0;
	brakeDown = (pad & CAR_PAD_BRAKE) ? 1 : 0;

	gasHit = (gasDown && !(st->prevPad & CAR_PAD_ACCEL)) ? 1 : 0;
	brakeHit = (brakeDown && !(st->prevPad & CAR_PAD_BRAKE)) ? 1 : 0;

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

			/* deliberately nothing per-frame here any more: repeated small
			 * impulses read as a wobble, and a boost should be one firm wheelie
			 * that settles, not a car shaking itself apart for 20 seconds */
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
		button = gasHit ? CAR_PAD_ACCEL : (brakeHit ? CAR_PAD_BRAKE : 0);
		hit = gasHit || brakeHit;

		if (hit)
		{
			/* FrameCnt restarts at every level: a tap remembered from the previous
			 * one would otherwise read as a negative difference, i.e. inside the
			 * window, and latch instantly on the first press of the new level. */
			if (st->tapFrame >= 0 && FrameCnt >= st->tapFrame && st->tapButton == button &&
				(FrameCnt - st->tapFrame) <= CD2_TURBO_TAP_GRACE && st->meter > 0)
			{
				st->active = 1;
				st->reverse = (button == CAR_PAD_BRAKE) ? 1 : 0;
				st->heldButton = button;
				st->tapFrame = -1;

				/* the kick: a shove owed to the integrator, and a knock that throws
				 * the weight. An IMPULSE, so the spring eases it in and settles it
				 * instead of the car snapping to a new attitude.
				 *
				 * A REVERSE boost gets the mirror of the forward one: the nose goes
				 * DOWN and the weight goes FORWARD, because that is what launching
				 * backwards looks like. Handing it the forward wheelie was giving it
				 * the gas button's gesture rather than its own. */
				st->shove = 1;

				cd2TurboKick(carId, st->reverse);
			}
			else
			{
				// first tap: remember it, and wait for its partner
				st->tapFrame = FrameCnt;
				st->tapButton = button;
			}
		}
		else if (st->tapFrame >= 0 && (FrameCnt < st->tapFrame ||
			(FrameCnt - st->tapFrame) > CD2_TURBO_TAP_GRACE))
		{
			st->tapFrame = -1;	// the window closed (or the level restarted)
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
	if (carId < 0 || carId >= MAX_CARS)
		return;

	if (!gCd2Cfg.enabled)
	{
		/* turned off mid-session: stop showing a turbo bar over a felony value.
		 * The engine re-initialises the bar at the next level. */
		FelonyBar.active = 0;
		return;
	}

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

		jer_log("[cainescrossfire] turbo car=%d %s%s meter=%d/%d (%.1fs left) topSpeed=%d accel=%d revs=%d (normal %d) bar=%d/%d %s\n",
			carId,
			gTurbo[carId].active ? "ACTIVE" : "idle",
			(gTurbo[carId].active && gTurbo[carId].reverse) ? " reverse" : "",
			gTurbo[carId].meter, CD2_TURBO_METER_FRAMES,
			(float)gTurbo[carId].meter / 30.0f,
			s.topSpeed, s.accel,
			cd2TurboRevCeiling(carId, (int)(CD2_REV_CEILING)), (int)(CD2_REV_CEILING),
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
		gTurbo[carId].reverse = (on == 2) ? 1 : 0;	/* 2 = force a REVERSE boost */
		gTurbo[carId].hold = 1;		/* keep it on so the meter can be watched */
		gTurbo[carId].shove = 1;
		cd2TurboKick(carId, gTurbo[carId].reverse);
	}
	else
	{
		gTurbo[carId].active = 0;
		gTurbo[carId].heldButton = -1;
		gTurbo[carId].hold = 0;
	}

	cd2TurboDump(carId);
}
