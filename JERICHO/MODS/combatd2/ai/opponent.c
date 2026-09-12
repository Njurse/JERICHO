// ai/opponent.c — Combat D2 PROTOTYPE OPPONENT AI.
//
// A single opponent car spawned beside the player. It is driven through the
// SAME combatd2 input path a human uses (it writes cp->thrust / cp->wheel_angle
// and the combatd2 handling model moves it), so it feels exactly like the
// player's car. Behaviours:
//
//   HUNT    - chase the player, fire the MG when lined up
//   FLEE    - run from the player when badly damaged
//   RECOVER - totalled: come to rest
//   WANDER  - explore (holding a heading, looking for pickups/opponents)
//   EVADE   - an OVERLAY (rides on top of whatever else): on detecting an
//             incoming shot it dodges perpendicular to that shot
//
// Obstacle handling: a lineClear() probe ahead steers it around buildings, and
// it nudges away from other cars it is about to hit. Spawn is right beside the
// player so it is easy to watch.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "civ_ai.h"
#include "players.h"
#include "mission.h"	// residentCarModels: the per-level loaded-car table
#include "objcoll.h"
#include "convert.h"
#include "system.h"
#include "pres.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "ai/ai.h"
#include "ai/nav.h"
#include "ai/grid.h"
#include "ai/flow.h"

#include <string.h>
#include <stdio.h>

#define CD2_AI_SPAWN_OFFSET	900	// how far beside the player to spawn
#define CD2_AI_ENGAGE_RANGE	3000	// close to this and it commits to a fight
#define CD2_AI_ENGAGE_KEEP		18000	// ...and stays committed out to here (hysteresis)
#define CD2_AI_DISPERSE_TICKS	420	// how long the opening spread lasts
#define CD2_AI_DISPERSE_LEG		26000	// how far the opening spread drives
#define CD2_AI_ROAM_MIN			800	// roam goal: nearest acceptable road node
#define CD2_AI_ROAM_MAX			70000	// roam goal: furthest acceptable road node
#define CD2_AI_GOAL_TICKS		240	// frames before a roam goal is re-picked
#define CD2_AI_LOOK		480	// look-ahead probe distance
#define CD2_AI_PROBE_ANG	450	// ~35 deg side probes
#define CD2_AI_AVOID_STEER	150	// steer nudge to dodge something
#define CD2_AI_STEER_DIV	6	// heading error -> wheel_angle divisor
#define CD2_AI_EVADE_FRAMES	85	// ~1.5 s of evasive driving
#define CD2_AI_HURT_FLEE	8000	// totalDamage above which it breaks off (rare)
#define CD2_AI_FIRE_RANGE	9000	// MG range when pursuing
#define CD2_AI_FIRE_CONE	420	// heading error it will still fire through
#define CD2_AI_FIRE_COOLDOWN	1	// frames between AI MG shots
#define CD2_AI_AIM_PULL_LIMIT	1100	// only bias the heading within this error (~97 deg)
#define CD2_AI_AIM_PULL		2	// lean this fraction of the remaining error onto the target
#define CD2_AI_PRIMARY_MIN	2200	// too close to launch a missile (world units)
#define CD2_AI_PRIMARY_RANGE	14000	// furthest it will launch a missile
#define CD2_AI_STEER_RATE	148	// max wheel_angle change per frame
#define CD2_AI_PIVOT_DIFF	1150	// heading error above which it stops + pivots
#define CD2_AI_PIVOT_SPEED	70	// only pivot below this forward speed (units/frame)
#define CD2_AI_REVERSE_TICKS	22	// frames of reversing after getting stuck
#define CD2_AI_STUCK_TICKS	40	// frames with no forward progress before reversing
#define CD2_AI_STUCK_SPEED	5	// forward-speed magnitude counted as "stuck"
#define CD2_AI_WP_REACH		700	// route waypoints within this are "reached" and skipped
#define CD2_AI_LOOKAHEAD_MIN	900	// pure-pursuit lookahead at a standstill
#define CD2_AI_LOOKAHEAD_MAX	4200	// pure-pursuit lookahead cap
#define CD2_AI_LOOKAHEAD_PER_SPEED 8	// extra lookahead per unit/frame of speed
#define CD2_AI_SEPARATE_RANGE	5200	// start spreading out within this of another opponent
#define CD2_AI_SEPARATE_CLOSE	2600	// ease off the throttle if packed this tight
#define CD2_AI_SEPARATE_STEER	120	// steering nudge away from a nearby opponent
#define CD2_AI_STATE_JITTER	60	// random extra frames between behaviour re-decisions
#define CD2_AI_ENGAGE_JITTER	300	// random spread on the aggression burst length
#define CD2_AI_WANDER_LEG	40000	// wander goal distance along the wander heading
#define CD2_AI_NEAR_LOOK	380	// base imminent-collision probe distance
#define CD2_AI_LOOK_PER_SPEED	3	// extra probe distance per unit/frame of speed
#define CD2_AI_BRAKE_SPEED	60	// forward speed above which it brakes instead of pivoting
#define CD2_AI_ENGAGE_TICKS	1620	// frames of sustained aggression before breaking off
#define CD2_AI_ROAM_TICKS	1500	// frames spent roaming/hunting for weapons
#define CD2_AI_ROAM_JITTER	440	// random extra roam frames (so they desync)
#define CD2_AI_STATE_TICKS	145	// frames between behaviour re-decisions
#define CD2_AI_MIN_STATE_TICKS	150	// minimum frames any new behaviour is held
#define CD2_AI_IDLE_TICKS	200	// frames near-standstill before it must get moving
#define CD2_AI_IDLE_SPEED	15	// forward speed counted as "sitting still"
					//    (45 counted a car doing 44 units/frame as parked, which is
					//    most of why they kept deciding they were stuck)

#define CD2_AI_FAN_RAYS		5	// rays in the forward scenery fan
#define CD2_AI_FAN_STEPS	5	// length samples along each ray
#define CD2_AI_STOP_FRAMES	60	// frames of travel the AI keeps in hand
#define CD2_AI_GOVERN_SLACK	0	// speed grace before the governor bites
#define CD2_AI_SIDE_MARGIN	250	// clearance gap before it biases steering
#define CD2_AI_SIDE_BIAS	384	// heading nudge away from the closer wall
#define CD2_AI_NEAR_BLOCK_MIN	350	// room below which a wall counts as imminent at any speed

// committed imminent-collision responses (hysteresis in cd2AiDrive)
enum { CD2_AI_AVOID_NONE = 0, CD2_AI_AVOID_BRAKE, CD2_AI_AVOID_PIVOT };

#define CD2_AI_MAX		4	// maximum simultaneous opponents
#define CD2_AI_SPAWN_COUNT	4	// opponents spawned per level (<= CD2_AI_MAX)

// Per-opponent state (one slot per spawned opponent, so several can run at once
// with independent behaviour, roles and routes).
typedef struct CD2_AI_CAR
{
	int carId;
	int state, stateTimer, evade, fireTimer;
	int wanderHeading, wanderTimer;
	int steer, reverse, stuck;
	int role, avoid, avoidTicks;
	int avoidCycles;	// consecutive avoid activations (escalates to reverse)
	int engageTicks;	// frames of the current aggressive burst
	int engageLimit;	// this contestant's own aggression burst length
	int disperseTicks;	// frames left of the opening spread
	int goalX, goalZ;	// stored roam destination
	int goalTimer;		// frames before the roam goal is re-picked
	int roamTicks;	// >0 while roaming (hit-and-run break-off period)
	int logTick;		// per-opponent debug log throttle
	int hold;		// frames a behaviour must be held before switching
	int idle;		// consecutive frames spent near-standstill
	int lastDamage, hits;
	CD2_NAV_ROUTE route;
} CD2_AI_CAR;

