// cainescrossfirecamerafx.c — Combat D2: chase framing + speed FOV pull.
//
// (Split out of the old cainescrossfiremedia.c; see cainescrossfire_internal.h for the file
// map.)
//
// JER_EVENT_CAMERA: after the engine places the chase camera, pull it closer to
// the car on the ground plane (and optionally lower it) - relative nudges of
// the gap, applied to camera_position only so the engine's own re-place each
// frame keeps it stable - and pull the FOV out with speed (the CD2_FOV_* macros
// + the gCd2Cfg.fovPull slider).

#include "driver2.h"
#include "cainescrossfire.h"
#include "camera.h"
#include "convert.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"

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
// Registration (called once by jer_module_cainescrossfire_entry in cainescrossfire.c)
// ---------------------------------------------------------------------------
void cd2CameraFxRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, cd2mOnCamera, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] camera effects registered (SDK v%d)\n", ctx->sdkVersion);
}
