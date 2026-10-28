/* onfoot.h — shared state for the on-foot ped addon.
 *
 * The module is a RUNTIME DLL addon: it uses only the JERICHO SDK types,
 * the event argument structs and the host API. It must never reference an
 * engine data global (Pads, camera_position, player[], ...) — Windows cannot
 * import those into a DLL. Ped movement is written through the exported
 * jer_npc_* setters; the skeleton through jer_anim_*.
 *
 * Two conventions used throughout:
 *   - "view heading" is the direction the camera looks, 0..4095, where
 *     forward = (RSIN(h), RCOS(h)).
 *   - "ped dir" is pPed->dir.vy. The engine moves forward along
 *     ped dir - 2048, so a ped that should MOVE along `h` needs dir = h + 2048.
 */

#ifndef D2PL_H
#define D2PL_H

#include "jericho.h"
#include "jer_events.h"
#include "jer_host_api.h"

#define D2PL_MOD_ID "d2pl"
#define D2PL_VERSION "1.2.0"

/* Pad bits (the same values as pad.h; kept local so the addon needs no
 * engine headers). */
#define D2PL_MPAD_L2		0x0001
#define D2PL_MPAD_R2		0x0002
#define D2PL_MPAD_L1		0x0004
#define D2PL_MPAD_R1		0x0008
#define D2PL_MPAD_SELECT	0x0100
#define D2PL_MPAD_CYCLE		D2PL_MPAD_L2	/* weapon cycle (on foot) */
#define D2PL_MPAD_CROSS	0x0040
#define D2PL_MPAD_D_UP	0x1000
#define D2PL_MPAD_D_RIGHT	0x2000
#define D2PL_MPAD_D_DOWN	0x4000
#define D2PL_MPAD_D_LEFT	0x8000

#define D2PL_PAD_GOFORWARD	(D2PL_MPAD_CROSS | D2PL_MPAD_D_UP)
#define D2PL_PAD_GOBACK		(0x0080 | D2PL_MPAD_D_DOWN)
#define D2PL_PAD_TURNLEFT		D2PL_MPAD_D_LEFT
#define D2PL_PAD_TURNRIGHT	D2PL_MPAD_D_RIGHT

/* Tuning */
#define D2PL_MOVE_DEADZONE	24
#define D2PL_LOOK_DEADZONE	24
#define D2PL_RUN_SPEED		40		/* the engine's SetupRunner speed */
#define D2PL_SPEED_LERP		8

/* Look (PSX angle units: 4096 = one turn). Mouse and the right stick share
 * one per-axis sensitivity; these curves are what make it feel like a
 * conventional shooter rather than the old yaw/pitch rate mismatch. */
#define D2PL_LOOK_PITCH_MAX	1000	/* ~88 degrees each way */
#define D2PL_STICK_BASE		16		/* sens value that equals the stock rate */
#define D2PL_STICK_RATE_MAX	72		/* full-deflection stick rate, units/frame (~190 deg/s at 30 Hz) */
#define D2PL_MOUSE_SCALE	16		/* mouse units per count = sens / MOUSE_SCALE */
#define D2PL_AIM_LOOK_SCALE	2		/* look rate divided by this while aiming (ADS slowdown) */

/* Flip these if a pad reports the axes the other way round. */
#define D2PL_STICK_X			1
#define D2PL_STICK_Y			1

/* Aim camera */
#define D2PL_AIM_PULL			2000	/* /4096: pull toward the ped */
#define D2PL_AIM_HEIGHT		-220	/* render y: -220 = 220 above the anchor */
#define D2PL_AIM_LATERAL		55		/* shoulder offset */
#define D2PL_AIM_RANGE		30000

/* Torso / neck limits for the aim twist (PSX angle units). */
#define D2PL_TORSO_LIMIT		455		/* ~40 degrees of spine */
#define D2PL_HEAD_LIMIT		341		/* ~30 degrees of neck */

/* Standing aim: the body turns in place once the twist passes TURN_START,
 * at up to TURN_RATE per frame. Past the clamps the arm IK fades out so the
 * arms never point sideways off a twisted torso. */
#define D2PL_TURN_START		228		/* ~20 degrees */
#define D2PL_TURN_RATE		160		/* ~14 degrees a frame */

/* Spine pitch: the torso takes a share of the aim elevation (JOINT_1 vx).
 * D2PL_PITCH_SIGN flips it if the lean comes out backwards in game. */
#define D2PL_PITCH_LIMIT		400		/* ~35 degrees */
#define D2PL_PITCH_SHARE		2048	/* /4096 of the camera pitch */
#ifndef D2PL_PITCH_SIGN
#define D2PL_PITCH_SIGN		1
#endif

/* Backpedal while aiming (Parallel Lines): the stock walk-back with the
 * walk frames played in reverse. Compile-time default; the pause menu
 * toggles it at runtime (gD2plBackpedalOn). */
#ifndef D2PL_BACKPEDAL
#define D2PL_BACKPEDAL		1
#endif
#define D2PL_BACKPEDAL_ANGLE	910		/* backpedal cone half-angle (~80 deg off straight back) */
#define D2PL_PED_HEALTH		100	/* damage stub */

#define D2PL_MPAD_TRIANGLE	0x0010	/* TANNER_PAD_ACTION: car entry */
#define D2PL_MPAD_L3		0x0200	/* TANNER_PAD_ACTION_DED */

typedef struct D2plSettings
{
	int cameraEnabled;
	int joyCamera;
	int sensX;
	int sensY;
	int invertY;
	int shoulder;		/* +1 right shoulder, -1 left */
} D2plSettings;

extern D2plSettings gS;

