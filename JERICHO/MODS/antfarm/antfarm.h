/*
 * antfarm.h — "Ant Farm" screensaver / idle mode (JERICHO module).
 *
 * A passive city observer. While active it disables player input, hides the
 * HUD, mutes car/player SFX, disables cop aggression, redirects the region
 * spool (and pins the hidden player car) to the camera focus, and cycles a
 * cinematic camera through diverse shot styles, hopping to far areas of the
 * map each cut:
 *
 *   CHASE    — behind-follow on a traffic car, framed to the vehicle size
 *   STATIC   — a fixed roadside camera tracking a car as it drives past
 *   OVERHEAD — scenic elevated 3/4 view down onto traffic (car or road)
 *   TRIPOD   — parked roadside camera watching a junction/road
 *   FLYOVER  — slow eased dolly along a long straight
 *
 * Static/overhead styles are weighted more heavily than the chase cam.
 * Optional "lead AI" mode (off by default): a rare event where a picked car
 * turns rogue (lead AI driving across the map), cops give chase, and the
 * camera follows it until it is totaled.
 *
 * Toggle: F9 (keyboard, PC) or Pause -> Modules -> Ant Farm.
 */
#ifndef ANTFARM_H
#define ANTFARM_H

/* shot styles (index into styles-enabled array / config keys) */
enum
{
	ANTFARM_STYLE_CHASE = 0,
	ANTFARM_STYLE_STATIC,
	ANTFARM_STYLE_OVERHEAD,
	ANTFARM_STYLE_TRIPOD,
	ANTFARM_STYLE_FLYOVER,
	ANTFARM_STYLE_COUNT
};

/* what the shot is pointed at */
enum
{
	ANTFARM_TARGET_ROAD = 0,
	ANTFARM_TARGET_CAR = 1
};

/* cut state machine */
enum
{
	ANTFARM_STATE_SHOW = 0,		/* holding a shot */
	ANTFARM_STATE_FADE_OUT,		/* fading to the wash */
	ANTFARM_STATE_CUT,		/* black: new far area picked, streaming */
	ANTFARM_STATE_FADE_IN		/* fading back in */
};

/* timing (milliseconds) */
#define ANTFARM_FADE_MS		600
#define ANTFARM_CUT_HOLD_MS	2500
#define ANTFARM_CAR_WAIT_MS	4000	/* black cap while traffic populates */
#define ANTFARM_LEAD_END_MS	4000	/* hold on a totaled lead car */
#define ANTFARM_STREAM_TIMEOUT_MS 6000	/* black cap while regions stream */
#define ANTFARM_BLACK_CAP_MS	12000	/* hard cap on any single black hold */

/* interval config (seconds) */
#define ANTFARM_MIN_INTERVAL	10
#define ANTFARM_MAX_INTERVAL	60
#define ANTFARM_DEFAULT_INTERVAL 20

/* far-area hop: a new shot's focus must be at least this far (world units)
 * from the previous one, so the screensaver tours the whole map */
#define ANTFARM_FAR_DIST	40000

/* lead-car wreck threshold (totalDamage is u_short) */
#define ANTFARM_LEAD_TOTAL	6000

/* default lead-AI trigger chance (percent) when the option is enabled */
#define ANTFARM_LEAD_CHANCE	6

#endif /* ANTFARM_H */
