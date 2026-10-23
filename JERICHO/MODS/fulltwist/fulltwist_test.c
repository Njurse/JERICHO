/*
 * fulltwist_test.c -- deterministic collision and rollover tests.
 *
 * The test kick in fulltwist.c exercises settling without a collision. These
 * levers make the actual paths reachable from a headless run:
 *
 *   test_crash    spawn a car down the road facing the player, give both a
 *                 closing velocity, and let the car-car path hit the hull
 *   test_rollover start from the normal upright heading, wait one second, then
 *                 apply a lateral impulse and a matching roll rate
 *
 * Both are debug/tuning levers. They never run unless the ini or the pause
 * menu asks for them.
 */

#include "driver2.h"
#include "jericho.h"
#include "cars.h"
#include "civ_ai.h"	/* InitCar, reservedSlots */
#include "fulltwist.h"

static CAR_DATA* ftFindFreeCar(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (car_data[i].controlType == CONTROL_TYPE_NONE && reservedSlots[i] == 0)
			return &car_data[i];
	}

	return NULL;
}

static void ftSpawnCrashCar(CAR_DATA* player)
{
	CAR_DATA* other;
	LONGVECTOR4 pos;
	static char padId = 0;
	int fwdX, fwdZ;
	int dist = 420;
	int vx, vz;
	int dir;

	if (player == NULL || player->ap.carCos == NULL)
		return;

	other = ftFindFreeCar();

	if (other == NULL)
	{
		FT_LOG("[fulltwist] test crash: no free CAR_DATA slot\n");
		return;
	}

	/* The forward axis is the matrix's third column. */
	fwdX = player->hd.where.m[0][2];
	fwdZ = player->hd.where.m[2][2];

	if (fwdX == 0 && fwdZ == 0)
		fwdZ = 4096;

	pos[0] = player->hd.where.t[0] + (int)(((long long)fwdX * dist) >> 12);
	pos[1] = player->hd.where.t[1];
	pos[2] = player->hd.where.t[2] + (int)(((long long)fwdZ * dist) >> 12);
	pos[3] = 0;

	dir = (player->hd.direction + 2048) & 0xfff;

	if (!InitCar(other, dir, &pos, CONTROL_TYPE_CUTSCENE,
				 player->ap.model, player->ap.palette & 255, &padId))
	{
		FT_LOG("[fulltwist] test crash: InitCar refused slot %d\n", (int)other->id);
		return;
	}

	/* Close on each other. linearVelocity is 12.12 fixed point (where.t is
	 * linearVelocity >> 12), so this is about 40 world units/frame each. */
	vx = (int)(((long long)fwdX * 160000) >> 12);
	vz = (int)(((long long)fwdZ * 160000) >> 12);

	player->st.n.linearVelocity[0] += vx;
	player->st.n.linearVelocity[2] += vz;
	player->hd.speed = 20;

	other->st.n.linearVelocity[0] -= vx;
	other->st.n.linearVelocity[2] -= vz;
	other->hd.speed = 20;

	player->hd.mayBeColliding |= 1u << other->id;
	other->hd.mayBeColliding |= 1u << player->id;

	FT_LOG("[fulltwist] test crash player=%d other=%d pos=(%d,%d,%d) fwd=(%d,%d) vel=(%d,%d)\n",
		(int)player->id, (int)other->id, pos[0], pos[1], pos[2],
		fwdX, fwdZ, vx, vz);
}

static void ftRolloverTest(CAR_DATA* cp)
{
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
	st->tumbleRollShare = 4096;
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

void ftTestTick(CAR_DATA* cp)
{
	if (cp == NULL || !ftIsPlayerCar(cp))
		return;

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

	if (gFullTwist.testRolloverHolding &&
		gFullTwist.frame >= gFullTwist.testRolloverImpulseAtFrame)
	{
		gFullTwist.testRolloverHolding = 0;
		ftRolloverImpulse(cp);
	}
}
