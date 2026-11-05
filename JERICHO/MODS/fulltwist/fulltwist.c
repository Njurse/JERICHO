/*
 * fulltwist.c -- FULL TWIST entry point and event wiring.
 *
 * The effect is one extra angular impulse on top of the engine's existing
 * collision response. Everything it reads comes from the two collision sites
 * the engine already exposes:
 *
 *   - car-vs-car:   JER_EVENT_COLLISION, fired from the RK/subframe loop in
 *                   GlobalTimeStep (handling.c), before the stock delta is
 *                   applied. The same pair can report at most 8 times per
 *                   frame (4 subframes x 2 RK steps).
 *   - car-vs-world: JER_EVENT_COLLISION, fired from CarBuildingCollision
 *                   (bcollide.c). Player contacts also come through
 *                   CheckScenaryCollisions inside GlobalTimeStep, and all
 *                   remaining cars come through DoScenaryCollisions (main.c),
 *                   so one contact can report up to 3 times per frame.
 *
 * The per-car, per-frame latch below is what keeps those repeats from stacking
 * into a launch. The first severe event applies; a stronger event in the same
 * frame may only top up the difference; weaker repeats are ignored.
 *
 * Sign convention (matches the stock response):
 *   car-vs-car  car0 receives -normal, car1 receives +normal
 *   car-vs-world car0 receives +normal (the world normal points away from
 *                the surface, which is the direction the stock reaction uses)
 *
 * Engine axes: local X is lateral (pitch), local Y is up (yaw), local Z is
 * longitudinal (roll). hd.where.m maps local -> world, so column 0 is the
 * world-space local-X axis and column 2 is the world-space local-Z axis.
 *
 * The rest of the module lives beside this file: config, menu, hull and the
 * contact/impulse math are split out so this file stays event wiring.
 */

#include "jericho.h"
#include "jer_events.h"
#include "fulltwist_host.h"
#include "fulltwist.h"

#include <string.h>

FULLTWIST_STATE gFullTwist;

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

/* A level start invalidates every cached hull. The first JER_EVENT_FRAME after
 * it rebuilds them, before any collision can ask for one. */
static int ftOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	memset(gFullTwist.hulls, 0, sizeof(gFullTwist.hulls));
	gFullTwist.hullsDirty = 1;
	gFullTwist.hullsBuilt = 0;

	return JER_RESULT_CONTINUE;
}

static int ftOnFrame(void* ud, void* args)
{
	(void)ud;
	(void)args;

	/* Prebuild every resident hull at the top of the frame, never inside a
	 * collision callback. A model swap marks hullsDirty and gets rebuilt here
	 * on the next frame; the contact that saw the swap falls back to colBox. */
	if (!gFullTwist.hullsBuilt || gFullTwist.hullsDirty)
		ftHullBuildAll();

	if (gFullTwist.cfg.debugLog && gFullTwist.collisionsThisFrame > 0)
	{
		FT_LOG("[fulltwist] frame=%u collision_events=%d\n",
			gFullTwist.frame, gFullTwist.collisionsThisFrame);
	}

	gFullTwist.frame++;
	gFullTwist.collisionsThisFrame = 0;

	return JER_RESULT_CONTINUE;
}

