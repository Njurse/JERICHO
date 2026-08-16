/*
 * antfarm.h — "Ant Farm" screensaver / idle mode (JERICHO module).
 *
 * A passive city observer. While active it disables player input, hides the
 * HUD, redirects the region spool to the camera focus, and cycles a
 * cinematic camera between three shot types:
 *
 *   A. Junction / Tripod    — parked elevated camera watching traffic flow
 *   B. Traffic Follow       — chase-cam attached to a random moving civilian
 *   C. Boulevard Flyover    — slow dolly/pan down a long straight
 *
 * Each cut fades out, reselects a target during the black frame (so the
 * region spooling has time to stream the new area), and fades back in.
 *
 * Toggle: F9 (keyboard, PC) or Pause -> Modules -> Ant Farm.
 *
 * This header is the module's own API; the engine only needs the generated
 * registry entry (jer_module_antfarm_entry) from antfarm.c.
 */
#ifndef ANTFARM_H
#define ANTFARM_H

/* shot types (index into the modes-enabled array / config keys) */
enum
{
	ANTFARM_MODE_TRIPOD = 0,
	ANTFARM_MODE_FOLLOW = 1,
	ANTFARM_MODE_FLYOVER = 2,
	ANTFARM_MODE_COUNT = 3
};

/* cut state machine */
enum
{
	ANTFARM_STATE_SHOW = 0,		/* holding a shot */
	ANTFARM_STATE_FADE_OUT,		/* fading to the wash */
	ANTFARM_STATE_CUT,		/* black: target switched, region spooling */
	ANTFARM_STATE_FADE_IN		/* fading back in */
};

/* timing (milliseconds) */
#define ANTFARM_FADE_MS		600
#define ANTFARM_CUT_HOLD_MS	900

/* config limits (seconds) */
#define ANTFARM_MIN_INTERVAL	8
#define ANTFARM_MAX_INTERVAL	20
#define ANTFARM_DEFAULT_INTERVAL 12

#endif /* ANTFARM_H */
