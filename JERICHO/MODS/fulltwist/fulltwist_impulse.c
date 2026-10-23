/*
 * fulltwist_impulse.c -- contact geometry and the angular impulse.
 *
 * This file owns the car-local math: the lever from the car origin to the hit,
 * the stock sign convention, the pitch/roll split and the write back to
 * st.n.angularVelocity. The hull queries that refine the contact point live in
 * fulltwist_hull.c; the event wiring lives in fulltwist.c.
 */

#include "driver2.h"
#include "jericho.h"
#include "jer_math.h"
#include "fulltwist.h"

#define FT_LAUNCH_BASE			120000	/* raw contact impulse at full severity */
#define FT_CONTACT_TORQUE_SHIFT	6		/* contact impulse r x J -> angular rate */

static int ftAbs(int v)
{
	return (v < 0) ? -v : v;
}

static long long ftAbs64(long long v)
{
	return (v < 0) ? -v : v;
}

static int ftCarInRange(const CAR_DATA* cp)
{
	return cp != NULL && cp->id >= 0 && cp->id < MAX_CARS;
}

int ftIsPlayerCar(const CAR_DATA* cp)
{
	return cp != NULL && cp->controlType == CONTROL_TYPE_PLAYER;
}

int ftIsTumbleCar(const CAR_DATA* cp)
{
	if (!ftCarInRange(cp))
		return 0;

	/* The camera and Tanner have CAR_DATA stand-ins; a scenery probe from
	 * either is not a vehicle and must not be tumbled. */
	if (cp->controlType == CONTROL_TYPE_NONE ||
		cp->controlType == CONTROL_TYPE_CAMERACOLLIDER ||
		cp->controlType == CONTROL_TYPE_TANNERCOLLIDER)
		return 0;

	return 1;
}

int ftWheelMask(const CAR_DATA* cp)
{
	int i;
	int mask = 0;

	for (i = 0; i < 4; i++)
	{
		if (cp->hd.wheel[i].susCompression != 0)
			mask |= 1 << i;
	}

	return mask;
}

/* Column i of hd.where.m is the world-space local axis i. */
static void ftGetAxes(const CAR_DATA* cp, int* ax, int* ay, int* az)
{
	ax[0] = cp->hd.where.m[0][0];
	ax[1] = cp->hd.where.m[1][0];
	ax[2] = cp->hd.where.m[2][0];

	ay[0] = cp->hd.where.m[0][1];
	ay[1] = cp->hd.where.m[1][1];
	ay[2] = cp->hd.where.m[2][1];

	az[0] = cp->hd.where.m[0][2];
	az[1] = cp->hd.where.m[1][2];
	az[2] = cp->hd.where.m[2][2];
}

static int ftDotLocal(int x, int y, int z, const int* axis)
{
	return (int)(((long long)x * axis[0] +
				  (long long)y * axis[1] +
				  (long long)z * axis[2]) >> 12);
}

/* Add a rotation about the car's local axes. roll is about local Z, pitch
 * about local X; local Y (yaw) is deliberately left alone. */
void ftAddLocalRate(CAR_DATA* cp, int roll, int pitch)
{
	int ax[3], ay[3], az[3];
	int i;

	if (roll == 0 && pitch == 0)
		return;

	ftGetAxes(cp, ax, ay, az);

	(void)ay;

	for (i = 0; i < 3; i++)
	{
		long long w = ((long long)az[i] * roll + (long long)ax[i] * pitch) >> 12;
		long long v = (long long)cp->st.n.angularVelocity[i] + w;

		if (v > FT_AV_CLAMP)
			v = FT_AV_CLAMP;
		else if (v < -FT_AV_CLAMP)
			v = -FT_AV_CLAMP;

		cp->st.n.angularVelocity[i] = (int)v;
	}
}

/* ------------------------------------------------------------------ */
/* Collision response                                                 */
/* ------------------------------------------------------------------ */

