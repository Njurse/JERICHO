// combatd2media.c — Combat D2: PRESENTATION source file.
//
// One of the source files of the single combatd2 module (merged back from
// the old separate "combatd2media" module; registered by the module entry
// in combatd2.c via cd2MediaRegister()). combatd2.c (core) owns the car
// feel: input layout, torque/physics, slides, walls. THIS file owns the
// presentation of that feel:
//
//   * JER_EVENT_CAR_GEARBOX      - short punchy gears, tall top gear whose
//                                  ratio levels engine pitch at the car's
//                                  combatd2 top speed (per-car, exported by
//                                  the core's cd2CarTopSpeed()).
//   * JER_EVENT_CAR_REVS         - how fast the pitch climbs/falls (slew).
//   * JER_EVENT_CAR_ENGINE_SOUND - louder rev + idle channels.
//   * JER_EVENT_CAMERA           - TMB chase framing + speed FOV pull.
//
// All tuners are compile-time macros in combatd2.h under the
// "ENGINE AUDIO & GEARBOX" and "VISUALS" sections; this file reads the
// core's shared runtime config (gCd2Cfg) for master enable + the FOV slider.

#include "driver2.h"
#include "combatd2.h"
#include "cars.h"
#include "cosmetic.h"
#include "camera.h"
#include "convert.h"
#include "players.h"
#include "pad.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"

// GEARBOX (JER_EVENT_CAR_GEARBOX, gamesnd.c GetEngineRevs): while a player
// drives, retune the rev model so gears feel SHORT and punchy but the tall
// top gear levels the pitch at the car's combatd2 top speed — revs never
// wind past CD2_REV_CEILING even as the point-mass speed keeps climbing.
static int cd2mOnCarGearbox(void* ud, void* args)
{
	JER_ARGS_CAR_GEARBOX* g = (JER_ARGS_CAR_GEARBOX*)args;
	CAR_DATA* cp = (CAR_DATA*)g->car;
	int top, i, prevHi, wsTop;
	(void)ud;

	if (!gCd2Cfg.enabled || !CD2_GEAR_AUTO)
		return JER_RESULT_CONTINUE;

	if (cp->controlType != CONTROL_TYPE_PLAYER)
		return JER_RESULT_CONTINUE;

	top = cd2CarTopSpeed(cp); // effective top (config x scale x chassis)
	if (top < 40)
		return JER_RESULT_CONTINUE;

	wsTop = (int)(((long long)top * CD2_WS_PER_SPEED) >> 12); // ws at top speed
	if (wsTop < 30)
		return JER_RESULT_CONTINUE;

	{
		int up[4];
		int fracs[3] = { CD2_GEAR_1_FRAC, CD2_GEAR_2_FRAC, CD2_GEAR_3_FRAC };

		prevHi = 0;
		for (i = 0; i < 3; i++)
		{
			up[i] = (int)(((long long)top * fracs[i]) >> 12);
			up[i] = (int)(((long long)up[i] * CD2_WS_PER_SPEED) >> 12);
			if (up[i] <= prevHi)
				up[i] = prevHi + 1;
			prevHi = up[i];
		}
		up[3] = wsTop;
		if (up[3] <= prevHi)
			up[3] = prevHi + 1;

		for (i = 0; i < 4; i++)
		{
			g->hiWs[i] = up[i];

			if (i == 0)
			{
				g->lowWs[i] = 0;
				g->lowIdleWs[i] = 0;
			}
			else
			{
				int lo = (int)(((long long)up[i - 1] * CD2_GEAR_DOWN_FRAC) >> 12);
				if (lo >= up[i])
					lo = up[i] - 1;
				g->lowWs[i] = lo;
				g->lowIdleWs[i] = (int)(((long long)lo * 3) >> 2);
			}

			if (i < 3)
			{
				int ratio = CD2_GEAR_SHIFT_REVS / up[i];
				if (ratio < 8) ratio = 8;
				g->ratioAc[i] = ratio;
				g->ratioIdle[i] = ratio;
			}
			else
			{
				int ratio = CD2_REV_CEILING / up[i];
				if (ratio < 4) ratio = 4;
				g->ratioAc[i] = ratio;
				g->ratioIdle[i] = ratio;
			}
		}

		g->revCeiling = CD2_REV_CEILING;
	}

	return JER_RESULT_CONTINUE;
}

