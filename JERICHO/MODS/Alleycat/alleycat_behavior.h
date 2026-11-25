#ifndef ALLEYCAT_BEHAVIOR_H
#define ALLEYCAT_BEHAVIOR_H

/* ------------------------------------------------------------------
 * alleycat_behavior.h -- the modular behavior table.
 *
 * A behavior is one entry point: "given the player's car, produce this
 * frame's pad". That is the whole contract, so a new way of driving is a new
 * function and a table row - no other file changes. The active behavior is the
 * config `mode` (see ALLEYCAT_MODE_*).
 * ------------------------------------------------------------------ */

#include "driver2.h"	/* CAR_DATA */

/* Behavior ids (also the config `mode` value; the table below is the menu order). */
#define ALLEYCAT_MODE_MANUAL	0	/* no pad injection: the car is the player's */
#define ALLEYCAT_MODE_JOYRIDE	1	/* drive around at random, on the streets */
#define ALLEYCAT_MODE_EVADE	2	/* wander at a pace, keep clear of the police */
#define ALLEYCAT_MODE_PURSUIT	3	/* chase another player's car (MP follow) */

typedef struct ALLEYCAT_BEHAVIOR
{
	int		id;
	const char*	name;
	unsigned int	(*tick)(CAR_DATA* car);	/* this frame's pad for the car */
} ALLEYCAT_BEHAVIOR;

/* The behavior for a mode id, or NULL for Manual / an unknown id. */
const ALLEYCAT_BEHAVIOR* AlleycatBehaviorFor(int mode);

/* The list, for the menu. Index 0 is Manual. */
int AlleycatBehaviorCount(void);
int AlleycatBehaviorIdAt(int index);
const char* AlleycatBehaviorNameAt(int index);

/* The display name for a mode id (falls back to "Manual"). */
const char* AlleycatBehaviorName(int mode);

/* Forget the per-level goal/threat state (called on JER_EVENT_GAME_START). */
void AlleycatBehaviorResetRun(void);

#endif /* ALLEYCAT_BEHAVIOR_H */
