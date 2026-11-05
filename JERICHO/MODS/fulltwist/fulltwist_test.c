/*
 * fulltwist_test.c -- deterministic collision and rollover tests.
 *
 * The test kick in fulltwist.c exercises settling without a collision. These
 * levers make the actual paths reachable from a headless run:
 *
 *   test_crash    spawn a car down the road facing the owner, give both a
 *                 closing velocity, and let the car-car path hit the hull
 *   test_rollover start from the normal upright heading, wait one second, then
 *                 apply a lateral impulse and a matching roll rate
 *
 * Both are debug/tuning levers. They never run unless the ini or the pause
 * menu asks for them.
 */

#include "jericho.h"
#include "jer_events.h"
#include "fulltwist_host.h"
#include "fulltwist.h"

static int ftFindFreeCarSlot(const JER_HOST_API* api)
{
	int i;

	if (api == NULL || api->car == NULL)
		return -1;

	for (i = 0; i < MAX_CARS; i++)
	{
		void* token = api->car->at(i);
		JER_CAR_STATE state;

		if (token == NULL || !api->car->get_state(token, &state))
			continue;

		if (state.controlType == CONTROL_TYPE_NONE)
			return i;
	}

	return -1;
}

static void ftSpawnCrashCar(CAR_DATA* owner)
{
	const JER_HOST_API* api = ftHost();
	CAR_DATA otherView;
	CAR_DATA* other = &otherView;
	JER_VEC pos;
	JER_SVEC orientation;
	void* otherToken;
	int slot;
	int fwdX, fwdZ;
	int dist = 420;
	int vx, vz;
	int dir;

	if (owner == NULL || owner->ap.carCos == NULL || api == NULL || api->car == NULL)
		return;

	slot = ftFindFreeCarSlot(api);

	if (slot < 0)
	{
		FT_LOG("[fulltwist] test crash: no free car slot\n");
		return;
	}

	/* The forward axis is the matrix's third column. */
	fwdX = owner->hd.where.m[0][2];
	fwdZ = owner->hd.where.m[2][2];

	if (fwdX == 0 && fwdZ == 0)
		fwdZ = 4096;

	pos.x = owner->hd.where.t[0] + (int)(((long long)fwdX * dist) >> 12);
	pos.y = owner->hd.where.t[1];
	pos.z = owner->hd.where.t[2] + (int)(((long long)fwdZ * dist) >> 12);
	pos.pad = 0;

	dir = (owner->hd.direction + 2048) & 0xfff;
	orientation.x = 0;
	orientation.y = (short)dir;
	orientation.z = 0;
	orientation.pad = 0;

	if (!api->car->init(slot, owner->ap.model, &pos, &orientation))
	{
		FT_LOG("[fulltwist] test crash: car.init refused slot %d\n", slot);
		return;
	}

	otherToken = api->car->at(slot);
	ftCarLoad(api, otherToken, other);
	if (other->token == NULL)
		other->token = otherToken;

	/* Close on each other. linearVelocity is 12.12 fixed point (where.t is
	 * linearVelocity >> 12), so this is about 40 world units/frame each. */
	vx = (int)(((long long)fwdX * 160000) >> 12);
	vz = (int)(((long long)fwdZ * 160000) >> 12);

	owner->st.n.linearVelocity[0] += vx;
	owner->st.n.linearVelocity[2] += vz;
	owner->hd.speed = 20;
	ftCarFlush(api, owner->token, owner);

	other->st.n.linearVelocity[0] -= vx;
	other->st.n.linearVelocity[2] -= vz;
	other->hd.speed = 20;
	ftCarFlush(api, otherToken, other);

	if (api->car->mark_collision != NULL)
	{
		api->car->mark_collision(owner->token, other->id);
		api->car->mark_collision(otherToken, owner->id);
	}

	FT_LOG("[fulltwist] test crash owner=%d other=%d pos=(%d,%d,%d) fwd=(%d,%d) vel=(%d,%d)\n",
		(int)owner->id, (int)other->id, pos.x, pos.y, pos.z,
		fwdX, fwdZ, vx, vz);
}

/* Spawn a car beside the owner, park it, and apply the roll-test shove
 * toward it. Both cars are flagged as colliding, so the engine resolves the
 * first car-car contact on the next RK step. This is the deterministic
 * car-car smash lever. */
