#ifndef JERAI_BEHAVIOR_H
#define JERAI_BEHAVIOR_H

/* ------------------------------------------------------------------
 * jerai_behavior.h -- the modular behavior table.
 *
 * A behavior is one entry point: "given the player's car, produce this
 * frame's pad". That is the whole contract, so a new way of driving is a new
 * function and a table row - no other file changes. The active behavior is the
 * config `mode` (see JERAI_MODE_*).
 * ------------------------------------------------------------------ */

#include "driver2.h"	/* CAR_DATA */

/* Behavior ids (also the config `mode` value and the menu order). */
#define JERAI_MODE_MANUAL	0	/* no pad injection: the car is the player's */
#define JERAI_MODE_JOYRIDE	1	/* drive around at random, on the streets */

typedef struct JERAI_BEHAVIOR
{
	int		id;
	const char*	name;
	unsigned int	(*tick)(CAR_DATA* car);	/* this frame's pad for the car */
} JERAI_BEHAVIOR;

/* The behavior for a mode id, or NULL for Manual / an unknown id. */
const JERAI_BEHAVIOR* JerAiBehaviorFor(int mode);

/* The list, for the menu. Index 0 is Manual. */
int JerAiBehaviorCount(void);
int JerAiBehaviorIdAt(int index);
const char* JerAiBehaviorNameAt(int index);

/* The display name for a mode id (falls back to "Manual"). */
const char* JerAiBehaviorName(int mode);

#endif /* JERAI_BEHAVIOR_H */