static CD2_AI_CAR sAi[CD2_AI_MAX];
static int sAiCount;		// number of live opponents
static unsigned int sLogTick;	// debugLog throttle counter
static int sNavProbeDone;	// one-shot arbitration probe at level start
static CD2_AI_DEBUG sDbg;	// latest values of the tracked (first) opponent

// Random2() IGNORES its argument - convert.c returns a raw 16-bit value
// regardless of the range you ask for. Every "jitter up to N" must reduce it
// here, or you get values in the tens of thousands (an "up to 210 frames"
// spread once came out as 28822 frames).
static int cd2AiRand(int n)
{
	if (n <= 0)
		return 0;

	return Random2(n) % n;
}

// A weapon that leaves fireCone unset falls back to the old global tolerance
// rather than becoming unfireable.
static int cd2AiCone(const CD2_WEAPON_DEF* d)
{
	if (d == NULL)
		return 0;

	return (d->fireCone > 0) ? d->fireCone : CD2_AI_FIRE_CONE;
}

// Random2() is a pure function of the frame counter, so every call made in the
// SAME frame returns the same value. All the opponents spawn on one frame, so
// they were all drawing the identical "random" model and colour. Mix in a
// per-caller salt (each spawner's index) to break the tie within a frame.
static int cd2AiRandSalt(int n, int salt)
{
	unsigned int mix;

	if (n <= 0)
		return 0;

	mix = (unsigned int)salt * 2654435761u;

	return (int)(((unsigned int)Random2(n) ^ (mix >> 7)) % (unsigned int)n);
}

static CD2_AI_CAR* cd2AiSlot(int carId)
{
	int i;

	for (i = 0; i < CD2_AI_MAX; i++)
		if (sAi[i].carId == carId)
			return &sAi[i];

	return NULL;
}

// Nearest thing worth attacking: the player, or another opponent. Returns the
// car id (-1 when alone) and copies its position into *out.
static int cd2AiFindTarget(CAR_DATA* cp, VECTOR* out)
{
	int i, best = -1;
	long long bestD = 0;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* o = &car_data[i];
		long long dx, dz, d2;

		if (o == cp || o->controlType == CONTROL_TYPE_NONE)
			continue;

		if (!(o->controlType == CONTROL_TYPE_PLAYER || cd2AiIsOpponent(o)))
			continue;

		dx = o->hd.where.t[0] - cp->hd.where.t[0];
		dz = o->hd.where.t[2] - cp->hd.where.t[2];
		d2 = dx * dx + dz * dz;

		if (best < 0 || d2 < bestD)
		{
			bestD = d2;
			best = i;
		}
	}

	if (best >= 0 && out != NULL)
	{
		out->vx = car_data[best].hd.where.t[0];
		out->vy = car_data[best].hd.where.t[1];
		out->vz = car_data[best].hd.where.t[2];
	}

	return best;
}

extern void DrawTargetBlip(VECTOR* pos, unsigned char r, unsigned char g, unsigned char b, int flags);

static int cd2AiSqrt(int v)
{
	int r = 0;
	int bit = 1 << 30;

	if (v <= 0)
		return 0;

	while (bit > v)
		bit >>= 2;

	while (bit != 0)
	{
		if (v >= r + bit)
		{
			v -= r + bit;
			r = (r >> 1) + bit;
		}
		else
			r >>= 1;

		bit >>= 2;
	}

	return r;
}

static void cd2AiPointAt(const CAR_DATA* cp, int heading, int dist, VECTOR* out)
{
	// Engine convention: a heading maps to a direction as (x = RSIN, z = RCOS)
	// (see civ_ai.c InitCar velocity). These were swapped, so every look-ahead
	// probe pointed in a mirrored direction - the AI was blind to scenery that
	// was not almost dead ahead.
	out->vx = cp->hd.where.t[0] + (int)(((long long)RSIN(heading) * dist) >> 12);
	out->vy = cp->hd.where.t[1];
	out->vz = cp->hd.where.t[2] + (int)(((long long)RCOS(heading) * dist) >> 12);
}

static void cd2AiCarPos(const CAR_DATA* cp, VECTOR* out)
{
	out->vx = cp->hd.where.t[0];
	out->vy = cp->hd.where.t[1];
	out->vz = cp->hd.where.t[2];
}

