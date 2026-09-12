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

#endif /* CD2_AI_H */
