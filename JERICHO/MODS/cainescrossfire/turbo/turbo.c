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
	int speedAtEngage;	// the car's speed the moment the boost engaged
	int kickLeft;		// frames of the measurement window still to run
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
// [D] [T]
void cd2TurboPad(int carId, int pad)
{
	CD2_TURBO_STATE* st;
	int gasDown, revDown, gasHit, revHit, button, hit;
	int gasBit, revBit;

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

	/* WHICH button is the gas is the MODULE's business, not the engine's. With
	 * TMB buttons - the driving scheme this mod uses - SQUARE is the gas and
	 * CIRCLE is the brake (cainescrossfiresim.c rewrites the pad that way, AFTER
	 * this runs). Reading the engine's stock names instead meant watching Cross
	 * (Tight Turn, unless tmbTight is set) for the forward boost and the BRAKE
	 * for the reverse one: double-tapping the gas did nothing at all, and the
	 * only boost anyone could find came off the brake - so the turbo "only
	 * worked in reverse". */
	if (gCd2Cfg.tmbButtons)
		gasBit = gCd2Cfg.tmbTight ? MPAD_CROSS : MPAD_SQUARE;
	else
		gasBit = CAR_PAD_ACCEL;

	revBit = MPAD_CIRCLE;	/* the brake here, the handbrake stock */

	gasDown = (pad & gasBit) ? 1 : 0;
	revDown = (pad & revBit) ? 1 : 0;

	gasHit = (gasDown && !(st->prevPad & gasBit)) ? 1 : 0;
	revHit = (revDown && !(st->prevPad & revBit)) ? 1 : 0;

	if (st->active)
	{
		/* hold it or lose it: the driver releasing that button ends the boost.
		 * A programmatic hold (st->hold) stands in for a held button. */
		if (!st->hold && (st->reverse ? revDown : gasDown) == 0)
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
		// a double tap on the gas, or on CIRCLE (the brake) for a reverse boost
		button = gasHit ? gasBit : (revHit ? revBit : 0);
		hit = gasHit || revHit;

		if (hit)
		{
			/* FrameCnt restarts at every level: a tap remembered from the previous
			 * one would otherwise read as a negative difference, i.e. inside the
			 * window, and latch instantly on the first press of the new level. */
			if (st->tapFrame >= 0 && FrameCnt >= st->tapFrame && st->tapButton == button &&
				(FrameCnt - st->tapFrame) <= CD2_TURBO_TAP_GRACE && st->meter > 0)
			{
				st->active = 1;
				st->reverse = (button == revBit) ? 1 : 0;
				st->heldButton = button;
				st->tapFrame = -1;

				/* remember where the car was, so the kick can be sized by what
				 * the boost actually does to it (see the measurement at the end) */
				st->speedAtEngage = car_data[carId].hd.speed;
				st->kickLeft = CD2_TURBO_KICK_FRAMES;

				/* the kick: a shove owed to the integrator, and a knock that throws
				 * the weight. An IMPULSE, so the spring eases it in and settles it
				 * instead of the car snapping to a new attitude.
				 *
				 * A REVERSE boost gets the mirror of the forward one: the nose goes
				 * DOWN and the weight goes FORWARD, because that is what launching
				 * backwards looks like. Handing it the forward wheelie was giving it
				 * the gas button's gesture rather than its own. */
				st->shove = 1;

				jer_log("[cainescrossfire] turbo engage car=%d (%s)\n",
					carId, st->reverse ? "reverse" : "forward");
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

	/* --- the measured kick -------------------------------------------------
	 * How hard the boost throws the weight is decided by what it ACTUALLY did:
	 * the speed the car gained over the window since it engaged. A boost from a
	 * crawl pitches the body hard; one that barely changes an already-fast car
	 * hardly moves it. A flat impulse slammed every engagement identically,
	 * which is the "always as strong as it doesn't need to be" this replaces. */
	if (st->kickLeft > 0 && --st->kickLeft == 0)
	{
		CAR_DATA* cp = &car_data[carId];
		int gain = cp->hd.speed - st->speedAtEngage;
		int ceiling = (CD2_KNOCK_IMPULSE_TO(CD2_KNOCK_MAX_PITCH) * CD2_TURBO_KICK_KNOCK_PCT) / 100;
		int floor = (ceiling * CD2_TURBO_KICK_MIN_PCT) / 100;
		int mag;

		if (st->reverse)
			gain = -gain;
		if (gain < 0)
			gain = 0;

		mag = (ceiling * gain) / CD2_TURBO_KICK_FULL_GAIN;
		if (mag > ceiling)
			mag = ceiling;
		if (mag < floor)
			mag = floor;

		cd2KnockAdd(carId, st->reverse ? -mag : mag, 0, 0,
			st->reverse ? 0 : 1,
			st->reverse ? CD2_TURBO_KICK_SHIFT : -CD2_TURBO_KICK_SHIFT);

		jer_log("[cainescrossfire] turbo kick car=%d gain=%d mag=%d of %d\n",
			carId, gain, mag, ceiling);
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
		gTurbo[carId].speedAtEngage = car_data[carId].hd.speed;
		gTurbo[carId].kickLeft = CD2_TURBO_KICK_FRAMES;
	}
	else
	{
		gTurbo[carId].active = 0;
		gTurbo[carId].heldButton = -1;
		gTurbo[carId].hold = 0;
	}

	cd2TurboDump(carId);
}
