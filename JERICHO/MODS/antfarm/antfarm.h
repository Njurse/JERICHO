/*
 * antfarm.h — "Ant Farm" screensaver / idle mode (JERICHO module).
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
	ANTFARM_STATE_SHOW = 0,
	ANTFARM_STATE_FADE_OUT,
	ANTFARM_STATE_CUT,
	ANTFARM_STATE_FADE_IN
};

/* timing (milliseconds) */
#define ANTFARM_FADE_MS     600
#define ANTFARM_CUT_HOLD_MS 1500
#define ANTFARM_CAR_WAIT_MS 5000
#define ANTFARM_LEAD_END_MS 4000
#define ANTFARM_STREAM_TIMEOUT_MS 1500
#define ANTFARM_BLACK_CAP_MS    1500

/* interval config (seconds) */
#define ANTFARM_MIN_INTERVAL    5    /* changed from 10 */
#define ANTFARM_MAX_INTERVAL    60
#define ANTFARM_DEFAULT_INTERVAL 20

/* far-area hop distance */
#define ANTFARM_FAR_DIST    12000

/* lead-car wreck threshold */
#define ANTFARM_LEAD_TOTAL    6000

/* default lead-AI trigger chance (percent) */
#define ANTFARM_LEAD_CHANCE    100

#endif /* ANTFARM_H */