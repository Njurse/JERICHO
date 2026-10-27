/* onfoot_cam.c — look, camera-relative movement and the aim camera.
 *
 * Everything here is DLL-safe: the pad is read through the host API, the
 * camera angle through the player token in the event args, and the ped is
 * driven through the exported jer_npc_face / jer_npc_set_speed setters
 * (PED_MOVE fires after the runner's speed re-arm, so the write survives).
 */

#include "onfoot.h"
#include "jer_math.h"
#include "jer_npc.h"

#include <math.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */

OnfootSettings gS = {
	1,		/* cameraEnabled */
	1,		/* joyCamera */
	16,		/* sensX */
	12,		/* sensY */
	1,		/* invertY */
	1		/* shoulder: right */
};

int gOnfootAiming = 0;
int gOnfootLookPitch = 0;
int gOnfootLookPitchTarget = 0;
int gOnfootViewHeading = 0;

int gOnfootCamPos[3] = { 0, 0, 0 };
int gOnfootCamAngle[3] = { 0, 0, 0 };
int gOnfootCamValid = 0;

int gOnfootHand[3] = { 0, 0, 0 };
int gOnfootHandValid = 0;

void* gOnfootPlayerPed0 = 0;

/* movement state, per pad (two local players at most) */
static int gYawSpeed = 0;
static int gMoveActive[2] = { 0, 0 };
static int gStickX[2] = { 0, 0 };
static int gStickY[2] = { 0, 0 };
static int gDesired[2] = { 0, 0 };
static int gRunSpeed[2] = { 0, 0 };

/* mouse look, polled once per frame and consumed by CAMERA_LOOK */
static int gMouseDX = 0;
static int gMouseDY = 0;
static int gMouseActive = 0;

#define MOUSE_AUTHORITY_FRAMES 45

/* Local camera/vector views of the engine types (event args are void*). */
typedef struct OF_VEC3
{
	int vx, vy, vz;
} OF_VEC3;

typedef struct OF_VEC4
{
	int vx, vy, vz, vw;
} OF_VEC4;

typedef struct OF_SVEC
{
	short vx, vy, vz, pad;
} OF_SVEC;

/* ------------------------------------------------------------------ */
/* Small math (the engine's rcossin_tbl is a data global, unavailable  */
/* to a DLL, so the addon carries its own fixed-point trig).           */
/* ------------------------------------------------------------------ */

#define ONFOOT_ANGLE_TO_RAD (6.28318530718f / 4096.0f)

int OnfootSin(int angle)
{
	return (int)(sinf((float)(angle & 0xfff) * ONFOOT_ANGLE_TO_RAD) * 4096.0f);
}

int OnfootCos(int angle)
{
	return OnfootSin(angle + 1024);
}

int OnfootAtan2(int y, int x)
{
	if (x == 0 && y == 0)
		return 0;

	return (int)(atan2f((float)y, (float)x) / ONFOOT_ANGLE_TO_RAD) & 0xfff;
}

/* ------------------------------------------------------------------ */
/* Mouse look                                                          */
/* ------------------------------------------------------------------ */

/* Resolve SDL_GetRelativeMouseState from the SDL2 DLL the game already
 * loaded. It is not part of the exe export surface, and linking SDL2 into
 * the addon would tie it to one build, so a runtime lookup is the least
 * invasive way to get relative motion. */
