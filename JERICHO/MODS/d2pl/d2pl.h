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
#define D2PL_VERSION "1.1.0"

/* Pad bits (the same values as pad.h; kept local so the addon needs no
 * engine headers). */
#define D2PL_MPAD_L2		0x0001
#define D2PL_MPAD_R2		0x0002
#define D2PL_MPAD_L1		0x0004
#define D2PL_MPAD_R1		0x0008
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

#define D2PL_PITCH_MAX		680		/* ~60 degrees each way */
#define D2PL_YAW_RATE			(128)	/* * sensX / 64 */
#define D2PL_PITCH_RATE		(1600)	/* * sensY / 64, /128 of stick */

/* Flip these if a pad reports the axes the other way round. */
#define D2PL_STICK_X			1
#define D2PL_STICK_Y			1

/* Aim camera */
#define D2PL_AIM_PULL			2000	/* /4096: pull toward the ped */
#define D2PL_AIM_HEIGHT		-220	/* render y: -220 = 220 above the anchor */
#define D2PL_AIM_LATERAL		55		/* shoulder offset */
#define D2PL_AIM_RANGE		30000

/* Torso / neck limits for the aim twist (PSX angle units). */
#define D2PL_TORSO_LIMIT		796		/* ~70 degrees of spine */
#define D2PL_HEAD_LIMIT		512

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
extern int gD2plLookPitch;		/* camera_angle.vx offset (negative = up) */
extern int gD2plLookPitchTarget;
extern int gD2plViewHeading;	/* latest view yaw, 0..4095 */

extern int gD2plCamPos[3];		/* camera_position from the CAMERA hook (render frame) */
extern int gD2plCamAngle[3];	/* camera_angle from the CAMERA hook */
extern int gD2plCamValid;

extern int gD2plHand[3];		/* RHAND world position (render frame) */
extern int gD2plHandValid;

extern void* gD2plPlayerPed0;	/* the local player's ped, captured from PED_MOVE */

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

/* --- shared helpers ------------------------------------------------- */
int D2plSin(int angle);					/* 4096-scaled */
int D2plCos(int angle);
int D2plAtan2(int y, int x);				/* 0..4095 */
int D2plAimPoint(int out[3]);				/* 0 when the camera is not known yet */

/* --- weapon (d2pl_weapon.c) -------------------------------------- */
void D2plWeaponInit(void);
void D2plWeaponUpdate(void);

/* --- -onfoot spawn swap (d2pl_boot.c) ---------------------------- */
void D2plBootTick(void);

#endif /* D2PL_H */