static int cd2AiSpawnOne(CAR_DATA* pcp, int index)
{
	CAR_DATA* slot = NULL;
	LONGVECTOR4 pos;
	VECTOR ppos, cand;
	static char cd2AiPadId = 0;	// CUTSCENE car pad id (no replay stream)
	int i, k, side, chosen = 0;
	int chosenModel = 0, chosenPalette = 0;
	int off = CD2_AI_SPAWN_OFFSET * (index + 1);	// fan each opponent out

	cand.vx = 0;
	cand.vy = 0;
	cand.vz = 0;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (car_data[i].controlType == CONTROL_TYPE_NONE)
		{
			slot = &car_data[i];
			break;
		}
	}

	if (slot == NULL)
		return 0;

	cd2AiCarPos(pcp, &ppos);

	// Probe BOTH sides at this opponent's fan distance (then a little further
	// out) and spawn on whichever is clear, so we never drop a car inside a
	// wall. lineClear: 0 = blocked.
	for (k = 0; k < 5 && !chosen; k++) // Upped test to five opponents for a total of six contestants
	{
		int d = off + k * CD2_AI_SPAWN_OFFSET;

		for (side = 1; side >= -1; side -= 2)
		{
			cand.vx = ppos.vx + (int)(((long long)pcp->hd.where.m[0][0] * d * side) >> 12);
			cand.vy = ppos.vy;
			cand.vz = ppos.vz + (int)(((long long)pcp->hd.where.m[2][0] * d * side) >> 12);

			if (lineClear(&ppos, &cand))
			{
				chosen = 1;
				break;
			}
		}
	}

	if (!chosen)
		return 0;	// boxed in - try again next frame

	pos[0] = cand.vx;
	pos[1] = cand.vy;
	pos[2] = cand.vz;
	pos[3] = 0;

	// CUTSCENE control: combatd2 drives it entirely from the inputs we write,
	// and (unlike CIV_AI) there is NO stock traffic AI to fight us - no
	// road-node snapping and no CheckPingOut() reset when it leaves the road
	// graph, which is what made the civ-AI car fidget in place.
	//
	// --- car and colour variety, chosen from what this level actually loaded.
	//
	// InitCar's model argument is a SLOT index (0..MAX_CAR_RESIDENT_MODELS-1)
	// into the per-level resident-car tables - NOT a global model id. A level
	// need not use every slot, and an unused slot has NULL model pointers which
	// fault the moment the car is drawn or dented. The engine's own traffic
	// spawner guards on exactly this (civ_ai.c PingInCivCar: if
	// gCarCleanModelPtr[model] == NULL, bail). So ENUMERATE the loaded slots
	// instead of guessing one at random, which is why every opponent previously
	// came out as the player's car.
	{
		int loaded[MAX_CAR_RESIDENT_MODELS];
		int n = 0;
		int model, palette = 0;
		int i;

		for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		{
			if (gCarCleanModelPtr[i] != NULL && gCarDamModelPtr[i] != NULL &&
			    i != pcp->ap.model)
				loaded[n++] = i;
		}

		if (n == 0)
			model = pcp->ap.model;		// the only car this level loaded
		else
			model = loaded[cd2AiRandSalt(n, index + 1)];

		// Palette 0..5 for the recolourable civ bodies, 0 for single-palette
		// ones (cop / special); index 0 is the original colour. Same rule the
		// engine applies in PingInCivCar. NPC scar palette: a CUTSCENE car DOES
		// take InitCar's palette argument - only CIV_AI overrides it from
		// extraData, which is why ours never varied.
		if (residentCarModels[model] != 0 && residentCarModels[model] <= 4)
			palette = cd2AiRandSalt(6, index * 31 + 17);

		InitCar(slot, pcp->hd.direction, &pos, CONTROL_TYPE_CUTSCENE, model, palette, &cd2AiPadId);

		chosenModel = model;
		chosenPalette = car_data[slot->id].ap.palette;

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] AI car variety: slot=%d ext=%d palette=%d (%d other loaded slots)\n",
				model, residentCarModels[model], chosenPalette, n);
	}

	// claim an AI slot for it
	{
		CD2_AI_CAR* A = NULL;
		int s;

		for (s = 0; s < CD2_AI_MAX; s++)
		{
			if (sAi[s].carId < 0)
			{
				A = &sAi[s];
				break;
			}
		}

		if (A == NULL)
			return 0;	// no free slot

		A->carId = slot->id;
		A->state = CD2_AI_DISPERSE;	// spawn scattered, not in formation
		A->stateTimer = 0;
		A->evade = 0;
		A->fireTimer = 0;
		A->steer = 0;
		A->reverse = 0;
		A->stuck = 0;
		A->avoid = 0;
		A->avoidTicks = 0;
		A->avoidCycles = 0;
		A->lastDamage = 0;
		A->hits = 0;
		A->wanderHeading = pcp->hd.direction;
		A->wanderTimer = 60;
		A->role = (gCd2Cfg.aiRole >= 0) ? gCd2Cfg.aiRole : (index % CD2_AI_ROLE_COUNT);
		A->route.count = 0;
		A->route.source = CD2_NAV_SRC_NONE;

		// Opening move: they spawn clustered beside the player, and a
		// cluster that keeps formation is one target instead of three. Send
		// each contestant out on its own fanned heading, jittered so they
		// do not all leave on the same bearing.
		A->state = CD2_AI_DISPERSE;
		A->disperseTicks = CD2_AI_DISPERSE_TICKS + cd2AiRand(CD2_AI_DISPERSE_TICKS / 2);
		A->wanderHeading = (car_data[A->carId].hd.direction + index * 1000 + cd2AiRand(600)) & 0xfff;

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] AI opponent spawned (car=%d slot=%d role=%d side=%d model=%d palette=%d)\n",
				A->carId, s, A->role, side, chosenModel, chosenPalette);
	}

	return 1;
}

static int cd2AiSpawn(void)
{
	CAR_DATA* pcp = NULL;
	int i, n = 0;

	if (!cd2WpnPlayerCar(&pcp))
		return 0;

	for (i = 0; i < CD2_AI_MAX; i++)
		sAi[i].carId = -1;

	for (i = 0; i < CD2_AI_MAX && i < CD2_AI_SPAWN_COUNT; i++)
		n += cd2AiSpawnOne(pcp, i);

	sAiCount = n;

	return n;
}

