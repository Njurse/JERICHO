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

#include "driver2.h"
#include "jericho.h"
#include "jer_events.h"
#include "cars.h"		/* JerCarSlotUsable for the car-car scale guard */
#include "camera.h"		/* gCameraAngle for the post-tumble snap-back */
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
	CAR_DATA* car0;
	CAR_DATA* car1;
	VECTOR* point;
	VECTOR* normal;
	int sign0;

	(void)ud;

	if (!gFullTwist.cfg.enabled || a == NULL || a->car0 == NULL)
		return JER_RESULT_CONTINUE;

	car0 = (CAR_DATA*)a->car0;
	car1 = (CAR_DATA*)a->car1;
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
						  JerCarSlotUsable(car1->ap.model);
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

	return JER_RESULT_CONTINUE;
}

static int ftOnWallRestitution(void* ud, void* args)
{
	JER_ARGS_WALL_RESTITUTION* a = (JER_ARGS_WALL_RESTITUTION*)args;
	CAR_DATA* cp;

	(void)ud;

	if (!gFullTwist.cfg.enabled || a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	cp = (CAR_DATA*)a->car;

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
	CAR_DATA* cp;

	(void)ud;

	if (!gFullTwist.cfg.enabled || !gFullTwist.cfg.freeSpin ||
		a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	cp = (CAR_DATA*)a->car;

	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(cp))
		return JER_RESULT_CONTINUE;

	if (ftFreeSpinActive(cp))
		a->angularDamping = gFullTwist.cfg.angularDamping;

	return JER_RESULT_CONTINUE;
}

static int ftOnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp;
	FULLTWIST_CAR* st;
	int mask, air, up;

	(void)ud;

	if (!gFullTwist.cfg.enabled || a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	cp = (CAR_DATA*)a->car;

	if (!ftIsTumbleCar(cp))
		return JER_RESULT_CONTINUE;

	st = &gFullTwist.cars[(int)cp->id];

	/* Player-only patch: traffic keeps the stock step. */
	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(cp))
		return JER_RESULT_CONTINUE;

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

		if (hull != NULL && ftHullLowest(hull, cp, &bodyPoint, &groundY, &bodyLocal))
		{
			int pen = groundY - bodyPoint.vy;
			int vy = cp->st.n.linearVelocity[1];

			bodyOnGround = (pen > 0 && pen < 500 && vy < 0);

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
					 * body drop, not to fight an established roll. */
					if (avMag < 1500000)
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
		}

		st->bodyContact = bodyOnGround;
	}
	else
	{
		st->bodyContact = 0;
	}

	st->airborne = air;
	st->contactMask = mask;

	/* Keep the free-spin state alive while the car is in the air or resting
	 * on its body, and let it decay once the car is back on its wheels. */
	if (gFullTwist.cfg.freeSpin)
	{
		if (air || st->bodyContact)
			st->freeSpinFrames = gFullTwist.cfg.freeSpinWindow;
		else if (st->freeSpinFrames > 0)
			st->freeSpinFrames--;
	}

	if (air)
		st->airFrames++;
	else
		st->airFrames = 0;

	if (up < 100)
		st->upsideFrames++;
	else
		st->upsideFrames = 0;

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

	return JER_RESULT_CONTINUE;
}

