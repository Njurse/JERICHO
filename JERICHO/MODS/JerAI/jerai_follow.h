#ifndef JERAI_FOLLOW_H
#define JERAI_FOLLOW_H

/* ------------------------------------------------------------------
 * jerai_follow.h -- "who should the AI car chase, and how far is it".
 *
 * The joyride behavior normally picks a random road sample as its
 * destination. IN A MULTIPLAYER SESSION it targets the HOST's car instead,
 * so the AI car tails the host around the map while multiplayer is being
 * tested - and holds a clearance so it never drives into them.
 *
 * This file answers only the question "where is the thing to follow and how
 * far away is it". The behavior owns what to do with that answer (drive to
 * it, or idle when the clearance is reached).
 *
 * Two sources, in order:
 *
 *   1. THE MP HOST - when a session is live and WE are not the host. The AI
 *      drives the LOCAL machine's car, so "follow the host" means the client
 *      machine's car chases the host's car. On the host itself there is
 *      nobody to chase, so this answers 0 and the normal wander resumes.
 *   2. THE TEST LEVER - `follow_car` names a CAR_DATA index to follow
 *      unconditionally. It exists so the follow logic (approach, clearance,
 *      idle) can be exercised in SINGLE PLAYER, with no session at all.
 * ------------------------------------------------------------------ */

#include "driver2.h"	/* CAR_DATA */

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve the car the AI should follow. When there is one, returns 1 and
 * writes its world position into *x / *z and the SQUARED world distance from
 * `car` into *dist2 - as a long long, because a 32-bit squared distance wraps
 * negative on a city-sized map and a far host would then read as "right next
 * to us" (the car would idle in the distance instead of driving over).
 *
 * Returns 0 when there is nothing to follow: no session, WE are the host
 * (falls back to the normal wander), the host has no car / is on foot, or the
 * target would be our own car. Never fails on a missing module: with no
 * session the SDK calls are safe no-ops. */
int JerAiFollowTarget(CAR_DATA* car, int* x, int* z, long long* dist2);

/* Resolve the car to RUN FROM - the host-side counterpart of the follow, so that
 * a joining AI car has something to chase. Returns 1 and writes its world
 * position when there is one.
 *
 * In a session this is the NEAREST other player's car, and only on the HOST: a
 * client chases the host, so a client has nothing to run from and answers 0.
 * The `flee_car` lever names a car_data index to run from with no session at
 * all, so the escape can be exercised in single player. */
int JerAiThreatTarget(CAR_DATA* car, int* x, int* z);

/* The CLIENT's half of the multiplayer flee (config flee_host): 1 and the host's
 * position when this machine is a CLIENT in a live session and flee_host is on, 0
 * otherwise. Off by default - a joining AI car chases the host, it does not run from
 * it - and it loses to nothing but being switched off. */
int JerAiHostThreatTarget(CAR_DATA* car, int* x, int* z);

/* Resolve the nearest POLICE car when the cops are actually after this car.
 * Returns 1 and writes its position when there is one.
 *
 * Gated on the engine's own pursuit flags (`copsAreInPursuit` /
 * `CopsCanSeePlayer`), not just on a cop being in the neighbourhood - a police
 * car parked down the road is scenery, not a threat. The car to run from is a
 * `CONTROL_TYPE_PURSUER_AI` slot.
 *
 * NOTE: JerAI's `immune` option (on by default) zeroes the wanted state every
 * frame, so with immunity on the police never pursue and this never answers 1.
 * Set `immune = 0` to be chaseable. */
int JerAiPoliceThreat(CAR_DATA* car, int* x, int* z);

#ifdef __cplusplus
}
#endif

#endif /* JERAI_FOLLOW_H */
