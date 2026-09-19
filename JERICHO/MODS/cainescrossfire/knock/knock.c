/*
 * knock/knock.c -- see knock/knock.h for what a knock is and how it moves.
 */
#include "driver2.h"
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

// [D] [T]
void cd2KnockReset(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

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
void cd2KnockAdd(int carId, int pitch, int roll, int yaw, int lift)
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

// [D] [T]
void cd2KnockTick(int carId)
{
	CD2_KNOCK_STATE* k;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	k = &gKnock[carId];

	if (k->pitch == 0 && k->roll == 0 && k->yaw == 0 && k->lift == 0 &&
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

	cd2KnockAxis(&k->pitch, &k->vpitch, CD2_KNOCK_DECAY, CD2_KNOCK_SETTLE, CD2_KNOCK_MAX_PITCH);
	cd2KnockAxis(&k->roll, &k->vroll, CD2_KNOCK_DECAY, CD2_KNOCK_SETTLE, CD2_KNOCK_MAX_ROLL);
	cd2KnockAxis(&k->yaw, &k->vyaw, CD2_KNOCK_DECAY, CD2_KNOCK_SETTLE, CD2_KNOCK_MAX_YAW);

	/* the lift wants to be zero, and never negative: this is a nudge to clear
	 * geometry, not a suspension */
	cd2KnockAxis(&k->lift, &k->vlift, CD2_KNOCK_LIFT_DECAY, CD2_KNOCK_LIFT_SETTLE, CD2_KNOCK_MAX_LIFT);

	if (k->lift < 0)
		k->lift = 0;

	/* the deadline: an exponential never arrives, so the settle is snapped shut
	 * after its 0.5s. Only once the impulse has been spent - a knock is never cut
	 * off on its way up. */
	if (k->settleFrames > 0)
	{
		k->settleFrames--;

		if (k->settleFrames == 0 && k->vpitch == 0 && k->vroll == 0 && k->vyaw == 0 && k->vlift == 0)
			memset(&gKnock[carId], 0, sizeof(gKnock[carId]));
		else if (k->settleFrames == 0)
			k->settleFrames = 1;	/* still moving: shut it next frame instead */
	}
}

// ---------------------------------------------------------------------------
// The render-only part: rotate the car's own matrix, and raise the body. Called
// from the car-draw path, so nothing here can reach the handling model.
// ---------------------------------------------------------------------------
void cd2KnockApply(void* matrix, int carId)
{
	MATRIX* m = (MATRIX*)matrix;
	const CD2_KNOCK_STATE* k;

	if (m == NULL)
		return;

	k = cd2KnockOf(carId);

	if (k->lift != 0)
		m->t[1] += k->lift;		/* up, never down - clear of what it leans on */

	if (k->pitch != 0)
		_RotMatrixX(m, (short)k->pitch);

	if (k->roll != 0)
		_RotMatrixZ(m, (short)k->roll);

	if (k->yaw != 0)
		_RotMatrixY(m, (short)k->yaw);
}
