#ifndef TURBO_H
#define TURBO_H

/*
 * turbo/turbo.h -- every number the turbo feature uses.
 *
 * The game steps at a fixed 30 Hz, so timings are given in frames with the real
 * duration in the comment (the convention the rest of the module's tuning uses).
 *
 * What this describes: double-tapping a drive button latches turbo ON for that
 * car. While it is on, the car gets 1.25x top speed and 1.25x acceleration, is
 * allowed to over-rev, kicks its body as it engages (and keeps a lighter kick
 * going while it lasts), and shows bright sparks and flames at the exhaust. It
 * stays on until the meter runs out or the driver releases that drive button.
 * Reverse works the same way, on the reverse button. The meter refills only on
 * the event in the last section.
 */

// ---------------------------------------------------------------------------
// The boost
// ---------------------------------------------------------------------------
// 1.25x, as percentages, so the arithmetic stays integer like the rest of the
// module's tuning (CD2_TOP_SPEED / CD2_ACCEL in cainescrossfire.h).
#define CD2_TURBO_SPEED_PCT	125	// top speed, and the derived reverse speed
#define CD2_TURBO_ACCEL_PCT	125	// acceleration / engine force

// How much further the engine may rev while boosting, as a percentage of the
// gearbox's own ceiling. This is the over-rev: it does not make the car faster by
// itself (the speed cap does that) -- it lets the car sound and behave like it is
// being pushed past its limit.
// How far toward FREE-REVVING the turbo may carry the engine, as a percentage.
//
// Not a percentage of the normal ceiling - a percentage of the way to where the
// note goes when the wheels are off the ground and nothing is clamping it, which
// is a good deal higher than the normal maximum.
//
// CD2_REV_FULL_REVS is that unclamped level, and it is not a new number: the
// module's hard clamp is already expressed as a fraction of it (cainescrossfire.h).
#define CD2_TURBO_REV_FREE_PCT	75

// ...and then THAT figure is raised by this much. 165 = the boost ceiling itself is
// 65% higher than the 75% figure above produces (21850 -> ~36000), which deliberately
// carries the note past the airborne revs - the engine is meant to sound like it is
// being over-revved against a limiter it cannot reach, not merely wound out.
//
// Watch the headroom when raising this: the pitch the SPU is handed saturates at
// 0x3FFF, and CD2_TURBO_PITCH_BOOST is added on top of whatever this produces.
#define CD2_TURBO_REV_EXTRA_PCT	165

// How much higher the engine note rides while boosting, in SPU pitch units
// (4096 = the nominal pitch). The over-rev above lets it wind higher; this makes
// sure the note is actually heard doing it.
#define CD2_TURBO_PITCH_BOOST	420	// ~10% up

// ---------------------------------------------------------------------------
// The meter
// ---------------------------------------------------------------------------
// Full at the start of a match. 20 seconds of boost from full: 600 frames at
// 30Hz. Depletion is cumulative -- it drains only while boosting, and the
// behaviour stops once it is empty.
#define CD2_TURBO_METER_FRAMES	600

// ---------------------------------------------------------------------------
// The trigger
// ---------------------------------------------------------------------------
// "Double tap in rapid succession": a second press of the same drive button
// arriving within this many frames of the previous one. 0.4s at 30Hz.
#define CD2_TURBO_TAP_GRACE	12

// ---------------------------------------------------------------------------
// The kick
// ---------------------------------------------------------------------------
// The engagement kick: the body is pitched as if the car had braked hard enough
// to lift its far wheels, which is the Twisted Metal "boost shove". Pitch is an
// angular acceleration, so this is expressed against the same limiter the car's
// roll uses (CD2_ROLL_LIMIT_DEFAULT, cainescrossfire.h) rather than as a raw
// fixed-point number nobody can calibrate.
#define CD2_TURBO_KICK_FRAMES		6	// how long it takes to settle back (0.2s)

// The shove itself, as a percentage of top speed, applied along the car's
// heading as turbo engages -- so it reads as one push, not a sustained extra
// engine.
#define CD2_TURBO_KICK_FORCE_PCT	6

