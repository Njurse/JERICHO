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
#define CD2_AI_HUNT_RANGE	12200	// start hunting within this
#define CD2_AI_LOOK		10560	// look-ahead probe distance
#define CD2_AI_PROBE_ANG	450	// ~35 deg side probes
#define CD2_AI_AVOID_STEER	150	// steer nudge to dodge something
#define CD2_AI_STEER_DIV	6	// heading error -> wheel_angle divisor
#define CD2_AI_EVADE_FRAMES	85	// ~1.5 s of evasive driving
#define CD2_AI_HURT		12000	// totalDamage above which it flees
#define CD2_AI_FIRE_RANGE	6600	// MG range when hunting
#define CD2_AI_FIRE_COOLDOWN	10	// frames between AI MG shots
#define CD2_AI_STEER_RATE	48	// max wheel_angle change per frame
#define CD2_AI_PIVOT_DIFF	1150	// heading error above which it stops + pivots
#define CD2_AI_PIVOT_SPEED	70	// only pivot below this forward speed (units/frame)
#define CD2_AI_REVERSE_TICKS	42	// frames of reversing after getting stuck
#define CD2_AI_STUCK_TICKS	22	// frames with no forward progress before reversing
#define CD2_AI_STUCK_SPEED	5	// forward-speed magnitude counted as "stuck"
#define CD2_AI_WP_REACH		700	// route waypoints within this are "reached" and skipped
#define CD2_AI_WANDER_LEG	6000	// wander goal distance along the wander heading

static int sAiCarId = -1;
static int sSpawned;
static int sState = CD2_AI_WANDER;
static int sStateTimer;
static int sEvade;
static int sFireTimer;
static int sWanderHeading;
static int sWanderTimer;
static int sSteer;		// rate-limited steering actually applied
static int sReverse;		// frames of backing up left (stuck recovery)
static int sStuck;		// frames without forward progress
static int sRole = CD2_AI_ROLE_CHASER;	// this opponent's role archetype
static int sLastDamage;		// car totalDamage last frame (to count new hits)
static int sHits;		// collisions observed on the opponent
static unsigned int sLogTick;	// debugLog throttle counter
static CD2_NAV_ROUTE sRoute;	// navigator route to the current goal
static int sRouteSrc = -1;	// last logged route source
static int sNavProbeDone;	// one-shot arbitration probe at level start
static CD2_AI_DEBUG sDbg;	// latest internal values (observability)

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

static int cd2AiSpawn(void)
{
	CAR_DATA* pcp = NULL;
	CAR_DATA* slot = NULL;
	LONGVECTOR4 pos;
	VECTOR ppos, cand;
	static char cd2AiPadId = 0;	// CUTSCENE car pad id (no replay stream)
	int i, k, side, chosen = 0;

	cand.vx = 0;
	cand.vy = 0;
	cand.vz = 0;

	if (!cd2WpnPlayerCar(&pcp))
		return 0;

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

	// Probe BOTH sides of the player (each side, then a little further out)
	// and spawn on whichever side is clear, so we don't drop the car inside a
	// wall on tight maps. lineClear: 0 = blocked.
	for (k = 0; k < 2 && !chosen; k++)
	{
		int off = CD2_AI_SPAWN_OFFSET * (k + 1);

		for (side = 1; side >= -1; side -= 2)
		{
			cand.vx = ppos.vx + (int)(((long long)pcp->hd.where.m[0][0] * off * side) >> 12);
			cand.vy = ppos.vy;
			cand.vz = ppos.vz + (int)(((long long)pcp->hd.where.m[2][0] * off * side) >> 12);

			if (lineClear(&ppos, &cand))
			{
				chosen = 1;
				break;
			}
		}
	}

	if (!chosen)
		return 0;	// boxed in on both sides - try again next frame

	pos[0] = cand.vx;
	pos[1] = cand.vy;
	pos[2] = cand.vz;
	pos[3] = 0;

	// CUTSCENE control: combatd2 drives it entirely from the inputs we write,
	// and (unlike CIV_AI) there is NO stock traffic AI to fight us - no
	// road-node snapping and no CheckPingOut() reset when it leaves the road
	// graph, which is what made the civ-AI car fidget in place.
	InitCar(slot, pcp->hd.direction, &pos, CONTROL_TYPE_CUTSCENE, pcp->ap.model, 0, &cd2AiPadId);

	sAiCarId = slot->id;
	sState = CD2_AI_WANDER;
	sStateTimer = 0;
	sEvade = 0;
	sFireTimer = 0;
	sSteer = 0;
	sReverse = 0;
	sStuck = 0;
	sRole = (gCd2Cfg.aiRole >= 0) ? gCd2Cfg.aiRole : (slot->id % CD2_AI_ROLE_COUNT);
	sHits = 0;
	sLastDamage = 0;
	sWanderHeading = pcp->hd.direction;
	sWanderTimer = 60;

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] AI opponent spawned (car=%d side=%d)\n", sAiCarId, side);

	return 1;
}

