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
	ANTFARM_STYLE_ORBIT,   /* slow orbit around the subject */
	ANTFARM_STYLE_CRANE,   /* slow vertical rise revealing a road */
	ANTFARM_STYLE_LOW,     /* ground-level "ant" view of passing traffic */
	ANTFARM_STYLE_FENDER,  /* rig on the front bumper, looking down the road */
	ANTFARM_STYLE_SILL,    /* rig low on the side, along the flank */
	ANTFARM_STYLE_NOSE34,  /* front-quarter leading */
	ANTFARM_STYLE_TAIL34,  /* rear-quarter trailing */
	ANTFARM_STYLE_KERB,    /* wheel-height pass with a long lens */
	ANTFARM_STYLE_TRIPZOOM,/* parked vantage on a car, slow zoom */
	ANTFARM_STYLE_FARPAN,  /* distant long lens, slow pan + zoom */
	ANTFARM_STYLE_WATERFRONT,/* dolly along a waterfront road */
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

/* timing (milliseconds). Transitions are deliberately short: the cut only has
 * to cover a region force-load, which is one frame on PC, so a long black hold
 * just reads as dead air. */
#define ANTFARM_FADE_MS     700
#define ANTFARM_CUT_HOLD_MS 250
#define ANTFARM_CAR_WAIT_MS 5000
#define ANTFARM_LEAD_END_MS 4000
#define ANTFARM_STREAM_TIMEOUT_MS 400
#define ANTFARM_BLACK_CAP_MS    700

/* how long a rig or long-lens shot will hold on a subject that has stopped
 * moving before it cuts away (a frozen frame is the one thing a screensaver
 * must not show) */
#define ANTFARM_STILL_MS        2500

/* interval config (seconds) */
#define ANTFARM_MIN_INTERVAL    10
#define ANTFARM_MAX_INTERVAL    300
#define ANTFARM_DEFAULT_INTERVAL 45

/* how many cuts' worth of recent styles to avoid repeating */
#define ANTFARM_STYLE_MEMORY    3

/* dwell trimming: a shot's visible time is scaled by its scene interest */
#define ANTFARM_DWELL_MIN       55    /* x100 */
#define ANTFARM_DWELL_MAX       190   /* x100 */

/* one full revolution in an ORBIT shot */
#define ANTFARM_ORBIT_PERIOD_MS  60000

/* presentation dressing: bar height top/bottom (~16:10 visible frame) and
 * how long an occasional place-name caption stays up */
#define ANTFARM_LETTERBOX_H 34
#define ANTFARM_CAPTION_MS  4200

/* CRANE shots rise from this height to the archetype's picked height */
#define ANTFARM_CRANE_LOW        90

/* lead-car wreck threshold */
#define ANTFARM_LEAD_TOTAL    6000

/* default lead-AI trigger chance (percent) */
#define ANTFARM_LEAD_CHANCE    100

#endif /* ANTFARM_H */