// How hard the engage knocks the car, as a percentage of the impulse that exactly
// reaches the knock's ceiling (CD2_KNOCK_IMPULSE_TO). Over 100 on purpose: a
// wheelie should ARRIVE at the top rather than creep toward it, and expressing it
// this way means changing the knock's rates does not quietly make the wheelie
// smaller - which is exactly what happened when this was a bare percentage of the
// ceiling.
// The turbo's own note, played once when it engages. The engine already climbs in
// pitch while the boost is held, which reads as effort; this is the thing HAPPENING -
// a sample of its own, so a boost is heard rather than only inferred from the engine
// going higher. SFX 12 is the swept siren, which at this pitch is a dull swell rather
// than a siren. The dials are the sample, the volume, then the pitch.
#define CD2_SND_TURBO_SAMPLE	12		/* SOUND_BANK_SFX */
#define CD2_SND_TURBO_VOLUME	0		/* 0 = unattenuated */
#define CD2_SND_TURBO_PITCH	1400		/* 4096 = normal; low turns the sweep into a swell */

// The exhaust flame (Jaret: much larger and denser)
#define CD2_TURBO_FLAME_SIZE	60		/* was 18 */
#define CD2_TURBO_FLAME_LIFE	45
#define CD2_TURBO_FLAME_SPARKS	12		/* was 3 */

#define CD2_TURBO_KICK_KNOCK_PCT	120

// How far back the weight goes when it engages. A wheelie is the weight moving
// over the back wheels as much as the nose coming up, and without this the car just
// rotates on the spot. Negative is backwards, along the car.
#define CD2_TURBO_KICK_SHIFT		52

// While turbo is running, a lighter version of the same pitch keeps the car
// feeling shoved along. 0 disables it.

// ---------------------------------------------------------------------------
// The bar
// ---------------------------------------------------------------------------
// Replaces the Felony bar. White while the meter is simply full, and pulsing
// between white and red while boost is being spent: two full pulses per second,
// i.e. one half of the cycle every 15 frames.
#define CD2_TURBO_BAR_PULSE_FRAMES	15

// ---------------------------------------------------------------------------
// The refill event
// ---------------------------------------------------------------------------
// The meter does NOT trickle back. It comes back on these events only, which the
// module hooks anyway:
#define CD2_TURBO_REFILL_ON_RESPAWN	1	// the car is respawned (wreck/recycle)
#define CD2_TURBO_REFILL_ON_LEVEL	1	// a level/match starts

// ---------------------------------------------------------------------------
// the API (turbo/turbo.c)
// ---------------------------------------------------------------------------
// Timing and the meter live in turbo.c; callers just ask:
int cd2TurboActive(int carId);		// boosting right now
int cd2TurboReverse(int carId);		// ...and it is a reverse boost
int cd2TurboMeter(int carId);		// frames of boost left
int cd2TurboMeterFull(void);		// meter size, for a bar

// What to scale by (100 = no boost at all):
int cd2TurboSpeedPct(int carId);
int cd2TurboAccelPct(int carId);

// The over-rev: the caller passes its rev ceiling, this returns the one to use.
int cd2TurboRevCeiling(int carId, int ceiling);

// The kick. cd2TurboTakeShove returns a percentage of top speed ONCE per
// engagement (0 otherwise); cd2TurboKickPitch is the body pitch to add this frame.
int cd2TurboTakeShove(int carId);

// The engagement edge: 1 ONCE, on the frame the boost latches, so the note plays once
// rather than every frame the boost is held.
int cd2TurboTakeEngage(int carId);
// The buck itself is the knock's job (knock/knock.h), not the turbo's.

// Feed the pad once per frame per car (the pad hook does). Edge-detected there.
void cd2TurboPad(int carId, int pad);

// The meter, on its refill events only - it never trickles back up.
void cd2TurboRefill(int carId);
void cd2TurboResetAll(void);

// The bar: the module owns FelonyBar's position/colour/tag, and the engine draws
// it. Called from the draw-overlay hook so nothing else can overwrite it.
void cd2TurboBarTick(int carId);
int cd2TurboOnDrawOverlay(void* ud, void* args);

// For the debug driver: force it on/off, and log the state.
void cd2TurboForce(int carId, int on);
void cd2TurboDump(int carId);

#endif /* TURBO_H */
