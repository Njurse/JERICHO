// cainescrossfirerespawn.c — Combat D2: the wreck age-out.
//
// (Split out of cainescrossfire.c; see cainescrossfire_internal.h for the file map.)
//
// Every car the module owns (the player and the AI opponents) remembers the
// spot it started the level at; when it is wrecked (past the damage cap) it
// comes back there after gCd2Cfg.respawnDelay frames. Spawn points are a later
// feature - the start point stands in for now.
//
// Also here: the traffic tumble (civ cars that scrape the world get hurled),
// the wreck mass accounting (a wreck is quarter-weight, ref-counted per MODEL
// because ap.carCos points into the SHARED car_cosmetics[] table) and the
// casino death bang.

#include "driver2.h"
#include "mission.h"		/* wantedCar[] - the levels police car */
#include "cainescrossfire.h"
#include "cainescrossfire_internal.h"
#include "cars.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "sound.h"
#include "gamesnd.h"
#include "mc_snd.h"	/* GetMissionSound */
#include "turbo/turbo.h"		/* the meter refills on the level event, not on a timer */
#include "knock/knock.h"		/* the knock's caches reset with the level too */
#include <string.h>

extern void RebuildCarMatrix(RigidBodyState* st, CAR_DATA* cp);

// Traffic (civ) cars are cannon fodder. They get no handling override and
// are deliberately EXEMPT from the roll-over limiter, so a weapon hit or a
// wall scrape can throw them properly instead of being clamped back down.
#define CD2_TRAFFIC_TUMBLE_MIN_SPEED	60		// units/frame below which a scrape is ignored
#define CD2_TRAFFIC_TUMBLE_RATE		26000	// angular impulse per unit/frame of speed
#define CD2_TRAFFIC_TUMBLE_MAX		0x500000	// ceiling on that impulse

// ---- destroyed-car respawn ------------------------------------------------
// Every car the module owns (the player and the AI opponents) remembers the
// spot it started the level at; when it is wrecked (past the damage cap) it
// comes back there after gCd2Cfg.respawnDelay frames. Spawn points are a later
// feature - the start point stands in for now.

typedef struct CD2_RESPAWN
{
	int valid;		// home recorded
	int waiting;		// wrecked, counting down
	int timer;		// frames left until it returns
	int x, y, z;		// home position
	int dir;		// home heading
} CD2_RESPAWN;

static CD2_RESPAWN gCd2Respawn[MAX_CARS];

// A wrecked car should be light enough to get shoved around like the wreck it
// is, but ap.carCos points into the SHARED car_cosmetics[] table - one entry
// per MODEL, not per car. So a plain mass /= 4 would quarter every car of that
// model, then quarter it again if a second one died, and the first respawn
// would restore it out from under the second. Reference-count per model.
typedef struct CD2_MASS_MOD
{
	CAR_COSMETICS* cos;	// NULL when the slot is free
	int original;
	int count;		// cars currently wearing the reduced mass
} CD2_MASS_MOD;

static CD2_MASS_MOD gCd2MassMod[8];
static int gCd2DeathChannel = -1;

void cd2TrafficTumble(CAR_DATA* cp)
{
	int hits = gCd2SceneryHits[cp->id];
	int* av = cp->st.n.angularVelocity;
	int vx, vz, spd, rate, sign;

	if (hits == gCd2TrafficLastHit[cp->id])
		return;		// no fresh contact

	gCd2TrafficLastHit[cp->id] = hits;

	// raw 16.16 velocity -> an approximate units/frame
	vx = ABS(cp->st.n.linearVelocity[0]) >> 12;
	vz = ABS(cp->st.n.linearVelocity[2]) >> 12;
	spd = (vx > vz) ? (vx + vz / 2) : (vz + vx / 2);

	if (spd < CD2_TRAFFIC_TUMBLE_MIN_SPEED)
		return;		// a nudge at walking pace is not worth a barrel roll

	rate = spd * CD2_TRAFFIC_TUMBLE_RATE;

	if (rate > CD2_TRAFFIC_TUMBLE_MAX)
		rate = CD2_TRAFFIC_TUMBLE_MAX;

	if (rate > CD2_TRAFFIC_SCRAPE_ROLL)
		rate = CD2_TRAFFIC_SCRAPE_ROLL;

	// Alternate the throw per contact so a car scraping down a long wall
	// does not settle into one steady spin.
	sign = (hits & 1) ? 1 : -1;

	av[0] += sign * rate;		// pitch
	av[2] += (sign * rate) / 2;	// roll

	if (gCd2Cfg.debugLog)
		printInfo("[cainescrossfire] traffic tumble: car=%d spd=%d rate=%d sign=%d hits=%d\n",
			cp->id, spd, rate, sign, hits);
}

