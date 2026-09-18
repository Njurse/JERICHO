// cainescrossfiresim.c — Combat D2: the point-mass handling model.
//
// (Split out of cainescrossfire.c; see cainescrossfire_internal.h for the file map.)
//
// This is the heart of the mod. It replaces the horizontal motion of the stock
// Driver 2 sim with a point-mass rigid body, written at JER_EVENT_CAR_TORQUE
// (the tail of StepOneCar, right before the engine integrates velocity and
// orientation):
//
//   * throttle -> direct forward/backward velocity (no engine/gears)
//   * steering -> direct yaw rate (target yaw = handling * steer, works at
//     zero speed: rotate in place); authority is retained through any slide
//   * Tight Turn (Triangle/handbrake) -> an acute forced pivot, its own yaw
//     authority that bleeds a little speed (drift-slide with gas)
//   * brakes -> fast and proportional (strong at speed, smooth taper)
//   * lateral grip -> high and shallow-sagging, so skids are brief and never
//     a loss-of-control spiral
//
// Vertical motion (gravity + ground lift) and roll/pitch are left to the
// stock code so the car still rides the terrain. A gated engine query
// (JER_EVENT_GET_WALL_RESTITUTION) makes scenery hits absorb momentum;
// collisions are otherwise stock mass-based push.

#include "driver2.h"
#include "cainescrossfire.h"
#include "cainescrossfire_internal.h"
#include "cars.h"
#include "convert.h"
#include "players.h"
#include "pad.h"
#include "main.h"		/* FrameCnt */
#include "debris.h"		/* Setup_Smoke / SMOKE_FIRE */
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "turbo/turbo.h"		/* cd2TurboPad - the double-tap trigger */
#include "knock/knock.h"		/* the visual knock (buck and rock) */
#include "jer_math.h"
#include "sound.h"
#include "gamesnd.h"

// With no gas and no brake, below this speed (units/frame) the point-mass
// velocity snaps to a dead stop so the car can't creep / micro-roll a few
// units forever.
#define CD2_MICRO_STOP_SPEED		20

static unsigned int gDbgFrame;	// telemetry frame counter
static char gPendingTotalCar;	// set by the pause-menu "Total Car", applied next physics frame

// The pause menu QUEUES a "Total Car"; the next physics frame applies it so the
// wreck/explosion never fire while the pause menu is up. See cainescrossfiremenu.c.
void cd2CarQueueTotal(void)
{
	if (MainPlayer.playerCarId >= 0)
		gPendingTotalCar = 1;
}

