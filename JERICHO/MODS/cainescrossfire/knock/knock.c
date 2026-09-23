/*
 * knock/knock.c -- see knock/knock.h for what a knock is and how it moves.
 */
#include "driver2.h"
#include "main.h"		/* FrameCnt - the sampler's clock */
#include "cainescrossfire.h"
#include "cars.h"
#include "convert.h"		/* _RotMatrixX/Y/Z (the rotation helpers) */
#include "jericho.h"
#include "jer_math.h"		/* jer_clamp_int */

#include "knock/knock.h"

static CD2_KNOCK_STATE gKnock[MAX_CARS];

// [D] [T]
const CD2_KNOCK_STATE* cd2KnockOf(int carId)
{
	static const CD2_KNOCK_STATE none = { 0, 0, 0, 0, 0, 0, 0, 0 };

	if (carId < 0 || carId >= MAX_CARS)
		return &none;

	return &gKnock[carId];
}

static int cd2KnockLogEvery(void);	/* defined below the tick */

// [D] [T]
void cd2KnockReset(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	/* A knock CAN be taken away wholesale here, and that is correct: the car is being
	 * reset, so it is already teleporting and an angle that vanished with it is invisible.
	 * It is logged anyway so a wipe analysis can tell this apart from the settle
	 * deadline's wipe, which was a real bug - both used to look identical in the series. */
	if (cd2KnockLogEvery() > 0 && (gKnock[carId].pitch || gKnock[carId].roll || gKnock[carId].yaw))
		jer_log("[cainescrossfire] knockreset car=%d f=%d pitch=%d - the car is being reset\n", carId, FrameCnt, gKnock[carId].pitch);

	memset(&gKnock[carId], 0, sizeof(gKnock[carId]));
}

// [D] [T]
void cd2KnockResetAll(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
		cd2KnockReset(i);
}

// ---------------------------------------------------------------------------
// An impulse adds VELOCITY. It never sets an angle: that is what makes the car
// buck rather than snap to a new attitude and sit there.
// ---------------------------------------------------------------------------
void cd2KnockAdd(int carId, int pitch, int roll, int yaw, int lift, int shift)
{
	CD2_KNOCK_STATE* k;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	k = &gKnock[carId];

	jer_log("[cainescrossfire] knock car=%d impulse pitch=%d roll=%d yaw=%d lift=%d\n",
		carId, pitch, roll, yaw, lift);

	k->vpitch += pitch;
	k->vroll += roll;
	k->vyaw += yaw;

	if (lift != 0)
		k->vlift += lift * CD2_KNOCK_LIFT_PER_HIT;

	k->vshift += shift;

	/* How hard this was, which is what the return's rate is scaled by. The LARGEST single
	 * component: a hit that lands mostly on one axis should stiffen the whole return, not
	 * only that axis. Overwritten by each new impulse and cleared with the state, so it
	 * always describes the movement currently in progress. */
	{
		int mag = pitch < 0 ? -pitch : pitch;
		int r = roll < 0 ? -roll : roll;
		int y = yaw < 0 ? -yaw : yaw;

		if (r > mag) mag = r;
		if (y > mag) mag = y;

		/* the MAXIMUM, not the latest: every impulse used to overwrite this, so a slam landing
		 * during a live collision dropped the rate from the collision's down to the slam's and
		 * restarted the deadline - the return would get gentler because something else hit.
		 * It is zeroed with the state, and a lift/shift-only knock has mag 0, so it neither
		 * sets nor clears a live knock's value. */
		if (mag > k->force)
			k->force = mag;
	}

	/* the deadline restarts with every knock: 0.5s is per movement, not per car */
	k->settleFrames = CD2_KNOCK_SETTLE_FRAMES;
}

