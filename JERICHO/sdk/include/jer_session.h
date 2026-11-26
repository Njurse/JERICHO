#ifndef JER_SESSION_H
#define JER_SESSION_H

/* ------------------------------------------------------------------
 * jer_session.h — the MULTIPLAYER SESSION query contract (a CORE contract).
 *
 * A mod that changes the world (sandbox: spawned cars, cheats, the world step)
 * has to know whether a multiplayer session is running and what it allows, and it
 * must not take a car_data slot another machine uses for something else. The
 * session owner answers; nobody answering means "no session".
 *
 *   ASKED BY    sandbox
 *   ANSWERED BY mp
 *
 * Issued and answered through jer_fire, like jer_carquery.h: nothing links, and a
 * game without mp just reads the zeroed answer. The ids are in mp's block of the
 * custom-event registry (+50..+59; docs/interfaces.md), since mp owns the answer.
 * ------------------------------------------------------------------ */

#include "jericho.h"

#define JER_SESSQ_STATE		(JER_EVENT_MODULE_CUSTOM + 51)	/* "is there a session, and what may I do?" */
#define JER_SESSQ_SPAWN_SLOT	(JER_EVENT_MODULE_CUSTOM + 52)	/* "which car_data slot may I spawn a car in?" */

/* JER_SESSQ_STATE. Zero the struct before firing: an unanswered query leaves
 * active at 0, which means single player and everything is allowed. It has no
 * side effects, so it may be asked every frame. */
typedef struct JER_SESSQ_STATE_ARGS
{
	int active;		/* out: 1 = a multiplayer session is running */
	int takeARide;		/* out: the session's game mode is Take a Ride */
	int cheatsAllowed;	/* out: the host's Allow cheats rule */
	int isAdmin;		/* out: this machine is the host or an admin */
	int spawnLimit;		/* out: spawned cars allowed per player (0 = none) */
	int ownFirst;		/* out: the car_data slots this machine owns -- its own */
	int ownLast;		/*      traffic band -- or -1/-1. Anything outside them */
				/*      is another machine's: leave it alone. */
} JER_SESSQ_STATE_ARGS;

/* JER_SESSQ_SPAWN_SLOT. The session picks a free slot whose car the other machines
 * will see (mp: one in this machine's own traffic band, which it publishes like
 * the rest of its traffic). `slot` stays -1 when there is none free or no session:
 * the asker then uses its own single-player rule. `reuse` names a slot the asker
 * already holds (its previous spawn), offered back first. */
typedef struct JER_SESSQ_SPAWN_SLOT_ARGS
{
	int reuse;		/* in: the asker's current spawn slot, or -1 */
	int slot;		/* out: -1 = none */
	int synced;		/* out: 1 = other players will see a car in it */
} JER_SESSQ_SPAWN_SLOT_ARGS;

#endif /* JER_SESSION_H */
