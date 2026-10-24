#ifndef JER_CRUMPLE_H
#define JER_CRUMPLE_H

/* ------------------------------------------------------------------
 * jer_crumple.h — the per-car body-crush multiplier contract (a CORE contract).
 *
 * ASKED BY  mp: "what body-crush multiplier does this car carry?" (when it sends
 *                the owner's carstate), and "this is the value its owner
 *                reported" (when a peer's carstate arrives).
 * ANSWERED BY crumple, which owns the per-car multiplier and the deformation.
 *
 * It is a CONTRACT, not a function call. Issued and answered through jer_fire,
 * so a session without crumple is not an error and nothing has to link: a
 * module that does not answer leaves the caller's default (1.0x) in place.
 *
 * The ids sit in the core-contract block of the custom-event registry
 * (+45/+46). The value is per CAR_DATA slot, not per player and not per
 * machine: the pause menu edits the car the local player is in, and mp carries
 * each owner's value to the machines simulating that car.
 * ------------------------------------------------------------------ */

#include "jericho.h"

#define JER_CRUMPLE_GET_SCALE	(JER_EVENT_MODULE_CUSTOM + 45)	/* "what is this car's multiplier?" */
#define JER_CRUMPLE_SET_SCALE	(JER_EVENT_MODULE_CUSTOM + 46)	/* "this car's owner says N" */

/* 4096 = 1.0x, 40960 = 10.0x. GET fills `scale` from the car's stored value
 * (4096 when unset or when no module answers). SET writes it; a value below
 * 1.0x is treated as the 1.0x default. */
typedef struct JER_CRUMPLE_SCALE_ARGS
{
	int carId;	/* in: CAR_DATA slot */
	int scale;	/* in/out: 4096-fixed multiplier */
} JER_CRUMPLE_SCALE_ARGS;

#endif /* JER_CRUMPLE_H */