/* --- shared state (owned by d2pl_cam.c) -------------------------- */
extern int gD2plAiming;			/* local player is holding aim */
extern int gD2plLookPitch;		/* camera_angle.vx offset, holds where left (negative = up) */
extern int gD2plViewHeading;	/* latest view yaw, 0..4095 */

extern int gD2plCamPos[3];		/* camera_position from the CAMERA hook (render frame) */
extern int gD2plCamAngle[3];	/* camera_angle from the CAMERA hook */
extern int gD2plCamValid;

extern int gD2plHand[3];		/* RHAND world position (render frame) */
extern int gD2plHandValid;

extern int gD2plBackpedalOn;		/* runtime toggle, pause menu */
extern int gD2plBackpedalActive;	/* the local player is backing up now */
extern int gD2plSfxMute;			/* placeholder gun SFX muted */
extern int gD2plTestAim;
extern int gD2plBackpedalAngle;		/* runtime, starts at D2PL_BACKPEDAL_ANGLE */
extern int gD2plHitCount;			/* damage stub: hits this session */
extern int gD2plLastHitHealth;
extern int gD2plLastHitPos[3];		/* world, render frame (y down) */		/* the last ped hit, -1 = none yet */			/* D2PL_TEST_AIM lever: aim held without a pad */

extern void* gD2plPlayerPed0;	/* the local player's MOVEMENT ped, captured from PED_MOVE */
extern void* gD2plDrawPed0;		/* the local player's DRAWN ped (PED_POSE/PED_SKELETON);
					   differs from gD2plPlayerPed0 under mp (OTHER_MODEL) */
extern int gD2plDrawDir;		/* the drawn ped's raw dir.vy (JER_ARGS_PED_POSE.dir) */
extern int gD2plDrawDirValid;

/* fire feedback: 0..4096, kicked by a shot, decays per tick (d2pl_weapon.c),
 * read by the pose (arm buckle + torso recoil from the weapon-hand side) */
extern int gD2plRecoil;

void D2plDrawPedTick(void);		/* d2pl_pose.c: per-tick re-learn of the draw ped */

/* --- hook handlers ------------------------------------------------- */
int D2plOnLook(void* userdata, void* args);
int D2plOnPedInput(void* userdata, void* args);
int D2plOnPedMove(void* userdata, void* args);
int D2plOnPedPose(void* userdata, void* args);
int D2plOnPedSkeleton(void* userdata, void* args);
int D2plOnCamera(void* userdata, void* args);
int D2plOnFrame(void* userdata, void* args);
int D2plOnOverlay(void* userdata, void* args);
int D2plOnGameStart(void* userdata, void* args);

void D2plInit(void);
void D2plMenuRegister(void);		/* d2pl_menu.c */

/* weapon readouts for the pause menu (d2pl_weapon.c) */
const char* D2plWeaponName(void);
void D2plWeaponAmmoText(char* out, int max);
void D2plWeaponCycle(int dir);
void D2plSettingsReset(void);		/* pause menu "Reset to defaults" */

/* --- shared helpers ------------------------------------------------- */
int D2plSin(int angle);					/* 4096-scaled */
int D2plCos(int angle);
int D2plAtan2(int y, int x);				/* 0..4095 */
int D2plAimPoint(int out[3]);				/* 0 when the camera is not known yet */

/* --- holdtypes (d2pl_holdtype.c) --------------------------------- */
#define D2PL_MAX_HOLDTYPES	16

enum
{
	D2PL_HOLD_NONE = 0,	/* registered first, in this order */
	D2PL_HOLD_PISTOL = 1,
	D2PL_HOLD_RIFLE = 2
};

/* Optional per-holdtype pose hook: runs in PED_POSE after the twist and IK
 * have been written, with the same skeleton. This is where a keyframe
 * override or a custom clip sampler plugs in (write jer_anim_bone_rotation
 * values only for bones in `mask`). */
typedef void (*D2plPoseHook)(void* skel, int aiming, void* user);

typedef struct D2plHoldtype
{
	const char* name;
	unsigned int mask;		/* bit per JER_LIMB_* the holdtype may write */
	int aimIk;				/* 1: right arm IK at the aim point while aiming */
	int idleIk;				/* 1: idle hold (weapon out, not aiming) */
	int idlePitch;			/* idle arm angle below horizontal, PSX units */
	int idleWeight;			/* 0..4096: blend from the stock arm to the idle hold */
	int torsoLimit;			/* aim twist, PSX units */
	int headLimit;
	int allowBackpedal;		/* see D2PL_BACKPEDAL in d2pl_cam.c */
	int pitchLimit;			/* aim elevation into the spine (JOINT_1 vx), PSX units */
	int twoHand;			/* 1: left arm reaches for the fore-grip while aiming */
	int gripForward;		/* fore-grip: world units from the right hand along the aim */
	int gripElbow[3];		/* added to LELBOW (x, y, z) to bend the support arm */
	D2plPoseHook poseHook;	/* NULL = none */
	void* poseUser;
} D2plHoldtype;

void D2plHoldtypeInit(void);
D2plHoldtype* D2plHoldtypeEdit(int id);		/* live tuning (pause menu), NULL on a bad id */
int D2plHoldtypeRegister(const D2plHoldtype* h);	/* returns the id or -1 */
const D2plHoldtype* D2plHoldtypeGet(int id);		/* bad id -> "none" */
int D2plHoldtypeFind(const char* name);

/* --- weapon (d2pl_weapon.c) -------------------------------------- */
void D2plWeaponInit(void);
void D2plWeaponUpdate(void);
int D2plActiveHoldtype(void);		/* the equipped weapon's holdtype id */

/* --- -onfoot spawn swap (d2pl_boot.c) ---------------------------- */
void D2plBootTick(void);

#endif /* D2PL_H */
