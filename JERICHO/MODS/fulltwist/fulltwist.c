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
#include "fulltwist.h"

#include <string.h>

FULLTWIST_STATE gFullTwist;

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

static int ftOnFrame(void* ud, void* args)
{
	(void)ud;
	(void)args;

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

	gFullTwist.collisionsThisFrame++;

	/* Stock: car0 gets -normal, car1 gets +normal. A world hit has no
	 * car1 and the surface normal already points away from the wall. */
	sign0 = (car1 != NULL) ? -1 : 1;

	ftHandleCar(car0, car1, point, normal, a->howHard, sign0);

	if (ftIsTumbleCar(car1))
		ftHandleCar(car1, car0, point, normal, a->howHard, 1);

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
		st->settleFrames = 0;
		st->tumbleStrength = 4096;
		st->tumbleRollShare = 3072;
		st->tumblePitchShare = 1024;
		st->tumbleRate = rate;
		st->landed = 0;
		st->latchFrame = gFullTwist.frame;

		gFullTwist.testKickPending = 0;

		FT_LOG("[fulltwist] test kick car=%d frame=%u roll=%d pitch=%d launch=%d window=%d\n",
			(int)cp->id, gFullTwist.frame, rate * 3 / 4, rate / 4, launch, st->tumbleFrames);
	}

	/* Deterministic crash/rollover levers (fulltwist_test.c). */
	ftTestTick(cp);

	/* Count how many times the up vector crosses the horizon while the test
	 * or a tumble is active. Multiple crossings are multiple rollovers. */
	up = cp->hd.where.m[1][1];

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

	st->airborne = air;
	st->contactMask = mask;

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
				FT_LOG("[fulltwist] settled car=%d frame=%u frames=%d strength=%d upside=%d rolls=%d\n",
					(int)cp->id, gFullTwist.frame, st->settleFrames,
					st->tumbleStrength, st->upsideFrames, st->rolloverCount);
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
	int k;

	(void)ud;

	if (!gFullTwist.cfg.enabled || a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	cp = (CAR_DATA*)a->car;

	if (!ftIsTumbleCar(cp))
		return JER_RESULT_CONTINUE;

	st = &gFullTwist.cars[(int)cp->id];

	/* The rollover test holds the 90-degree pose for one second before the
	 * impulse; do not let the rollover catch right it during that hold. */
	if (gFullTwist.testRolloverHolding)
		return JER_RESULT_CONTINUE;

	int up = cp->hd.where.m[1][1];
	int aup = (up < 0) ? -up : up;
	int grounded = (cp->wasOnGround != 0) || (ftWheelMask(cp) != 0);
	int applyDamping = 0;
	int applyCatch = 0;
	int catchK = 0;

	if (st->tumbleFrames > 0)
	{
		if (gFullTwist.cfg.damping > 0)
		{
			applyDamping = 1;
			k = gFullTwist.cfg.damping * 4;	/* 0..400 */
		}

		/* A ground contact during a tumble is the body landing. Let the
		 * catch assist at half strength instead of waiting for the whole
		 * window to expire; airborne frames still get no catch. */
		if (gFullTwist.cfg.rolloverCatch > 0 && grounded && aup < 3000)
		{
			applyCatch = 1;
			catchK = gFullTwist.cfg.rolloverCatch * 180;
		}
	}
	else
	{
		/* The tumble window is over, but a car on its side or roof can keep
		 * spinning on the stock damping alone. Catch it while it is still in
		 * contact; a rollover with no wheel compression still needs the body
		 * damping, so the ground condition is the car's own settle state, not
		 * the wheel mask alone. */
		if (gFullTwist.cfg.rolloverDamping > 0 &&
			(st->upsideFrames > 0 || aup < 2500) && grounded)
		{
			applyDamping = 1;
			k = gFullTwist.cfg.rolloverDamping * 8;	/* 0..800 */
		}

		if (gFullTwist.cfg.rolloverCatch > 0 && grounded && aup < 2500)
		{
			applyCatch = 1;
			catchK = gFullTwist.cfg.rolloverCatch * 200;
		}
	}

	if (!applyDamping && !applyCatch)
		return JER_RESULT_CONTINUE;

	if (applyDamping)
	{
		cp->hd.aacc[0] -= (int)(((long long)cp->st.n.angularVelocity[0] * k) >> 12);
		cp->hd.aacc[2] -= (int)(((long long)cp->st.n.angularVelocity[2] * k) >> 12);
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

	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, ftOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_COLLISION, ftOnCollision, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, ftOnCarStep, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_TORQUE, ftOnCarTorque, NULL, 0);
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