static void ftSmashTest(CAR_DATA* owner)
{
	const JER_HOST_API* api = ftHost();
	CAR_DATA otherView;
	CAR_DATA* other = &otherView;
	JER_VEC pos;
	JER_SVEC orientation;
	void* otherToken;
	int slot;
	int sideX, sideZ;
	int dist = 220;
	int sign = 1;

	if (owner == NULL || owner->ap.carCos == NULL || api == NULL || api->car == NULL)
		return;

	slot = ftFindFreeCarSlot(api);

	if (slot < 0)
	{
		FT_LOG("[fulltwist] test smash: no free car slot\n");
		return;
	}

	sideX = owner->hd.where.m[0][0] * sign;
	sideZ = owner->hd.where.m[2][0] * sign;

	pos.x = owner->hd.where.t[0] + (int)(((long long)sideX * dist) >> 12);
	pos.y = owner->hd.where.t[1];
	pos.z = owner->hd.where.t[2] + (int)(((long long)sideZ * dist) >> 12);
	pos.pad = 0;

	orientation.x = 0;
	orientation.y = (short)owner->hd.direction;
	orientation.z = 0;
	orientation.pad = 0;

	if (!api->car->init(slot, owner->ap.model, &pos, &orientation))
	{
		FT_LOG("[fulltwist] test smash: car.init refused slot %d\n", slot);
		return;
	}

	otherToken = api->car->at(slot);
	ftCarLoad(api, otherToken, other);
	if (other->token == NULL)
		other->token = otherToken;

	/* Park it; the owner is shoved into it by the roll-test force. */
	other->st.n.linearVelocity[0] = 0;
	other->st.n.linearVelocity[1] = 0;
	other->st.n.linearVelocity[2] = 0;
	other->st.n.angularVelocity[0] = 0;
	other->st.n.angularVelocity[1] = 0;
	other->st.n.angularVelocity[2] = 0;
	other->hd.speed = 0;
	ftCarFlush(api, otherToken, other);

	if (api->car->mark_collision != NULL)
	{
		api->car->mark_collision(owner->token, other->id);
		api->car->mark_collision(otherToken, owner->id);
	}

	/* The roll-test shove, aimed at the parked car. */
	owner->st.n.linearVelocity[0] += (int)(((long long)sideX * FT_TEST_ROLLOVER_FORCE) >> 12);
	owner->st.n.linearVelocity[2] += (int)(((long long)sideZ * FT_TEST_ROLLOVER_FORCE) >> 12);
	ftAddLocalRate(owner, -sign * FT_TEST_ROLLOVER_RATE, 0);
	owner->hd.speed = 20;

	FT_LOG("[fulltwist] test smash owner=%d other=%d pos=(%d,%d,%d) side=%d force=%d rate=%d\n",
		(int)owner->id, (int)other->id, pos.x, pos.y, pos.z, sign,
		FT_TEST_ROLLOVER_FORCE, -sign * FT_TEST_ROLLOVER_RATE);
}

static void ftRolloverTest(CAR_DATA* cp){
	FULLTWIST_CAR* st;
	int up;
	int side = (gFullTwist.cfg.testRolloverSide < 0) ? -1 : 1;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	st = &gFullTwist.cars[(int)cp->id];

	/* No orientation change: the car starts from its normal upright heading.
	 * Hold for one second, then ftRolloverImpulse applies the lateral shove. */
	gFullTwist.testRolloverHolding = 1;
	gFullTwist.testRolloverImpulseAtFrame = gFullTwist.frame + FT_TEST_ROLLOVER_WAIT;

	st->tumbleFrames = 0;
	st->tumbled = 1;
	st->settleFrames = 0;
	st->tumbleStrength = 4096;
	st->tumbleRollShare = 4096;
	st->tumblePitchShare = 0;
	st->tumbleRate = 0;
	st->landed = 0;
	st->latchFrame = gFullTwist.frame;
	st->rolloverCount = 0;
	st->peakAv = 0;

	up = cp->hd.where.m[1][1];
	st->upSign = (up >= 0) ? 1 : -1;

	FT_LOG("[fulltwist] test rollover start car=%d frame=%u up=%d side=%d impulse_at=%u\n",
		(int)cp->id, gFullTwist.frame, up, side, gFullTwist.testRolloverImpulseAtFrame);
}

