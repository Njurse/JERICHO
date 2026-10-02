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
	ANTFARM_STYLE_JUNCTION,/* parked vantage watching a junction mouth */
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

/* The screensaver's lens range, as the projection distance passed to
 * SetGeomScreen (scr_z): SMALLER = WIDER. The engine itself never goes below
 * gCameraDefaultScrZ (256) - camera.c floors scr_z there, and grows it with
 * distance up to 800 - so 256 is the widest the game ever is. The module used
 * to allow 200, which is wider than the game ever goes and read as a fisheye
 * when a shot's lens "expanded"; the floor is now the engine's own widest. */
#define ANTFARM_FOV_MIN         256
#define ANTFARM_FOV_MAX         360

/* Road-following cameras (roadside, dolly, crane, junction) aim this many
 * world units ABOVE the road, so the frame carries more of the street and what
 * is beyond it instead of pointing too far down. The per-model aim offsets
 * below are the base; this is added to them. */
#define ANTFARM_ROAD_AIM_LIFT   60

/* ...plus a small upward bias on the final pitch, in the same units as
 * camera_angle.vx (4096 = 360 deg, so ~11.4 units per degree; this is ~1.8 deg).
 * vx is 0 at level, positive = looking down, so subtracting tilts the view up. */
#define ANTFARM_ROAD_PITCH_UP   20

/* The very first shot of an activation fades in FAST and begins as soon as its
 * assets have streamed, instead of waiting out the normal CUT hold: a
 * screensaver that opens on several seconds of black reads as broken. Every
 * later cut keeps the normal ANTFARM_FADE_MS transition. */
#define ANTFARM_FIRST_FADE_MS   500

/* extra black after a shot's region lands, so the destination's texture pages
 * (which the engine streams per AREA, keyed off the camera position) are in
 * VRAM before the scene is revealed */
#define ANTFARM_TEX_SETTLE_MS   250

/* how long the camera may sit in a region that has not streamed before the
 * module gives up on the shot and re-anchors the tour on the nearest road
 * (the "get itself back to solid ground" recovery; see AntFarmRecoverToRoad) */
#define ANTFARM_RECOVER_MS      2500

/* interval config (seconds) */
#define ANTFARM_MIN_INTERVAL    10
#define ANTFARM_MAX_INTERVAL    300
#define ANTFARM_DEFAULT_INTERVAL 30

/* A shot's visible time is clamped to this range whatever the interest
 * multiplier works out to: this is the pace of the whole tour. Cutting every
 * few seconds is a slideshow; dwelling for minutes stops being something you
 * can leave on in the background. */
#define ANTFARM_SHOT_MIN_MS     15000
#define ANTFARM_SHOT_MAX_MS     45000

/* how many cuts' worth of recent styles to avoid repeating */
#define ANTFARM_STYLE_MEMORY    3

/* shot-pace trim: the dwell the interest score asks for is scaled by this, so
 * the tour changes shots a little more often. 85 = shots ~15% shorter. */
#define ANTFARM_PACE_PCT        85

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