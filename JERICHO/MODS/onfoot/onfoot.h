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

#ifndef ONFOOT_H
#define ONFOOT_H

#include "jericho.h"
#include "jer_events.h"
#include "jer_host_api.h"

#define ONFOOT_MOD_ID "onfoot"
#define ONFOOT_VERSION "0.18.0"

/* Pad bits (the same values as pad.h; kept local so the addon needs no
 * engine headers). */
#define ONFOOT_MPAD_L2		0x0001
#define ONFOOT_MPAD_R2		0x0002
#define ONFOOT_MPAD_L1		0x0004
#define ONFOOT_MPAD_R1		0x0008
#define ONFOOT_MPAD_CROSS	0x0040
#define ONFOOT_MPAD_D_UP	0x1000
#define ONFOOT_MPAD_D_RIGHT	0x2000
#define ONFOOT_MPAD_D_DOWN	0x4000
#define ONFOOT_MPAD_D_LEFT	0x8000

#define ONFOOT_PAD_GOFORWARD	(ONFOOT_MPAD_CROSS | ONFOOT_MPAD_D_UP)
#define ONFOOT_PAD_GOBACK		(0x0080 | ONFOOT_MPAD_D_DOWN)
#define ONFOOT_PAD_TURNLEFT		ONFOOT_MPAD_D_LEFT
#define ONFOOT_PAD_TURNRIGHT	ONFOOT_MPAD_D_RIGHT

/* Tuning */
#define ONFOOT_MOVE_DEADZONE	24
#define ONFOOT_LOOK_DEADZONE	24
#define ONFOOT_RUN_SPEED		40		/* the engine's SetupRunner speed */
#define ONFOOT_SPEED_LERP		8

#define ONFOOT_PITCH_MAX		680		/* ~60 degrees each way */
#define ONFOOT_YAW_RATE			(128)	/* * sensX / 64 */
#define ONFOOT_PITCH_RATE		(1600)	/* * sensY / 64, /128 of stick */

/* Flip these if a pad reports the axes the other way round. */
#define ONFOOT_STICK_X			1
#define ONFOOT_STICK_Y			1

/* Aim camera */
#define ONFOOT_AIM_PULL			2000	/* /4096: pull toward the ped */
#define ONFOOT_AIM_HEIGHT		-220	/* render y: -220 = 220 above the anchor */
#define ONFOOT_AIM_LATERAL		55		/* shoulder offset */
#define ONFOOT_AIM_RANGE		30000

/* Torso / neck limits for the aim twist (PSX angle units). */
#define ONFOOT_TORSO_LIMIT		796		/* ~70 degrees of spine */
#define ONFOOT_HEAD_LIMIT		512

typedef struct OnfootSettings
{
	int cameraEnabled;
	int joyCamera;
	int sensX;
	int sensY;
	int invertY;
	int shoulder;		/* +1 right shoulder, -1 left */
} OnfootSettings;

extern OnfootSettings gS;

/* --- shared state (owned by onfoot_cam.c) -------------------------- */
extern int gOnfootAiming;			/* local player is holding aim */
extern int gOnfootLookPitch;		/* camera_angle.vx offset (negative = up) */
extern int gOnfootLookPitchTarget;
extern int gOnfootViewHeading;	/* latest view yaw, 0..4095 */

extern int gOnfootCamPos[3];		/* camera_position from the CAMERA hook (render frame) */
extern int gOnfootCamAngle[3];	/* camera_angle from the CAMERA hook */
extern int gOnfootCamValid;

extern int gOnfootHand[3];		/* RHAND world position (render frame) */
extern int gOnfootHandValid;

extern void* gOnfootPlayerPed0;	/* the local player's ped, captured from PED_MOVE */

/* --- hook handlers ------------------------------------------------- */
int OnfootOnLook(void* userdata, void* args);
int OnfootOnPedInput(void* userdata, void* args);
int OnfootOnPedMove(void* userdata, void* args);
int OnfootOnPedPose(void* userdata, void* args);
int OnfootOnPedSkeleton(void* userdata, void* args);
int OnfootOnCamera(void* userdata, void* args);
int OnfootOnFrame(void* userdata, void* args);
int OnfootOnOverlay(void* userdata, void* args);
int OnfootOnGameStart(void* userdata, void* args);

void OnfootInit(void);

/* --- shared helpers ------------------------------------------------- */
int OnfootSin(int angle);					/* 4096-scaled */
int OnfootCos(int angle);
int OnfootAtan2(int y, int x);				/* 0..4095 */
int OnfootAimPoint(int out[3]);				/* 0 when the camera is not known yet */

/* --- weapon (onfoot_weapon.c) -------------------------------------- */
void OnfootWeaponInit(void);
void OnfootWeaponUpdate(void);

/* --- -onfoot spawn swap (onfoot_boot.c) ---------------------------- */
void OnfootBootTick(void);

#endif /* ONFOOT_H */