static void cd2AiDrive(CAR_DATA* cp, CD2_AI_CAR* A)
{
	CAR_DATA* pcp = NULL;
	VECTOR tpos;
	int desired, diff, steer, thrust;
	int fx, fz, rx, rz;
	int threatFlag = 0, blockedAhead = 0;
	int speedFwd = 0, pivotDir = 0;
	int clearAhead = 0, clearL = 0, clearR = 0, nearBlocked = 0, dodgeDir = 0;
	int freeRun[CD2_AI_FAN_RAYS], freeL = 0, freeR = 0, freeAhead = 0;
	int probeStep = 0;	// length of one fan segment (governor uses it as margin)
	int governed = 0;
	int crowded = 0;	// packed against another opponent (ease off)
	int targetId = -1;
	long long targetD2 = 0;
	VECTOR carV, goalV, targetV;

	// this opponent's state, aliased so the body below reads naturally
#define sState		A->state
#define sStateTimer	A->stateTimer
#define sEvade		A->evade
#define sFireTimer	A->fireTimer
#define sWanderHeading	A->wanderHeading
#define sWanderTimer	A->wanderTimer
#define sSteer		A->steer
#define sReverse	A->reverse
#define sStuck		A->stuck
#define sRole		A->role
#define sAvoid		A->avoid
#define sAvoidTicks	A->avoidTicks
#define sAvoidCycles	A->avoidCycles
#define sEngageTicks	A->engageTicks
#define sEngageLimit	A->engageLimit
#define sDisperseTicks	A->disperseTicks
#define sGoalX		A->goalX
#define sGoalZ		A->goalZ
#define sGoalTimer	A->goalTimer
#define sRoamTicks	A->roamTicks
#define sLogTick2	A->logTick
#define sHold		A->hold
#define sIdle		A->idle
#define sLastDamage	A->lastDamage
#define sHits		A->hits
#define sRoute		A->route

	carV.vx = cp->hd.where.t[0];
	carV.vy = cp->hd.where.t[1];
	carV.vz = cp->hd.where.t[2];
	goalV = carV;

	// health/collision tracking: totalDamage only ever rises, so a change means
	// the opponent just took a hit this frame (from any source).
	if (cp->totalDamage > sLastDamage)
		sHits++;

	sLastDamage = cp->totalDamage;
	int i;

	tpos.vx = 0;
	tpos.vy = 0;
	tpos.vz = 0;

	sDbg.valid = 0;

	if (cd2CarTotaled(cp))
	{
		cp->thrust = 0;
		cp->wheel_angle = 0;
		cp->handbrake = 0;
		cp->wheelspin = 0;
		sSteer = 0;
		sReverse = 0;
		sStuck = 0;
		cd2CarSetAiPivot(cp, 0);

		sDbg.valid = 1;
		sDbg.carId = cp->id;
		sDbg.state = CD2_AI_RECOVER;
		sDbg.evadeLeft = 0;
		sDbg.heading = cp->hd.direction;
		sDbg.desired = cp->hd.direction;
		sDbg.diff = 0;
		sDbg.steer = 0;
		sDbg.thrust = 0;
		sDbg.speed = cp->hd.wheel_speed / 256;
		sDbg.playerDist = 0;
		sDbg.threat = 0;
		sDbg.blockedAhead = 0;
		sDbg.reverse = 0;
		sDbg.pivot = 0;
		sDbg.damage = cp->totalDamage;
		sDbg.hits = sHits;
		return;
	}

	cd2WpnPlayerCar(&pcp);

	fx = cp->hd.where.m[0][2];
	fz = cp->hd.where.m[2][2];
	rx = cp->hd.where.m[0][0];
	rz = cp->hd.where.m[2][0];

	// --- evade overlay: any incoming shot? ---
	if (cd2WpnIncomingThreat(cp, &tpos, NULL))
	{
		threatFlag = 1;
		sEvade = CD2_AI_EVADE_FRAMES;

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] AI evade (car=%d)\n", cp->id);
	}

	if (sEvade > 0)
		sEvade--;

	// --- pick the nearest target (the player, or another opponent) ---
	targetV.vx = 0;
	targetV.vy = 0;
	targetV.vz = 0;
	targetId = cd2AiFindTarget(cp, &targetV);

	if (targetId >= 0)
	{
		long long tx = targetV.vx - cp->hd.where.t[0];
		long long tz = targetV.vz - cp->hd.where.t[2];

		targetD2 = tx * tx + tz * tz;
	}

	// --- base state (forced, or decided from conditions) ---
	if (gCd2Cfg.aiForceState != CD2_AI_AUTO)
	{
		sState = gCd2Cfg.aiForceState;
	}
	else if (sHold > 0)
	{
		// minimum dwell: a freshly chosen behaviour sticks, so it can't ping-pong
		sHold--;
		// jittered here too - a fixed window while holding would re-sync every
		// contestant back onto the same behaviour clock
		sStateTimer = CD2_AI_STATE_TICKS + cd2AiRand(CD2_AI_STATE_JITTER);
	}
	else if (--sStateTimer <= 0)
	{
		int want;
		int engage = (sState == CD2_AI_ATTACK) ? CD2_AI_ENGAGE_KEEP : CD2_AI_ENGAGE_RANGE;

		if (sDisperseTicks > 0)
		{
			// Opening move. They spawn in a cluster and none of them has a
			// fight yet, so the useful thing to do is break formation.
			// (counted down per frame, above - not per re-decision)
			want = CD2_AI_DISPERSE;
		}
		else if (targetId >= 0 && targetD2 < (long long)engage * engage)
		{
			// Committed to the fight, and STAYING committed: the range it
			// takes to get into an attack and the range it takes to stay in
			// one are deliberately different. With a single threshold the AI
			// crossed back and forth over it every few frames and flipped
			// between hunting and driving away.
			want = CD2_AI_ATTACK;
		}
		else
		{
			want = CD2_AI_ROAM;
		}

		if (want != sState)
			sHold = CD2_AI_MIN_STATE_TICKS;

		sState = want;
		// jittered window: without this every contestant re-decides on the
		// same frame and they switch behaviour in lockstep like one actor
		sStateTimer = CD2_AI_STATE_TICKS + cd2AiRand(CD2_AI_STATE_JITTER);
	}

	// opening spread counts down in real frames
	if (sDisperseTicks > 0)
		sDisperseTicks--;

	// --- never park. Sitting still just makes it a target: short stops to
	// pivot or line up a shot are fine, a long idle is not. ---
	if (ABS(speedFwd) < CD2_AI_IDLE_SPEED && sReverse == 0 && sEvade == 0)
		++sIdle;
	else
		sIdle = 0;

	if (sIdle > CD2_AI_IDLE_TICKS)
	{
		sIdle = 0;
			sWanderHeading = (sWanderHeading + 1200 + cd2AiRand(1200)) & 0xfff;
		sAvoid = CD2_AI_AVOID_NONE;
		sAvoidTicks = 0;
		sReverse = CD2_AI_REVERSE_TICKS;	// back out of whatever is holding it

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] AI car=%d idle too long - moving out\n", cp->id);
	}

	// --- navigator: goal, route and the shared flow field ---
	{
		if (sState == CD2_AI_ATTACK && targetId >= 0)
		{
			// attack: the target, with a role-specific offset
			CAR_DATA* t = &car_data[targetId];
			int ox = targetV.vx;
			int oz = targetV.vz;

			if (sRole == CD2_AI_ROLE_FLANKER)
			{
				// sweep to the target's right side of travel
				ox += (int)(((long long)t->hd.where.m[0][0] * 2600) >> 12);
				oz += (int)(((long long)t->hd.where.m[2][0] * 2600) >> 12);
			}
			else if (sRole == CD2_AI_ROLE_AMBUSHER)
			{
				// get ahead of the target
				ox += (int)(((long long)t->hd.where.m[0][2] * 5000) >> 12);
				oz += (int)(((long long)t->hd.where.m[2][2] * 5000) >> 12);
			}

			goalV.vx = ox;
			goalV.vy = targetV.vy;
			goalV.vz = oz;
		}
		else if (sState == CD2_AI_DISPERSE)
		{
			// opening spread: drive out along this contestant's own heading
			goalV.vx = carV.vx + (int)(((long long)RSIN(sWanderHeading) * CD2_AI_DISPERSE_LEG) >> 12);
			goalV.vy = carV.vy;
			goalV.vz = carV.vz + (int)(((long long)RCOS(sWanderHeading) * CD2_AI_DISPERSE_LEG) >> 12);
		}
		else
		{
			int gdx = sGoalX - carV.vx;
			int gdz = sGoalZ - carV.vz;

			// ROAM: cruise toward a road node, keeping the goal between
			// frames so the car travels instead of re-aiming every frame.
			// The road graph is a GUIDE here, not a cage - cd2NavRoute still
			// A*s over it, and falls back to the off-road grid when the road
			// detours badly.
			if (--sGoalTimer <= 0 ||
				    gdx * gdx + gdz * gdz < CD2_AI_WP_REACH * CD2_AI_WP_REACH)
			{
				if (!cd2NavRoamGoal(&carV, CD2_AI_ROAM_MIN, CD2_AI_ROAM_MAX, &goalV))
				{
					if (--sWanderTimer <= 0)
					{
						sWanderHeading = (sWanderHeading + 900 + cd2AiRand(1400)) & 0xfff;
						sWanderTimer = 60 + cd2AiRand(90);
					}

					goalV.vx = carV.vx + (int)(((long long)RSIN(sWanderHeading) * CD2_AI_WANDER_LEG) >> 12);
					goalV.vy = carV.vy;
					goalV.vz = carV.vz + (int)(((long long)RCOS(sWanderHeading) * CD2_AI_WANDER_LEG) >> 12);
				}

				sGoalX = goalV.vx;
				sGoalZ = goalV.vz;
				sGoalTimer = CD2_AI_GOAL_TICKS + cd2AiRand(CD2_AI_GOAL_TICKS);
			}

			goalV.vx = sGoalX;
			goalV.vy = carV.vy;
			goalV.vz = sGoalZ;
		}

	cd2NavRoute(cp->id, &carV, &goalV, &sRoute);

		// shared pursuit field toward the same goal, budgeted per frame
		cd2FlowSetGoal(&goalV);
		cd2FlowUpdate(64);

		if (gCd2Cfg.debugLog && (sLogTick % 120) == 0)
			printInfo("[combatd2] nav flow: car=%d role=%s src=%s wp=%d flow=%d cells=%d goal=(%d,%d)\n",
				cp->id, cd2AiRoleName(),
				(sRoute.source == CD2_NAV_SRC_SCENERY) ? "scenery" :
				(sRoute.source == CD2_NAV_SRC_ROAD) ? "road" :
				(sRoute.source == CD2_NAV_SRC_DIRECT) ? "direct" : "none",
				sRoute.count, cd2FlowReady(), cd2FlowCoverage(), goalV.vx, goalV.vz);
	}

	// --- forward speed (world units/frame, signed). Needed before the heading
	// choice: pure pursuit scales its lookahead with it. ---
	speedFwd = (int)(((long long)fx * FIXEDH(cp->st.n.linearVelocity[0])
			+ (long long)fz * FIXEDH(cp->st.n.linearVelocity[2])) >> 12);

	// --- desired heading: navigator look-ahead, else flow, else direct aim ---
	if (sEvade > 0)
	{
		// dodge toward the side the threat is NOT on
		int side = (tpos.vx - cp->hd.where.t[0]) * rx + (tpos.vz - cp->hd.where.t[2]) * rz;

		desired = cp->hd.direction + ((side > 0) ? -1024 : 1024);
	}
	else
	{
		// --- pure pursuit: follow the route POLYLINE at a speed-scaled
		// lookahead rather than charging at a fixed waypoint. Aiming at one
		// point makes the car cut the corner between it and the previous one -
		// into whatever scenery sits on the inside of the bend. Interpolating
		// along the path keeps the car on the road through the turn. ---
		int look = CD2_AI_LOOKAHEAD_MIN + speedFwd * CD2_AI_LOOKAHEAD_PER_SPEED;
		int k, seg = -1, got = 0;

		if (look < CD2_AI_LOOKAHEAD_MIN)
			look = CD2_AI_LOOKAHEAD_MIN;

		if (look > CD2_AI_LOOKAHEAD_MAX)
			look = CD2_AI_LOOKAHEAD_MAX;

		// the first waypoint past the reach radius starts the path ahead
		for (k = 0; k < sRoute.count; k++)
		{
			int wdx = sRoute.wp[k].vx - carV.vx;
			int wdz = sRoute.wp[k].vz - carV.vz;

			if (wdx * wdx + wdz * wdz > CD2_AI_WP_REACH * CD2_AI_WP_REACH)
			{
				seg = k;
				break;
			}
		}

		if (seg >= 0)
		{
			int rem = look;
			int px = carV.vx, pz = carV.vz;

			for (k = seg; k < sRoute.count; k++)
			{
				int qx = sRoute.wp[k].vx, qz = sRoute.wp[k].vz;
				int adx = ABS(qx - px), adz = ABS(qz - pz);
				int d = (adx > adz) ? (adx + adz / 2) : (adz + adx / 2);
				int tx = qx, tz = qz;

				if (d > 0 && rem < d)
				{
					// part way along this leg
					tx = px + (int)(((long long)(qx - px) * rem) / d);
					tz = pz + (int)(((long long)(qz - pz) * rem) / d);
				}

				if ((d >= rem || k == sRoute.count - 1) &&
				    (tx != carV.vx || tz != carV.vz))
				{
					desired = ratan2(tx - carV.vx, tz - carV.vz);
					got = 1;
					break;
				}

				rem -= d;
				px = qx;
				pz = qz;
			}
		}

		// no usable route: shared flow field, then direct aim at the goal
		if (!got)
		{
			int head;

			if (cd2FlowDir(&carV, &head))
				desired = head;
			else
				desired = ratan2(goalV.vx - carV.vx, goalV.vz - carV.vz);
		}
	}

	// --- attack alignment: while engaging, lean the heading onto the target so
	// the guns actually line up. Navigation keeps priority by construction: the
	// pull is capped to CD2_AI_AIM_PULL_LIMIT, so a route that disagrees by more
	// than that still wins outright, and the navigation heading is only ever
	// nudged, never replaced. ---
	if (sState == CD2_AI_ATTACK && sEvade == 0 && targetId >= 0 &&
	    targetD2 < (long long)CD2_AI_FIRE_RANGE * CD2_AI_FIRE_RANGE)
	{
		int toTarget = ratan2(targetV.vx - carV.vx, targetV.vz - carV.vz);
		int aimErr = toTarget - desired;

		while (aimErr > 2048)
			aimErr -= 4096;

		while (aimErr < -2048)
			aimErr += 4096;

		if (ABS(aimErr) < CD2_AI_AIM_PULL_LIMIT)
			desired = desired + aimErr / CD2_AI_AIM_PULL;
	}

	// --- steering ---
	diff = desired - cp->hd.direction;
	while (diff > 2048) diff -= 4096;
	while (diff < -2048) diff += 4096;

	// --- scenery probes: a fan of rays whose CLEAR LENGTH is measured, not just
	// blocked/clear. Knowing how much room it actually has is what lets the AI
	// ease off and lean away from a wall instead of discovering it on contact.
	// The fan reaches the full look-ahead, subdivided so the lengths are usable.
	{
		static const int fanOff[CD2_AI_FAN_RAYS] = { 900, 450, 0, -450, -900 };
		VECTOR carPos;
		int segLen = CD2_AI_LOOK / CD2_AI_FAN_STEPS;
		int i, seg;

		if (segLen < 1)
			segLen = 1;

		probeStep = segLen;

		cd2AiCarPos(cp, &carPos);

		for (i = 0; i < CD2_AI_FAN_RAYS; i++)
		{
			int len = 0;

			for (seg = 1; seg <= CD2_AI_FAN_STEPS; seg++)
			{
				VECTOR p;
				int d = seg * segLen;

				cd2AiPointAt(cp, cp->hd.direction + fanOff[i], d, &p);

				if (lineClear(&carPos, &p) == 0)
					break;		// this ray stops here

				len = d;
			}

			freeRun[i] = len;
		}

		freeAhead = freeRun[2];
		freeL = (freeRun[0] > freeRun[1]) ? freeRun[0] : freeRun[1];
		freeR = (freeRun[3] > freeRun[4]) ? freeRun[3] : freeRun[4];

		clearAhead = (freeAhead > 0);
		clearL = (freeL > 0);
		clearR = (freeR > 0);
		blockedAhead = (freeAhead == 0);

		// imminent when the room left is less than the room it needs to stop -
		// or less than a car length, which catches the pinned case (stopped
		// against a wall: speed 0, room 0, where the speed test alone is false)
		nearBlocked = (freeAhead < ABS(speedFwd) * CD2_AI_STOP_FRAMES) ||
		              (freeAhead < CD2_AI_NEAR_BLOCK_MIN);
	}

	// --- imminent-collision response with hysteresis. Preference order: swerve
	// (normal steering below) -> brake -> pivot in place -> reverse (stuck).
	// The choice is held for a few frames so the car commits rather than
	// flickering between braking and steering. ---
	if (sAvoidTicks > 0)
	{
		sAvoidTicks--;
	}
	else if (nearBlocked)
	{
		// Pivoting in place next to a wall never clears the probe on its own, so
		// after one failed cycle back off rather than scrape along the wall.
		if (++sAvoidCycles > 1)
		{
			sReverse = CD2_AI_REVERSE_TICKS;
			sAvoidCycles = 0;
			sAvoid = CD2_AI_AVOID_NONE;
		}
		else if (speedFwd > CD2_AI_BRAKE_SPEED)
		{
			sAvoid = CD2_AI_AVOID_BRAKE;
			sAvoidTicks = 20;
		}
		else
		{
			sAvoid = CD2_AI_AVOID_PIVOT;
			sAvoidTicks = 12;
		}
	}
	else
	{
		sAvoid = CD2_AI_AVOID_NONE;
		sAvoidCycles = 0;
	}

	// --- stuck detection: commanded forward but going nowhere -> back up ---
	if (sReverse > 0)
	{
		sReverse--;
	}
	else if (ABS(diff) > CD2_AI_PIVOT_DIFF || sAvoid != CD2_AI_AVOID_NONE)
	{
		// pivoting / avoiding on purpose is not "stuck"
		sStuck = 0;
	}
	else if (ABS(speedFwd) < CD2_AI_STUCK_SPEED)
	{
		if (++sStuck > CD2_AI_STUCK_TICKS)
		{
			sReverse = CD2_AI_REVERSE_TICKS;
			sStuck = 0;
		}
	}
	else
	{
		sStuck = 0;
	}

	// escape toward whichever side has more measured clearance
	dodgeDir = (freeL >= freeR) ? 1 : -1;

	pivotDir = 0;

	if (sReverse > 0)
	{
		// --- backing up: swing the nose toward the target while reversing ---
		thrust = -CD2_TMB_THRUST;
		steer = (diff >= 0) ? -CD2_STEER_MAX : CD2_STEER_MAX;
		sSteer = steer;
	}
	else if (sAvoid == CD2_AI_AVOID_PIVOT)
	{
		// --- too close and too slow to steer around it: stop and pivot away ---
		pivotDir = dodgeDir;
		steer = pivotDir * CD2_STEER_MAX;
		thrust = 0;
		sSteer = steer;
	}
	else if (sAvoid == CD2_AI_AVOID_BRAKE)
	{
		// --- closing on an obstacle at speed: brake hard and steer around it ---
		thrust = -CD2_TMB_THRUST;
		steer = dodgeDir * CD2_STEER_MAX;

		if (steer > sSteer + CD2_AI_STEER_RATE)
			steer = sSteer + CD2_AI_STEER_RATE;
		else if (steer < sSteer - CD2_AI_STEER_RATE)
			steer = sSteer - CD2_AI_STEER_RATE;

		sSteer = steer;
	}
	else if (ABS(diff) > CD2_AI_PIVOT_DIFF && ABS(speedFwd) < CD2_AI_PIVOT_SPEED)
	{
		// --- stop and pivot on the spot (acute in-place turn) ---
		pivotDir = (diff >= 0) ? 1 : -1;
		steer = pivotDir * CD2_STEER_MAX;
		thrust = 0;
		sSteer = steer;
	}
	else
	{
		// --- normal steering ---
		steer = diff / CD2_AI_STEER_DIV;

		// obstacle avoidance: steer around scenery/wall dead ahead
		if (clearAhead == 0)
		{
			if (clearL && !clearR)
				steer += CD2_AI_AVOID_STEER;
			else if (clearR && !clearL)
				steer -= CD2_AI_AVOID_STEER;
			else
				steer += (steer >= 0) ? CD2_AI_AVOID_STEER : -CD2_AI_AVOID_STEER;
		}

		// obstacle avoidance: cars about to be hit
		for (i = 0; i < MAX_CARS; i++)
		{
			CAR_DATA* o = &car_data[i];
			int dx, dz, aheadDot, side;

			if (o == cp || o->controlType == CONTROL_TYPE_NONE)
				continue;

			dx = o->hd.where.t[0] - cp->hd.where.t[0];
			dz = o->hd.where.t[2] - cp->hd.where.t[2];

			if (dx * dx + dz * dz > CD2_AI_LOOK * CD2_AI_LOOK)
				continue;

			aheadDot = dx * fx + dz * fz;
			if (aheadDot <= 0)
				continue;	// behind us

			side = dx * rx + dz * rz;	// > 0 = obstacle on our right
			steer += (side > 0) ? -CD2_AI_AVOID_STEER : CD2_AI_AVOID_STEER;
		}

		// bias the heading away from whichever side is walled in, so it leans
		// off a wall well before the fan shows one dead ahead
		if (freeAhead < CD2_AI_LOOK)
		{
			if (freeL > freeR + CD2_AI_SIDE_MARGIN)
				steer += CD2_AI_SIDE_BIAS;
			else if (freeR > freeL + CD2_AI_SIDE_MARGIN)
				steer -= CD2_AI_SIDE_BIAS;
		}

		// --- separation: spread out from the other opponents unless actively
		// engaging a target. A pack that drives as one blob reads as a single
		// contestant; they should close on you from different directions. ---
		if (sState != CD2_AI_ATTACK || targetId < 0)
		{
			for (i = 0; i < MAX_CARS; i++)
			{
				CAR_DATA* o = &car_data[i];
				int odx, odz, od2;

				if (i == cp->id || !cd2AiIsOpponent(o))
					continue;

				odx = o->hd.where.t[0] - cp->hd.where.t[0];
				odz = o->hd.where.t[2] - cp->hd.where.t[2];
				od2 = odx * odx + odz * odz;

				if (od2 > CD2_AI_SEPARATE_RANGE * CD2_AI_SEPARATE_RANGE || od2 < 1)
					continue;

				// which side of us they are on decides which way "apart" is
				steer -= (odx * rx + odz * rz > 0) ? CD2_AI_SEPARATE_STEER
				                                   : -CD2_AI_SEPARATE_STEER;

				if (od2 < CD2_AI_SEPARATE_CLOSE * CD2_AI_SEPARATE_CLOSE)
					crowded = 1;
			}
		}

		steer = jer_clamp_int(steer, -CD2_STEER_MAX, CD2_STEER_MAX);
		// rate-limit the steering so an oscillating heading error can't slam
		// the wheel full-lock one way then the other every frame (the "twitch")
		if (steer > sSteer + CD2_AI_STEER_RATE)
			steer = sSteer + CD2_AI_STEER_RATE;
		else if (steer < sSteer - CD2_AI_STEER_RATE)
			steer = sSteer - CD2_AI_STEER_RATE;

		sSteer = steer;

		// proactive speed governor: judge the room ahead conservatively - the
		// probes are only fine to one segment, so assume a wall could be at the
		// near edge of the last clear one - and shed speed BEFORE the wall
		// instead of arriving on it at full tilt
		{
			int room = freeAhead - probeStep;

			if (room < 0)
				room = 0;

			if (speedFwd > room / CD2_AI_STOP_FRAMES + CD2_AI_GOVERN_SLACK)
				governed = 1;
		}

		// --- throttle ---
		thrust = CD2_TMB_THRUST;

		if (governed)
		{
			thrust = -CD2_TMB_THRUST;	// brake: no room to carry this speed
		}
		else if (crowded && thrust > CD2_TMB_THRUST / 2)
		{
			thrust = CD2_TMB_THRUST / 2;	// give the other contestant room
		}
		else if (sEvade == 0)
		{
			if (ABS(diff) > 1700)
				thrust = CD2_TMB_THRUST / 3;
			else if (ABS(diff) > 1000)
				thrust = CD2_TMB_THRUST / 2;
		}
	}

	// tell combatd2's torque whether to pivot this frame
	cd2CarSetAiPivot(cp, pivotDir);

	cp->wheel_angle = (short)steer;
	cp->thrust = (short)thrust;
	cp->handbrake = 0;
	cp->wheelspin = 0;

	// --- offensive: MG when lined up, plus a primary (missile) at range ---
	// Both go through cd2WpnTryFire, so each weapon's own refire cooldown sets
	// the cadence and the AI can't out-shoot what the player is allowed to do.
	if (sEvade == 0 && targetId >= 0 &&
	    targetD2 < (long long)CD2_AI_FIRE_RANGE * CD2_AI_FIRE_RANGE)
	{
		const CD2_WEAPON_DEF* ms = cd2WpnDef(CD2_WID_MISSILE);
		const CD2_WEAPON_DEF* hm = cd2WpnDef(CD2_WID_HOMING);
		const CD2_WEAPON_DEF* mg = cd2WpnDef(CD2_WID_MG);
		int inPrimary = (targetD2 > (long long)CD2_AI_PRIMARY_MIN * CD2_AI_PRIMARY_MIN &&
		                 targetD2 < (long long)CD2_AI_PRIMARY_RANGE * CD2_AI_PRIMARY_RANGE);
		int launched = 0;
		int which = -1;

		// Each weapon states its own firing tolerance. Lined up, take the one
		// that hits hardest; off to one side, take the one that steers.
		if (inPrimary && ms != NULL && ABS(diff) < cd2AiCone(ms))
		{
			launched = cd2WpnTryFire(cp, CD2_WID_MISSILE);
			which = CD2_WID_MISSILE;
		}
		else if (inPrimary && hm != NULL && ABS(diff) < cd2AiCone(hm))
		{
			launched = cd2WpnTryFire(cp, CD2_WID_HOMING);
			which = CD2_WID_HOMING;
		}

		if (!launched && (mg == NULL || ABS(diff) < cd2AiCone(mg)))
			cd2WpnTryFire(cp, CD2_WID_MG);

		if (launched && gCd2Cfg.debugLog)
			printInfo("[combatd2] AI car=%d fired %s at car=%d (err=%d)\n",
				cp->id, (which == CD2_WID_HOMING) ? "SEEKER" : "MISSILE", targetId, diff);
	}

	// --- observability snapshot (tracked opponent only) ---
	if (A == &sAi[0])
	{
		int dx = 0, dz = 0, d2;

		if (pcp != NULL)
		{
			dx = pcp->hd.where.t[0] - cp->hd.where.t[0];
			dz = pcp->hd.where.t[2] - cp->hd.where.t[2];
		}

		d2 = dx * dx + dz * dz;

		sDbg.valid = 1;
		sDbg.carId = cp->id;
		sDbg.state = sState;
		sDbg.evadeLeft = sEvade;
		sDbg.heading = cp->hd.direction;
		sDbg.desired = desired;
		sDbg.diff = diff;
		sDbg.steer = sSteer;
		sDbg.thrust = thrust;
		sDbg.speed = cp->hd.wheel_speed / 256;
		sDbg.playerDist = cd2AiSqrt(d2);
		sDbg.threat = threatFlag;
		sDbg.blockedAhead = blockedAhead;
		sDbg.reverse = (sReverse > 0) ? 1 : 0;
		sDbg.pivot = pivotDir;
		sDbg.damage = cp->totalDamage;
		sDbg.hits = sHits;
	}

	if (gCd2Cfg.debugLog && (sLogTick2++ % 60) == 0)
		printInfo("[combatd2] AI car=%d %s state=%s disp=%d dmg=%d hits=%d walls=%d spd=%d steer=%d rev=%d pivot=%d avoid=%d gov=%d free=%d threat=%d wall=%d\n",
			cp->id, cd2AiRoleNameOf(sRole), cd2AiStateName(), sDisperseTicks, cp->totalDamage, sHits, cd2SceneryHits(cp),
			speedFwd, sSteer, (sReverse > 0) ? 1 : 0, pivotDir, sAvoid, governed, freeAhead, threatFlag, blockedAhead);