static int ftOnCollision(void* ud, void* args)
{
	JER_ARGS_COLLISION* a = (JER_ARGS_COLLISION*)args;
	const JER_HOST_API* api = ftHost();
	CAR_DATA v0;
	CAR_DATA v1;
	CAR_DATA* car0;
	CAR_DATA* car1;
	VECTOR* point;
	VECTOR* normal;
	int sign0;

	(void)ud;

	if (!gFullTwist.cfg.enabled || a == NULL || a->car0 == NULL)
		return JER_RESULT_CONTINUE;

	ftCarLoad(api, a->car0, &v0);
	car0 = &v0;
	car1 = NULL;

	if (a->car1 != NULL)
	{
		ftCarLoad(api, a->car1, &v1);
		car1 = &v1;
	}

	point = (VECTOR*)a->point;
	normal = (VECTOR*)a->normal;

	if (!ftIsTumbleCar(car0))
		return JER_RESULT_CONTINUE;

	/* Player-only patch: a traffic/player pair goes through the player car,
	 * never through the traffic car. Traffic-traffic is ignored entirely. */
	if (gFullTwist.cfg.playerOnly &&
		!ftIsPlayerCar(car0) &&
		!(car1 != NULL && ftIsPlayerCar(car1)))
		return JER_RESULT_CONTINUE;

	gFullTwist.collisionsThisFrame++;

	/* Stock: car0 gets -normal, car1 gets +normal. A world hit has no
	 * car1 and the surface normal already points away from the wall. */
	sign0 = (car1 != NULL) ? -1 : 1;

	ftHandleCar(car0, car1, point, normal, a->howHard, sign0);

	/* World hit: remove part of the tangential velocity as well. The engine's
	 * wall-restitution query (below) then cancels the normal component and
	 * skips the stock wall spin, so the body snags instead of sliding. */
	if (car1 == NULL && normal != NULL && gFullTwist.cfg.wallSnag > 0)
	{
		FULLTWIST_CAR* st0 = &gFullTwist.cars[(int)car0->id];
		int nx = normal->vx;
		int nz = normal->vz;
		int vx = car0->st.n.linearVelocity[0];
		int vz = car0->st.n.linearVelocity[2];
		int vn = (int)(((long long)vx * nx + (long long)vz * nz) >> 12);
		int tx = vx - (int)(((long long)nx * vn) >> 12);
		int tz = vz - (int)(((long long)nz * vn) >> 12);

		car0->st.n.linearVelocity[0] -= (int)(((long long)tx * gFullTwist.cfg.wallSnag) / 100);
		car0->st.n.linearVelocity[2] -= (int)(((long long)tz * gFullTwist.cfg.wallSnag) / 100);
		st0->wallSnagFrame = gFullTwist.frame;

		if (gFullTwist.cfg.debugLog)
		{
			FT_LOG("[fulltwist] wall snag car=%d frame=%u vt=(%d,%d) drop=%d%%\n",
				(int)car0->id, gFullTwist.frame, tx, tz, gFullTwist.cfg.wallSnag);
		}
	}

	if (ftIsTumbleCar(car1))
		ftHandleCar(car1, car0, point, normal, a->howHard, 1);

	if (car1 == NULL)
	{
		gFullTwist.cars[(int)car0->id].wallHitFrame = gFullTwist.frame;
	}
	else if (normal != NULL && gFullTwist.cfg.carCarImpulse < 100)
	{
		int pairInScope = !gFullTwist.cfg.playerOnly ||
						  (ftIsPlayerCar(car0) && ftIsPlayerCar(car1));
		int otherUsable = ftIsTumbleCar(car1) &&
						  car1->ap.carCos != NULL &&
						  ftCarSlotUsable(api, car1->ap.model);
		int scale = gFullTwist.cfg.carCarImpulse;

		if (scale < 0)
			scale = 0;
		else if (scale > 100)
			scale = 100;

		/* The engine computes the stock car-car impulse and torque from this
		 * same normal immediately after the event. Scale it only for a real,
		 * resident pair; a cutscene/camera/culled car keeps the stock
		 * response. */
		if (pairInScope && otherUsable && scale < 100)
		{
			normal->vx = (int)(((long long)normal->vx * scale) / 100);
			normal->vy = (int)(((long long)normal->vy * scale) / 100);
			normal->vz = (int)(((long long)normal->vz * scale) / 100);

			if (gFullTwist.cfg.debugLog)
			{
				FT_LOG("[fulltwist] car-car recoil car0=%d car1=%d scale=%d%% normal=(%d,%d,%d)\n",
					(int)car0->id, (int)car1->id, scale,
					normal->vx, normal->vy, normal->vz);
			}
		}
		else if (gFullTwist.cfg.debugLog)
		{
			FT_LOG("[fulltwist] car-car scale skip car0=%d car1=%d pair=%d usable=%d scale=%d\n",
				(int)car0->id, (int)car1->id, pairInScope, otherUsable, scale);
		}
	}

	ftCarFlush(api, a->car0, car0);
	if (car1 != NULL)
		ftCarFlush(api, a->car1, car1);

	return JER_RESULT_CONTINUE;
}

/* FULL TWIST keeps a flipped car alive four times longer than the stock
 * rule. mission.c asks every frame a player car is upside down; leaving the
 * value alone (or disabling the module) keeps the stock 180 frames. */
static int ftOnUpsideDownFrames(void* ud, void* args)
{
	JER_ARGS_UPSIDE_DOWN* a = (JER_ARGS_UPSIDE_DOWN*)args;
	const JER_HOST_API* api = ftHost();
	CAR_DATA view;
	CAR_DATA* cp = &view;

	(void)ud;

	if (!gFullTwist.cfg.enabled || a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	ftCarLoad(api, a->car, cp);

	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(cp))
		return JER_RESULT_CONTINUE;

	a->frames = FT_UPSIDE_DOWN_FRAMES;

	if (gFullTwist.cfg.debugLog && (gFullTwist.frame % 30) == 0)
	{
		FT_LOG("[fulltwist] upside-down limit car=%d player=%d frames=%d\n",
			(int)cp->id, a->playerId, a->frames);
	}

	return JER_RESULT_CONTINUE;
}