void ftHandleCar(CAR_DATA* car, CAR_DATA* other,
				 const VECTOR* point, const VECTOR* normal,
				 int hard, int impulseSign)
{
	FULLTWIST_CAR* st;
	CAR_COSMETICS* cos;
	FULLTWIST_HULL* hull;
	SVECTOR contact;
	int usedHull;
	int ax[3], ay[3], az[3];
	int rx, ry, rz;
	int lx, ly, lz;
	int nx, ny, nz;
	int lfx, lfy, lfz;
	int sx, sy, sz;
	int hx, hy, hz;
	int horiz, vert, off;
	long long tx, ty, tz, tumbleMag;
	int pitchNorm, rollNorm;
	int fxAbs, fzAbs, fyAbs, sum;
	int sideShare, frontShare;
	int rollDir, pitchDir;
	int rollMix, pitchMix;
	int span, sev, totalStrength, applyStrength;
	int rate, rollRate, pitchRate;
	int launch;
	int firstThisFrame;
	int mode;

	if (!ftIsTumbleCar(car) || point == NULL || normal == NULL || car->ap.carCos == NULL)
		return;

	if (gFullTwist.cfg.playerOnly &&
		!ftIsPlayerCar(car) &&
		!ftIsPlayerCar(other))
		return;

	st = &gFullTwist.cars[(int)car->id];
	firstThisFrame = (st->latchFrame != gFullTwist.frame);

	/* The hull is the real body shape: it supplies the contact point (and
	 * therefore the lever and the impulse angle) instead of the colBox guess.
	 * A car without built geometry keeps the old box path. */
	hull = ftHullForCar(car);

	ftGetAxes(car, ax, ay, az);

	rx = point->vx - car->hd.where.t[0];
	ry = point->vy - car->hd.where.t[1];
	rz = point->vz - car->hd.where.t[2];

	lx = ftDotLocal(rx, ry, rz, ax);
	ly = ftDotLocal(rx, ry, rz, ay);
	lz = ftDotLocal(rx, ry, rz, az);

	nx = normal->vx * impulseSign;
	ny = normal->vy * impulseSign;
	nz = normal->vz * impulseSign;

	lfx = ftDotLocal(nx, ny, nz, ax);
	lfy = ftDotLocal(nx, ny, nz, ay);
	lfz = ftDotLocal(nx, ny, nz, az);

	cos = car->ap.carCos;
	usedHull = 0;

	/* Snap the engine's reported point to the hull face it belongs to. The
	 * hull contact is the lever the impulse actually acts through; without a
	 * built hull (or on a degenerate one) the box point is kept. */
	if (hull != NULL)
	{
		if (ftHullContact(hull, lx, ly, lz, lfx, lfy, lfz, &contact))
		{
			lx = contact.vx;
			ly = contact.vy;
			lz = contact.vz;
			usedHull = 1;
		}
	}

	hx = (usedHull && hull->hx > 0) ? hull->hx : ((cos != NULL && cos->colBox.vx > 0) ? cos->colBox.vx : 100);
	hy = (usedHull && hull->hy > 0) ? hull->hy : ((cos != NULL && cos->colBox.vy > 0) ? cos->colBox.vy : 60);
	hz = (usedHull && hull->hz > 0) ? hull->hz : ((cos != NULL && cos->colBox.vz > 0) ? cos->colBox.vz : 180);
	sx = hx;
	sy = hy;
	sz = hz;

	/* Horizontal lever offset dominates "off-centre"; a vertical offset is
	 * what a wall contact has (the engine puts its hit point below the
	 * centre), and it is what turns a head-on wall hit into a pitch. */
	horiz = (ftAbs(lx) * 100 / sx + ftAbs(lz) * 100 / sz) / 2;
	vert = (ftAbs(ly) * 100 / sy) * 3 / 4;
	off = (horiz > vert) ? horiz : vert;

	if (hard < gFullTwist.cfg.minImpact)
	{
		if (gFullTwist.cfg.debugLog && firstThisFrame)
		{
			FT_LOG("[fulltwist] ignore frame=%u car=%d other=%d hard=%d min=%d off=%d lever=(%d,%d,%d) local=(%d,%d,%d)\n",
				gFullTwist.frame, (int)car->id, other ? (int)other->id : -1,
				hard, gFullTwist.cfg.minImpact, off, rx, ry, rz, lx, ly, lz);
		}
		return;
	}

	if (off < gFullTwist.cfg.offCenter)
	{
		if (gFullTwist.cfg.debugLog && firstThisFrame)
		{
			FT_LOG("[fulltwist] ignore frame=%u car=%d other=%d hard=%d off=%d need=%d lever=(%d,%d,%d) local=(%d,%d,%d)\n",
				gFullTwist.frame, (int)car->id, other ? (int)other->id : -1,
				hard, off, gFullTwist.cfg.offCenter, rx, ry, rz, lx, ly, lz);
		}
		return;
	}

	/* Physical torque in the car's own frame: r_local x F_local. tx is
	 * pitch, ty is yaw, tz is roll. */
	tx = (long long)ly * lfz - (long long)lz * lfy;
	ty = (long long)lz * lfx - (long long)lx * lfz;
	tz = (long long)lx * lfy - (long long)ly * lfx;

	tumbleMag = ftAbs64(tx) + ftAbs64(tz);

	pitchNorm = 0;
	rollNorm = 0;

	if (tumbleMag > 0)
	{
		long long rollI = (long long)hx * hx + (long long)hy * hy;
		long long pitchI = (long long)hy * hy + (long long)hz * hz;
		long long rollW, pitchW, totalW;

		if (rollI < 1) rollI = 1;
		if (pitchI < 1) pitchI = 1;

		/* Angular impulse / hull inertia about the two tumbling axes. The
		 * reference inertia cancels in the normalisation, so this is the
		 * physical split of the same rate between roll and pitch. */
		rollW = ftAbs64(tz) * 4096 / rollI;
		pitchW = ftAbs64(tx) * 4096 / pitchI;
		totalW = rollW + pitchW;

		if (totalW > 0)
		{
			rollNorm = (int)((tz >= 0 ? rollW : -rollW) * 4096 / totalW);
			pitchNorm = (int)((tx >= 0 ? pitchW : -pitchW) * 4096 / totalW);
		}
	}
	/* The pure lever x normal result is often mostly yaw (a flat side hit).
	 * The trip percentage redirects a share of the impulse into the two
	 * axes a tumble needs, in the direction the car was actually pushed. */
	fxAbs = ftAbs(lfx);
	fzAbs = ftAbs(lfz);
	fyAbs = ftAbs(lfy);
	sum = fxAbs + fzAbs + fyAbs + 1;

	sideShare = fxAbs * 4096 / sum;
	frontShare = fzAbs * 4096 / sum;

	rollDir = (lfx >= 0 ? -1 : 1) * sideShare;
	pitchDir = (lfz >= 0 ? 1 : -1) * frontShare;

	rollMix = (rollNorm * (100 - gFullTwist.cfg.trip) + rollDir * gFullTwist.cfg.trip) / 100;
	pitchMix = (pitchNorm * (100 - gFullTwist.cfg.trip) + pitchDir * gFullTwist.cfg.trip) / 100;

	span = FT_FULL_IMPACT - gFullTwist.cfg.minImpact;
	if (span < 1)
		span = 1;

	sev = hard - gFullTwist.cfg.minImpact;
	if (sev > span)
		sev = span;
	if (sev < 0)
		sev = 0;

	totalStrength = sev * 4096 / span;

	mode = 0;
	applyStrength = totalStrength;

	if (!firstThisFrame)
	{
		int extra = hard - st->latchHard;

		if (extra <= 0)
		{
			if (gFullTwist.cfg.debugLog)
			{
				FT_LOG("[fulltwist] latch frame=%u car=%d hard=%d strongest=%d skip\n",
					gFullTwist.frame, (int)car->id, hard, st->latchHard);
			}
			return;
		}

		if (extra > span)
			extra = span;

		applyStrength = extra * 4096 / span;
		mode = 1;
	}

	rate = (int)((long long)gFullTwist.cfg.maxRate * applyStrength / 4096 *
				 gFullTwist.cfg.intensity / 100);

	/* Heavier / more twist-resistant chassis take less of the impulse. The
	 * reference is the probe's typical record (mass 4096, twistRateY 224), so
	 * the default car's scale is 4096 (1.0x). */
	if (cos != NULL)
	{
		int mass = (cos->mass > 0) ? cos->mass : 4096;
		int twistY = (cos->twistRateY > 0) ? cos->twistRateY : 224;
		long long inertiaScale = ((long long)4096 * 4096 * 224) /
								 ((long long)mass * twistY);

		if (inertiaScale < 2048)
			inertiaScale = 2048;
		else if (inertiaScale > 8192)
			inertiaScale = 8192;

		rate = (int)((long long)rate * inertiaScale / 4096);
	}

	rollRate = (int)((long long)rate * rollMix / 4096);
	pitchRate = (int)((long long)rate * pitchMix / 4096);

	ftAddLocalRate(car, rollRate, pitchRate);

	if (firstThisFrame)
	{
		st->latchFrame = gFullTwist.frame;
		st->settleFrames = 0;
	}

	st->latchHard = hard;
	st->tumbleFrames = gFullTwist.cfg.window;
	st->tumbleStrength = totalStrength;
	st->tumbleRollShare = rollMix;
	st->tumblePitchShare = pitchMix;
	st->tumbleRate = rate;
	st->landed = 0;

	/* A grounded car is held by its suspension: the stock contact response
	 * cancels most of the roll/pitch before it can rotate. This pop is a
	 * contact impulse at the hit point, along the collision normal (plus a
	 * small upward lift), so the normal itself contributes r x J to the
	 * tumble. The linear part is deliberately modest; the angular rate above
	 * is what does the tumbling. */
	launch = 0;

	if (firstThisFrame && gFullTwist.cfg.launch > 0)
	{
		int imp = (int)((long long)FT_LAUNCH_BASE * totalStrength / 4096 *
						gFullTwist.cfg.launch / 100);

		if (imp > 0)
		{
			int lift = (ftWheelMask(car) != 0) ? imp / 4 : 0;
			int fx = (normal->vx * impulseSign * imp) >> 12;
			int fy = ((normal->vy * impulseSign * imp) >> 12) + lift;
			int fz = (normal->vz * impulseSign * imp) >> 12;
			int jx, jy, jz;
			long long contactPitch, contactRoll;

			car->st.n.linearVelocity[0] += fx;
			car->st.n.linearVelocity[1] += fy;
			car->st.n.linearVelocity[2] += fz;

			/* Local components of the contact impulse. */
			jx = ftDotLocal(fx, fy, fz, ax);
			jy = ftDotLocal(fx, fy, fz, ay);
			jz = ftDotLocal(fx, fy, fz, az);

			/* r_local x J_local: pitch about local X, roll about local Z. */
			contactPitch = (long long)ly * jz - (long long)lz * jy;
			contactRoll = (long long)lx * jy - (long long)ly * jx;

			ftAddLocalRate(car,
				(int)(contactRoll >> FT_CONTACT_TORQUE_SHIFT),
				(int)(contactPitch >> FT_CONTACT_TORQUE_SHIFT));

			launch = imp;
		}
	}

	if (gFullTwist.cfg.debugLog)
	{
		FT_LOG("[fulltwist] hit frame=%u car=%d other=%d mode=%d hard=%d off=%d lever=(%d,%d,%d) local=(%d,%d,%d) normalLocal=(%d,%d,%d) torque=(%d,%d,%d) rollMix=%d pitchMix=%d rate=%d launch=%d strength=%d hull=%d inertia=(%d,%d,%d) contact=(%d,%d,%d)\n",
			gFullTwist.frame, (int)car->id, other ? (int)other->id : -1,
			mode, hard, off, rx, ry, rz, lx, ly, lz, lfx, lfy, lfz,
			(int)tx, (int)ty, (int)tz, rollMix, pitchMix, rate, launch, totalStrength,
			usedHull, hx, hy, hz, lx, ly, lz);
	}
}
