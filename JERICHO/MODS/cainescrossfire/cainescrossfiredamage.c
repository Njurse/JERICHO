// cainescrossfiredamage.c — Combat D2: damage scaling.
//
// (Split out of cainescrossfire.c; see cainescrossfire_internal.h for the file map.)
//
// Both damage paths route through cd2ScaleDamage so the player and the AI
// opponents are treated identically: a scenery knock (JER_EVENT_DAMAGE_SCALE,
// fired per wall/building hit) and car-vs-car (JER_EVENT_CAR_VS_CAR, fired once
// per colliding pair). cd2SceneryHits also ticks the per-car contact counter
// the traffic tumble watches.

#include "driver2.h"
#include "cainescrossfire.h"
#include "cainescrossfire_internal.h"
#include "cars.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"
#include "ai/ai.h"	/* cd2AiIsOpponent, via cd2IsTraffic/cd2OwnsCar */
#include "profiles/profile.h"	/* the vehicle profiles (Armor stat) */

// Shared damage rediuction: `value` scaled to `pct` percent (both damage hooks
// route through this so player and opponent cars are treated identically).
int cd2ScaleDamage(int value, int pct)
{
	return (value * pct) / 100;
}

// The profile's Armor stat (1..5) as the percentage of incoming damage the car
// takes: armor 3 = 100%, 5 = 70%, 1 = 130%. Applied on every damage path that
// hits the car (scenery and car-to-car), so a heavy-armor profile really does
// soak hits. Returns 100 for a car with no profile.
static int cd2VehArmorPct(void* vcar)
{
	const CD2_VEH_PROFILE* p = cd2VehDef(cd2VehOfCar(vcar));

	if (p == NULL)
		return 100;

	return jer_clamp_int(100 + (3 - p->armor) * 15, 25, 200);
}

// Scenery impacts taken by `car` this level (see cd2OnDamageScale).
int cd2SceneryHits(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return 0;

	return gCd2SceneryHits[cp->id];
}