static int ftOnWallRestitution(void* ud, void* args)
{
	JER_ARGS_WALL_RESTITUTION* a = (JER_ARGS_WALL_RESTITUTION*)args;
	const JER_HOST_API* api = ftHost();
	CAR_DATA view;
	CAR_DATA* cp = &view;

	(void)ud;

	if (!gFullTwist.cfg.enabled || a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	ftCarLoad(api, a->car, cp);

	if (!ftIsTumbleCar(cp))
		return JER_RESULT_CONTINUE;

	/* 100 % = stock bounce, 0 = the wall absorbs the normal component. The
	 * engine's low-restitution branch also skips the stock wall spin, so this
	 * is the knob the Free Physics preset sets to 100 and the arcade presets
	 * keep at 0. */
	if (gFullTwist.cars[(int)cp->id].wallHitFrame == gFullTwist.frame)
		a->result = gFullTwist.cfg.wallBounce * 4096 / 100;

	return JER_RESULT_CONTINUE;
}

/* The exposed part of the stock angular settle. The lift-triggered -avel/8 is
 * not exposed, so ftOnCarTorque cancels it after ConvertTorqueToAngularAcceleration.
 * Only cars in the free-spin state are touched, so another physics module's
 * angularDamping survives on every other car. */
static int ftOnPhysicsParams(void* ud, void* args)
{
	JER_ARGS_PHYSICS_PARAMS* a = (JER_ARGS_PHYSICS_PARAMS*)args;
	const JER_HOST_API* api = ftHost();
	CAR_DATA view;
	CAR_DATA* cp = &view;

	(void)ud;

	if (!gFullTwist.cfg.enabled || !gFullTwist.cfg.freeSpin ||
		a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	ftCarLoad(api, a->car, cp);

	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(cp))
		return JER_RESULT_CONTINUE;

	if (ftFreeSpinActive(cp))
		a->angularDamping = gFullTwist.cfg.angularDamping;

	return JER_RESULT_CONTINUE;
}

static int ftOnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	const JER_HOST_API* api = ftHost();
	CAR_DATA view;
	CAR_DATA* cp = &view;
	FULLTWIST_CAR* st;
	int mask, air, up;

	(void)ud;

	if (!gFullTwist.cfg.enabled || a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	ftCarLoad(api, a->car, cp);

	if (!ftIsTumbleCar(cp))
		return JER_RESULT_CONTINUE;

	st = &gFullTwist.cars[(int)cp->id];

	/* Player-only patch: traffic keeps the stock step. */
	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(cp))
		return JER_RESULT_CONTINUE;

	/* Horn cannon roll: one roll per horn press on the main player's car. */
	if (ftIsPlayerCar(cp))
	{
		JER_PLAYER_STATE ps;

		if (ftMainPlayerState(api, &ps) &&
			((int)ps.playerCarId == (int)cp->id ||
			 (int)ps.cameraCarId == (int)cp->id))
		{
			int horn = ps.horn ? 1 : 0;

			if (gFullTwist.cfg.hornRoll && horn && !st->hornHeld)
				ftHornRoll(cp);

			st->hornHeld = horn;
		}
	}

	/* GlobalTimeStep zeroes angular velocity for a car whose hd.speed is 0,
	 * later in the same frame. A tumbling car is not "stopped" even when its
	 * linear speed is tiny, so keep hd.speed off zero while the free-spin
	 * window is open. CAR_STEP fires before that branch. */
	if (gFullTwist.cfg.freeSpin && gFullTwist.cfg.keepSpinAtRest &&
		ftFreeSpinActive(cp) && cp->hd.speed == 0)
	{
		cp->hd.speed = 1;
	}

	/* Build/cache the mesh hull once for the player. A collision would do
	 * this too, but this makes a headless test kick show the hull's measured
	 * size and placement without waiting for a crash. */
	if (ftIsPlayerCar(cp) && gFullTwist.cfg.debugLog && st->logTick == 0)
	{
		FULLTWIST_HULL* hull = ftHullForCar(cp);

		if (hull != NULL)
		{
			FT_LOG("[fulltwist] hull ready car=%d slot=%d verts=%d size=(%d,%d,%d) footprint=%d\n",
				(int)cp->id, hull->slot, hull->count,
				hull->hx * 2, hull->hy * 2, hull->hz * 2, hull->hullCount);
			st->logTick = 1;
		}
	}

	/* Headless measurement lever. The pause-menu action remains the normal
	 * way to fire it; this flag lets a -frames run do it from the ini after
	 * the level-start reset has finished. */
	if (gFullTwist.cfg.testKick && !gFullTwist.testKickDone && ftIsPlayerCar(cp))
	{
		if (!gFullTwist.testKickScheduled)
		{
			gFullTwist.testKickScheduled = 1;
			gFullTwist.testKickAtFrame = gFullTwist.frame + FT_TEST_KICK_DELAY;
		}
		else if (gFullTwist.frame >= gFullTwist.testKickAtFrame)
		{
			gFullTwist.testKickPending = 1;
			gFullTwist.testKickDone = 1;
		}
	}

	mask = ftWheelMask(cp);
	air = (mask == 0);
	up = cp->hd.where.m[1][1];

	/* Deterministic measurement lever: one known roll/pitch kick on the
	 * player car, so settling can be compared without hunting for a crash. */
	if (gFullTwist.testKickPending && ftIsPlayerCar(cp))
	{
		int rate = gFullTwist.cfg.maxRate / 2;
		int launch = FT_TEST_LAUNCH;

		if (rate < 200000)
			rate = 200000;

		ftAddLocalRate(cp, rate * 3 / 4, rate / 4);

		if (launch > 0)
			cp->st.n.linearVelocity[1] += launch;

		st->tumbleFrames = (gFullTwist.cfg.window > 0) ? gFullTwist.cfg.window : 120;
		st->tumbled = 1;
		st->settleFrames = 0;
		st->tumbleStrength = 4096;
		st->tumbleRollShare = 3072;
		st->tumblePitchShare = 1024;
		st->tumbleRate = rate;
		st->landed = 0;
		st->latchFrame = gFullTwist.frame;
		st->peakAv = 0;

		gFullTwist.testKickPending = 0;

		FT_LOG("[fulltwist] test kick car=%d frame=%u roll=%d pitch=%d launch=%d window=%d\n",
			(int)cp->id, gFullTwist.frame, rate * 3 / 4, rate / 4, launch, st->tumbleFrames);
	}

	/* Deterministic crash/rollover levers (fulltwist_test.c). */
	ftTestTick(cp);

	/* Count how many times the up vector crosses the horizon while the test
	 * or a tumble is active. Multiple crossings are multiple rollovers. */
	up = cp->hd.where.m[1][1];

	if (st->tumbleFrames > 0 || gFullTwist.testRolloverHolding)
	{
		int av0 = cp->st.n.angularVelocity[0];
		int av1 = cp->st.n.angularVelocity[1];
		int av2 = cp->st.n.angularVelocity[2];
		int peak = (av0 < 0) ? -av0 : av0;
		int oldPeak = st->peakAv;

		if (av1 < 0) av1 = -av1;
		if (av2 < 0) av2 = -av2;
		if (av1 > peak) peak = av1;
		if (av2 > peak) peak = av2;

		if (peak > st->peakAv)
			st->peakAv = peak;

		if (gFullTwist.cfg.debugLog && oldPeak < FT_AV_CLAMP && st->peakAv >= FT_AV_CLAMP)
		{
			FT_LOG("[fulltwist] av at clamp car=%d frame=%u peak=%d\n",
				(int)cp->id, gFullTwist.frame, st->peakAv);
		}
	}

	if ((st->tumbleFrames > 0 || gFullTwist.testRolloverHolding) && up != 0)
	{
		int sign = (up > 0) ? 1 : -1;

		if (st->upSign != 0 && sign != st->upSign)
		{
			st->rolloverCount++;

			if (gFullTwist.cfg.debugLog)
			{
				FT_LOG("[fulltwist] rollover count car=%d frame=%u count=%d up=%d\n",
					(int)cp->id, gFullTwist.frame, st->rolloverCount, up);
			}
		}

		st->upSign = sign;
	}

	/* First wheel contact after a tumble continues it once. Roof/side
	 * contacts without wheel compression are still handled by the engine's
	 * 12-point ground path; see wheelforces.c. */
	if (st->airborne && !air && st->tumbleFrames > 0 && !st->landed)
	{
		int rate = (int)((long long)gFullTwist.cfg.maxRate *
						 gFullTwist.cfg.groundKick / 100 *
						 st->tumbleStrength / 4096);
		int roll = (int)((long long)rate * st->tumbleRollShare / 4096);
		int pitch = (int)((long long)rate * st->tumblePitchShare / 4096);

		ftAddLocalRate(cp, roll, pitch);
		st->landed = 1;

		if (gFullTwist.cfg.debugLog)
		{
			FT_LOG("[fulltwist] ground kick car=%d frame=%u rate=%d roll=%d pitch=%d\n",
				(int)cp->id, gFullTwist.frame, rate, roll, pitch);
		}
	}

	/* Roof/side landing: the wheel kick above only sees compressed wheels.
	 * If the lowest hull vertex is on the road, use that point as the pivot
	 * and turn the vertical impact into roll/pitch instead of losing it. The
	 * probe also runs for free-spin cars so bodyContact stays a live grounded
	 * signal for the damping cancellation in ftOnCarTorque. */
	if (st->tumbled || ftFreeSpinActive(cp))
	{
		FULLTWIST_HULL* hull = ftHullForCar(cp);
		VECTOR bodyPoint;
		SVECTOR bodyLocal;
		int groundY;
		int bodyOnGround = 0;
		int touching = 0;

		if (hull != NULL && ftHullLowest(hull, cp, &bodyPoint, &groundY, &bodyLocal))
		{
			int pen = groundY - bodyPoint.vy;
			int vy = cp->st.n.linearVelocity[1];
			int upNow = cp->hd.where.m[1][1];
			int uprightOnWheels = (ftWheelMask(cp) != 0) && (upNow > 2048);

			st->bodyContactLocalValid = 0;

			bodyOnGround = (pen > 0 && pen < FT_BODY_TOUCH_RANGE && vy < 0);

			/* The grip keeps the contact while the hull is still resting on
			 * the road: a body at rest settles at pen ~ 0, where the strict
			 * > 0 landing test above would drop it every other step. A car
			 * that is upright with its wheels down is held by the tires, not
			 * by the body, so it is not a body contact. */
			touching = !uprightOnWheels &&
					   pen < FT_BODY_TOUCH_RANGE &&
					   (pen > 0 ||
						(st->bodyTouching && pen > -FT_BODY_TOUCH_TOL));

			if (bodyOnGround && !st->bodyContact)
			{
				int rate = 0;
				int pitch = 0;
				int roll = 0;

				/* The hull-landing assist is a tuning lever, not part of the
				 * free-spin model. Clamp the raw rate so a fast roof landing
				 * cannot inject hundreds of millions before ftAddLocalRate's
				 * per-component clamp. */
				if (st->tumbled && st->tumbleFrames > 0 && gFullTwist.cfg.bodyKick > 0)
				{
					int jx, jy, jz;
					int av0 = cp->st.n.angularVelocity[0];
					int av2 = cp->st.n.angularVelocity[2];
					int avMag = (av0 < 0 ? -av0 : av0) + (av2 < 0 ? -av2 : av2);

					rate = (int)(-(long long)vy * gFullTwist.cfg.bodyKick * 32);

					if (rate > FT_AV_CLAMP)
						rate = FT_AV_CLAMP;
					else if (rate < -FT_AV_CLAMP)
						rate = -FT_AV_CLAMP;

					jx = (int)(((long long)cp->hd.where.m[1][0] * rate) >> 12);
					jy = (int)(((long long)cp->hd.where.m[1][1] * rate) >> 12);
					jz = (int)(((long long)cp->hd.where.m[1][2] * rate) >> 12);

					pitch = (int)(((long long)bodyLocal.vy * jz -
								   (long long)bodyLocal.vz * jy) >> 12);
					roll = (int)(((long long)bodyLocal.vx * jy -
								  (long long)bodyLocal.vy * jx) >> 12);

					/* A car that is already tumbling should keep its spin. The
					 * landing torque is only there to start a tumble from a
					 * body drop, not to fight an established roll - and a car
					 * that is merely RESTING on its body must get nothing at
					 * all. Without the downward-velocity test a parked car
					 * (or one settling on the spawn frame) was still pressed
					 * into a roll: measured as "rollover count car=0
					 * frame=133 up=-275" with no collision anywhere in the log,
					 * i.e. the player's car flipped itself while sitting still.
					 * A real drop arrives with vy well under -200; a resting
					 * body reads a few units either side of zero. */
					if (avMag < 1500000 && bodyLocal.vy < -200)
						ftAddLocalRate(cp, roll, pitch);
				}

				st->bodyContactFrame = gFullTwist.frame;

				if (gFullTwist.cfg.debugLog)
				{
					FT_LOG("[fulltwist] body contact car=%d frame=%u point=(%d,%d,%d) groundY=%d up=%d vy=%d pen=%d rate=%d roll=%d pitch=%d free=%d\n",
						(int)cp->id, gFullTwist.frame, bodyPoint.vx, bodyPoint.vy, bodyPoint.vz,
						groundY, up, vy, pen, rate, roll, pitch, st->freeSpinFrames);
				}
			}

			/* Live body-ground grip: continuous, unlike the one-shot landing
			 * kick above. The contact point is the same lowest hull vertex,
			 * so the friction's reaction pivots the body about it. */
			if (touching)
			{
				st->bodyContactLocal = bodyLocal;
				st->bodyContactLocalValid = 1;
				ftBodyFriction(cp, &bodyLocal);
			}
		}

		st->bodyTouching = touching;
		st->bodyContact = bodyOnGround;
	}
	else
	{
		st->bodyContact = 0;
		st->bodyTouching = 0;
		st->bodyContactLocalValid = 0;
	}

	st->airborne = (air && !st->bodyTouching) ? 1 : 0;
	st->contactMask = mask;

	/* Keep the free-spin state alive only while the car is genuinely in the
	 * air (no wheel compression AND no body contact, with the hysteresis the
	 * body probe uses); a car sliding/bouncing on its roof has mask == 0 but
	 * is not airborne. Using bodyContact here let one contact-free frame
	 * refresh the window forever. */
	if (gFullTwist.cfg.freeSpin)
	{
		if (air && !st->bodyTouching)
			st->freeSpinFrames = gFullTwist.cfg.freeSpinWindow;
		else if (mask != 0 && up > 2048)
			st->freeSpinFrames = 0;
		else if (st->freeSpinFrames > 0)
			st->freeSpinFrames--;
	}

	if (st->airborne)
		st->airFrames++;
	else
		st->airFrames = 0;

	if (up < 100)
		st->upsideFrames++;
	else
		st->upsideFrames = 0;

	/* JERICHO test lever: report the parked-on-roof settle path. The upright
	 * transition is logged even without debug_log so the frames-to-upright is
	 * always available from a -frames run. */
	if (gFullTwist.restFlipActive && ftIsPlayerCar(cp))
	{
		unsigned int elapsed = gFullTwist.frame - gFullTwist.restFlipStartFrame;
		int logUp = cp->hd.where.m[1][1];

		if (gFullTwist.cfg.debugLog && ((gFullTwist.frame + (unsigned int)cp->id) % 5) == 0)
		{
			FT_LOG("[fulltwist] rest flip car=%d frame=%u up=%d av=(%d,%d) mask=%x body=%d elapsed=%u\n",
				(int)cp->id, gFullTwist.frame, logUp,
				cp->st.n.angularVelocity[0], cp->st.n.angularVelocity[2],
				mask, st->bodyContact, elapsed);
		}

		if (logUp > 3000 && mask != 0)
		{
			FT_LOG("[fulltwist] rest flip upright car=%d frame=%u frames=%u\n",
				(int)cp->id, gFullTwist.frame, elapsed);
			gFullTwist.restFlipActive = 0;
		}
		else if (elapsed > 600)
		{
			FT_LOG("[fulltwist] rest flip timeout car=%d frame=%u up=%d mask=%x\n",
				(int)cp->id, gFullTwist.frame, logUp, mask);
			gFullTwist.restFlipActive = 0;
		}
	}

	if (st->tumbleFrames > 0)
	{
		st->settleFrames++;
		st->tumbleFrames--;

		if (st->tumbleFrames == 0)
		{
			if (gFullTwist.cfg.debugLog)
			{
				FT_LOG("[fulltwist] settled car=%d frame=%u frames=%d strength=%d upside=%d rolls=%d peakAv=%d atClamp=%d\n",
					(int)cp->id, gFullTwist.frame, st->settleFrames,
					st->tumbleStrength, st->upsideFrames, st->rolloverCount,
					st->peakAv, (st->peakAv >= FT_AV_CLAMP) ? 1 : 0);
			}

			st->landed = 0;
		}
	}

	if (gFullTwist.cfg.debugLog &&
		(st->tumbleFrames > 0 || air || up < 100) &&
		((gFullTwist.frame + (unsigned int)cp->id) % 5) == 0)
	{
		FT_LOG("[fulltwist] step car=%d frame=%u av=(%d,%d,%d) up=%d air=%d mask=%x tumble=%d airFrames=%d upside=%d\n",
			(int)cp->id, gFullTwist.frame,
			cp->st.n.angularVelocity[0], cp->st.n.angularVelocity[1],
			cp->st.n.angularVelocity[2], up, air, mask,
			st->tumbleFrames, st->airFrames, st->upsideFrames);
	}

	ftCarFlush(api, a->car, cp);

	return JER_RESULT_CONTINUE;
}

