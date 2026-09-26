// ai/ai.h — Combat D2 opponent AI. The brain lives in ai/opponent.c; this
// header is the small surface the rest of the module (menu, cainescrossfire.c) uses.
//
// States are the CD2_AI_* enum in cainescrossfire.h (HUNT / FLEE / RECOVER / WANDER,
// plus the EVADE overlay that rides on top of whichever is active).

#ifndef CD2_AI_H
#define CD2_AI_H

#include "jericho.h"

// How many AI opponents a match can field at once, and how many it spawns per
// level (<= the max). The count itself is the match setting `ai_opponents`
// (CD2_CONFIG), which defaults to 0: opponents are opted into, never assumed.
#define CD2_AI_MAX		6	// maximum simultaneous opponents
#define CD2_AI_SPAWN_COUNT	6	// most opponents spawned per level (<= CD2_AI_MAX)

void cd2AiRegister(JERICHO_CONTEXT* ctx);

// Per-run RNG seed (a value that actually varies between launches, unlike the
// engine's frame-counter Random2()). Cached for the process; other AI files
// (nav.c) use it to vary their starting picks so opponents don't replay the
// same routes every launch.
unsigned int cd2AiRunSeed(void);

// Pin the run seed (0 = unpin, seed from ASLR/rdtsc as usual). Set from the
// engine's debug -seed flag via JER_EVENT_GAME_START, so a test run can be made
// reproducible and two runs diffed.
void cd2AiSetRunSeed(unsigned int seed);

// 1 when the prototype opponent is currently spawned.
int cd2AiActive(void);

// Human-readable name of the AI's current behaviour (for the pause menu).
const char* cd2AiStateName(void);

// Human-readable name of this opponent's role archetype.
const char* cd2AiRoleName(void);
const char* cd2AiRoleNameOf(int role);

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
	int damage;		// car totalDamage (health: 0 = pristine, rising = hurt)
	int hits;		// collisions observed on the opponent so far
} CD2_AI_DEBUG;

// Fill *out with the latest AI values. Returns 1 when an opponent is active.
int cd2AiGetDebug(CD2_AI_DEBUG* out);

// Hand the PLAYER's car to this AI (1) or give it back to the pad (0) - the
// cc_debug.txt `playerai:` test mode. The player's car is adopted as an AI
// contestant: it keeps CONTROL_TYPE_PLAYER and so keeps the camera, the HUD and
// the module's springs, but the AI drives it (its pad is blanked and its throttle
// and steering are written by cd2AiDrive). Driven even when the match has no
// opponents, and not counted as a spawned opponent, so the match still respawns
// its own.
void cd2AiAdoptPlayer(int on);

// 1 when `car` (a CAR_DATA*) is the opponent this module owns.
int cd2AiIsOpponent(const void* car);

// Role index of `car` (a CAR_DATA*), or -1 when it is not an opponent. Name it
// with cd2AiRoleNameOf for a display string.
int cd2AiRoleOf(const void* car);

// The car an opponent is chasing, as a world position: fill `*out` (a VECTOR*)
// and return 1, or return 0 (and leave *out) when `car` is not an opponent or
// has no live target. The mounted crew aims at this, so a ped and its car
// agree on what they are attacking.
int cd2AiTargetPos(const void* car, void* out);

#endif /* CD2_AI_H */