// Scenery (building/wall) damage scale: soften the damage a car takes from
// hitting solid objects (the momentum-absorbing walls make these hits bite
// hard). Fired from DamageCar (bcollide.c) before ApplyDamage.
int cd2OnDamageScale(void* ud, void* args)
{
	JER_ARGS_DAMAGE_SCALE* a = (JER_ARGS_DAMAGE_SCALE*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;

	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	a->result = cd2ScaleDamage(4096, gCd2Cfg.sceneryDamage);

	// Count real scenery impacts for EVERY car, not just opponents. The AI
	// readout uses it to measure "keeps smashing into walls", and the
	// traffic tumble detects a world contact from a CHANGE in it - so with
	// the increment buried in the opponent branch below, traffic could
	// never register a hit and never tumbled at all.
	if (cp->id >= 0 && cp->id < MAX_CARS)
		gCd2SceneryHits[cp->id]++;

	// THRESHOLD: below this raw strike velocity the contact is a scrape, not a
	// crash, and the car takes NO damage from it. The engine already refuses
	// anything under strikeVel 20480 (bcollide.c DamageCar); this raises the
	// bar so only a genuinely hard hit costs anything - the map is not worth
	// taking real damage over. Checked AFTER the counter above so a scrape
	// still registers as a contact for the traffic tumble.
	if (gCd2Cfg.sceneryDamageThreshold > 0 && a->impact < gCd2Cfg.sceneryDamageThreshold)
	{
		a->result = 0;

		if (gCd2Cfg.debugLog)
		{
			static unsigned int t = 0;
			if ((t++ & 63) == 0)
				printInfo("[cainescrossfire] scenery dmg ignored: car=%d type=%d impact=%d < thresh=%d dmg=%d\n",
					cp->id, cp->controlType, a->impact, gCd2Cfg.sceneryDamageThreshold, cp->totalDamage);
		}

		return JER_RESULT_CONTINUE;
	}

	// an opponent that keeps clipping walls needs the extra cushion, or a
	// single corner ends its run
	if (cd2AiIsOpponent(a->car))
		a->result = cd2ScaleDamage(a->result, gCd2Cfg.aiDamageTaken);

	// Traffic takes a further half off scenery impacts.
	if (cd2IsTraffic(cp))
		a->result = cd2ScaleDamage(a->result, CD2_TRAFFIC_SCENERY_EXTRA);

	// ARMOR: the profile's Armor stat scales what the car takes, whatever the
	// source (scenery here).
	a->result = cd2ScaleDamage(a->result, cd2VehArmorPct(a->car));

	if (gCd2Cfg.debugLog)
	{
		static unsigned int t = 0;
		if ((t++ & 63) == 0)
			printInfo("[cainescrossfire] scenery dmg scale: car=%d type=%d -> %d%% (%d) impact=%d dmg=%d\n",
				cp->id, cp->controlType, gCd2Cfg.sceneryDamage, a->result, a->impact, cp->totalDamage);
	}

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_CAR_VS_CAR: retune the damage two cars exchange. An owned opponent
// uses the player damage model (not the harsher traffic multiplier), and every
// car-to-car impact is scaled down by the car-to-car nerf.
int cd2OnCarVsCar(void* ud, void* args)
{
	JER_ARGS_CAR_VS_CAR* a = (JER_ARGS_CAR_VS_CAR*)args;
	int v;
	int aggressor = 0;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	v = a->value;

	// AGGRESSOR IMMUNITY: in a car-to-car hit the car driving INTO the other
	// deals the damage, and that car should not be hurt by its own attack.
	// Compare each car's approach speed along the line between them - both
	// dots use the same vector, so they compare correctly without normalising.
	// This car's approach is `da` (positive = moving toward the other); the
	// other car's is `-db`. The faster-approaching one is the aggressor and
	// takes nothing; the car it hit still takes the full value. This is what
	// stops Deadstar dying to its own dash (a ram always reads as the
	// aggressor).
	{
		CAR_DATA* ca = (CAR_DATA*)a->car;
		CAR_DATA* ot = (CAR_DATA*)a->other;

		if (ca != NULL && ot != NULL)
		{
			long long dx = (long long)ot->hd.where.t[0] - ca->hd.where.t[0];
			long long dz = (long long)ot->hd.where.t[2] - ca->hd.where.t[2];
			long long da = (long long)ca->st.n.linearVelocity[0] * dx + (long long)ca->st.n.linearVelocity[2] * dz;
			long long db = (long long)ot->st.n.linearVelocity[0] * dx + (long long)ot->st.n.linearVelocity[2] * dz;

			aggressor = (da > -db);
		}
	}

	// the aggressor keeps its damage-dealing role but takes none of the
	// exchange itself (zeroed at the very end, after every scaling)

	if (cd2AiIsOpponent(a->car))
		v = a->playerValue;

	v = cd2ScaleDamage(v, gCd2Cfg.carCarDamage);

	// opponents take a further cut so they survive long enough to be a threat
	if (cd2AiIsOpponent(a->car))
		v = cd2ScaleDamage(v, gCd2Cfg.aiDamageTaken);

	// ARMOR: the profile's Armor stat scales what this car takes (car-to-car).
	v = cd2ScaleDamage(v, cd2VehArmorPct(a->car));

	if (gCd2Cfg.debugLog)
	{
		static unsigned int t = 0;
		if ((t++ & 63) == 0)
			printInfo("[cainescrossfire] car-car dmg: car=%d type=%d opp=%d stock=%d -> %d (pct=%d) dmg=%d\n",
				((CAR_DATA*)a->car)->id, ((CAR_DATA*)a->car)->controlType,
				cd2AiIsOpponent(a->car), a->value, v, gCd2Cfg.carCarDamage,
				((CAR_DATA*)a->car)->totalDamage);
	}

	// Exactly one of the pair being ours means this is a cainescrossfire car
	// trading paint with civilian traffic. Take 80% off.
	if (a->car != NULL && a->other != NULL &&
		    (cd2OwnsCar((CAR_DATA*)a->car) != cd2OwnsCar((CAR_DATA*)a->other)))
		v = cd2ScaleDamage(v, CD2_CAR_TRAFFIC_DAMAGE);

	// Being shunted. A cainescrossfire car punting a civ car rolls it over: the
	// roll axis is horizontal and perpendicular to the shove, so it tumbles
	// end over end along the ground instead of spinning on the spot.
	{
		CAR_DATA* ca = (CAR_DATA*)a->car;
		CAR_DATA* ot = (CAR_DATA*)a->other;
		CAR_DATA* tc = NULL;
		CAR_DATA* sc = NULL;

		if (ca != NULL && ot != NULL)
		{
			if (cd2IsTraffic(ca) && cd2OwnsCar(ot))
			{
				tc = ca;
				sc = ot;
			}
			else if (cd2IsTraffic(ot) && cd2OwnsCar(ca))
			{
				tc = ot;
				sc = ca;
			}

			if (tc != NULL)
			{
				int dx = tc->hd.where.t[0] - sc->hd.where.t[0];
				int dz = tc->hd.where.t[2] - sc->hd.where.t[2];
				int adx = ABS(dx), adz = ABS(dz);
				int al = (adx > adz) ? (adx + adz / 2) : (adz + adx / 2);
				int rate = ABS(a->strikeVel) * CD2_TRAFFIC_ROLL_RATE;
				int* av = tc->st.n.angularVelocity;

				if (rate > CD2_TRAFFIC_ROLL_MAX)
					rate = CD2_TRAFFIC_ROLL_MAX;

				if (al < 1)
					al = 1;

				// roll about the horizontal axis perpendicular to the shove
				av[0] += (int)(((long long)(-dz) * rate) / al);
				av[2] += (int)(((long long)(-dx) * rate) / al);

				if (gCd2Cfg.debugLog)
					printInfo("[cainescrossfire] traffic shove: car=%d by=%d strike=%d rate=%d\n",
						tc->id, sc->id, a->strikeVel, rate);
			}
		}
	}

	// finally, the aggressor takes none of the exchange (its own hit deals
	// FULL damage to the other car; this only spares the attacker). Applied
	// here so no earlier scaling branch can resurrect the value.
	if (aggressor)
	{
		v = 0;

		if (gCd2Cfg.debugLog)
		{
			static unsigned int t = 0;
			if ((t++ & 31) == 0)
				printInfo("[cainescrossfire] car-car aggressor: car=%d spared, other=%d (strike=%d)\n",
					((CAR_DATA*)a->car)->id, ((CAR_DATA*)a->other)->id, a->strikeVel);
		}
	}

	a->value = v;
	return JER_RESULT_CONTINUE;
}