static int ftOnCarTorque(void* ud, void* args)
{
	JER_ARGS_CAR_TORQUE* a = (JER_ARGS_CAR_TORQUE*)args;
	const JER_HOST_API* api = ftHost();
	CAR_DATA view;
	CAR_DATA* cp = &view;
	FULLTWIST_CAR* st;
	int k = 0;

	(void)ud;

	if (!gFullTwist.cfg.enabled || a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	ftCarLoad(api, a->car, cp);

	if (!ftIsTumbleCar(cp))
		return JER_RESULT_CONTINUE;

	/* Player-only patch: traffic keeps the stock torque path. */
	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(cp))
		return JER_RESULT_CONTINUE;

	st = &gFullTwist.cars[(int)cp->id];

	if (!gFullTwist.cfg.enabled || a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	cp = (CAR_DATA*)a->car;

	if (!ftIsTumbleCar(cp))
		return JER_RESULT_CONTINUE;

	st = &gFullTwist.cars[(int)cp->id];

	/* Player-only patch: traffic keeps the stock torque. */
	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(cp))
		return JER_RESULT_CONTINUE;

	/* Fulltwist torque only belongs to a car it has actually tumbled, or to a
	 * free-spinning car that needs the stock lift damping cancelled. */
	if (!st->tumbled && !ftFreeSpinActive(cp))
		return JER_RESULT_CONTINUE;

	/* The rollover test holds the 90-degree pose for one second before the
	 * impulse; do not let the settle damping fight that hold. */
	if (gFullTwist.testRolloverHolding)
		return JER_RESULT_CONTINUE;

	int up = cp->hd.where.m[1][1];
	int aup = (up < 0) ? -up : up;
	int grounded = (ftWheelMask(cp) != 0) || (st->bodyContact != 0);
	int applyDamping = 0;
	int applyRetention = 0;
	int applyBodyDamp = 0;
	int applyGravity = 0;
	int bodyK = 0;
	int gravityX = 0;
	int gravityZ = 0;
	int gravityRx = 0;
	int gravityRz = 0;
	int gravityK = 0;

	/* ConvertTorqueToAngularAcceleration applies the lift -avel/8 only when
	 * the body has ground contact (extraDamping). Cancel that term only when
	 * it was actually applied, and only on roll/pitch: yaw keeps the body
	 * damping so the car cannot feed its own spin. The old code cancelled all
	 * three axes whenever the car looked grounded, which added energy on any
	 * frame where the engine had not applied the lift term. */
	if (ftFreeSpinActive(cp) && gFullTwist.cfg.cancelExtraDamping > 0 && a->extraDamping)
	{
		int cancelK = gFullTwist.cfg.cancelExtraDamping;

		cp->hd.aacc[0] += (int)(((long long)cp->st.n.angularVelocity[0] * cancelK) / 800);
		cp->hd.aacc[2] += (int)(((long long)cp->st.n.angularVelocity[2] * cancelK) / 800);

		if (gFullTwist.cfg.debugLog && ((gFullTwist.frame + (unsigned int)cp->id) % 10) == 0)
		{
			FT_LOG("[fulltwist] free spin car=%d frame=%u damp=%d cancel=%d extra=%d av=(%d,%d,%d)\n",
				(int)cp->id, gFullTwist.frame, gFullTwist.cfg.angularDamping,
				cancelK, a->extraDamping, cp->st.n.angularVelocity[0],
				cp->st.n.angularVelocity[1], cp->st.n.angularVelocity[2]);
		}
	}

	/* The tires stabilize yaw whenever the wheels are down. The free-spin
	 * physics hook lowered angularDamping for every axis; add the yaw share
	 * back so the stock yaw damping (and the tire forces) settle the spin
	 * instead of letting it grow. */
	if (ftFreeSpinActive(cp) && ftWheelMask(cp) != 0)
	{
		int yawK = 128 - gFullTwist.cfg.angularDamping;

		if (yawK > 0)
		{
			cp->hd.aacc[1] += (int)(((long long)cp->st.n.angularVelocity[1] * yawK) / 4096);

			if (gFullTwist.cfg.debugLog && ((gFullTwist.frame + (unsigned int)cp->id) % 10) == 0)
			{
				FT_LOG("[fulltwist] tire damp car=%d frame=%u yawK=%d avY=%d\n",
					(int)cp->id, gFullTwist.frame, yawK, cp->st.n.angularVelocity[1]);
			}
		}
	}

	if (st->tumbleFrames > 0)
	{
		if (gFullTwist.cfg.damping > 0)
		{
			applyDamping = 1;
			k = gFullTwist.cfg.damping * 4;	/* 0..400 */
		}

		if (gFullTwist.cfg.spinRetention > 0)
			applyRetention = 1;
	}
	else if (st->tumbled)
	{
		/* The tumble window is over: a tumbled car on the ground gets its
		 * roll/pitch spin damped in every orientation, not only while it is
		 * already tilted. Stopping damping as it passes upright was what let
		 * the righting motion rock back and forth. */
		if (gFullTwist.cfg.rolloverDamping > 0 && grounded)
		{
			applyDamping = 1;
			k = gFullTwist.cfg.rolloverDamping * 8;	/* 0..800 */
		}
	}

	/* Body spin scrub: while the hull is resting on the road, body_friction
	 * damps roll/pitch so the car stops rolling on its roof/side. This writes
	 * only the angular channel; the body hinge deliberately leaves the car's
	 * linear momentum alone (collision impulses still push it). */
	if (gFullTwist.cfg.bodyFriction > 0 && st->bodyContact)
	{
		applyBodyDamp = 1;
		bodyK = gFullTwist.cfg.bodyFriction * 8;	/* 0..800 */
	}

	/* COG gravity torque. The ground normal at the body contact acts through
	 * the lever from the centre of gravity: torque = r x (0,N,0). It is not an
	 * uprighting assist and has no preferred direction; it simply accelerates
	 * the fall the car's centre of gravity is already on. Fully upright and
	 * fully inverted poses are excluded so a parked roof rest is left alone. */
	if (gFullTwist.cfg.bodyGravity > 0 && st->bodyTouching &&
		st->bodyContactLocalValid && aup < 3000)
	{
		int lx = st->bodyContactLocal.vx;
		int ly = st->bodyContactLocal.vy;
		int lz = st->bodyContactLocal.vz;
		int rx = (int)(((long long)cp->hd.where.m[0][0] * lx +
						(long long)cp->hd.where.m[0][1] * ly +
						(long long)cp->hd.where.m[0][2] * lz) >> 12);
		int rz = (int)(((long long)cp->hd.where.m[2][0] * lx +
						(long long)cp->hd.where.m[2][1] * ly +
						(long long)cp->hd.where.m[2][2] * lz) >> 12);

		gravityK = gFullTwist.cfg.bodyGravity * FT_BODY_GRAVITY_TORQUE / 100;
		gravityRx = rx;
		gravityRz = rz;
		gravityX = (int)(((long long)(-rz) * gravityK) >> 12);
		gravityZ = (int)(((long long)rx * gravityK) >> 12);
		applyGravity = 1;
	}

	if (!applyDamping && !applyRetention && !applyBodyDamp && !applyGravity)
		return JER_RESULT_CONTINUE;

	if (applyDamping && st->bodyContactFrame != gFullTwist.frame)
	{
		cp->hd.aacc[0] -= (int)(((long long)cp->st.n.angularVelocity[0] * k) >> 12);
		cp->hd.aacc[2] -= (int)(((long long)cp->st.n.angularVelocity[2] * k) >> 12);
	}

	if (applyBodyDamp)
	{
		cp->hd.aacc[0] -= (int)(((long long)cp->st.n.angularVelocity[0] * bodyK) >> 12);
		cp->hd.aacc[2] -= (int)(((long long)cp->st.n.angularVelocity[2] * bodyK) >> 12);

		if (gFullTwist.cfg.debugLog && ((gFullTwist.frame + (unsigned int)cp->id) % 5) == 0)
		{
			FT_LOG("[fulltwist] body spin damp car=%d frame=%u k=%d av=(%d,%d)\n",
				(int)cp->id, gFullTwist.frame, bodyK,
				cp->st.n.angularVelocity[0], cp->st.n.angularVelocity[2]);
		}
	}

	if (applyGravity)
	{
		cp->hd.aacc[0] += gravityX;
		cp->hd.aacc[2] += gravityZ;

		if (gFullTwist.cfg.debugLog && ((gFullTwist.frame + (unsigned int)cp->id) % 5) == 0)
		{
			FT_LOG("[fulltwist] body gravity car=%d frame=%u up=%d r=(%d,%d) a=(%d,%d) g=%d\n",
				(int)cp->id, gFullTwist.frame, up,
				gravityRx, gravityRz, gravityX, gravityZ, gravityK);
		}
	}

	/* Offset part of the stock -avel * angularDamping (128) so the tumble
	 * retains its twist. This is only active while the tumble window is
	 * open; the settle path above owns the final damping. */
	if (applyRetention)
	{
		int retainK = gFullTwist.cfg.spinRetention * 128 / 100;

		cp->hd.aacc[0] += (int)(((long long)cp->st.n.angularVelocity[0] * retainK) >> 12);
		cp->hd.aacc[2] += (int)(((long long)cp->st.n.angularVelocity[2] * retainK) >> 12);
	}

	/* Damping is the only settle force here. Nothing rotates the body toward
	 * upright: a car at rest on its roof stays there, and one with enough
	 * momentum finishes the roll on its own. */

	return JER_RESULT_CONTINUE;
}