// ENGINE SOUND (JER_EVENT_CAR_ENGINE_SOUND, gamesnd.c SoundTasks): scale and
// offset the player car's rev + idle channel pitch and volume. Tuners live in
// combatd2.h as CD2_SND_*; see the header guide for units and directions.
static int cd2mOnCarEngineSound(void* ud, void* args)
{
	JER_ARGS_CAR_ENGINE_SOUND* e = (JER_ARGS_CAR_ENGINE_SOUND*)args;
	long long p;
	long long v;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	p = ((long long)e->revPitch * CD2_SND_PITCH_SCALE) >> 12;
	e->revPitch = (int)p + CD2_SND_PITCH_BIAS;

	p = ((long long)e->idlePitch * CD2_SND_PITCH_SCALE) >> 12;
	e->idlePitch = (int)p + CD2_SND_IDLE_PITCH_BIAS;

	// Volume is PSX attenuation (0 loudest, -10000 silent): gain divides the
	// attenuation (8192 ≈ twice as loud), bias moves it toward 0 (louder).
	v = -e->revVolume;
	if (CD2_SND_REV_GAIN > 0 && v > 0)
		v = (v * 4096) / CD2_SND_REV_GAIN;
	v = -v + CD2_SND_REV_BIAS;
	if (v < CD2_SND_MIN_VOL) v = CD2_SND_MIN_VOL;
	if (v > CD2_SND_MAX_VOL) v = CD2_SND_MAX_VOL;
	e->revVolume = (int)v;

	v = -e->idleVolume;
	if (CD2_SND_IDLE_GAIN > 0 && v > 0)
		v = (v * 4096) / CD2_SND_IDLE_GAIN;
	v = -v + CD2_SND_IDLE_BIAS;
	if (v < CD2_SND_MIN_VOL) v = CD2_SND_MIN_VOL;
	if (v > CD2_SND_MAX_VOL) v = CD2_SND_MAX_VOL;
	e->idleVolume = (int)v;

	return JER_RESULT_CONTINUE;
}

// REV SLEW (JER_EVENT_CAR_REVS, gamesnd.c ControlCarRevs): scale how fast the
// engine pitch climbs toward its target revs (CD2_REV_RISE_SCALE) and how
// hard it falls on shifts / let-off (CD2_REV_DROP_SCALE). Player cars only.
static int cd2mOnCarRevs(void* ud, void* args)
{
	JER_ARGS_CAR_REVS* r = (JER_ARGS_CAR_REVS*)args;
	CAR_DATA* cp = (CAR_DATA*)r->car;
	(void)ud;

	if (!gCd2Cfg.enabled || cp->controlType != CONTROL_TYPE_PLAYER)
		return JER_RESULT_CONTINUE;

	r->revRise = (int)(((long long)r->revRise * CD2_REV_RISE_SCALE) >> 12);
	r->revDrop = (int)(((long long)r->revDrop * CD2_REV_DROP_SCALE) >> 12);

	if (r->revRise < 0) r->revRise = 0;
	if (r->revDrop < 0) r->revDrop = 0;

	return JER_RESULT_CONTINUE;
}

// CAMERA (JER_EVENT_CAMERA, camera.c): TMB chase framing + speed FOV pull.
static int cd2mOnCamera(void* ud, void* args)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	(void)ud;

	if (!gCd2Cfg.enabled || !a->inCar)
		return JER_RESULT_CONTINUE;

	// TMB chase framing: after the engine places the main chase camera, pull
	// it closer to the car on the ground plane (and optionally lower). These
	// are relative nudges (fractions of the gap) applied to camera_position
	// only, so the engine's own re-place each frame keeps this stable.
	if (a->cameraView == 0 && CD2_CAM_PULL > 0)
	{
		int* bp = (int*)a->basePos;
		int* cp = (int*)a->cameraPosition;
		long long dx = (long long)bp[0] - cp[0];
		long long dz = (long long)bp[2] - cp[2];

		cp[0] += (int)((dx * CD2_CAM_PULL) >> 12);
		cp[2] += (int)((dz * CD2_CAM_PULL) >> 12);

		if (CD2_CAM_LOW > 0)
		{
			long long dy = (long long)bp[1] - cp[1];
			cp[1] += (int)((dy * CD2_CAM_LOW) >> 12);
		}
	}

	if (gCd2Cfg.fovPull <= 0)
		return JER_RESULT_CONTINUE;

	int speed = FIXEDH(a->carSpeed);
	int spd = jer_clamp_int(speed, 0, CD2_FOV_REF_SPEED);
	int pull = spd * gCd2Cfg.fovPull * CD2_FOV_PULL_SCRZ / (CD2_FOV_REF_SPEED * 100);

	SetGeomScreen(gCameraDefaultScrZ - pull);
	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_combatd2_entry in combatd2.c)
// ---------------------------------------------------------------------------

void cd2MediaRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_GEARBOX, cd2mOnCarGearbox, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_REVS, cd2mOnCarRevs, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE_SOUND, cd2mOnCarEngineSound, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, cd2mOnCamera, NULL, 0);

	ctx->jer_log(ctx, "[combatd2] presentation registered (SDK v%d)\n", ctx->sdkVersion);
}