// ---------------------------------------------------------------------------
// One axis, in the two phases from the header: the velocity carries the angle up
// and decays, then the angle eases straight back to level. There is no spring, so
// there is nothing to oscillate - which is the difference between a car doing one
// firm movement and a car wobbling.
//
// A useful consequence: an impulse of about maxAngle * (1 - decay/4096) reaches
// the ceiling, so "how hard was that" is one number against the max.
// ---------------------------------------------------------------------------
static void cd2KnockAxis(int* angle, int* velocity, int decay, int settle, int maxAngle)
{
	int a;

	if (*velocity != 0)
	{
		/* the angle follows the velocity's SIGN: cd2KnockAdd adds the impulse to the
		 * velocity, so a positive (nose-up) impulse raises the angle. This is the
		 * other half of "positive pitch = the front lifts" - while the transform was
		 * turning the car about the world axes, the sign that looked wrong here got
		 * flipped by hand to compensate; with cd2VisualApply fixed to the car's own
		 * axes, the plain sum is correct and the workaround is gone. */
		a = *angle + *velocity;
		*velocity = (int)(((long long)*velocity * decay) >> 12);
	}
	else
	{
		/* easing out: a fixed fraction of the remaining angle each frame */
		a = *angle - (int)(((long long)*angle * settle) >> 12);

		if (a == *angle)
			a = (*angle > 0) ? *angle - 1 : *angle + 1;

		if (a < 2 && a > -2)
			a = 0;			/* and it has to actually arrive */
	}

	if (a > maxAngle) { a = maxAngle; *velocity = 0; }
	if (a < -maxAngle) { a = -maxAngle; *velocity = 0; }

	*angle = a;
}

// The settle rate for this frame, given how violently the car's speed just changed.
// [D] [T]
static int cd2KnockSettleRate(int force)
{
	int extra;

	if (force < 0)
		force = -force;

	extra = force * CD2_KNOCK_SETTLE_PER_FORCE;

	if (extra > CD2_KNOCK_SETTLE_EXTRA_MAX)
		extra = CD2_KNOCK_SETTLE_EXTRA_MAX;

	return CD2_KNOCK_SETTLE + extra;
}

static void cd2KnockSample(int carId, int force);	/* defined below the tick */
static int cd2KnockLogEvery(void);	/* ditto */

// [D] [T]
void cd2KnockTick(int carId)
{
	CD2_KNOCK_STATE* k;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	k = &gKnock[carId];

	if (k->pitch == 0 && k->roll == 0 && k->yaw == 0 && k->lift == 0 && k->shift == 0 && k->vshift == 0 &&
		k->vpitch == 0 && k->vroll == 0 && k->vyaw == 0 && k->vlift == 0)
	{
		return;		/* at rest: nothing to do, which is the common case */
	}

	/* the top of the movement, logged on the last frame the impulse still carries:
	 * this is the number that says whether the impulse was sized to reach the
	 * ceiling */
	if (k->vpitch != 0 &&
		(k->pitch + k->vpitch >= CD2_KNOCK_MAX_PITCH || k->pitch + k->vpitch <= -CD2_KNOCK_MAX_PITCH ||
		 (((long long)k->vpitch * CD2_KNOCK_DECAY) >> 12) == 0))
	{
		int peak = k->pitch + k->vpitch;

		/* the clamp in cd2KnockAxis is what the car actually gets */
		if (peak > CD2_KNOCK_MAX_PITCH) peak = CD2_KNOCK_MAX_PITCH;
		if (peak < -CD2_KNOCK_MAX_PITCH) peak = -CD2_KNOCK_MAX_PITCH;

		jer_log("[cainescrossfire] knock car=%d peak pitch=%d of %d\n",
			carId, peak, CD2_KNOCK_MAX_PITCH);
	}

	int settle = cd2KnockSettleRate(k->force);

	/* the three angles return at the rate the impulse earned; the lift and the shift keep
	 * their own rates, because they are a nudge and a weight transfer rather than the body
	 * coming back from an impact */
	cd2KnockAxis(&k->pitch, &k->vpitch, CD2_KNOCK_DECAY, settle, CD2_KNOCK_MAX_PITCH);
	cd2KnockAxis(&k->roll, &k->vroll, CD2_KNOCK_DECAY, settle, CD2_KNOCK_MAX_ROLL);
	cd2KnockAxis(&k->yaw, &k->vyaw, CD2_KNOCK_DECAY, settle, CD2_KNOCK_MAX_YAW);

	/* the lift wants to be zero, and never negative: this is a nudge to clear
	 * geometry, not a suspension */
	cd2KnockAxis(&k->lift, &k->vlift, CD2_KNOCK_LIFT_DECAY, CD2_KNOCK_LIFT_SETTLE, CD2_KNOCK_MAX_LIFT);

	if (k->lift < 0)
		k->lift = 0;

	cd2KnockAxis(&k->shift, &k->vshift, CD2_KNOCK_SHIFT_DECAY, CD2_KNOCK_SHIFT_SETTLE, CD2_KNOCK_MAX_SHIFT);

	/* the deadline: an exponential never arrives, so the settle is snapped shut
	 * after its 0.5s. Only once the impulse has been spent - a knock is never cut
	 * off on its way up. */
	if (k->settleFrames > 0)
	{
		k->settleFrames--;

		/* The counter closes only when there is nothing left to close - velocity AND
		 * angle. Checking velocity alone was the bug: an impulse that reaches its
		 * ceiling has its velocity ZEROED by the clip, so a knock sitting at five
		 * degrees looked finished and the memset took the whole angle in one frame.
		 * Measured before the fix: pitch -57 -> 0 between two frames. */
		if (k->settleFrames == 0 &&
			k->vpitch == 0 && k->vroll == 0 && k->vyaw == 0 && k->vlift == 0 && k->vshift == 0 &&
			CD2_KNOCK_IDLE(k->pitch) && CD2_KNOCK_IDLE(k->roll) && CD2_KNOCK_IDLE(k->yaw) &&
			CD2_KNOCK_IDLE(k->lift) && CD2_KNOCK_IDLE(k->shift))
		{
			memset(&gKnock[carId], 0, sizeof(gKnock[carId]));
		}
		else if (k->settleFrames == 0)
		{
			k->settleFrames = 1;	/* something is still live: keep settling it */
		}
	}

	cd2KnockSample(carId, k->force);
}