static int ftOnCarTorque(void* ud, void* args)
{
	JER_ARGS_CAR_TORQUE* a = (JER_ARGS_CAR_TORQUE*)args;
	CAR_DATA* cp;
	FULLTWIST_CAR* st;
	int k = 0;

	(void)ud;

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
	 * free-spinning car that needs the stock lift damping cancelled. This keeps
	 * the near-upright catch off normal traffic during the load window. */
	if (!st->tumbled && !ftFreeSpinActive(cp))
		return JER_RESULT_CONTINUE;

	/* The rollover test holds the 90-degree pose for one second before the
	 * impulse; do not let the rollover catch right it during that hold. */
	if (gFullTwist.testRolloverHolding)
		return JER_RESULT_CONTINUE;

	int up = cp->hd.where.m[1][1];
	int aup = (up < 0) ? -up : up;
	int grounded = (ftWheelMask(cp) != 0) || (st->bodyContact != 0);
	int applyDamping = 0;
	int applyCatch = 0;
	int applyRetention = 0;
	int catchK = 0;

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
		/* The tumble window is over, but a car on its side or roof can keep
		 * spinning on the stock damping alone. Damp the spin; do not upright
		 * it -- a car resting on its side/end should stay there. */
		if (gFullTwist.cfg.rolloverDamping > 0 &&
			(st->upsideFrames > 0 || aup < 2500) && grounded)
		{
			applyDamping = 1;
			k = gFullTwist.cfg.rolloverDamping * 8;	/* 0..800 */
		}

		if (gFullTwist.cfg.rolloverCatch > 0 && grounded && aup > 3000)
		{
			applyCatch = 1;
			catchK = gFullTwist.cfg.rolloverCatch * 40;
		}
	}

	if (!applyDamping && !applyCatch && !applyRetention)
		return JER_RESULT_CONTINUE;

	if (applyDamping && st->bodyContactFrame != gFullTwist.frame)
	{
		cp->hd.aacc[0] -= (int)(((long long)cp->st.n.angularVelocity[0] * k) >> 12);
		cp->hd.aacc[2] -= (int)(((long long)cp->st.n.angularVelocity[2] * k) >> 12);
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

	/* The damping stops the spin; it does not put the car back on its
	 * wheels. Add a torque that rotates the world-space local-up vector
	 * toward world up: +Z roll for an up vector leaning +X, -X pitch for
	 * one leaning +Z. */
	if (applyCatch)
	{
		int ux = cp->hd.where.m[0][1];
		int uz = cp->hd.where.m[2][1];

		cp->hd.aacc[0] += (int)(-((long long)uz * catchK) >> 12);
		cp->hd.aacc[2] += (int)(((long long)ux * catchK) >> 12);

		if (gFullTwist.cfg.debugLog && ((gFullTwist.frame + (unsigned int)cp->id) % 5) == 0)
		{
			FT_LOG("[fulltwist] rollover catch car=%d frame=%u up=%d ux=%d uz=%d k=%d catchK=%d av=(%d,%d)\n",
				(int)cp->id, gFullTwist.frame, up, ux, uz, k, catchK,
				cp->st.n.angularVelocity[0], cp->st.n.angularVelocity[2]);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* Keep the chase camera's heading offset still while its car tumbles. The
 * engine's settle-back lerp normally chases baseDir + gCameraAngle; gripOrbit
 * skips that lerp for this frame, so the offset the camera had when the tumble
 * began is held until the car settles. */
static int ftOnCameraLook(void* ud, void* args)
{
	JER_ARGS_CAMERA_LOOK* a = (JER_ARGS_CAMERA_LOOK*)args;
	PLAYER* lp;
	FULLTWIST_CAR* st;
	int carId;

	(void)ud;

	if (!gFullTwist.cfg.enabled || !gFullTwist.cfg.holdCamera ||
		a == NULL || a->player == NULL)
		return JER_RESULT_CONTINUE;

	lp = (PLAYER*)a->player;
	carId = (int)lp->cameraCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (!ftIsTumbleCar(&car_data[carId]))
		return JER_RESULT_CONTINUE;

	st = &gFullTwist.cars[carId];

	if (st->tumbleFrames > 0 || ftFreeSpinActive(&car_data[carId]))
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
		/* The tumble just ended: start the fast settle-back window. */
		st->cameraHeld = 0;
		st->cameraSnapFrames = FT_CAMERA_SNAP_FRAMES;
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
	PLAYER* lp;
	FULLTWIST_CAR* st;
	int carId;
	int target;
	int delta;

	(void)ud;

	if (!gFullTwist.cfg.enabled || !gFullTwist.cfg.holdCamera ||
		gFullTwist.cfg.cameraSnap <= 0 || a == NULL || a->player == NULL || !a->inCar)
		return JER_RESULT_CONTINUE;

	lp = (PLAYER*)a->player;
	carId = (int)lp->cameraCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (gFullTwist.cfg.playerOnly && !ftIsPlayerCar(&car_data[carId]))
		return JER_RESULT_CONTINUE;

	st = &gFullTwist.cars[carId];

	if (st->cameraSnapFrames <= 0)
		return JER_RESULT_CONTINUE;

	target = (a->baseDir + gCameraAngle) & 0xfff;
	delta = ((target - lp->cameraAngle + 2048) & 0xfff) - 2048;

	lp->cameraAngle = (lp->cameraAngle +
		(int)(((long long)delta * gFullTwist.cfg.cameraSnap) / 100)) & 0xfff;
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

	/* The 3D scenery check. A NULL slot is stock; on a handled hit the engine
	 * restores the predictive position offset and skips its response. */
	ctx->jer_override(ctx, JER_OVERRIDE_SLOT_SCENERY_COLLISION, (void*)ftOnSceneryCollision);

	ftRegisterMenu();

	FT_LOG("[fulltwist] v%s registered (enabled=%d intensity=%d min=%d off=%d maxRate=%d damping=%d debug=%d)\n",
		FT_VERSION, gFullTwist.cfg.enabled, gFullTwist.cfg.intensity,
		gFullTwist.cfg.minImpact, gFullTwist.cfg.offCenter,
		gFullTwist.cfg.maxRate, gFullTwist.cfg.damping, gFullTwist.cfg.debugLog);
}
