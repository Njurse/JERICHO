/* d2pl_cam.c — look, camera-relative movement and the aim camera.
 *
 * Everything here is DLL-safe: the pad is read through the host API, the
 * camera angle through the player token in the event args, and the ped is
 * driven through the exported jer_npc_face / jer_npc_set_speed setters
 * (PED_MOVE fires after the runner's speed re-arm, so the write survives).
 */

#include "d2pl.h"
#include "jer_anim.h"
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

D2plSettings gS = {
	1,		/* cameraEnabled */
	1,		/* joyCamera */
	16,		/* sensX */
	16,		/* sensY */
	0,		/* invertY: 0 = push up looks up (conventional) */
	1		/* shoulder: right */
};

int gD2plAiming = 0;
int gD2plBackpedalOn = D2PL_BACKPEDAL;
int gD2plBackpedalActive = 0;
int gD2plBackpedalAngle = D2PL_BACKPEDAL_ANGLE;

static int gBackpedal[2] = { 0, 0 };	/* PED_INPUT chose walk-back this frame */
static void* gReversePed = 0;			/* ped whose flag 4 d2pl set, or 0 */

/* Clear the reverse-frames bit only if d2pl set it: the engine uses the
 * same bit for the car-door animation and must keep its own. */
static void D2plReleaseReverse(void)
{
	if (gReversePed != 0)
		jer_npc_set_reverse_anim((JerNpc*)gReversePed, 0);

	gReversePed = 0;
	gD2plBackpedalActive = 0;
}
int gD2plLookPitch = 0;
int gD2plViewHeading = 0;

int gD2plCamPos[3] = { 0, 0, 0 };
int gD2plCamAngle[3] = { 0, 0, 0 };
int gD2plCamValid = 0;

int gD2plHand[3] = { 0, 0, 0 };
int gD2plHandValid = 0;

void* gD2plPlayerPed0 = 0;

/* movement state, per pad (two local players at most) */
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

#define D2PL_ANGLE_TO_RAD (6.28318530718f / 4096.0f)

int D2plSin(int angle)
{
	return (int)(sinf((float)(angle & 0xfff) * D2PL_ANGLE_TO_RAD) * 4096.0f);
}

int D2plCos(int angle)
{
	return D2plSin(angle + 1024);
}

int D2plAtan2(int y, int x)
{
	if (x == 0 && y == 0)
		return 0;

	return (int)(atan2f((float)y, (float)x) / D2PL_ANGLE_TO_RAD) & 0xfff;
}

/* ------------------------------------------------------------------ */
/* Mouse look                                                          */
/* ------------------------------------------------------------------ */

/* Resolve SDL_GetRelativeMouseState from the SDL2 DLL the game already
 * loaded. It is not part of the exe export surface, and linking SDL2 into
 * the addon would tie it to one build, so a runtime lookup is the least
 * invasive way to get relative motion. */
static void D2plPollMouse(void)
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