#undef sState
#undef sStateTimer
#undef sEvade
#undef sFireTimer
#undef sWanderHeading
#undef sWanderTimer
#undef sSteer
#undef sReverse
#undef sStuck
#undef sRole
#undef sAvoid
#undef sAvoidTicks
#undef sAvoidCycles
#undef sEngageTicks
#undef sEngageLimit
#undef sDisperseTicks
#undef sGoalX
#undef sGoalZ
#undef sGoalTimer
#undef sRoamTicks
#undef sLogTick2
#undef sHold
#undef sIdle
#undef sLastDamage
#undef sHits
#undef sRoute
}

// --- hooks -----------------------------------------------------------------

static int cd2AiOnFrame(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled || !gCd2Cfg.aiOpponent)
		return JER_RESULT_CONTINUE;

	// Build the nav graph once the level's road data is resident (GAME_START
	// can fire before the road lumps are loaded). Cheap no-op once built.
	cd2NavReady();

	// One-shot probe: ask the router for a route to a point diagonally off the
	// road network and log which source won (road vs off-road scenery).
	// carId -1 so no car's route cache is touched.
	if (!sNavProbeDone && cd2NavNodeCount() > 0)
	{
		CAR_DATA* pcp = NULL;

		if (cd2WpnPlayerCar(&pcp))
		{
			VECTOR from, to;
			CD2_NAV_ROUTE r;

			from.vx = pcp->hd.where.t[0];
			from.vy = pcp->hd.where.t[1];
			from.vz = pcp->hd.where.t[2];

			to = from;
			to.vx += 7000;
			to.vz -= 5000;

			cd2NavRoute(-1, &from, &to, &r);

			printInfo("[combatd2] nav probe: src=%s wp=%d len=%d expanded=%d nodes=%d edges=%d\n",
				(r.source == CD2_NAV_SRC_SCENERY) ? "scenery" : (r.source == CD2_NAV_SRC_ROAD) ? "road" : "none",
				r.count, r.length, r.expanded, cd2NavNodeCount(), cd2NavEdgeCount());

			// Standalone off-road probes so the scenery router is verified even
			// when the arbiter picks the road route.
			{
				static const int offs[4][2] = { { 3000, 0 }, { 0, 3000 }, { 7000, -5000 }, { -4000, 2500 } };
				int kk;

				for (kk = 0; kk < 4; kk++)
				{
					VECTOR g = from, wp[CD2_NAV_MAX_ROUTE];
					int ge = 0;
					int gn;

					g.vx += offs[kk][0];
					g.vz += offs[kk][1];

					gn = cd2GridPath(&from, &g, wp, CD2_NAV_MAX_ROUTE, &ge);

					printInfo("[combatd2] grid probe o=(%d,%d): wp=%d expanded=%d\n",
						offs[kk][0], offs[kk][1], gn, ge);
				}
			}

			sNavProbeDone = 1;
		}
	}

	// drop destroyed/removed opponents so their ids don't go stale
	{
		int k, live = 0;

		for (k = 0; k < CD2_AI_MAX; k++)
		{
			int id = sAi[k].carId;

			if (id < 0)
				continue;

			if (id >= MAX_CARS || car_data[id].controlType == CONTROL_TYPE_NONE)
			{
				sAi[k].carId = -1;
				continue;
			}

			live++;
		}

		sAiCount = live;

		if (live == 0)
			cd2AiSpawn();
	}

	return JER_RESULT_CONTINUE;
}