/* Keep the chase camera's heading offset still while its car tumbles. The
 * engine's settle-back lerp normally chases baseDir + gCameraAngle; gripOrbit
 * skips that lerp for this frame, so the offset the camera had when the tumble
 * began is held until the car settles. */
/* The hold exists to stop the camera chasing a spinning car. Once the car is
 * back on its wheels, upright, and the spin has fallen below camera_release_av,
 * there is nothing left to chase: release the hold so the snap-back and the
 * normal heading lock happen while the car is still rolling. Returns the
 * angular-velocity sum through outAv for the log. */
static int ftCameraSettling(const CAR_DATA* cp, int* outAv)
{
	int up, ax, ay, az, av;

	if (outAv != NULL)
		*outAv = 0;

	if (!gFullTwist.cfg.cameraRelease)
		return 0;

	if (ftWheelMask(cp) == 0)
		return 0;

	up = cp->hd.where.m[1][1];

	if (up <= 0)
		return 0;

	ax = cp->st.n.angularVelocity[0]; if (ax < 0) ax = -ax;
	ay = cp->st.n.angularVelocity[1]; if (ay < 0) ay = -ay;
	az = cp->st.n.angularVelocity[2]; if (az < 0) az = -az;
	av = ax + ay + az;

	if (outAv != NULL)
		*outAv = av;

	return av <= gFullTwist.cfg.cameraReleaseAv;
}