int D2plOnFrame(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	D2plBootTick();
	D2plPollMouse();
	D2plWeaponUpdate();

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_CAMERA_LOOK — right stick + mouse orbit                   */
/* ------------------------------------------------------------------ */

/* Right-stick response curve: deadzoned magnitude 0..4096, squared, sign
 * preserved. Fine control near centre (aiming), fast at the edge. */
static int D2plStickCurve(int raw)
{
	int mag = abs(raw);
	int m, c;

	if (mag <= D2PL_LOOK_DEADZONE)
		return 0;

	m = (mag - D2PL_LOOK_DEADZONE) * 4096 / (127 - D2PL_LOOK_DEADZONE);
	c = (m * m) >> 12;

	return raw < 0 ? -c : c;
}

int D2plOnLook(void* userdata, void* args)
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
		(abs(sx) > D2PL_LOOK_DEADZONE || abs(sy) > D2PL_LOOK_DEADZONE);
	mouseLook = (gMouseActive > 0) && (gMouseDX != 0 || gMouseDY != 0);

	/* Conventional shooter look:
	 *   - mouse is position control: 1:1 per count on BOTH axes;
	 *   - stick is rate control: one shared curve, same max rate on both axes;
	 *   - pitch holds where you leave it (no auto-centre);
	 *   - aiming (ADS) slows the look by D2PL_AIM_LOOK_SCALE. */
	if (stickLook || mouseLook)
	{
		int yawDelta = 0;
		int pitchDelta = 0;
		int aimScale = gD2plAiming ? D2PL_AIM_LOOK_SCALE : 1;
		int vdir = gS.invertY ? -1 : 1;		/* +1 = push up looks up */

		a->suppress = 1;		/* skip the stock L2/R2 look */
		a->gripOrbit = 1;		/* the addon owns the orbit this frame */

		if (stickLook)
		{
			int cx = D2plStickCurve(sx);
			int cy = D2plStickCurve(sy);

			yawDelta += (cx * gS.sensX * D2PL_STICK_RATE_MAX) /
				(4096 * D2PL_STICK_BASE * aimScale);
			pitchDelta += (cy * gS.sensY * D2PL_STICK_RATE_MAX) /
				(4096 * D2PL_STICK_BASE * aimScale);
		}

		if (mouseLook)
		{
			yawDelta += (gMouseDX * gS.sensX) / (D2PL_MOUSE_SCALE * aimScale);
			pitchDelta += (gMouseDY * gS.sensY) / (D2PL_MOUSE_SCALE * aimScale);

			gMouseDX = 0;
			gMouseDY = 0;
		}

		gD2plLookPitch = jer_clamp_int(
			gD2plLookPitch + vdir * pitchDelta, -D2PL_LOOK_PITCH_MAX, D2PL_LOOK_PITCH_MAX);

		if (yawDelta != 0 && api->player->set_camera_angle != 0)
			api->player->set_camera_angle(a->player, (ps.cameraAngle + yawDelta) & 0xfff);
	}
	else if (gD2plAiming)
	{
		/* aiming: hold the orbit and the pitch so the shot does not drift */
		a->suppress = 1;
		a->gripOrbit = 1;
	}
	/* released and not aiming: the engine may ease the yaw back behind the
	 * ped (the chase cam); the pitch stays where the player left it. */

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_PED_INPUT — camera-relative movement                      */
/* ------------------------------------------------------------------ */

int D2plOnPedInput(void* userdata, void* args)
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

	ps.padid = -1;

	if (!api->player->get_state(a->player, &ps) || ps.playerCarId >= 0)
	{
		if (ps.padid == 0 || ps.padid == 1)
			gBackpedal[ps.padid] = 0;
		D2plReleaseReverse();

		return JER_RESULT_CONTINUE;
	}

	pad = ps.padid;

	if (pad < 0 || pad > 1)
		return JER_RESULT_CONTINUE;

	sx = a->stickX;
	sy = a->stickY;

	if (pad == 0)
		gD2plAiming = (a->pad & D2PL_MPAD_L1) != 0;

	/* On foot the shoulders belong to d2pl (L1 aim, R1 fire, L2 weapon
	 * cycle; the weapon code reads the pad itself), so stock Tanner never
	 * sees them. In a car this hook returned above, so driving is untouched. */
	a->pad &= ~(D2PL_MPAD_L1 | D2PL_MPAD_R1 | D2PL_MPAD_L2);

	mag = abs(sx) + abs(sy);

	/* Car entry reads flag 4 for the door side: hand the bit back before
	 * the engine sees the action press. */
	if (a->pad & (D2PL_MPAD_TRIANGLE | D2PL_MPAD_L3))
	{
		gBackpedal[pad] = 0;
		D2plReleaseReverse();
	}

	if (mag <= D2PL_MOVE_DEADZONE)
	{
		/* Left stick centred: the directional arrows (and the keyboard
		 * arrows, which map to the same pad bits) are also camera-relative.
		 * Synthesise an equivalent stick so both paths share one curve. */
		int dx = 0, dy = 0;

		if (a->pad & D2PL_MPAD_D_UP)    dy -= 127;
		if (a->pad & D2PL_MPAD_D_DOWN)  dy += 127;
		if (a->pad & D2PL_MPAD_D_LEFT)  dx -= 127;
		if (a->pad & D2PL_MPAD_D_RIGHT) dx += 127;

		if (dx == 0 && dy == 0)
		{
			gMoveActive[pad] = 0;
			gBackpedal[pad] = 0;

			return JER_RESULT_CONTINUE;		/* stock D-pad/keyboard handling */
		}

		sx = dx;
		sy = dy;
	}

	gMoveActive[pad] = 1;

	/* record the EFFECTIVE input (stick or arrows) for the analog speed */
	gStickX[pad] = sx;
	gStickY[pad] = sy;

	/* The movement frame is the camera view. stick up = 0, right = 1024. */
	stickHeading = D2plAtan2(D2PL_STICK_X * sx, -D2PL_STICK_Y * sy);
	desired = (gD2plViewHeading + stickHeading) & 0xfff;

	gDesired[pad] = desired;

	/* Backpedal cone: input within +-gD2plBackpedalAngle of straight back
	 * (heading 2048 in the view frame) backs the ped up at run speed with
	 * the run clip reversed, facing the view. Outside the cone the ped
	 * turns and runs forward along `desired`. */
	{
		int backOff = (stickHeading & 0xfff) - 2048;

		gBackpedal[pad] = pad == 0 && gD2plBackpedalOn &&
			!(a->pad & (D2PL_MPAD_TRIANGLE | D2PL_MPAD_L3)) &&
			D2plHoldtypeGet(D2plActiveHoldtype())->allowBackpedal &&
			abs(backOff) < gD2plBackpedalAngle;
	}

	/* Both forward and backpedal use the RUN clip (backpedal plays it in
	 * reverse via flag 4 in PED_MOVE), so the engine always gets GOFORWARD.
	 * The stock turn bits are cleared so they never fight jer_npc_face. */
	a->pad &= ~(D2PL_PAD_GOFORWARD | D2PL_PAD_GOBACK |
		D2PL_PAD_TURNLEFT | D2PL_PAD_TURNRIGHT);
	a->pad |= D2PL_PAD_GOFORWARD;

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_PED_MOVE — heading + analog speed, after the runner re-arm */
/* ------------------------------------------------------------------ */

int D2plOnPedMove(void* userdata, void* args)
{
	JER_ARGS_PED_MOVE* a = (JER_ARGS_PED_MOVE*)args;
	JerNpc* ped;
	int pad, m, target, delta;

	(void)userdata;

	if (a == 0 || a->ped == 0)
		return JER_RESULT_CONTINUE;

	pad = a->padId;

	if (pad < 0 || pad > 1)
		return JER_RESULT_CONTINUE;		/* never drive an ambient ped */

	if (pad == 0)
		gD2plPlayerPed0 = a->ped;

	if (!gS.cameraEnabled)
		return JER_RESULT_CONTINUE;

	/* JerNpc* IS the ped pointer (jer_npc.c casts it straight back). */
	ped = (JerNpc*)a->ped;

	if (pad == 0 && (!gMoveActive[pad] || !gBackpedal[pad]))
		D2plReleaseReverse();

	if (!gMoveActive[pad])
	{
		/* stick centred: the engine's D-pad movement still works. Bleed
		 * the momentum off only while the ped is stopped. */
		gRunSpeed[pad] -= gRunSpeed[pad] / 8;

		if (gRunSpeed[pad] < 1)
			gRunSpeed[pad] = 0;

		/* aiming while standing: turn the whole body toward the aim once
		 * the twist passes D2PL_TURN_START, so the spine never has to
		 * carry more than its clamp (no 180-degree torso) */
		if (pad == 0 && gD2plAiming)
		{
			const JER_HOST_API* api = jerHostApiGet();
			JER_PLAYER_STATE ps;
			int body = -1;

			/* the body facing (visual convention, = ped dir - 2048) comes
			 * from the DRAWN ped when the pose has seen it, else from the
			 * player state (already the visual facing on foot) */
			if (gD2plDrawDirValid)
				body = (gD2plDrawDir - 2048) & 0xfff;
			else if (api != 0 && api->player != 0 && api->player->get_state != 0 &&
				api->player->main != 0 && api->player->get_state(api->player->main(), &ps))
				body = ps.dir & 0xfff;

			if (body >= 0)
			{
				/* positive = the aim is to the right of the body */
				int diff = jer_anim_aim_diff(gD2plViewHeading, body);

				if (abs(diff) > D2PL_TURN_START)
				{
					int step = jer_clamp_int(diff, -D2PL_TURN_RATE, D2PL_TURN_RATE);
					int raw = (body + step + 2048) & 0xfff;	/* raw ped dir */

					jer_npc_face(ped, raw);

					/* mp: the drawn character is a separate ped */
					/* never face the drawn mp ped directly (crumpled OTHER_MODEL) */

					gD2plDrawDir = raw;
				}
			}
		}

		return JER_RESULT_CONTINUE;
	}

	/* The ped moves along dir.vy - 2048, so a MOVEMENT heading h needs
	 * dir.vy = h + 2048. */
	jer_npc_face(ped, (gDesired[pad] + 2048) & 0xfff);

	m = abs(gStickX[pad]);

	if (abs(gStickY[pad]) > m)
		m = abs(gStickY[pad]);

	target = (m - D2PL_MOVE_DEADZONE) * D2PL_RUN_SPEED / (127 - D2PL_MOVE_DEADZONE);

	if (target < 0)
		target = 0;
	if (target > D2PL_RUN_SPEED)
		target = D2PL_RUN_SPEED;

	delta = target - gRunSpeed[pad];

	if (abs(delta) < D2PL_SPEED_LERP)
		gRunSpeed[pad] = target;
	else
		gRunSpeed[pad] += delta / D2PL_SPEED_LERP;

	if (gBackpedal[pad])
	{
		/* Face the view and back up at run speed: flag 4 plays the RUN clip
		 * in reverse (AnimatePed decrements frame1), so this is a fast
		 * backpedal instead of the stock slow walk-back. */
		jer_npc_face(ped, (gD2plViewHeading + 2048) & 0xfff);
		jer_npc_set_speed(ped, -gRunSpeed[pad]);
		jer_npc_set_reverse_anim(ped, 1);
		gReversePed = a->ped;
		gD2plBackpedalActive = 1;

		return JER_RESULT_CONTINUE;
	}

	jer_npc_set_speed(ped, gRunSpeed[pad]);

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_CAMERA — aim framing, pitch and the cached view            */
/* ------------------------------------------------------------------ */

int D2plOnCamera(void* userdata, void* args)
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
	gD2plViewHeading = heading;

	if (gD2plAiming)
	{
		if (base != 0)
		{
			/* over the shoulder: pull in, lift to the shoulder, shift
			 * sideways. The engine already placed (and collision-checked)
			 * the camera; these are small deltas around that point. */
			pos->vx += ((base->vx - pos->vx) * D2PL_AIM_PULL) / 4096;
			pos->vz += ((base->vz - pos->vz) * D2PL_AIM_PULL) / 4096;
			pos->vy = -base->vy + D2PL_AIM_HEIGHT;
			pos->vx += (D2plCos(heading) * D2PL_AIM_LATERAL * gS.shoulder) >> 12;
			pos->vz -= (D2plSin(heading) * D2PL_AIM_LATERAL * gS.shoulder) >> 12;
		}

		ang->vx = (short)(ang->vx + gD2plLookPitch);
		a->override = 1;
	}
	else if (gD2plLookPitch != 0)
	{
		/* free-look tilt: rotate the view, keep the engine's position */
		ang->vx = (short)(ang->vx + gD2plLookPitch);
		a->override = 1;
	}

	gD2plCamPos[0] = pos->vx;
	gD2plCamPos[1] = pos->vy;
	gD2plCamPos[2] = pos->vz;
	gD2plCamAngle[0] = ang->vx;
	gD2plCamAngle[1] = ang->vy;
	gD2plCamAngle[2] = ang->vz;
	gD2plCamValid = 1;

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void D2plInit(void)
{
	int i;

	gD2plAiming = 0;
	gD2plLookPitch = 0;
	gD2plViewHeading = 0;
	gMouseDX = 0;
	gMouseDY = 0;
	gMouseActive = 0;

	gD2plCamPos[0] = gD2plCamPos[1] = gD2plCamPos[2] = 0;
	gD2plCamAngle[0] = gD2plCamAngle[1] = gD2plCamAngle[2] = 0;
	gD2plCamValid = 0;
	gD2plHand[0] = gD2plHand[1] = gD2plHand[2] = 0;
	gD2plHandValid = 0;
	gD2plPlayerPed0 = 0;
	gD2plDrawPed0 = 0;
	gD2plDrawDirValid = 0;

	for (i = 0; i < 2; i++)
	{
		gMoveActive[i] = 0;
		gStickX[i] = 0;
		gStickY[i] = 0;
		gDesired[i] = 0;
		gRunSpeed[i] = 0;
	}
}