static int cd2AiOnCarPad(void* ud, void* args)
{
	JER_ARGS_CAR_PAD* a = (JER_ARGS_CAR_PAD*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!cd2AiIsOpponent(cp))
		return JER_RESULT_CONTINUE;

	// Capture what the engine handed the opponent before we blank it. A
	// CUTSCENE car is fed cjpPlay stream 0 = the PLAYER's live replay, so
	// without this the opponent would literally mirror the player's controls
	// (the stock pedal path also runs because this car is not "live").
	sDbg.padIn = a->pad;

	// The opponent is driven only by the AI - never by a pad.
	a->pad = 0;
	a->padSteer = 0;
	a->useAnalogue = 0;
	a->handled = 1;	// skip the stock pedal assignment entirely

	return JER_RESULT_CONTINUE;
}

static int cd2AiOnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	CD2_AI_CAR* A;
	(void)ud;

	if (!gCd2Cfg.enabled || !gCd2Cfg.aiOpponent)
		return JER_RESULT_CONTINUE;

	A = cd2AiSlot(cp->id);

	if (A != NULL)
		cd2AiDrive(cp, A);

	return JER_RESULT_CONTINUE;
}

static int cd2AiOnGameStart(void* ud, void* args)
{
	int i;
	(void)ud;
	(void)args;

	for (i = 0; i < CD2_AI_MAX; i++)
		sAi[i].carId = -1;

	sAiCount = 0;
	sDbg.valid = 0;

	// spawn as soon as the frame hook sees a player car (the player car may
	// not exist yet at GAME_START)
	return JER_RESULT_CONTINUE;
}