// ---- TMB in-car button layout (JER_EVENT_CAR_PAD override) --------------
//
// This module does NOT rebind physical buttons. Instead it takes over the
// car's pedal semantics at JER_EVENT_CAR_PAD (ProcessCarPad): when the TMB
// layout is on and the pad is a live player, we write cp->thrust/
// cp->handbrake/cp->wheelspin ourselves and set handled=1, so the engine
// SKIPS its stock face-button assignment for that car this frame -- the
// original car binds can never double-fire alongside ours. Engine steering
// (wheel_angle) is untouched, so the analog curve stays stock. On foot,
// AI/lead/cutscene cars and the clamped locked-car state are never overridden.
int cd2OnCarPad(void* ud, void* args)
{
	JER_ARGS_CAR_PAD* a = (JER_ARGS_CAR_PAD*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	int pad, tight, gas, brake;
	(void)ud;

	if (!gCd2Cfg.enabled || !a->live)
		return JER_RESULT_CONTINUE;

	/* the turbo trigger sees the pad before anything in here rewrites it: the
	 * double tap is a driver gesture, not a control remap */
	cd2TurboPad(cp->id, a->pad);

	// The shoulders + triggers are the weapon controls now (L1/R1 = prev/next
	// weapon, L2/R2 = fire). Strip them from the car's action bits so the
	// stock fast-steer (L1) never fires alongside; the stock horn (R1) is
	// cleared in the weapons FRAME handler.
	a->pad &= ~(MPAD_L1 | MPAD_L2 | MPAD_R1 | MPAD_R2);

	if (!gCd2Cfg.tmbButtons)
		return JER_RESULT_CONTINUE;

	pad = a->pad;

	if (gCd2Cfg.debugLog)
	{
		static unsigned int t = 0;
		if ((t++ & 15) == 0)
			printInfo("[cainescrossfire] pad=0x%04X (Cross 0x%X/Square 0x%X/Circle 0x%X/Triangle 0x%X)\n",
				pad, MPAD_CROSS, MPAD_SQUARE, MPAD_CIRCLE, MPAD_TRIANGLE);
	}

	// PS-position reference (PlayStation face buttons):
	//   default : Cross(bottom) = Tight Turn, Square(left) = Gas, Circle = Brake
	//   tmbTight: Square(left)  = Tight Turn, Cross(bottom) = Gas, Circle = Brake
	gas   = gCd2Cfg.tmbTight ? (pad & MPAD_CROSS)  : (pad & MPAD_SQUARE);
	tight = gCd2Cfg.tmbTight ? (pad & MPAD_SQUARE) : (pad & MPAD_CROSS);
	brake = pad & MPAD_CIRCLE;

	cp->handbrake = 0;
	// Deliberately never set cp->wheelspin: holding the tight button would
	// trip the stock burnout path (rev scream, gear-0 wheelspin revs). The
	// torque reads the tight button straight from the raw pad instead.
	cp->wheelspin = 0;

	// sign convention matches stock (positive = drive force, negative =
	// brake/reverse); cainescrossfire's torque only reads the sign at CAR_STEP.
	if (gas)
		cp->thrust = (short)CD2_TMB_THRUST;
	else if (brake)
		cp->thrust = -(short)CD2_TMB_THRUST;
	else
		cp->thrust = 0;

	a->handled = 1; // stock face-button binds are skipped this frame
	return JER_RESULT_CONTINUE;
}

// Roll-over suppression. Runs at CAR_STEP (after cainescrossfirewreckfx's wreck toss,
// before the engine integrates velocity + orientation): the pitch/roll rates
// are capped so no single impulse can flip a car in one frame, and the car is
// stopped rolling once it leans past gCd2Cfg.rollLimit degrees. Weapons can
// still knock a car onto two wheels, but it can't end up on its roof.
static void cd2LimitRoll(CAR_DATA* cp)
{
	int* av = cp->st.n.angularVelocity;	// [0] pitch, [1] yaw, [2] roll
	int dotUp, cosLimit;

	if (gCd2Cfg.rollLimit <= 0)
		return;

	av[0] = jer_clamp_int(av[0], -CD2_ROLL_MAX_AV, CD2_ROLL_MAX_AV);
	av[2] = jer_clamp_int(av[2], -CD2_ROLL_MAX_AV, CD2_ROLL_MAX_AV);

	// car up . world up: 4096 upright, 0 fully on its side, -4096 on the roof
	dotUp = cp->hd.where.m[1][1];
	cosLimit = RCOS((gCd2Cfg.rollLimit * 4096) / 360);

	if (dotUp < cosLimit)
	{
		if (gCd2Cfg.debugLog)
			printInfo("[cainescrossfire] roll clamp: car=%d dotUp=%d limit=%d\n",
				cp->id, dotUp, cosLimit);

		// past the allowed lean: stop rolling; the stock suspension + gravity
		// settle the car back onto its wheels
		av[0] = 0;
		av[2] = 0;
	}
}

extern void RebuildCarMatrix(RigidBodyState* st, CAR_DATA* cp);

static int cd2RollIsqrt(int v)
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

// Post-physics roll recovery. JER_EVENT_DEBUG_TICK fires at the very end of
// GlobalTimeStep, after collisions, so this catches a car the crash code has
// already tipped past the limit: it rotates the car's up axis back toward world
// up by CD2_ROLL_RECOVER_DEG and kills the tilt rates. Uses the engine's own
// left-multiply quaternion convention (q += omega (x) q), axis = normalize(carUp
// x worldUp) = normalize((-cz, 0, cx)).
static void cd2RecoverRoll(CAR_DATA* cp)
{
	MATRIX* w = &cp->hd.where;
	int cx = w->m[0][1], cy = w->m[1][1], cz = w->m[2][1];
	int cosLimit, ax, az, len, ang, half, s, c, qx, qz, qw;
	int ox, oy, oz, ow;
	int* q = cp->st.n.orientation;

	cosLimit = RCOS((gCd2Cfg.rollLimit * 4096) / 360);
	if (cy >= cosLimit)
		return;		// within the allowed lean

	ax = -cz;
	az = cx;
	len = cd2RollIsqrt(ax * ax + az * az);
	if (len < 1)
		return;		// car is perfectly inverted on its forward axis: n/a

	ax = (int)(((long long)ax * 4096) / len);
	az = (int)(((long long)az * 4096) / len);

	ang = (CD2_ROLL_RECOVER_DEG * 4096) / 360;
	half = ang / 2;
	s = RSIN(half);
	c = RCOS(half);

	// q_corr = (axis*sin(half), cos(half)) as (x,y,z,w)
	qx = (int)(((long long)ax * s) >> 12);
	qz = (int)(((long long)az * s) >> 12);
	qw = c;

	// q = q_corr (x) q
	ox = q[0]; oy = q[1]; oz = q[2]; ow = q[3];
	q[0] = (qw * ox + qx * ow - qz * oy) >> 12;
	q[1] = (qw * oy - qx * oz + qz * ox) >> 12;
	q[2] = (qw * oz + qx * oy + qz * ow) >> 12;
	q[3] = (qw * ow - qx * ox - qz * oz) >> 12;

	cp->st.n.angularVelocity[0] = 0;
	cp->st.n.angularVelocity[2] = 0;

	RebuildCarMatrix(&cp->st, cp);

	if (gCd2Cfg.debugLog)
		printInfo("[cainescrossfire] roll recover: car=%d dotUp=%d\n", cp->id, cy);
}

// CAR_STEP: capture the raw throttle BEFORE the stock wheel-force code can
// change it. AddWheelForcesDriver1 -> GetFrictionScalesDriver1 forces
// cp->thrust = 0 while the handbrake is held (so the handbrake alone would
// otherwise read as "coast"); the tight turn needs to know gas is still down.
int cd2OnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!gCd2Cfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	// deferred "Total Car" debug: applied on the first physics frame after
	// unpausing so the wreck/explosion don't fire while the pause menu is up.
	if (gPendingTotalCar && cp->id == MainPlayer.playerCarId)
	{
		gPendingTotalCar = 0;
		cp->totalDamage = 0xffff;
		Start3DSoundVolPitch(
			-1,
			SOUND_BANK_MISSION,
			29,
			cp->hd.where.t[0],
			cp->hd.where.t[1],
			cp->hd.where.t[2],
			-2000,
			4096 + 2048
		);
	}

	gCd2Car[cp->id].throttle = (cp->thrust > 0) ? 1 : (cp->thrust < 0) ? -1 : 0;

	// Traffic is exempt from the roll-over limiter: it is meant to be thrown
	// around, and clamping its pitch/roll is exactly what stopped that.
	if (cd2IsTraffic(cp))
		cd2TrafficTumble(cp);
	else
		cd2LimitRoll(cp);

	// wrecked cars return to their start after a delay
	cd2RespawnTick(cp);

	return JER_RESULT_CONTINUE;
}

