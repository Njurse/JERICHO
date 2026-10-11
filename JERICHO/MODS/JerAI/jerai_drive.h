#ifndef JERAI_DRIVE_H
#define JERAI_DRIVE_H

/* ------------------------------------------------------------------
 * jerai_drive.h -- the driving primitives: plan, aim, and turn a plan
 * into a pad.
 *
 * Driving is done the way the mp bot drives (mp_bot.c): JerAI produces a
 * CAR_PAD_* bitmask and the module hands it to the engine through
 * JER_EVENT_CAR_PAD, so the car stays the PLAYER's own car
 * (CONTROL_TYPE_PLAYER) and the engine's normal input path applies it. No
 * ai.c.* knobs, no controlType hand-off.
 *
 * The module must NEVER emit CAR_PAD_HANDBRAKE: the stock pad handler
 * cycles g_PlayerControlMode on a handbrake press edge (handling.c), which
 * would hand the car to the engine AI and break pad injection.
 * ------------------------------------------------------------------ */

#include "driver2.h"	/* CAR_DATA */

/* Plan to (toX, toZ) over a fresh occupancy grid centred on the car. The
 * current plan is reused while it is younger than replan_ms and the goal has
 * not drifted more than replan_move; `force` always re-plans. Returns 1 when a
 * usable plan exists (complete or not). */
int JerAiDrivePlanTo(CAR_DATA* car, int toX, int toZ, int force);

/* The point to steer at this frame: the furthest waypoint within the
 * look-ahead. Returns 1 and writes it, or 0 when there is no plan. */
int JerAiDriveAim(int* aimX, int* aimZ);

/* A steering/throttle pad toward (aimX, aimZ), with wall and traffic
 * avoidance. */
unsigned int JerAiDriveSteerPad(CAR_DATA* car, int aimX, int aimZ);

/* A seeded RNG. The engine's Random2 is frame-deterministic (it replays the
 * same sequence every run), so a wander that used it would drive the same route
 * every time; this is seeded per run instead (JER_EVENT_GAME_START). */
unsigned int JerAiRand(void);
void JerAiRandSeed(unsigned int s);

/* The player's own car (whatever its controlType), or NULL. Unlike
 * JerAiPlayerCar (which is the old AI-hand-off helper), this is the car pad
 * injection drives. */
CAR_DATA* JerAiOwnCar(void);

/* What the car is actually DOING, as opposed to where the plan says to go.
 * `speed` and `slip` are the difference between walking a line and driving a
 * car: a car carries momentum, and its velocity can point somewhere other than
 * its nose - that angle IS the slide, and it is what makes the car understeer
 * into a corner it entered too fast. */
typedef struct JERAI_CARSTATE
{
	int	speed;		/* forward speed, engine units (hd.wheel_speed) */
	int	dir;		/* heading */
	int	yawRate;	/* body rotation, engine units/frame */
	int	slip;		/* signed angle velocity-vs-heading, game units (-2048..2048) */
} JERAI_CARSTATE;

JERAI_CARSTATE JerAiReadCar(CAR_DATA* car);

/* The corner the plan leads into: a car cannot turn or brake instantly, so the
 * driver has to know what the line does BEFORE it arrives. `sharp` is the
 * heading change at the sharpest waypoint ahead within the horizon (game units,
 * signed: + right, - left), `dist` is how far along the plan it sits. */
typedef struct JERAI_CURVE
{
	int	sharp;		/* sharpest heading change ahead, game units (-2048..2048) */
	int	dist;		/* world units from the car to that corner */
	int	sign;		/* +1 / -1: which way the sharpest turn goes */
	int	current;	/* heading change at the very next waypoint ahead */
	int	span;		/* arc length the sharp turn happens over (its in+out legs) */
} JERAI_CURVE;

JERAI_CURVE JerAiScanCurve(int cx, int cz, int horizon);

#endif /* JERAI_DRIVE_H */