// Drop this car's model mass to a quarter until it respawns.
static void cd2MassQuarter(CAR_DATA* cp)
{
	int i, spare = -1;

	if (cp->ap.carCos == NULL)
		return;

	for (i = 0; i < 8; i++)
	{
		if (gCd2MassMod[i].cos == cp->ap.carCos)
		{
			gCd2MassMod[i].count++;
			return;
		}

		if (spare < 0 && gCd2MassMod[i].cos == NULL)
			spare = i;
	}

	if (spare < 0)
		return;			// table full; leave the mass alone rather than corrupt it

	gCd2MassMod[spare].cos = cp->ap.carCos;
	gCd2MassMod[spare].original = cp->ap.carCos->mass;
	gCd2MassMod[spare].count = 1;
	cp->ap.carCos->mass = gCd2MassMod[spare].original / 4;
}

// Put it back, once the last car wearing that model has respawned.
static void cd2MassRestore(CAR_DATA* cp)
{
	int i;

	if (cp->ap.carCos == NULL)
		return;

	for (i = 0; i < 8; i++)
	{
		if (gCd2MassMod[i].cos == cp->ap.carCos)
		{
			if (--gCd2MassMod[i].count <= 0)
			{
				gCd2MassMod[i].cos->mass = gCd2MassMod[i].original;
				gCd2MassMod[i].cos = NULL;
			}

			return;
		}
	}
}

// The casino bang. ExplosionSound uses GetMissionSound(29) on missions 30 and
// 35 (the casino ones) and the missile WIP reached for the same sample. Guard
// the way the engine does: a mission sample that is not resident comes back as
// 255, so fall back to the always-loaded SFX impact rather than going silent.
static void cd2DeathBang(CAR_DATA* cp)
{
	int bang = (unsigned char)GetMissionSound(29);	// char return: 0xFF arrives as -1
	int bank = SOUND_BANK_MISSION;

	if (bang == 255)
	{
		bang = 5;
		bank = SOUND_BANK_SFX;
	}

	if (gCd2DeathChannel < 0)
	{
		gCd2DeathChannel = GetFreeChannel(1);
		LockChannel(gCd2DeathChannel);
	}

	Start3DSoundVolPitch(gCd2DeathChannel, bank, bang,
		cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], 0, 3584);
}

static void cd2RespawnCar(CAR_DATA* cp, CD2_RESPAWN* r)
{
	int i;

	/* the other refill event: a wrecked and recycled car gets its boost back */
	cd2TurboRefill(cp->id);
	cd2KnockReset(cp->id);

	cp->totalDamage = 0;

	for (i = 0; i < 6; i++)
		cp->ap.damage[i] = 0;

	cp->hd.where.t[0] = r->x;
	cp->hd.where.t[1] = r->y;
	cp->hd.where.t[2] = r->z;
	cp->hd.direction = r->dir;

	for (i = 0; i < 3; i++)
	{
		cp->st.n.linearVelocity[i] = 0;
		cp->st.n.angularVelocity[i] = 0;
	}

	cp->thrust = 0;
	cp->wheel_angle = 0;
	cp->handbrake = 0;
	cp->wheelspin = 0;
	cp->hd.speed = 0;
	cp->hd.wheel_speed = 0;

	RebuildCarMatrix(&cp->st, cp);

	// reset this module's per-car handling state so nothing carries over
	gCd2Car[cp->id].yawRate = 0;
	gCd2Car[cp->id].slip = 0;
	gCd2Car[cp->id].roll = 0;
	gCd2Car[cp->id].throttle = 0;
	gCd2Car[cp->id].pivotDir = 0;
	gCd2Car[cp->id].slideTicks = 0;
	gCd2Car[cp->id].aiPivot = 0;

	cd2MassRestore(cp);	// full weight again

	printInfo("[cainescrossfire] respawn: car=%d back at start (%d,%d,%d), mass %d\n",
		cp->id, r->x, r->y, r->z, (cp->ap.carCos != NULL) ? cp->ap.carCos->mass : -1);
}