static void cd2AiDrive(CAR_DATA* cp)
{
	CAR_DATA* pcp = NULL;
	VECTOR tpos;
	int desired, diff, steer, thrust;
	int fx, fz, rx, rz;
	int threatFlag = 0, blockedAhead = 0;
	int speedFwd = 0, pivotDir = 0;
	VECTOR carV, goalV;

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

	// --- base state (forced, or decided from conditions) ---
	if (gCd2Cfg.aiForceState != CD2_AI_AUTO)
	{
		sState = gCd2Cfg.aiForceState;
	}
	else if (--sStateTimer <= 0)
	{
		long long d2 = 0;

		if (pcp != NULL)
		{
			long long px = pcp->hd.where.t[0] - cp->hd.where.t[0];
			long long pz = pcp->hd.where.t[2] - cp->hd.where.t[2];
			d2 = px * px + pz * pz;
		}

		if (pcp != NULL && cp->totalDamage > CD2_AI_HURT &&
		    d2 < (long long)CD2_AI_HUNT_RANGE * CD2_AI_HUNT_RANGE)
			sState = CD2_AI_FLEE;
		else if (pcp != NULL && d2 < (long long)CD2_AI_HUNT_RANGE * CD2_AI_HUNT_RANGE)
			sState = CD2_AI_HUNT;
		else
			sState = CD2_AI_WANDER;

		sStateTimer = 45;
	}

	// --- navigator: goal, route and the shared flow field ---
	{
		int useWander = 0;

		if (sRole == CD2_AI_ROLE_HARVESTER)
			useWander = 1;	// roam for pickups/caches (rally points until they exist)
		else if (!(sState == CD2_AI_HUNT && pcp != NULL) && !(sState == CD2_AI_FLEE && pcp != NULL))
			useWander = 1;	// WANDER state

		if (useWander)
		{
			if (--sWanderTimer <= 0)
			{
				sWanderHeading = (sWanderHeading + 900 + Random2(1400)) & 0xfff;
				sWanderTimer = 60 + Random2(90);
			}

			goalV.vx = carV.vx + (int)(((long long)RSIN(sWanderHeading) * CD2_AI_WANDER_LEG) >> 12);
			goalV.vy = carV.vy;
			goalV.vz = carV.vz + (int)(((long long)RCOS(sWanderHeading) * CD2_AI_WANDER_LEG) >> 12);
		}
		else if (sState == CD2_AI_FLEE && pcp != NULL)
		{
			// run away: a point mirrored through the car from the player
			goalV.vx = carV.vx + (carV.vx - pcp->hd.where.t[0]);
			goalV.vy = carV.vy;
			goalV.vz = carV.vz + (carV.vz - pcp->hd.where.t[2]);
		}
		else if (pcp != NULL)
		{
			// hunting: the player, with a role-specific offset
			int ox = pcp->hd.where.t[0];
			int oz = pcp->hd.where.t[2];

			if (sRole == CD2_AI_ROLE_FLANKER)
			{
				// sweep to the player's right side of travel
				ox += (int)(((long long)pcp->hd.where.m[0][0] * 2200) >> 12);
				oz += (int)(((long long)pcp->hd.where.m[2][0] * 2200) >> 12);
			}
			else if (sRole == CD2_AI_ROLE_AMBUSHER)
			{
				// get ahead of the player and wait
				ox += (int)(((long long)pcp->hd.where.m[0][2] * 5000) >> 12);
				oz += (int)(((long long)pcp->hd.where.m[2][2] * 5000) >> 12);
			}

			goalV.vx = ox;
			goalV.vy = pcp->hd.where.t[1];
			goalV.vz = oz;
		}

		cd2NavRoute(cp->id, &carV, &goalV, &sRoute);

		// shared pursuit field toward the same goal, budgeted per frame
		cd2FlowSetGoal(&goalV);
		cd2FlowUpdate(64);

		if (gCd2Cfg.debugLog && (sLogTick % 120) == 0)
			printInfo("[combatd2] nav flow: car=%d role=%s src=%s wp=%d flow=%d cells=%d goal=(%d,%d)\n",
				cp->id, cd2AiRoleName(),
				(sRoute.source == CD2_NAV_SRC_SCENERY) ? "scenery" : (sRoute.source == CD2_NAV_SRC_ROAD) ? "road" : "none",
				sRoute.count, cd2FlowReady(), cd2FlowCoverage(), goalV.vx, goalV.vz);
	}

	// --- desired heading: navigator look-ahead, else flow, else direct aim ---
	if (sEvade > 0)
	{
		// dodge toward the side the threat is NOT on
		int side = (tpos.vx - cp->hd.where.t[0]) * rx + (tpos.vz - cp->hd.where.t[2]) * rz;

		desired = cp->hd.direction + ((side > 0) ? -1024 : 1024);
	}
	else
	{
		int k, got = 0;

		// steer at the first route waypoint beyond the reach radius, so the car
		// follows the routed path instead of aiming straight at the goal
		for (k = 0; k < sRoute.count; k++)
		{
			int wdx = sRoute.wp[k].vx - carV.vx;
			int wdz = sRoute.wp[k].vz - carV.vz;

			if (wdx * wdx + wdz * wdz > CD2_AI_WP_REACH * CD2_AI_WP_REACH)
			{
				desired = ratan2(wdx, wdz);
				got = 1;
				break;
			}
		}

		if (!got)
		{
			int head;

			if (cd2FlowDir(&carV, &head))
			{
				desired = head;
				got = 1;
			}
		}

		if (!got)
			desired = ratan2(goalV.vx - carV.vx, goalV.vz - carV.vz);
	}

	// --- steering ---
	diff = desired - cp->hd.direction;
	while (diff > 2048) diff -= 4096;
	while (diff < -2048) diff += 4096;

	// --- forward speed (world units/frame, signed) ---
	speedFwd = (int)(((long long)fx * FIXEDH(cp->st.n.linearVelocity[0])
			+ (long long)fz * FIXEDH(cp->st.n.linearVelocity[2])) >> 12);

	// --- stuck detection: commanded forward but going nowhere -> back up ---
	if (sReverse > 0)
	{
		sReverse--;
	}
	else if (ABS(diff) > CD2_AI_PIVOT_DIFF)
	{
		// A pivot on the spot is intentional, not "stuck".
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

	pivotDir = 0;

	if (sReverse > 0)
	{
		// --- backing up: swing the nose toward the target while reversing ---
		thrust = -CD2_TMB_THRUST;
		steer = (diff >= 0) ? -CD2_STEER_MAX : CD2_STEER_MAX;
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

		// obstacle avoidance: scenery/wall ahead (lineClear: 0 = blocked)
		{
			VECTOR carPos, ahead, leftP, rightP;
			int clearAhead, clearL, clearR;

			cd2AiCarPos(cp, &carPos);
			cd2AiPointAt(cp, cp->hd.direction, CD2_AI_LOOK, &ahead);
			cd2AiPointAt(cp, cp->hd.direction + CD2_AI_PROBE_ANG, CD2_AI_LOOK, &leftP);
			cd2AiPointAt(cp, cp->hd.direction - CD2_AI_PROBE_ANG, CD2_AI_LOOK, &rightP);

			clearAhead = lineClear(&carPos, &ahead);
			clearL = lineClear(&carPos, &leftP);
			clearR = lineClear(&carPos, &rightP);

			blockedAhead = (clearAhead == 0);

			if (clearAhead == 0)
			{
				if (clearL && !clearR)
					steer += CD2_AI_AVOID_STEER;
				else if (clearR && !clearL)
					steer -= CD2_AI_AVOID_STEER;
				else
					steer += (steer >= 0) ? CD2_AI_AVOID_STEER : -CD2_AI_AVOID_STEER;
			}
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

		steer = jer_clamp_int(steer, -CD2_STEER_MAX, CD2_STEER_MAX);

		// rate-limit the steering so an oscillating heading error can't slam
		// the wheel full-lock one way then the other every frame (the "twitch")
		if (steer > sSteer + CD2_AI_STEER_RATE)
			steer = sSteer + CD2_AI_STEER_RATE;
		else if (steer < sSteer - CD2_AI_STEER_RATE)
			steer = sSteer - CD2_AI_STEER_RATE;

		sSteer = steer;

		// --- throttle ---
		thrust = CD2_TMB_THRUST;

		if (sEvade == 0)
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

	// --- offensive: MG the player when lined up (hunt only, not while dodging) ---
	if (sFireTimer > 0)
		sFireTimer--;

	if (sState == CD2_AI_HUNT && sEvade == 0 && pcp != NULL && sFireTimer == 0)
	{
		long long dx = pcp->hd.where.t[0] - cp->hd.where.t[0];
		long long dz = pcp->hd.where.t[2] - cp->hd.where.t[2];

		if (dx * dx + dz * dz < (long long)CD2_AI_FIRE_RANGE * CD2_AI_FIRE_RANGE &&
		    ABS(diff) < 260)
		{
			const CD2_WEAPON_DEF* mg = cd2WpnDef(CD2_WID_MG);

			if (mg != NULL && mg->fire != NULL)
			{
				mg->fire(cp);
				sFireTimer = CD2_AI_FIRE_COOLDOWN;
			}
		}
	}

	// --- observability snapshot ---
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

	if (gCd2Cfg.debugLog && (sLogTick++ % 60) == 0)
		printInfo("[combatd2] AI car=%d state=%s dmg=%d hits=%d spd=%d steer=%d rev=%d pivot=%d threat=%d wall=%d\n",
			cp->id, cd2AiStateName(), cp->totalDamage, sHits, speedFwd, sSteer,
			(sReverse > 0) ? 1 : 0, pivotDir, threatFlag, blockedAhead);

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

	// drop a destroyed/removed opponent so the id doesn't go stale
	if (sAiCarId >= 0 &&
	    (sAiCarId >= MAX_CARS || car_data[sAiCarId].controlType == CONTROL_TYPE_NONE))
		sAiCarId = -1;

	if (sAiCarId < 0 && !sSpawned)
		cd2AiSpawn();

	return JER_RESULT_CONTINUE;
}

static int cd2AiOnCarPad(void* ud, void* args)
{
	JER_ARGS_CAR_PAD* a = (JER_ARGS_CAR_PAD*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (sAiCarId < 0 || cp->id != sAiCarId)
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
	(void)ud;

	if (!gCd2Cfg.enabled || !gCd2Cfg.aiOpponent || sAiCarId < 0)
		return JER_RESULT_CONTINUE;

	if (cp->id == sAiCarId)
		cd2AiDrive(cp);

	return JER_RESULT_CONTINUE;
}

static int cd2AiOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	sAiCarId = -1;
	sSpawned = 0;
	sState = CD2_AI_WANDER;
	sStateTimer = 0;
	sEvade = 0;
	sFireTimer = 0;
	sSteer = 0;
	sReverse = 0;
	sStuck = 0;
	sDbg.valid = 0;

	// spawn as soon as the frame hook sees a player car (the player car may
	// not exist yet at GAME_START)
	return JER_RESULT_CONTINUE;
}

const char* cd2AiRoleName(void)
{
	static const char* names[] = { "Chaser", "Flanker", "Ambusher", "Harvester" };

	if (sRole < 0 || sRole >= CD2_AI_ROLE_COUNT)
		return "?";

	return names[sRole];
}

int cd2AiActive(void)
{
	return (sAiCarId >= 0) ? 1 : 0;
}

int cd2AiIsOpponent(const void* car)
{
	const CAR_DATA* cp = (const CAR_DATA*)car;

	return (cp != NULL && sAiCarId >= 0 && cp->id == sAiCarId) ? 1 : 0;
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

	return JER_RESULT_CONTINUE;
}

const char* cd2AiStateName(void)
{
	static const char* const names[] =
	{
		"Auto", "Hunt", "Flee", "Recover", "Wander"
	};
	int s = gCd2Cfg.aiForceState;

	if (s < 0 || s >= CD2_AI_STATE_COUNT)
		s = 0;

	if (s == CD2_AI_AUTO)
	{
		int cur = sState;

		if (cur < 0 || cur >= CD2_AI_STATE_COUNT)
			cur = 0;

		return names[cur];
	}

	return names[s];
}

void cd2AiRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2AiOnFrame, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2AiOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2AiOnCarStep, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_PAD, cd2AiOnCarPad, NULL, -1);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, cd2AiOnOverlay, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, cd2AiOnDrawWorld, NULL, 2);

	ctx->jer_log(ctx, "[combatd2] opponent AI registered (SDK v%d)\n", ctx->sdkVersion);
}