// CAR_TORQUE: the point-mass integrator. Runs at the very end of StepOneCar,
// after the stock wheel forces are computed but before the engine integrates
// velocity + orientation, so we can zero the stock horizontal forces and write
// our own velocity / yaw.
int cd2OnCarTorque(void* ud, void* args)
{
	JER_ARGS_CAR_TORQUE* a = (JER_ARGS_CAR_TORQUE*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!gCd2Cfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	switch (cp->controlType)
	{
		case CONTROL_TYPE_PLAYER:
		case CONTROL_TYPE_CIV_AI:
		case CONTROL_TYPE_PURSUER_AI:
		case CONTROL_TYPE_LEAD_AI:
		case CONTROL_TYPE_CUTSCENE:
			break;
		default:
			return JER_RESULT_CONTINUE; // NONE / camera & tanner colliders
	}

	// Traffic keeps STOCK handling. The TMB handling / top-speed override is
	// for the player and the opponents; civilian traffic should stay slow and
	// unwieldy so it reads as scenery that can be knocked about.
	if (cd2IsTraffic(cp))
		return JER_RESULT_CONTINUE;

	CD2_CAR* c = &gCd2Car[cp->id];
	CD2_STATS s = cd2GetStats(cp);

	// ---- steering: direct yaw control -------------------------------
	int steer = jer_clamp_int((int)cp->wheel_angle, -CD2_STEER_MAX, CD2_STEER_MAX);
	int steerFp = (steer * 4096) / CD2_STEER_MAX;           // -4096..4096
	int handlingNow = (int)(((long long)s.handling * s.control) >> 12);

	// TM2 speed-sensitive yaw: turn authority tapers as speed rises so the
	// car can't spin out at top speed (opt-in via CD2_YAW_SPEED_FALLOFF).
	if (CD2_YAW_SPEED_FALLOFF)
	{
		int lvx = cp->st.n.linearVelocity[0];
		int lvz = cp->st.n.linearVelocity[2];
		int mag = (ABS(FIXEDH(lvx)) < ABS(FIXEDH(lvz)))
			? (ABS(FIXEDH(lvz)) + ABS(FIXEDH(lvx)) / 2)
			: (ABS(FIXEDH(lvx)) + ABS(FIXEDH(lvz)) / 2);
		if (mag > s.topSpeed) mag = s.topSpeed;
		int norm = (s.topSpeed > 0) ? (int)(((long long)mag * 4096) / s.topSpeed) : 0;
		handlingNow = (int)(((long long)handlingNow * (4096 - ((CD2_YAW_SPEED_FALLOFF * norm) >> 12))) >> 12);
	}

	// ---- Tight Turn (TMB): acute pivot on its own yaw authority -----
	// Player-only. While the trigger is held, steering picks/latches a pivot
	// direction; releasing clears it. It is independent of the grip pass, so
	// the pivot keeps full authority through any slide (TMB: "overrides
	// normal physics for a brief moment").
	int tightActive = 0;
	int slideNow = 0; // traction-suspended slide active (computed once per frame)
	if (gCd2Cfg.tightTurn)
	{
		if (c->aiPivot != 0)
		{
			// An AI car asked for an acute in-place pivot this frame.
			tightActive = 1;
		}
		else if (cp->controlType == CONTROL_TYPE_PLAYER)
		{
		if (gCd2Cfg.tmbButtons)
		{
			// Tight Turn = the layout's tight face button, read from the raw
			// pad (engine-native bits). NOT cp->wheelspin: the CAR_PAD
			// override leaves wheelspin clear, so X never triggers burnout.
			int padm = 0;
			if (cp->ai.padid != NULL && *cp->ai.padid >= 0 && *cp->ai.padid < 2)
				padm = Pads[*cp->ai.padid].mapped;
			tightActive = ((gCd2Cfg.tmbTight ? (padm & MPAD_SQUARE) : (padm & MPAD_CROSS)) != 0);
		}
		else if (gCd2Cfg.tightInput == CD2_TIGHT_INPUT_HANDBRAKE)
			tightActive = cp->handbrake;
		else if (gCd2Cfg.tightInput == CD2_TIGHT_INPUT_WHEELSPIN)
			tightActive = cp->wheelspin;
		}
	}

	if (tightActive)
	{
		if ((steer < 0 ? -steer : steer) > CD2_TIGHT_STEER_MIN)
			c->pivotDir = steer > 0 ? 1 : -1;
	}
	else
	{
		c->pivotDir = 0;
	}

	int targetYaw;
	if (tightActive && c->pivotDir != 0)
	{
		// forced acute rotation: normal handling x CD2_TIGHT_MULT, so the
		// pivot scales with the car's own steering authority; the strength
		// slider blends between plain handling and the full pivot.
		long long pivot = (handlingNow * CD2_TIGHT_MULT) >> 12;
		pivot = handlingNow + ((pivot - handlingNow) * gCd2Cfg.tightStrength) / 100;
		targetYaw = (int)pivot * c->pivotDir;
	}
	else
	{
		targetYaw = (handlingNow * steerFp) >> 12;           // PSX-units/frame
	}

	// yaw accelerates toward the target; snappier onset during a pivot, and a
	// stronger step while returning to center so the car stops spinning
	// promptly instead of carrying rotation (less angular momentum).
	int yawStep = s.angularAccel;
	if (tightActive)
		yawStep = (int)(yawStep * CD2_TIGHT_ANG_MULT);
	int yaw = c->yawRate;
	if (ABS(targetYaw) < ABS(yaw))
	{
		// stifle the tight-turn rotation velocity below speed: once the
		// trigger is released a barely-rolling car shouldn't keep spinning
		int lvx = cp->st.n.linearVelocity[0];
		int lvz = cp->st.n.linearVelocity[2];
		int mag = (ABS(FIXEDH(lvx)) < ABS(FIXEDH(lvz)))
			? (ABS(FIXEDH(lvz)) + ABS(FIXEDH(lvx)) / 2)
			: (ABS(FIXEDH(lvx)) + ABS(FIXEDH(lvz)) / 2);
		if (!tightActive && mag < CD2_TIGHT_ROT_STOP_SPEED)
			yaw = targetYaw; // snap the residual pivot spin off
		else
			yawStep = (int)(((long long)yawStep * CD2_YAW_DECAY) >> 12);
	}
	if (yaw < targetYaw)
	{
		yaw += yawStep;
		if (yaw > targetYaw) yaw = targetYaw;
	}
	else if (yaw > targetYaw)
	{
		yaw -= yawStep;
		if (yaw < targetYaw) yaw = targetYaw;
	}
	c->yawRate = yaw;

	// ---- forward / right vectors (x-z plane, fixed-point unit) -------
	int fx = cp->hd.where.m[0][2];
	int fz = cp->hd.where.m[2][2];
	int rx = cp->hd.where.m[0][0];
	int rz = cp->hd.where.m[2][0];

	int velX = cp->st.n.linearVelocity[0];
	int velZ = cp->st.n.linearVelocity[2];

	/* TURBO: flames out of the back while the boost runs. fx/fz are the car's own
	 * forward unit vector, so the emitter sits behind the car wherever it points.
	 * The vanilla exhaust puff is left alone - suppressing it would mean an engine
	 * change, and the flames read over it anyway. */
	if (cd2TurboActive(cp->id) && (FrameCnt & 3) == 0)
	{
		VECTOR sp, drift;

		sp.vx = cp->hd.where.t[0] - (int)(((long long)fx * 150) >> 12);
		sp.vz = cp->hd.where.t[2] - (int)(((long long)fz * 150) >> 12);
		sp.vy = cp->hd.where.t[1] + 30;

		drift.vx = 0;
		drift.vy = 0;
		drift.vz = 0;

		Setup_Smoke(&sp, 18, 45, SMOKE_FIRE, 0, &drift, 0);
	}

	/* TURBO: the engagement shove. A push along the heading, applied as soon as
	 * the boost starts - throttle or not, which is the point of a shove - and
	 * ahead of the speed maths below, so the rest of the frame sees it. */
	{
		int shovePct = cd2TurboTakeShove(cp->id);

		if (shovePct > 0)
		{
			int shove = (s.topSpeed * shovePct) / 100;
			velX += fx * shove;
			velZ += fz * shove;
			jer_log("[cainescrossfire] turbo: shove +%d percent of top speed (%d)\n", shovePct, shove);
		}
	}

	long long fwdSpeed = ((long long)velX * fx + (long long)velZ * fz) >> 24; // speed units (vel and the unit vector are both 4096-scaled)

	// Current horizontal speed (magnitude), used for the slide decision.
	// NOTE steering below is deliberately NOT inverted in reverse: like TMB
	// ("you steer the front of the car as-is"), steer input rotates the nose
	// the same way whether driving forward or backward — which is exactly what
	// gives reversing its mirrored, rear-led feel.
	{
		int ax = ABS(FIXEDH(velX));
		int az = ABS(FIXEDH(velZ));
		int speedNow = (ax < az) ? (az + ax / 2) : (ax + az / 2); // speed units
		slideNow = tightActive && c->pivotDir != 0 && speedNow > CD2_SLIDE_MIN_SPEED;
	}

	// Airborne: conserve momentum — no throttle/brake/drag and no tire
	// friction (grip/bleed). Yaw above still applies, so the player can
	// rotate mid-air (TM air control) without the car being "driven" or
	// air-braked in flight.
	int grounded = (cp->hd.wheel[0].susCompression | cp->hd.wheel[1].susCompression |
	                cp->hd.wheel[2].susCompression | cp->hd.wheel[3].susCompression) != 0;

	long long latVel = 0; // in scope for the telemetry below too
	int grip = 0;

	if (grounded)
	{
	// throttle is snapshotted at CAR_STEP: the stock wheel-force code zeroes
	// cp->thrust while the handbrake is held, which would otherwise read as
	// "coast" mid tight turn.
	int throttle = c->throttle;

	// ---- throttle / brake / reverse / drag --------------------------
	if (throttle > 0)
	{
		int accel = s.accel;
		if (slideNow)
			accel = (int)(((long long)accel * CD2_SLIDE_ACCEL_FRAC) >> 12);
		if (fwdSpeed < s.topSpeed)
		{
			velX += fx * accel;
			velZ += fz * accel;

	}
		else
		{
			velX -= (int)(((long long)velX * s.drag) >> 12);
			velZ -= (int)(((long long)velZ * s.drag) >> 12);
		}
	}
	else if (throttle < 0)
	{
		if (fwdSpeed > -s.reverseSpeed)
		{
			// TMB "fast and proportional" brake: strong scrub at speed that
			// tapers near zero (a small constant floor keeps it from being
			// asymptotic), so stopping is quick but never a jarring dead stop.
			int brake = s.brake;
			if (fwdSpeed > 0)
			{
				long long ratio = fwdSpeed * 4096 / s.topSpeed;
				if (ratio > 4096) ratio = 4096;
				brake = (int)(((long long)s.brake *
					(CD2_BRAKE_FLOOR + (4096 - CD2_BRAKE_FLOOR) * ratio / 4096)) >> 12);
			}
			else
			{
				// reversing: gentler, continuous push (no dead zone at 0)
				brake = (int)(((long long)s.brake * CD2_REVERSE_ACCEL_FRAC) >> 12);
			}
			velX -= fx * brake;
			velZ -= fz * brake;
		}
		else
		{
			velX -= (int)(((long long)velX * s.drag) >> 12);
			velZ -= (int)(((long long)velZ * s.drag) >> 12);
		}
	}
	else if (!slideNow)
	{
		// no input: rolling drag — skipped during a slide so the car glides
		velX -= (int)(((long long)velX * s.drag) >> 12);
		velZ -= (int)(((long long)velZ * s.drag) >> 12);

		// idle micro-roll guard: with no gas and no brake (this is the branch
		// where throttle == 0) and barely any speed left, snap to a full stop
		// so the car doesn't creep / roll a few units forever
		{
			int ax = ABS(FIXEDH(velX));
			int az = ABS(FIXEDH(velZ));

			if ((ax < az ? (az + ax / 2) : (ax + az / 2)) < CD2_MICRO_STOP_SPEED)
			{
				velX = 0;
				velZ = 0;
			}
		}
	}

	// tight-turn momentum bleed: the forced pivot sheds a little horizontal
	// speed so holding gas yields a drift-slide arc rather than a dead stop.
	// While the slide is active the scrub is stronger: gas + Tight Turn must
	// shed speed (TMB), not build it.
	if (tightActive && c->pivotDir != 0)
	{
		int bleed = slideNow ? CD2_SLIDE_BLEED : CD2_TIGHT_BLEED;
		velX = (int)(((long long)velX * (4096 - bleed)) >> 12);
		velZ = (int)(((long long)velZ * (4096 - bleed)) >> 12);
	}

	// ---- lateral grip (drift) ---------------------------------------
	latVel = ((long long)velX * rx + (long long)velZ * rz) >> 24; // speed units (4096-scaled dot)

	int absSteer = steerFp < 0 ? -steerFp : steerFp;          // 0..4096
	long long absSpeed = fwdSpeed < 0 ? -fwdSpeed : fwdSpeed;
	if (absSpeed > s.topSpeed) absSpeed = s.topSpeed;

	// slipFactor 0..4096: hard steer + high speed -> grip falls off
	long long slipFactor = (absSteer * absSpeed) / s.topSpeed;
	int gripDrop = (int)((slipFactor * CD2_SLIP_REDUCTION) / 10);
	grip = (int)(((long long)s.grip * (4096 - gripDrop)) >> 12);

	// Iconic TMB slide: while the Tight Turn pivot is active and the car is
	// fast enough, traction is suspended — grip drops to a few percent, so
	// the car keeps travelling along its ORIGINAL velocity vector while the
	// pivot rotates the heading underneath it (steerable slide). Letting off
	// RECOVERS the velocity toward the heading WITHOUT stopping: the old
	// lateral wipe scrubbed the car dead once the pivot had rotated the
	// heading away from the travel direction. The recovery instead rotates
	// the velocity back onto the heading while conserving its magnitude, so
	// you exit the turn carrying your speed (TMB momentum).
	if (slideNow)
	{
		c->slideTicks = CD2_RECOVER_FRAMES; // re-prime for the release
		// grip during the slide: normal slides keep CD2_SLIDE_GRIP_FRAC of
		// traction (they arc a little); once the skid is big enough to be
		// sliding SIDEWAYS (|latVel| > CD2_SKID_LOCK_LAT) the slide LOCKS onto
		// its original momentum line — grip near zero so the rotating heading
		// never scrubs the velocity (pure TMB ice drift until you release).
		if (ABS(latVel) > CD2_SKID_LOCK_LAT)
			grip = (int)(((long long)grip * CD2_SKID_LOCK_GRIP) >> 12);
		else
			grip = (int)(((long long)grip * CD2_SLIDE_GRIP_FRAC) >> 12);
	}
	else if (c->slideTicks > 0)
	{
		// velocity recovery: rotate toward the heading, keep the magnitude
		c->slideTicks--;

		{
			int ax = ABS(velX);
			int az = ABS(velZ);
			int mag = (ax < az) ? (az + ax / 2) : (ax + az / 2);

			if (mag > 0)
			{
				// target = heading scaled to the current speed
				int tx = (int)(((long long)fx * mag) >> 12);
				int tz = (int)(((long long)fz * mag) >> 12);
				long long rate = CD2_RECOVER_RATE;
				int nvx = (int)(velX + ((((long long)tx - velX) * rate) >> 12));
				int nvz = (int)(velZ + ((((long long)tz - velZ) * rate) >> 12));
				int nax = ABS(nvx);
				int naz = ABS(nvz);
				int nm = (nax < naz) ? (naz + nax / 2) : (nax + naz / 2);

				if (nm > 0)
				{
					// renormalise: the slide's speed is carried out, not lost
					velX = (int)(((long long)nvx * mag) / nm);
					velZ = (int)(((long long)nvz * mag) / nm);
				}
			}
		}

		grip = 0; // rotation above replaces the lateral scrub this frame

		if (c->slideTicks == 0)
			c->slideTicks = -CD2_GRIP_RAMP_FRAMES; // chain into the grip ramp
	}
	else if (c->slideTicks < 0)
	{
		// grip ramps back up gradually instead of snapping to full
		grip = (int)(((long long)grip * (CD2_GRIP_RAMP_FRAMES + c->slideTicks)) /
			CD2_GRIP_RAMP_FRAMES);
		c->slideTicks++;
	}

	// damp the lateral component: vel -= right * latVel * grip
	velX -= (int)(((long long)rx * latVel * grip) >> 12);
	velZ -= (int)(((long long)rz * latVel * grip) >> 12);

	c->slip = (int)latVel; // for the visual lean
	}

	// hard ceiling: total horizontal speed never exceeds topSpeed. Without it,
	// a tight slide with gas keeps adding speed along a rotating heading and
	// can feel like it is accelerating out of control.
	{
		int ax = ABS(FIXEDH(velX));
		int az = ABS(FIXEDH(velZ));
		int mag = (ax < az) ? (az + ax / 2) : (ax + az / 2);
		if (mag > s.topSpeed)
		{
			long long k = ((long long)s.topSpeed * 4096) / mag;
			velX = (int)(((long long)velX * k) >> 12);
			velZ = (int)(((long long)velZ * k) >> 12);
		}
	}

	// ---- write the point-mass state ---------------------------------
	cp->st.n.linearVelocity[0] = velX;
	cp->st.n.linearVelocity[2] = velZ;
	cp->st.n.angularVelocity[1] = yaw * CD2_AV_PER_UNIT;

	// neutralise the stock horizontal force + yaw torque so the engine's
	// later velocity += acc / angularVelocity += aacc add nothing on these axes
	cp->hd.acc[0] = 0;
	cp->hd.acc[2] = 0;
	cp->hd.aacc[1] = 0;

	// refresh the derived speed fields the rest of the game reads this frame
	{
		int ax = ABS(FIXEDH(velX));
		int az = ABS(FIXEDH(velZ));
		cp->hd.speed = (ax < az) ? (az + ax / 2) : (ax + az / 2);
		cp->hd.wheel_speed = (int)(((long long)velX * fx + (long long)velZ * fz) >> 12);

		// In-place turning at a COMPLETE standstill: the engine treats a car
		// with hd.speed == 0 as fully stopped and wipes ALL angular velocity
		// before it integrates orientation, so the car can't rotate until it
		// rolls a tiny bit. While the driver is actually steering (or
		// pivoting) and the car is motionless, flag it as barely rolling so
		// the yaw we wrote above survives and the car spins on the spot.
		if (cp->hd.speed == 0 && cp->controlType == CONTROL_TYPE_PLAYER &&
			(steerFp != 0 || tightActive))
		{
			cp->hd.speed = 1;
		}
	}

	// ---- telemetry (opt-in): export input/velocity/twist for tuning ----
	if (gCd2Cfg.debugLog && cp->controlType == CONTROL_TYPE_PLAYER &&
		(gDbgFrame++ & 7) == 0)
	{
			int padm = 0;
			if (cp->ai.padid != NULL && *cp->ai.padid >= 0 && *cp->ai.padid < 2)
				padm = Pads[*cp->ai.padid].mapped; // engine-native bits (raw, pre-override)
			printInfo("[cainescrossfire] fr=%u thr=%d steer=%d tight=%d slide=%d "
				"fwd=%d spd=%d lat=%d grip=%d yaw=%d hb=%d ws=%d pad=0x%04X hdspd=%d hdws=%d\n",
				gDbgFrame, c->throttle, steerFp / 4096,
				tightActive ? (c->pivotDir ? c->pivotDir : 8) : 0, slideNow ? 1 : 0,
				(int)fwdSpeed, (int)(((long long)velX * fx + (long long)velZ * fz) >> 24),
				(int)latVel, grip, yaw, cp->handbrake, cp->wheelspin, padm,
				cp->hd.speed, cp->hd.wheel_speed);
	}

	return JER_RESULT_CONTINUE;
}

// GET_WALL_RESTITUTION: TMB "walls absorb momentum". Returning a low scale
// makes building/scenery hits a hard stop (momentum bled) instead of the stock
// outward bounce + spin. No handler (cainescrossfire disabled) = stock behaviour.
int cd2OnGetWallRestitution(void* ud, void* args)
{
	JER_ARGS_WALL_RESTITUTION* a = (JER_ARGS_WALL_RESTITUTION*)args;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	a->result = CD2_WALL_KEEP;
	return JER_RESULT_CONTINUE;
}

// GET_PHYSICS_PARAMS: gravity + faster angular settling + tighter suspension.
int cd2OnPhysicsParams(void* ud, void* args)
{
	JER_ARGS_PHYSICS_PARAMS* a = (JER_ARGS_PHYSICS_PARAMS*)args;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	a->gravity = CD2_GRAVITY;
	a->angularDamping = CD2_ANGULAR_DAMPING;
	a->springRate = CD2_SPRING_RATE;
	a->springDamping = CD2_SPRING_DAMPING;
	return JER_RESULT_CONTINUE;
}

// CAR_DRAW: render-only body lean into the slide (physics matrix untouched).
int cd2OnCarDraw(void* ud, void* args)
{
	JER_ARGS_CAR_DRAW* a = (JER_ARGS_CAR_DRAW*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	MATRIX* m = (MATRIX*)a->matrix;
	(void)ud;

	if (!gCd2Cfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	CD2_CAR* c = &gCd2Car[cp->id];
	int target = jer_clamp_int(-c->slip * CD2_ROLL_GAIN, -CD2_BODY_MAX_ROLL, CD2_BODY_MAX_ROLL);
	c->roll = jer_lerp_int(c->roll, target, CD2_ROLL_LERP);

	if (c->roll != 0)
		_RotMatrixZ(m, (short)c->roll);

	/* the vehicle knock: buck and rock, render-only. The turbo's kick is one
	 * source of it and collisions are another - they all land in the same place,
	 * and none of it reaches the handling model. */
	cd2KnockTick(cp->id);
	cd2KnockApply(m, cp->id);

	return JER_RESULT_CONTINUE;
}

int cd2OnDebugTick(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled || gCd2Cfg.rollLimit <= 0)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (cp->controlType == CONTROL_TYPE_NONE)
			continue;

		// Traffic is SUPPOSED to be tumbling. Standing it back up would undo
		// the whole point, and it fights the weapon knockback besides.
		if (cd2IsTraffic(cp))
			continue;

		cd2RecoverRoll(cp);
	}

	return JER_RESULT_CONTINUE;
}