static void ftRolloverImpulse(CAR_DATA* cp)
{
	FULLTWIST_CAR* st;
	int side;
	int sideX, sideZ;
	int rollRate;
	int up;

	if (cp == NULL)
		return;

	st = &gFullTwist.cars[(int)cp->id];
	side = (gFullTwist.cfg.testRolloverSide < 0) ? -1 : 1;

	/* The side axis is column 0. Push along the chosen side, and roll the
	 * opposite way: a side push applied above the CoG produces -r_y*J_x, so
	 * the two parts of the test now reinforce instead of cancelling. */
	sideX = cp->hd.where.m[0][0] * side;
	sideZ = cp->hd.where.m[2][0] * side;
	rollRate = -side * FT_TEST_ROLLOVER_RATE;

	cp->st.n.linearVelocity[0] += (int)(((long long)sideX * FT_TEST_ROLLOVER_FORCE) >> 12);
	cp->st.n.linearVelocity[2] += (int)(((long long)sideZ * FT_TEST_ROLLOVER_FORCE) >> 12);

	ftAddLocalRate(cp, rollRate, 0);

	st->tumbleFrames = (gFullTwist.cfg.window > 0) ? gFullTwist.cfg.window : 120;
	st->tumbled = 1;
	st->settleFrames = 0;
	st->tumbleStrength = 4096;
	st->tumbleRollShare = (rollRate >= 0) ? 4096 : -4096;
	st->tumblePitchShare = 0;
	st->tumbleRate = rollRate;
	st->landed = 0;
	st->latchFrame = gFullTwist.frame;
	st->peakAv = 0;

	up = cp->hd.where.m[1][1];

	FT_LOG("[fulltwist] test rollover impulse car=%d frame=%u up=%d side=%d rollSign=%d lateral=%d rollRate=%d av=(%d,%d,%d) window=%d\n",
		(int)cp->id, gFullTwist.frame, up, side,
		(rollRate >= 0) ? 1 : -1, FT_TEST_ROLLOVER_FORCE, rollRate,
		cp->st.n.angularVelocity[0], cp->st.n.angularVelocity[1],
		cp->st.n.angularVelocity[2], st->tumbleFrames);
}

/* Park the owner upside down on its roof with no motion. This is the
 * deterministic rest case for the settle/catch path: it removes the collision
 * impulse and airborne phase, so the log shows whether the catch is doing the
 * work or something else is keeping the car afloat. */
static void ftRestFlipTest(CAR_DATA* cp)
{
	FULLTWIST_CAR* st;
	FULLTWIST_HULL* hull;
	VECTOR point;
	SVECTOR local;
	int groundY;
	int up;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	st = &gFullTwist.cars[(int)cp->id];

	/* 180 degrees about world Z keeps the heading and flips the up vector. */
	cp->st.n.orientation[0] = 0;
	cp->st.n.orientation[1] = 0;
	cp->st.n.orientation[2] = 4096;
	cp->st.n.orientation[3] = 0;

	cp->st.n.fposition[0] = cp->hd.where.t[0] << 4;
	cp->st.n.fposition[1] = cp->hd.where.t[1] << 4;
	cp->st.n.fposition[2] = cp->hd.where.t[2] << 4;

	cp->st.n.linearVelocity[0] = 0;
	cp->st.n.linearVelocity[1] = 0;
	cp->st.n.linearVelocity[2] = 0;
	cp->st.n.angularVelocity[0] = 0;
	cp->st.n.angularVelocity[1] = 0;
	cp->st.n.angularVelocity[2] = 0;
	cp->hd.speed = 0;
	cp->hd.acc[0] = 0;
	cp->hd.acc[1] = 0;
	cp->hd.acc[2] = 0;
	cp->hd.aacc[0] = 0;
	cp->hd.aacc[1] = 0;
	cp->hd.aacc[2] = 0;

	ftCarRebuild(ftHost(), cp);
	ftCarLoad(ftHost(), cp->token, cp);

	/* Put the lowest hull vertex exactly on the road so the car starts
	 * resting on its roof instead of dropping or penetrating. */
	hull = ftHullForCar(cp);

	if (hull != NULL && ftHullLowest(hull, cp, &point, &groundY, &local))
	{
		cp->st.n.fposition[1] += (groundY - point.vy) << 4;
		ftCarRebuild(ftHost(), cp);
		ftCarLoad(ftHost(), cp->token, cp);
	}

	st->tumbled = 1;
	st->tumbleFrames = 0;
	st->freeSpinFrames = 0;
	st->bodyContact = 0;
	st->bodyTouching = 1;
	st->airborne = 0;
	st->landed = 0;
	st->upsideFrames = 0;
	st->upSign = -1;
	st->peakAv = 0;
	st->latchFrame = gFullTwist.frame;

	gFullTwist.testRolloverHolding = 0;
	gFullTwist.restFlipActive = 1;
	gFullTwist.restFlipStartFrame = gFullTwist.frame;

	up = cp->hd.where.m[1][1];

	FT_LOG("[fulltwist] test rest flip car=%d frame=%u up=%d pos=(%d,%d,%d)\n",
		(int)cp->id, gFullTwist.frame, up,
		cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2]);
}