void cd2RespawnTick(CAR_DATA* cp)
{
	CD2_RESPAWN* r = &gCd2Respawn[cp->id];

	if (!cd2OwnsCar(cp))
	{
		r->valid = 0;
		r->waiting = 0;
		return;
	}

	// record the home spot the first time the car is seen alive at the start
	if (!r->valid && !cd2CarTotaled(cp))
	{
		r->valid = 1;
		r->x = cp->hd.where.t[0];
		r->y = cp->hd.where.t[1];
		r->z = cp->hd.where.t[2];
		r->dir = cp->hd.direction;
	}

	if (!gCd2Cfg.respawn)
	{
		r->waiting = 0;
		return;
	}

	if (cd2CarTotaled(cp))
	{
		if (!r->waiting)
		{
			r->waiting = 1;
			r->timer = gCd2Cfg.respawnDelay;

			// Hand the car straight to the engine as a write-off instead of
			// letting damage creep up to the cap: slam totalDamage to the
			// engine's own maximum (the value ApplyDamage clamps to) and drop
			// every control input, so the engine's lockup / kill-patrol
			// handling takes over this frame.
			cp->totalDamage = USHRT_MAX;

			// wrecked cars are lighter until they come back, and they go out with
			// the casino bang
			cd2MassQuarter(cp);
			cd2DeathBang(cp);

			printInfo("[cainescrossfire] respawn: car=%d DESTROYED (type=%d) - control stripped, mass now %d, returning in %d frames\n",
				cp->id, cp->controlType, (cp->ap.carCos != NULL) ? cp->ap.carCos->mass : -1, r->timer);
		}

		// A wreck must not keep its throttle or spin its wheels while it burns
		// down. The stock pad path re-applies these every frame (the damaged-car
		// fallback selects the handbrake, whose branch never clears wheelspin,
		// so a car that was mid-burnout when it died would keep its wheels
		// spinning), so strip the controls on EVERY waiting frame, not once.
		cp->thrust = 0;
		cp->handbrake = 0;
		cp->wheelspin = 0;
		cp->wheel_angle = 0;
	}
	else if (r->waiting)
	{
		r->waiting = 0;		// recovered some other way
	}

	if (r->waiting && r->valid && --r->timer <= 0)
	{
		cd2RespawnCar(cp, r);
		r->waiting = 0;
	}
}

// New level: forget every home.
int cd2OnGameStart(void* ud, void* args)
{
	/* Which model the LEVEL treats as its police car. The engine keeps this in the
	 * level header as wantedCar[], it differs per city, and the player picks a resident
	 * SLOT - so to drive the cop you need the slot whose model this is, which the
	 * per-car dump reports. Logged rather than guessed. */
	jer_log("[cainescrossfire] level police car: wantedCar[0]=%d wantedCar[1]=%d\n", wantedCar[0], wantedCar[1]);
	(void)ud;
	(void)args;

	/* a match is starting: no getting out of the car by choice. Only while the module
	 * is actually on, so a disabled module cannot block the engine's exit either. */
	if (gCd2Cfg.enabled)
		gBlockPlayerExit = 1;

	memset(gCd2Respawn, 0, sizeof(gCd2Respawn));
	memset(gCd2SceneryHits, 0, sizeof(gCd2SceneryHits));
	memset(gCd2TrafficLastHit, 0, sizeof(gCd2TrafficLastHit));
	memset(gCd2MassMod, 0, sizeof(gCd2MassMod));	// masses are level data, re-read on load

	/* the turbo meter's refill event: it never trickles back up, so this is the
	 * only thing that gives a player a boost at the start of a level. The knock's
	 * caches go with it - they are keyed on FrameCnt, which restarts here. */
	cd2TurboResetAll();
	cd2KnockResetAll();

	return JER_RESULT_CONTINUE;
}
