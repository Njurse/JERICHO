/*
 * jerai_follow.c -- who the AI car chases in multiplayer. See jerai_follow.h.
 *
 * The joyride behavior owns WHAT to do with the answer (drive to the target,
 * idle within the clearance). This file only answers "which car, and how far
 * away is it" - and it is the one place that knows about the mp module.
 *
 * It is a COMPILED-IN sibling of mp, so it calls mp's own accessors directly
 * (the build puts JERICHO/MODS on every module's include path). That is safe
 * with no session: jer_net_is_active() is a plain SDK call and only mp's
 * registry is consulted after it says a session is live.
 */
#include "jerai_internal.h"
#include "jerai_follow.h"

#include "cars.h"	/* car_data, MAX_CARS */
#include "jer_net.h"	/* jer_net_is_active / jer_net_is_host */
#include "cop_ai.h"	/* copsAreInPursuit - the engine's own "they are after us" */
#include "felony.h"	/* CopsCanSeePlayer */
#include "mp/mp.h"	/* MpGetPlayer, MP_PLAYER - the compiled-in mp module */

/* Which CAR_DATA index should be followed right now, or -1 for "nothing".
 *
 * Order matters: the single-player test lever wins, because it is exactly the
 * "pretend there is a target, with no session" case. */
static int JerAiResolveTargetCar(CAR_DATA* car)
{
	int slot;

	/* the test lever: follow a named car unconditionally (see JerAI.ini
	 * `follow_car`). It is how the approach + clearance + idle are exercised
	 * in SINGLE PLAYER, where no session exists. */
	if (gJerAi.followCar >= 0)
	{
		slot = gJerAi.followCar;
	}
	else
	{
		MP_PLAYER* host;

		if (!jer_net_is_active())
			return -1;

		/* The AI drives THIS machine's own car. If WE are the host there is
		 * nobody to chase, so answer "none" and the normal wander resumes. */
		if (jer_net_is_host())
			return -1;

		host = MpGetPlayer(0);	/* player id 0 = the host */

		/* No car yet (they have not picked), gone, or on foot: nothing to
		 * drive after this frame. */
		if (host == NULL || !host->connected || host->onFoot || host->carId < 0)
			return -1;

		slot = host->carId;
	}

	if (slot < 0 || slot >= MAX_CARS)
		return -1;

	/* never target our own car: the client's own slot and the host's can
	 * resolve to the same index in a single-player arena, and a car cannot
	 * follow itself */
	if (car != NULL && &car_data[slot] == car)
		return -1;

	return slot;
}

int JerAiFollowTarget(CAR_DATA* car, int* x, int* z, long long* dist2)
{
	int slot;
	long long dx, dz, d2;

	if (car == NULL)
		return 0;

	slot = JerAiResolveTargetCar(car);

	if (slot < 0)
		return 0;

	/* 64-bit on purpose: a 32-bit squared world distance wraps NEGATIVE on a
	 * city-sized map (past ~46,000 units), so a host across the level would read
	 * as "right next to us" and the car would idle in the distance instead of
	 * driving over. */
	dx = (long long)car_data[slot].hd.where.t[0] - (long long)car->hd.where.t[0];
	dz = (long long)car_data[slot].hd.where.t[2] - (long long)car->hd.where.t[2];
	d2 = dx * dx + dz * dz;

	/* An optional RANGE. 0 - the default - means NO LIMIT, and that is the point:
	 * the chase is CHUNKED, not truncated. The behavior clamps the plan goal to
	 * the planner's window and re-plans as the car closes, so an unlimited chase
	 * still crosses a whole city to rendezvous. A non-zero range makes the AI
	 * give the target up (and wander again) once they are farther than that. */
	if (gJerAi.followRange > 0)
	{
		long long r = (long long)gJerAi.followRange;

		if (d2 > r * r)
			return 0;
	}

	if (x != NULL) *x = car_data[slot].hd.where.t[0];
	if (z != NULL) *z = car_data[slot].hd.where.t[2];
	if (dist2 != NULL) *dist2 = d2;

	/* tell the driver this car is a DESTINATION, not traffic - see JerAiCarAhead */
	gJerAi.followCarId = slot;

	return 1;
}

/* The car to RUN FROM: the nearest other player's, on the host only. See
 * jerai_follow.h - this is the host's half of the multiplayer pairing, the
 * mirror of JerAiFollowTarget's client half. */
int JerAiThreatTarget(CAR_DATA* car, int* x, int* z)
{
	int best = -1;
	long long bestD2 = 0;
	int i;

	if (car == NULL)
		return 0;

	if (gJerAi.fleeCar >= 0)
	{
		best = gJerAi.fleeCar;
	}
	else
	{
		/* In a session only the HOST runs: a client is the one CHASING, and
		 * giving it a threat as well would have it flee from the car it was
		 * told to follow. */
		if (!jer_net_is_active() || !jer_net_is_host())
			return 0;

		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			MP_PLAYER* p = MpGetPlayer(i);
			long long dx, dz, d2;
			int slot;

			if (p == NULL || p->isLocal || !p->connected || p->onFoot)
				continue;

			slot = p->carId;

			if (slot < 0 || slot >= MAX_CARS || &car_data[slot] == car)
				continue;

			dx = (long long)car_data[slot].hd.where.t[0] - (long long)car->hd.where.t[0];
			dz = (long long)car_data[slot].hd.where.t[2] - (long long)car->hd.where.t[2];
			d2 = dx * dx + dz * dz;

			if (best < 0 || d2 < bestD2)
			{
				best = slot;
				bestD2 = d2;
			}
		}
	}

	if (best < 0 || best >= MAX_CARS)
		return 0;

	if (&car_data[best] == car)
		return 0;	/* never run from ourselves */

	gJerAi.followCarId = best;	/* a destination, not traffic - see JerAiCarAhead */

	if (x != NULL) *x = car_data[best].hd.where.t[0];
	if (z != NULL) *z = car_data[best].hd.where.t[2];

	return 1;
}

/* The nearest police car, when the police are actually after this car. See
 * jerai_follow.h - the flags are the engine's own pursuit state, so a police car
 * merely parked nearby is not a threat. */
int JerAiPoliceThreat(CAR_DATA* car, int* x, int* z)
{
	int best = -1;
	long long bestD2 = 0;
	int i;

	if (car == NULL)
		return 0;

	/* The engine's own answer to "are they after us": either a pursuit is
	 * running or a cop has line of sight. Both are cleared by JerAI's `immune`
	 * option, which is why immunity makes this unreachable (see the header). */
	if (!copsAreInPursuit && !CopsCanSeePlayer)
		return 0;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* o = &car_data[i];
		long long dx, dz, d2;

		if (o == car || o->controlType != CONTROL_TYPE_PURSUER_AI)
			continue;

		dx = (long long)o->hd.where.t[0] - (long long)car->hd.where.t[0];
		dz = (long long)o->hd.where.t[2] - (long long)car->hd.where.t[2];
		d2 = dx * dx + dz * dz;		/* 64-bit: see JerAiFollowTarget */

		if (best < 0 || d2 < bestD2)
		{
			best = i;
			bestD2 = d2;
		}
	}

	if (best < 0)
		return 0;

	gJerAi.followCarId = best;	/* a destination, not traffic - see JerAiCarAhead */

	if (x != NULL) *x = car_data[best].hd.where.t[0];
	if (z != NULL) *z = car_data[best].hd.where.t[2];

	return 1;
}