static int ftOnCameraLook(void* ud, void* args)
{
	JER_ARGS_CAMERA_LOOK* a = (JER_ARGS_CAMERA_LOOK*)args;
	const JER_HOST_API* api = ftHost();
	JER_PLAYER_STATE ps;
	CAR_DATA view;
	CAR_DATA* cp = &view;
	FULLTWIST_CAR* st;
	int carId;

	(void)ud;

	if (!gFullTwist.cfg.enabled || !gFullTwist.cfg.holdCamera ||
		a == NULL || a->player == NULL || api == NULL || api->player == NULL)
		return JER_RESULT_CONTINUE;

	if (!api->player->get_state(a->player, &ps))
		return JER_RESULT_CONTINUE;

	carId = ps.cameraCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	ftCarLoadSlot(api, carId, cp, NULL);

	if (!ftIsTumbleCar(cp))
		return JER_RESULT_CONTINUE;

	st = &gFullTwist.cars[carId];

	{
		int hold = (st->tumbleFrames > 0 || ftFreeSpinActive(cp));
		int av = 0;
		int settling = hold && ftCameraSettling(cp, &av);

		if (hold && !settling)
		{
			a->gripOrbit = 1;
			st->cameraHeld = 1;
			st->cameraSnapFrames = 0;

			if (gFullTwist.cfg.debugLog && ((gFullTwist.frame + (unsigned int)carId) % 10) == 0)
			{
				FT_LOG("[fulltwist] camera hold car=%d frame=%u tumble=%d free=%d\n",
					carId, gFullTwist.frame, st->tumbleFrames, st->freeSpinFrames);
			}
		}
		else if (st->cameraHeld)
		{
			/* The tumble ended, or the car settled on its wheels while the
			 * window was still open: start the fast settle-back now. */
			st->cameraHeld = 0;
			st->cameraSnapFrames = FT_CAMERA_SNAP_FRAMES;

			if (settling && gFullTwist.cfg.debugLog)
			{
				FT_LOG("[fulltwist] camera release car=%d frame=%u av=%d releaseAv=%d\n",
					carId, gFullTwist.frame, av, gFullTwist.cfg.cameraReleaseAv);
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

/* The engine's settle-back lerp resumes as soon as the tumble hold ends, but
 * at one eighth of the remaining angle per frame. While the snap window is
 * open, add a configurable share of the remaining angle so the camera comes
 * back in quickly instead of drifting in over half a second. */
static int ftOnCamera(void* ud, void* args)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	const JER_HOST_API* api = ftHost();
	JER_PLAYER_STATE ps;
	JER_CAMERA_STATE cs;
	CAR_DATA view;
	CAR_DATA* cp = &view;
	FULLTWIST_CAR* st;
	int carId;
	int target;
	int delta;
	int angle;

	(void)ud;

	if (!gFullTwist.cfg.enabled || !gFullTwist.cfg.holdCamera ||
		gFullTwist.cfg.cameraSnap <= 0 || a == NULL || a->player == NULL || !a->inCar ||
		api == NULL || api->player == NULL || api->camera == NULL)
		return JER_RESULT_CONTINUE;

	if (!api->player->get_state(a->player, &ps))
		return JER_RESULT_CONTINUE;

	carId = ps.cameraCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	ftCarLoadSlot(api, carId, cp, NULL);

	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(cp))
		return JER_RESULT_CONTINUE;

	st = &gFullTwist.cars[carId];

	if (st->cameraSnapFrames <= 0)
		return JER_RESULT_CONTINUE;

	if (!api->camera->get_state(&cs))
		return JER_RESULT_CONTINUE;

	target = (a->baseDir + cs.baseAngle) & 0xfff;
	delta = ((target - ps.cameraAngle + 2048) & 0xfff) - 2048;

	angle = (ps.cameraAngle +
		(int)(((long long)delta * gFullTwist.cfg.cameraSnap) / 100)) & 0xfff;

	api->player->set_camera_angle(a->player, angle);
	st->cameraSnapFrames--;

	if (gFullTwist.cfg.debugLog &&
		(st->cameraSnapFrames == FT_CAMERA_SNAP_FRAMES - 1 || st->cameraSnapFrames == 0))
	{
		FT_LOG("[fulltwist] camera snap car=%d frame=%u delta=%d left=%d\n",
			carId, gFullTwist.frame, delta, st->cameraSnapFrames);
	}

	return JER_RESULT_CONTINUE;
}

static int ftOnResetCar(void* ud, void* args)
{
	JER_ARGS_RESET_CAR* a = (JER_ARGS_RESET_CAR*)args;

	(void)ud;

	if (a != NULL && a->carId >= 0 && a->carId < MAX_CARS)
		memset(&gFullTwist.cars[a->carId], 0, sizeof(gFullTwist.cars[a->carId]));

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Entry                                                              */
/* ------------------------------------------------------------------ */

JER_MODULE_ENTRY(jer_module_fulltwist_entry)(JERICHO_CONTEXT* ctx)
{
	memset(&gFullTwist, 0, sizeof(gFullTwist));

	gFullTwist.ctx = ctx;

	ftLoadConfig();

	if (gFullTwist.cfg.preset != FT_PRESET_CUSTOM)
		ftApplyPreset();

	ftSaveConfig();

	ctx->jer_register_module(ctx,
		FT_MOD_ID,
		"FULL TWIST",
		FT_VERSION,
		"JERICHO",
		"Driv3r-style tumbles from severe off-centre collisions, built on the existing collision response.",
		"",
		JERICHO_SDK_VERSION);

	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, ftOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, ftOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_COLLISION, ftOnCollision, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, ftOnCarStep, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_TORQUE, ftOnCarTorque, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA_LOOK, ftOnCameraLook, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, ftOnCamera, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_PHYSICS_PARAMS, ftOnPhysicsParams, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_WALL_RESTITUTION, ftOnWallRestitution, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, ftOnResetCar, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_UPSIDE_DOWN_FRAMES, ftOnUpsideDownFrames, NULL, 0);

	/* The 3D scenery check. A NULL slot is stock; on a handled hit the engine
	 * restores the predictive position offset and skips its response. */
	ctx->jer_override(ctx, JER_OVERRIDE_SLOT_SCENERY_COLLISION, (void*)ftOnSceneryCollision);

	ftRegisterMenu();

	FT_LOG("[fulltwist] v%s registered (enabled=%d intensity=%d min=%d off=%d maxRate=%d damping=%d debug=%d)\n",
		FT_VERSION, gFullTwist.cfg.enabled, gFullTwist.cfg.intensity,
		gFullTwist.cfg.minImpact, gFullTwist.cfg.offCenter,
		gFullTwist.cfg.maxRate, gFullTwist.cfg.damping, gFullTwist.cfg.debugLog);
}