const char* cd2AiRoleNameOf(int role)
{
	static const char* names[] = { "Chaser", "Flanker", "Ambusher", "Harvester" };

	if (role < 0 || role >= CD2_AI_ROLE_COUNT)
		return "?";

	return names[role];
}

const char* cd2AiRoleName(void)
{
	int r = (sAi[0].carId >= 0) ? sAi[0].role : CD2_AI_ROLE_CHASER;

	return cd2AiRoleNameOf(r);
}

int cd2AiActive(void)
{
	return (sAiCount > 0) ? 1 : 0;
}

int cd2AiIsOpponent(const void* car)
{
	const CAR_DATA* cp = (const CAR_DATA*)car;

	return (cp != NULL && cd2AiSlot(cp->id) != NULL) ? 1 : 0;
}

int cd2AiGetDebug(CD2_AI_DEBUG* out)
{
	if (out != NULL)
		*out = sDbg;

	return sDbg.valid;
}

// JER_EVENT_DRAW_WORLD: nav_debug - draw the navigation graph (nodes + edges)
// around the player into the real OT.
static int cd2AiOnDrawWorld(void* ud, void* args)
{
	CAR_DATA* pcp = NULL;
	VECTOR c;
	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled || !gCd2Cfg.navDebug)
		return JER_RESULT_CONTINUE;

	if (!cd2WpnPlayerCar(&pcp))
		return JER_RESULT_CONTINUE;

	c.vx = pcp->hd.where.t[0];
	c.vy = pcp->hd.where.t[1];
	c.vz = pcp->hd.where.t[2];

	cd2NavDraw(&c, CD2_NAV_DRAW_RADIUS);

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_DRAW_OVERLAY: an on-screen readout of the AI's internal values,
// so you can watch what it is thinking while playing (config ai_debug / menu).
static int cd2AiOnOverlay(void* ud, void* args)
{
	char text[128];
	int y = 150;
	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled || !gCd2Cfg.aiDebug || !sDbg.valid)
		return JER_RESULT_CONTINUE;

	sprintf(text, "AI car %d %s state=%s evade=%d", sDbg.carId, cd2AiRoleName(), cd2AiStateName(), sDbg.evadeLeft);
	PrintString(text, 20, y);
	y += 12;

	sprintf(text, "heading=%d desired=%d diff=%d steer=%d", sDbg.heading, sDbg.desired, sDbg.diff, sDbg.steer);
	PrintString(text, 20, y);
	y += 12;

	sprintf(text, "thrust=%d speed=%d playerDist=%d", sDbg.thrust, sDbg.speed, sDbg.playerDist);
	PrintString(text, 20, y);
	y += 12;

	sprintf(text, "threat=%d wallAhead=%d padIn=0x%04X rev=%d pivot=%d",
		sDbg.threat, sDbg.blockedAhead, sDbg.padIn, sDbg.reverse, sDbg.pivot);
	PrintString(text, 20, y);
	y += 12;

	sprintf(text, "health dmg=%d hits=%d", sDbg.damage, sDbg.hits);
	PrintString(text, 20, y);
	y += 12;

	if (sAi[0].carId >= 0)
	{
		const CD2_NAV_ROUTE* r = cd2NavLastRoute(sAi[0].carId);

		if (r != NULL)
			sprintf(text, "route %s wp=%d len=%d goal=%d",
				(r->source == CD2_NAV_SRC_SCENERY) ? "scenery" : (r->source == CD2_NAV_SRC_ROAD) ? "road" : "none",
				r->count, r->length, r->goalNode);
		else
			sprintf(text, "route none");

		PrintString(text, 20, y);
	}

	return JER_RESULT_CONTINUE;
}

