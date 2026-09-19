#ifndef YARISBOUNCE_H
#define YARISBOUNCE_H

/* yarisbounce.h — tuning for the squishy-Yaris squash & stretch.
 *
 * The effect itself lives in yarisbounce.c; only the knobs are here so the
 * look can be retuned without reading the draw code. Every value is a plain
 * float, matching the engine's own floating-point path for this effect (the
 * physics core is 4096 fixed-point, but this is a render-only deformation).
 */

/* Body squash/stretch amplitude. 0.40 = the original WIP value: the car
 * stretches 40% taller/narrower at one extreme of the cycle and squashes
 * 40% shorter/wider at the other. It applies to the wheels too, so the whole
 * car deforms together. 0 disables the effect entirely (a useful "module on
 * but inert" check). */
#define YARIS_BOUNCE_AMP		0.40f

/* Phase advance per frame, in 4096-unit angle steps (4096 = a full circle).
 * 120 ≈ one full squash cycle per second at the fixed 30 fps sim step. This
 * is the DEFAULT: the player can retune it live (see below). */
#define YARIS_BOUNCE_PHASE_STEP		120

/* Live speed control: holding D-pad Up/Down moves the phase step by
 * STEP_RATE every frame, clamped to [STEP_MIN, STEP_MAX]. 0 turns the effect
 * off (stock geometry, no bounce); the value is saved to CONFIG/yarisbounce.ini
 * (key `phase_step`) when the pad is released, so it survives a restart. */
#define YARIS_BOUNCE_STEP_RATE		4
#define YARIS_BOUNCE_STEP_MIN		0
#define YARIS_BOUNCE_STEP_MAX		600

/* The RSIN output is eased into this range before it is normalised by 4096
 * to get the -1..1 bounce. The original used a much larger span than RSIN's
 * own range and relied on the quartic ease to clamp it - kept as-is so the
 * shape of the squash matches the WIP. */
#define YARIS_BOUNCE_SIN_MIN		(-9000.0f)
#define YARIS_BOUNCE_SIN_MAX		(9000.0f)
#define YARIS_BOUNCE_MARGIN		4500.0f

/* Vertical lift applied so the squash doesn't sink the car into the road. */
#define YARIS_BOUNCE_LIFT		(-36.0f)

/* Debug HUD: 1 = draw a live "bounce / scaleX / scaleY / speed" readout every
 * frame (the port of the engine's old UpdateBounceDisplay() overlay). Off by
 * default - it is a dev instrument, not part of the effect. */
#define YARIS_DEBUG_HUD			0

#endif /* YARISBOUNCE_H */