// ---------------------------------------------------------------------------
// CC_KNOCK_LOG=<frames> samples the knock's own state every N frames into the log - a
// run-only override like CC_MOTION_LOG. This exists because a one-frame jump in a
// rendered angle is invisible in a summary: the series is the only place it shows.
// ---------------------------------------------------------------------------
// [D] [T]
static int cd2KnockLogEvery(void)
{
	static int every = -1;

	if (every < 0)
	{
		const char* env = getenv("CC_KNOCK_LOG");

		every = 0;

		if (env != NULL && env[0] != 0)
		{
			int v = atoi(env);

			if (v > 0)
				every = v;
		}
	}

	return every;
}

// [D] [T]
static void cd2KnockSample(int carId, int force)
{
	const CD2_KNOCK_STATE* k;
	int every = cd2KnockLogEvery();

	if (every <= 0 || (FrameCnt % every) != 0)
		return;

	k = &gKnock[carId];

	jer_log("[cainescrossfire] knocksample car=%d f=%d pitch=%d vpitch=%d lift=%d shift=%d settle=%d rate=%d\n",
		carId, FrameCnt, k->pitch, k->vpitch, k->lift, k->shift, k->settleFrames, cd2KnockSettleRate(force));
}

// ---------------------------------------------------------------------------
// The render-only part: translate the body, then rotate the car about its OWN
// axes. Called from the car-draw path, so nothing here can reach the handling
// model.
//
// THE ONE THING TO GET RIGHT - and the bug this file had for a long time - is
// WHICH axes the angles turn about. The engine's rotation helpers (_RotMatrixX/Y/Z,
// convert.h) PRE-multiply: they do M' = R * M, i.e. they turn the car about the
// fixed WORLD axis of that name. That is only the car's own pitch/roll/yaw when
// the car happens to point along world +Z. At any other heading a "pitch" leaks
// into a roll and back, and the SIGN flips once with every half turn - so the same
// knock read as a nose-dive facing north and a nose-lift facing south, which is
// exactly "the cars don't knock according to their heading". Measured on the body
// matrix handed in here: a positive pitch nosed the car DOWN at heading 0, rolled
// it at 90 and lifted it at 180 (see the replication in the commit message).
//
// So the rotations are POST-multiplied here (M' = M * R, via MulMatrix0): the same
// pitch/roll/yaw numbers, but applied about the car's own axles, which makes the
// motion identical whichever way the car is pointing. With that, positive pitch =
// the FRONT lifts and negative = the REAR lifts - the convention every caller in
// this module was already written against (turbo engages with a nose-up wheelie,
// a frontal collision dives the nose). Nothing else had to change to fix the axis.
//
// BASIS, because it is not what it looks like: the matrix passed in is the car's
// DRAW matrix (cars.c), which is the physics matrix with rows 0 and 2 negated (a
// 180 turn about up - the model's nose is local -Z). Its COLUMNS are the world
// images of the model's axes: column 1 is the car's UP, column 2 is the car's
// BACKWARD. So the car's forward is -column 2, and its up is column 1. ROW 2 is
// NOT forward - it equals -column 2 only at heading 0 and diverges from there,
// which is the second half of the heading bug (the weight shift pointed the wrong
// way as soon as the car turned).
//
// This takes the COMPOSED offset and nothing else - no car id, no state - so every
// layer that wants to move a car goes through exactly this code and the pivot maths
// exists once.
// ---------------------------------------------------------------------------
void cd2VisualApply(void* matrix, const CD2_VISUAL_OFFSET* o)
{
	MATRIX* m = (MATRIX*)matrix;
	MATRIX rot, res;
	int i;

	if (m == NULL || o == NULL)
		return;

	if (o->bob != 0)
		m->t[1] += o->bob;		/* up or down, whichever the layer asked for */

	/* The pivot. Rotating the basis turns the car about the model's origin, which
	 * is somewhere in its middle - a wheelie has to turn about the rear axle, with
	 * the nose coming up, and a stoppie about the front, with the tail coming up.
	 * Moving the origin to compensate is what fakes that: the body rises by the
	 * arc the far end would have swept. Note the sign works out so the car only
	 * ever moves UP, whichever way it is pitching - so it can never be pushed down
	 * through the ground it is standing on. The rise is along the car's own up
	 * (column 1), which for an upright car is straight up. */
	if (o->pitch != 0)
	{
		int rise = ((o->pitch < 0 ? -o->pitch : o->pitch) * CD2_KNOCK_PIVOT_ARC) >> 12;

		for (i = 0; i < 3; i++)
			m->t[i] += (int)(((long long)m->m[i][1] * rise) >> 12);
	}

	if (o->shift != 0)
	{
		/* the weight moving: a translation along the car's own FORWARD axis (the
		 * model's nose is local -Z, so forward is -column 2), so a wheelie squats
		 * onto the back wheels and a frontal hit throws the weight forward. */
		for (i = 0; i < 3; i++)
			m->t[i] += (int)(((long long)(-m->m[i][2]) * o->shift) >> 12);
	}

	/* The rotations, composed onto the RIGHT so they act about the car's own axes
	 * rather than the world's. Built in the engine's own YZX order (RotMatrixX,
	 * then Z, then Y - Calc_Object_MatrixYZX, convert.c); the angles are small so
	 * the order barely matters, but matching the engine keeps it predictable.
	 * MulMatrix0 sets only the 3x3 (m2 = m0 * m1) and leaves t alone, so the
	 * translation applied above survives - that is why res is pre-loaded from *m. */
	if (o->pitch != 0 || o->roll != 0 || o->yaw != 0)
	{
		InitMatrix(rot);

		if (o->pitch != 0)
			RotMatrixX((int)o->pitch, &rot);
		if (o->roll != 0)
			RotMatrixZ((int)o->roll, &rot);
		if (o->yaw != 0)
			RotMatrixY((int)o->yaw, &rot);

		res = *m;
		MulMatrix0(m, &rot, &res);
		*m = res;
	}
}

// [D] [T]
void cd2KnockApply(void* matrix, int carId)
{
	const CD2_KNOCK_STATE* k = cd2KnockOf(carId);
	CD2_VISUAL_OFFSET o;

	o.pitch = k->pitch;
	o.roll = k->roll;
	o.yaw = k->yaw;
	o.bob = k->lift;		/* the knock's lift is never negative */
	o.shift = k->shift;

	cd2VisualApply(matrix, &o);
}