const char* cd2AiStateName(void)
{
	static const char* const names[] =
	{
		"Auto", "Disperse", "Roam", "Attack", "Recover"
	};
	int s = gCd2Cfg.aiForceState;

	if (s < 0 || s >= CD2_AI_STATE_COUNT)
		s = 0;

	if (s == CD2_AI_AUTO)
	{
		int cur = (sAi[0].carId >= 0) ? sAi[0].state : CD2_AI_ROAM;

		if (cur < 0 || cur >= CD2_AI_STATE_COUNT)
			cur = CD2_AI_ROAM;

		return names[cur];
	}

	return names[s];
}

// JER_EVENT_DRAW_MAP: plot every opponent on the overhead/fullscreen map, so
// they are easy to keep track of. Colour is per role.
static int cd2AiOnDrawMap(void* ud, void* args)
{
	JER_ARGS_DRAW_MAP* a = (JER_ARGS_DRAW_MAP*)args;
	static unsigned int sMapLog;
	int i, plotted = 0;
	(void)ud;

	if (!gCd2Cfg.enabled || !gCd2Cfg.aiOpponent)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < CD2_AI_MAX; i++)
	{
		int id = sAi[i].carId;
		VECTOR p;
		unsigned char r, g, b;

		if (id < 0 || id >= MAX_CARS)
			continue;

		p.vx = car_data[id].hd.where.t[0];
		p.vy = car_data[id].hd.where.t[1];
		p.vz = car_data[id].hd.where.t[2];

		switch (sAi[i].role)
		{
			case CD2_AI_ROLE_FLANKER:   r = 255; g = 160; b = 0;   break;
			case CD2_AI_ROLE_AMBUSHER:  r = 200; g = 60;  b = 255; break;
			case CD2_AI_ROLE_HARVESTER: r = 60;  g = 255; b = 60;  break;
			default:                    r = 255; g = 40;  b = 40;  break;
		}

		DrawTargetBlip(&p, r, g, b, a->flags);
		plotted++;
	}

	if (gCd2Cfg.debugLog && (sMapLog++ % 120) == 0)
		printInfo("[combatd2] map blips: flags=0x%02X plotted=%d\n", a->flags, plotted);

	return JER_RESULT_CONTINUE;
}

void cd2AiRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2AiOnFrame, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2AiOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2AiOnCarStep, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_PAD, cd2AiOnCarPad, NULL, -1);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_MAP, cd2AiOnDrawMap, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, cd2AiOnOverlay, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, cd2AiOnDrawWorld, NULL, 2);

	ctx->jer_log(ctx, "[combatd2] opponent AI registered (SDK v%d)\n", ctx->sdkVersion);
}
