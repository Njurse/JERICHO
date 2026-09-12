// ai/ai.h — Combat D2 opponent AI. The brain lives in ai/opponent.c; this
// header is the small surface the rest of the module (menu, combatd2.c) uses.
//
// States are the CD2_AI_* enum in combatd2.h (HUNT / FLEE / RECOVER / WANDER,
// plus the EVADE overlay that rides on top of whichever is active).

#ifndef CD2_AI_H
#define CD2_AI_H

#include "jericho.h"

void cd2AiRegister(JERICHO_CONTEXT* ctx);

// 1 when the prototype opponent is currently spawned.
int cd2AiActive(void);

// Human-readable name of the AI's current behaviour (for the pause menu).
const char* cd2AiStateName(void);

// Snapshot of the AI's internal decision values, for the on-screen readout /
// logging ("what is it thinking?").
typedef struct CD2_AI_DEBUG
{
	int valid;		// 1 when an opponent is spawned and being driven
	int carId;
	int state;		// CD2_AI_* base behaviour
	int evadeLeft;		// frames of the evade overlay remaining
	int heading;		// current car heading (PSX angle)
	int desired;		// heading the AI wants
	int diff;		// shortest signed heading error
	int steer;		// wheel_angle the AI wrote
	int thrust;		// thrust the AI wrote
	int speed;		// wheel_speed (scaled)
	int playerDist;		// distance to the player
	int threat;		// incoming-weapon detected this frame
	int blockedAhead;	// 1 when the forward probe hit scenery
	int padIn;		// pad the engine handed the car (should be forced to 0)
	int reverse;		// 1 while backing up (stuck recovery)
	int pivot;		// -1/0/+1 acute in-place pivot requested this frame
} CD2_AI_DEBUG;

// Fill *out with the latest AI values. Returns 1 when an opponent is active.
int cd2AiGetDebug(CD2_AI_DEBUG* out);

// 1 when `car` (a CAR_DATA*) is the opponent this module owns.
int cd2AiIsOpponent(const void* car);

#endif /* CD2_AI_H */