static void OnfootPollMouse(void)
{
#ifdef _WIN32
	typedef unsigned int(__cdecl *SDL_GRMS)(int*, int*);
	static SDL_GRMS fn = 0;
	static int tried = 0;

	if (!tried)
	{
		HMODULE sdl = GetModuleHandleA("SDL2.dll");

		tried = 1;

		if (sdl == 0)
			sdl = LoadLibraryA("SDL2.dll");

		if (sdl != 0)
			fn = (SDL_GRMS)(void*)GetProcAddress(sdl, "SDL_GetRelativeMouseState");
	}

	if (fn != 0)
	{
		int dx = 0;
		int dy = 0;

		fn(&dx, &dy);

		if (dx != 0 || dy != 0)
		{
			gMouseDX = dx;
			gMouseDY = dy;
			gMouseActive = MOUSE_AUTHORITY_FRAMES;
		}
		else if (gMouseActive > 0)
		{
			gMouseActive--;
		}
	}
#endif
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_FRAME — poll the mouse, then tick the weapon              */
/* ------------------------------------------------------------------ */

int OnfootOnFrame(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	OnfootBootTick();
	OnfootPollMouse();
	OnfootWeaponUpdate();

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_CAMERA_LOOK — right stick + mouse orbit                   */
/* ------------------------------------------------------------------ */

int OnfootOnLook(void* userdata, void* args)
{
	JER_ARGS_CAMERA_LOOK* a = (JER_ARGS_CAMERA_LOOK*)args;
	const JER_HOST_API* api = jerHostApiGet();
	JER_PLAYER_STATE ps;
	int sx, sy, stickLook, mouseLook;

	(void)userdata;

	if (a == 0 || api == 0 || api->player == 0 || api->player->get_state == 0)
		return JER_RESULT_CONTINUE;

	if (!gS.cameraEnabled)
		return JER_RESULT_CONTINUE;

	if (!api->player->get_state(a->player, &ps) || ps.playerCarId >= 0)
		return JER_RESULT_CONTINUE;		/* on foot only */

	sx = a->stickX;
	sy = a->stickY;

	stickLook = gS.joyCamera &&
		(abs(sx) > ONFOOT_LOOK_DEADZONE || abs(sy) > ONFOOT_LOOK_DEADZONE);
	mouseLook = (gMouseActive > 0) && (gMouseDX != 0 || gMouseDY != 0);

	if (stickLook || mouseLook)
	{
		int yawDelta = 0;

		a->suppress = 1;		/* skip the stock L2/R2 look */
		a->gripOrbit = 1;		/* the addon owns the orbit this frame */

		if (stickLook)
		{
			int yawMax = (ONFOOT_YAW_RATE * gS.sensX) / 64;
			int pitchRate = (ONFOOT_PITCH_RATE * gS.sensY) / 64;
			int vert = gS.invertY ? -sy : sy;

			gYawSpeed = jer_lerp_int(gYawSpeed, (sx * yawMax) / 127, 10);
			yawDelta += gYawSpeed;

			/* stick up (negative) = look up = negative camera_angle.vx */
			gOnfootLookPitchTarget = jer_clamp_int(
				(vert * pitchRate) / 128, -ONFOOT_PITCH_MAX, ONFOOT_PITCH_MAX);
		}
		else
		{
			gYawSpeed -= gYawSpeed / 8;
		}

		if (mouseLook)
		{
			int pitchRate = (ONFOOT_PITCH_RATE * gS.sensY) / 64;

			yawDelta += (gMouseDX * gS.sensX) / 4;
			gOnfootLookPitchTarget = jer_clamp_int(
				gOnfootLookPitchTarget + (gMouseDY * pitchRate) / 64,
				-ONFOOT_PITCH_MAX, ONFOOT_PITCH_MAX);

			gMouseDX = 0;
			gMouseDY = 0;
		}

		if (yawDelta != 0 && api->player->set_camera_angle != 0)
			api->player->set_camera_angle(a->player, (ps.cameraAngle + yawDelta) & 0xfff);
	}
	else
	{
		/* released: let the momentum decay. The engine's own settle-back
		 * lerp turns the camera behind the ped again. While aiming, hold
		 * the orbit and the pitch so the shot does not drift. */
		gYawSpeed -= gYawSpeed / 8;

		if (abs(gYawSpeed) < 8)
			gYawSpeed = 0;

		if (gOnfootAiming)
		{
			a->suppress = 1;
			a->gripOrbit = 1;
		}
		else
		{
			gOnfootLookPitchTarget = jer_lerp_int(gOnfootLookPitchTarget, 0, 8);
		}
	}

	gOnfootLookPitch = jer_lerp_int(gOnfootLookPitch, gOnfootLookPitchTarget, 6);

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_PED_INPUT — camera-relative movement                      */
/* ------------------------------------------------------------------ */

int OnfootOnPedInput(void* userdata, void* args)
{
	JER_ARGS_PED_INPUT* a = (JER_ARGS_PED_INPUT*)args;
	const JER_HOST_API* api = jerHostApiGet();
	JER_PLAYER_STATE ps;
	int pad, sx, sy, mag, stickHeading, desired;

	(void)userdata;

	if (a == 0 || api == 0 || api->player == 0 || api->player->get_state == 0)
		return JER_RESULT_CONTINUE;

	if (!gS.cameraEnabled)
		return JER_RESULT_CONTINUE;

	if (!api->player->get_state(a->player, &ps) || ps.playerCarId >= 0)
		return JER_RESULT_CONTINUE;

	pad = ps.padid;

	if (pad < 0 || pad > 1)
		return JER_RESULT_CONTINUE;

	sx = a->stickX;
	sy = a->stickY;

	gStickX[pad] = sx;
	gStickY[pad] = sy;

	if (pad == 0)
		gOnfootAiming = (a->pad & ONFOOT_MPAD_L1) != 0;

	mag = abs(sx) + abs(sy);

	if (mag <= ONFOOT_MOVE_DEADZONE)
	{
		gMoveActive[pad] = 0;

		return JER_RESULT_CONTINUE;		/* stock D-pad/keyboard handling */
	}

	gMoveActive[pad] = 1;

	/* The movement frame is the camera view. stick up = 0, right = 1024. */
	stickHeading = OnfootAtan2(ONFOOT_STICK_X * sx, -ONFOOT_STICK_Y * sy);
	desired = (gOnfootViewHeading + stickHeading) & 0xfff;

	gDesired[pad] = desired;

	/* Always run forward: the direction is driven by jer_npc_face in
	 * PED_MOVE, so the stock turn bits must not fight it. */
	a->pad &= ~(ONFOOT_PAD_GOFORWARD | ONFOOT_PAD_GOBACK |
		ONFOOT_PAD_TURNLEFT | ONFOOT_PAD_TURNRIGHT);
	a->pad |= ONFOOT_PAD_GOFORWARD;

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_PED_MOVE — heading + analog speed, after the runner re-arm */
/* ------------------------------------------------------------------ */

int OnfootOnPedMove(void* userdata, void* args)
{
	JER_ARGS_PED_MOVE* a = (JER_ARGS_PED_MOVE*)args;
	JerNpc ped;
	int pad, m, target, delta;

	(void)userdata;

	if (a == 0 || a->ped == 0)
		return JER_RESULT_CONTINUE;

	pad = a->padId;

	if (pad < 0 || pad > 1)
		return JER_RESULT_CONTINUE;		/* never drive an ambient ped */

	if (pad == 0)
		gOnfootPlayerPed0 = a->ped;

	if (!gS.cameraEnabled)
		return JER_RESULT_CONTINUE;

	ped.ped = a->ped;

	if (!gMoveActive[pad])
	{
		/* stick centred: the engine's D-pad movement still works. Bleed
		 * the momentum off only while the ped is stopped. */
		gRunSpeed[pad] -= gRunSpeed[pad] / 8;

		if (gRunSpeed[pad] < 1)
			gRunSpeed[pad] = 0;

		return JER_RESULT_CONTINUE;
	}

	/* The ped moves along dir.vy - 2048, so a MOVEMENT heading h needs
	 * dir.vy = h + 2048. */
	jer_npc_face(&ped, (gDesired[pad] + 2048) & 0xfff);

	m = abs(gStickX[pad]);

	if (abs(gStickY[pad]) > m)
		m = abs(gStickY[pad]);

	target = (m - ONFOOT_MOVE_DEADZONE) * ONFOOT_RUN_SPEED / (127 - ONFOOT_MOVE_DEADZONE);

	if (target < 0)
		target = 0;
	if (target > ONFOOT_RUN_SPEED)
		target = ONFOOT_RUN_SPEED;

	delta = target - gRunSpeed[pad];

	if (abs(delta) < ONFOOT_SPEED_LERP)
		gRunSpeed[pad] = target;
	else
		gRunSpeed[pad] += delta / ONFOOT_SPEED_LERP;

	jer_npc_set_speed(&ped, gRunSpeed[pad]);

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_CAMERA — aim framing, pitch and the cached view            */
/* ------------------------------------------------------------------ */

int OnfootOnCamera(void* userdata, void* args)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	OF_SVEC* ang;
	OF_VEC3* pos;
	OF_VEC4* base;
	int heading;

	(void)userdata;

	if (a == 0 || !gS.cameraEnabled)
		return JER_RESULT_CONTINUE;

	/* chase camera, on foot only */
	if (a->cameraView != 0 || a->inCar != 0)
		return JER_RESULT_CONTINUE;

	if (a->cameraPosition == 0 || a->cameraAngle == 0)
		return JER_RESULT_CONTINUE;

	ang = (OF_SVEC*)a->cameraAngle;
	pos = (OF_VEC3*)a->cameraPosition;
	base = (OF_VEC4*)a->basePos;

	heading = (-ang->vy) & 0xfff;
	gOnfootViewHeading = heading;

	if (gOnfootAiming)
	{
		if (base != 0)
		{
			/* over the shoulder: pull in, lift to the shoulder, shift
			 * sideways. The engine already placed (and collision-checked)
			 * the camera; these are small deltas around that point. */
			pos->vx += ((base->vx - pos->vx) * ONFOOT_AIM_PULL) / 4096;
			pos->vz += ((base->vz - pos->vz) * ONFOOT_AIM_PULL) / 4096;
			pos->vy = -base->vy + ONFOOT_AIM_HEIGHT;
			pos->vx += (OnfootCos(heading) * ONFOOT_AIM_LATERAL * gS.shoulder) >> 12;
			pos->vz -= (OnfootSin(heading) * ONFOOT_AIM_LATERAL * gS.shoulder) >> 12;
		}

		ang->vx = (short)(ang->vx + gOnfootLookPitch);
		a->override = 1;
	}
	else if (gOnfootLookPitch != 0)
	{
		/* free-look tilt: rotate the view, keep the engine's position */
		ang->vx = (short)(ang->vx + gOnfootLookPitch);
		a->override = 1;
	}

	gOnfootCamPos[0] = pos->vx;
	gOnfootCamPos[1] = pos->vy;
	gOnfootCamPos[2] = pos->vz;
	gOnfootCamAngle[0] = ang->vx;
	gOnfootCamAngle[1] = ang->vy;
	gOnfootCamAngle[2] = ang->vz;
	gOnfootCamValid = 1;

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void OnfootInit(void)
{
	int i;

	gOnfootAiming = 0;
	gOnfootLookPitch = 0;
	gOnfootLookPitchTarget = 0;
	gOnfootViewHeading = 0;
	gYawSpeed = 0;
	gMouseDX = 0;
	gMouseDY = 0;
	gMouseActive = 0;

	gOnfootCamPos[0] = gOnfootCamPos[1] = gOnfootCamPos[2] = 0;
	gOnfootCamAngle[0] = gOnfootCamAngle[1] = gOnfootCamAngle[2] = 0;
	gOnfootCamValid = 0;
	gOnfootHand[0] = gOnfootHand[1] = gOnfootHand[2] = 0;
	gOnfootHandValid = 0;
	gOnfootPlayerPed0 = 0;

	for (i = 0; i < 2; i++)
	{
		gMoveActive[i] = 0;
		gStickX[i] = 0;
		gStickY[i] = 0;
		gDesired[i] = 0;
		gRunSpeed[i] = 0;
	}
}