void ftTestTick(CAR_DATA* cp)
{
	if (cp == NULL || !ftIsPlayerCar(cp))
		return;

	if (gFullTwist.cfg.testHorn && !gFullTwist.testHornDone)
	{
		if (!gFullTwist.testHornScheduled)
		{
			gFullTwist.testHornScheduled = 1;
			gFullTwist.testHornAtFrame = gFullTwist.frame + FT_TEST_KICK_DELAY;
		}
		else if (gFullTwist.frame >= gFullTwist.testHornAtFrame)
		{
			gFullTwist.testHornPending = 1;
			gFullTwist.testHornDone = 1;
		}
	}

	if (gFullTwist.testHornPending)
	{
		gFullTwist.testHornPending = 0;
		ftHornRoll(cp);
		FT_LOG("[fulltwist] test horn car=%d frame=%u\n", (int)cp->id, gFullTwist.frame);
	}

	if (gFullTwist.cfg.testCrash && !gFullTwist.testCrashDone)
	{
		if (!gFullTwist.testCrashScheduled)
		{
			gFullTwist.testCrashScheduled = 1;
			gFullTwist.testCrashAtFrame = gFullTwist.frame + FT_TEST_KICK_DELAY;
		}
		else if (gFullTwist.frame >= gFullTwist.testCrashAtFrame)
		{
			gFullTwist.testCrashPending = 1;
			gFullTwist.testCrashDone = 1;
		}
	}

	if (gFullTwist.testCrashPending)
	{
		gFullTwist.testCrashPending = 0;
		ftSpawnCrashCar(cp);
	}

	if (gFullTwist.cfg.testSmash && !gFullTwist.testSmashDone)
	{
		if (!gFullTwist.testSmashScheduled)
		{
			gFullTwist.testSmashScheduled = 1;
			gFullTwist.testSmashAtFrame = gFullTwist.frame + FT_TEST_KICK_DELAY;
		}
		else if (gFullTwist.frame >= gFullTwist.testSmashAtFrame)
		{
			gFullTwist.testSmashPending = 1;
			gFullTwist.testSmashDone = 1;
		}
	}

	if (gFullTwist.testSmashPending)
	{
		gFullTwist.testSmashPending = 0;
		ftSmashTest(cp);
	}

	if (gFullTwist.cfg.testRollover && !gFullTwist.testRolloverDone)
	{
		if (!gFullTwist.testRolloverScheduled)
		{
			gFullTwist.testRolloverScheduled = 1;
			gFullTwist.testRolloverAtFrame = gFullTwist.frame + FT_TEST_KICK_DELAY;
		}
		else if (gFullTwist.frame >= gFullTwist.testRolloverAtFrame)
		{
			gFullTwist.testRolloverPending = 1;
			gFullTwist.testRolloverDone = 1;
		}
	}

	if (gFullTwist.testRolloverPending)
	{
		gFullTwist.testRolloverPending = 0;
		ftRolloverTest(cp);
	}

	if (gFullTwist.cfg.testRestFlip && !gFullTwist.testRestFlipDone)
	{
		if (!gFullTwist.testRestFlipScheduled)
		{
			gFullTwist.testRestFlipScheduled = 1;
			gFullTwist.testRestFlipAtFrame = gFullTwist.frame + FT_TEST_KICK_DELAY;
		}
		else if (gFullTwist.frame >= gFullTwist.testRestFlipAtFrame)
		{
			gFullTwist.testRestFlipPending = 1;
			gFullTwist.testRestFlipDone = 1;
		}
	}

	if (gFullTwist.testRestFlipPending)
	{
		gFullTwist.testRestFlipPending = 0;
		ftRestFlipTest(cp);
	}

	if (gFullTwist.testRolloverHolding &&
		gFullTwist.frame >= gFullTwist.testRolloverImpulseAtFrame)
	{
		gFullTwist.testRolloverHolding = 0;
		ftRolloverImpulse(cp);
	}
}
