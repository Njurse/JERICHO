// yarisbounce.c — the squishy-Yaris meme effect, as an external addon.
//
// This used to live inside the engine: the gBouncePhase/gBounceAmp globals and
// fiddleWithTheModel() in cars.c, a gBouncePhase write in handling.c, and the
// UpdateBounceDisplay() HUD in overlay.c. It is now an ordinary JERICHO module
// and the engine no longer knows the effect exists — enabling and disabling it
// is a modlist.ini entry like any other module.
//
// HOW IT WORKS
//   JER_EVENT_CAR_DRAW fires once per drawn car inside DrawCar(), before the
//   body model is plotted. The handler copies the car's clean model vertices
//   over the car's per-car draw buffer (the buffer DrawCar uses as the body's
//   vlist), then squashes and stretches them: x by scaleX, y by scaleY, plus a
//   vertical lift.
//
//   The WHEELS get the same transform, on their own hook: JER_EVENT_DRAW_WHEEL
//   hands the module the per-wheel vertex copy DrawCarWheels is about to draw,
//   so the tyres squash and ride up with the body instead of being left behind
//   rigid. (The body's scale is remembered per car for the frame so the wheel
//   handler uses exactly the same transform.)
//
//   The "bounce" value swings a full circle once per second (sine over the
//   frame counter) and is passed through a quartic ease so the squash whips in
//   and settles. Holding D-pad Up/Down changes how fast that circle turns (the
//   phase step), saved to CONFIG/yarisbounce.ini when released.
//
//   This is render-only: the physics, the collision, and the clean model
//   itself are never touched.
//
// EXTERNAL ADDON
//   The module uses only the JERICHO SDK and JER_HOST_API: car/model state,
//   the per-car draw vertex buffer, the frame counter, raw pad input, config
//   and HUD. No engine header and no engine data global is imported.
//
// KNOWN INTERACTION — on the record
//   The vertex source is the CLEAN model, copied fresh every frame (exactly as
//   the WIP did, to avoid a self-feeding distortion). That buffer is shared
//   with the deformation system: crumple (and the stock denter) write dents
//   into the per-car draw buffer, and this module overwrites it from the clean
//   model for the frame. So while yarisbounce is active, damaged cars do not
//   show their dents. The upgrade — keep a per-car baseline instead of the
//   clean model — is isolated in yarisBounceWriteBody() so it is a
//   one-function change when wanted.

#include <math.h>
#include <stdio.h>

#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"
#include "jer_host_api.h"
#include "jer_host_helpers.h"
#include "yarisbounce.h"

#define YB_MAX_CARS        32
#define YB_MPAD_D_UP       0x1000
#define YB_MPAD_D_DOWN     0x4000

/* The live bounce speed (frames are 1/30 s at the fixed step). The player
 * changes it with the D-pad; it is the module's own copy of the knob. */
static int sPhaseStep = YARIS_BOUNCE_PHASE_STEP;
static int sPhaseStepAdjusting = 0;	/* d-pad was held last frame */

/* Per-car transform for the current frame, so the wheel hook applies exactly
 * what the body got (indexed by the engine car id). */
static float sCarLift[YB_MAX_CARS];
static int sCarLiftFrame[YB_MAX_CARS];

/* one-shot: log the first squish, so a headless run can prove the effect ran
 * without a per-frame spam */
static int sLoggedFirstSquish = 0;
static int sLoggedFirstWheel = 0;

#if YARIS_DEBUG_HUD
/* last values written, for the debug readout */
static float sLastBounce = 0.0f;
static float sLastScaleX = 1.0f;
static float sLastScaleY = 1.0f;
#endif

static const JER_HOST_API* sHost;

static const JER_HOST_API* ybHost(void)
{
	if (sHost == NULL)
		sHost = jerHostApiGet();

	return sHost;
}

#define YB_LOG(...) \
	do { const JER_HOST_API* ybApi_ = ybHost(); \
		 if (ybApi_ != NULL && ybApi_->core != NULL && ybApi_->core->log != NULL) \
			ybApi_->core->log(__VA_ARGS__); } while (0)

static int ybCfgGetInt(const char* key, int def)
{
	const JER_HOST_API* api = ybHost();
	return (api != NULL && api->config != NULL && api->config->get_int != NULL)
		? api->config->get_int("yarisbounce", key, def) : def;
}

static void ybCfgSetInt(const char* key, int value)
{
	const JER_HOST_API* api = ybHost();

	if (api != NULL && api->config != NULL && api->config->set_int != NULL)
		api->config->set_int("yarisbounce", key, value);
}

static int ybFrame(void)
{
	return jerHostFrame();
}

/* PSX angle units: 4096 = a full circle. */
static int ybSin(int a)
{
	return (int)(sin((double)a * 6.28318530717958647692 / 4096.0) * 4096.0);
}

/* The bounce for this frame: -1..1-ish, one circle per (4096/step) frames. */
static void yarisBounceCompute(float* outBounce, float* outScaleX, float* outScaleY)
{
	int phase = ybFrame() & 4095;
	int sinFixed = (int)interpolate_quartic_ease_out((float)ybSin(phase * sPhaseStep),
					YARIS_BOUNCE_SIN_MIN, YARIS_BOUNCE_SIN_MAX, YARIS_BOUNCE_MARGIN);
	float bounce = (float)sinFixed / 4096.0f;

	*outBounce = bounce;
	*outScaleX = 1.0f - bounce * YARIS_BOUNCE_AMP;	/* shorter/wider */
	*outScaleY = 1.0f + bounce * YARIS_BOUNCE_AMP;	/* taller/narrower */
}

/* How far the car's vertices are lifted so the squash doesn't sink it into the
 * road. Depends only on the car's box height and the frame's scale. The host
 * cosmetic box is the same half-height the old hd.oBox.length[1] / 2 used. */
static float yarisBounceLift(int boxHalfHeight, float bounce, float scaleX)
{
	float verticalOffsetScaled = YARIS_BOUNCE_LIFT * (bounce < 0 ? -bounce : bounce);

	return YARIS_BOUNCE_LIFT + scaleX * (float)boxHalfHeight - verticalOffsetScaled;
}

/* Squash and stretch one car's body vertices in place. */
static void yarisBounceWriteBody(void* car)
{
	const JER_HOST_API* api = ybHost();
	JER_CAR_STATE state;
	JER_COSMETIC_INFO cos;
	JER_SVEC* dstVerts;
	const JER_SVEC* srcVerts;
	void* srcModel;
	int numVerts, capacity, i;
	int boxHalfHeight = 0;
	float bounce, scaleX, scaleY, lift;

	/* speed 0 = the effect is off (stock geometry), which also makes the
	 * D-pad's lowest setting a clean "no bounce" */
	if (sPhaseStep == 0)
		return;

	if (api == NULL || api->car == NULL || api->model == NULL || car == NULL)
		return;

	if (!api->car->get_state(car, &state))
		return;

	/* the pristine per-slot model; NULL in the frontend / before a level's
	 * car models are resident, so there is nothing to squish yet */
	srcModel = api->model->clean(state.model);

	if (srcModel == NULL)
		return;

	numVerts = api->model->vertex_count(srcModel);
	srcVerts = api->model->vertices(srcModel);

	if (srcVerts == NULL || numVerts <= 0)
		return;

	if (!api->car->draw_vertices(car, &dstVerts, &capacity) ||
		dstVerts == NULL || capacity <= 0)
		return;

	/* The clean model can be larger than the per-car denting buffer; never
	 * write past what the host handed us. */
	if (numVerts > capacity)
		numVerts = capacity;

	if (numVerts <= 0)
		return;

	/* refresh from the clean model, so the squash never compounds frame to
	 * frame (the WIP's "feedback loop as it keeps reading the distorted
	 * model") */
	for (i = 0; i < numVerts; i++)
		dstVerts[i] = srcVerts[i];

	yarisBounceCompute(&bounce, &scaleX, &scaleY);

	if (api->car->cosmetic_info != NULL && api->car->cosmetic_info(car, &cos))
		boxHalfHeight = cos.colBox.y;

	lift = yarisBounceLift(boxHalfHeight, bounce, scaleX);

	if (!sLoggedFirstSquish)
	{
		short vyMin = dstVerts[0].y, vyMax = dstVerts[0].y;

		for (i = 1; i < numVerts; i++)
		{
			if (dstVerts[i].y < vyMin)
				vyMin = dstVerts[i].y;

			if (dstVerts[i].y > vyMax)
				vyMax = dstVerts[i].y;
		}

		sLoggedFirstSquish = 1;
		YB_LOG("[yarisbounce] first squish: car=%d model=%d verts=%d scaleX=%.2f scaleY=%.2f step=%d boxY=%d vy=[%d..%d] lift=%.1f\n",
			state.carId, state.model, numVerts, scaleX, scaleY, sPhaseStep,
			boxHalfHeight, vyMin, vyMax, lift);
	}

	for (i = 0; i < numVerts; i++)
	{
		dstVerts[i].x = (short)(dstVerts[i].x * scaleX);
		dstVerts[i].y = (short)(dstVerts[i].y * scaleY + lift);
	}

	/* hand the transform to the wheel hook for this car, this frame */
	if (state.carId >= 0 && state.carId < YB_MAX_CARS)
	{
		sCarLift[state.carId] = lift;
		sCarLiftFrame[state.carId] = ybFrame();
	}

#if YARIS_DEBUG_HUD
	sLastBounce = bounce;
	sLastScaleX = scaleX;
	sLastScaleY = scaleY;
#endif
}

/* JER_EVENT_CAR_DRAW — fired per drawn car in DrawCar(), before the body
 * matrix is built and before the body vlist points at the per-car vertex
 * buffer, so writing that buffer here is what gets drawn. */
static int yarisBounceOnCarDraw(void* userdata, void* args)
{
	JER_ARGS_CAR_DRAW* a = (JER_ARGS_CAR_DRAW*)args;

	(void)userdata;

	if (a != NULL)
		yarisBounceWriteBody(a->car);

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_DRAW_WHEEL — fired per wheel in DrawCarWheels() with the module's
 * own per-wheel vertex copy (wheel-local coordinates, same 1:1 scale as the
 * body). Apply the SAME squash and lift the body got, so the tyres deform with
 * the car instead of staying rigid. */
static int yarisBounceOnDrawWheel(void* userdata, void* args)
{
	JER_ARGS_DRAW_WHEEL* a = (JER_ARGS_DRAW_WHEEL*)args;
	JER_SVEC* verts;
	float bounce, scaleX, scaleY, lift;
	int frame = ybFrame();
	int i;

	(void)userdata;

	if (a == NULL || a->verts == NULL || a->numVerts <= 0)
		return JER_RESULT_CONTINUE;

	if (a->carId < 0 || a->carId >= YB_MAX_CARS)
		return JER_RESULT_CONTINUE;

	/* only if the body was squished this frame (the transform lives there) */
	if (sCarLiftFrame[a->carId] != frame)
		return JER_RESULT_CONTINUE;

	yarisBounceCompute(&bounce, &scaleX, &scaleY);
	lift = sCarLift[a->carId];

	verts = (JER_SVEC*)a->verts;

	if (!sLoggedFirstWheel)
	{
		sLoggedFirstWheel = 1;
		YB_LOG("[yarisbounce] first wheel: car=%d wheel=%d verts=%d scaleX=%.2f scaleY=%.2f lift=%.1f\n",
			a->carId, a->wheelnum, a->numVerts, scaleX, scaleY, lift);
	}

	for (i = 0; i < a->numVerts; i++)
	{
		verts[i].x = (short)(verts[i].x * scaleX);
		verts[i].y = (short)(verts[i].y * scaleY + lift);
	}

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_FRAME — D-pad Up/Down tunes the bounce speed. The step moves every
 * frame the pad is held (clamped), and is saved + reported once, on release. */
static int yarisBounceOnFrame(void* userdata, void* args)
{
	int dpad = jerHostPadDirect(0);
	int dir = 0;

	(void)userdata;
	(void)args;

	if (dpad & YB_MPAD_D_UP)
		dir = 1;
	else if (dpad & YB_MPAD_D_DOWN)
		dir = -1;

	if (dir != 0)
	{
		sPhaseStep += dir * YARIS_BOUNCE_STEP_RATE;

		if (sPhaseStep < YARIS_BOUNCE_STEP_MIN)
			sPhaseStep = YARIS_BOUNCE_STEP_MIN;

		if (sPhaseStep > YARIS_BOUNCE_STEP_MAX)
			sPhaseStep = YARIS_BOUNCE_STEP_MAX;

		sPhaseStepAdjusting = 1;
	}
	else if (sPhaseStepAdjusting)
	{
		sPhaseStepAdjusting = 0;

		ybCfgSetInt("phase_step", sPhaseStep);
		YB_LOG("[yarisbounce] bounce speed: phase step -> %d (%.2f cycles/sec)\n",
			sPhaseStep, (float)sPhaseStep * 30.0f / 4096.0f);
	}

	return JER_RESULT_CONTINUE;
}

#if YARIS_DEBUG_HUD
/* JER_EVENT_DRAW_OVERLAY — the live phase/scale readout. */
static int yarisBounceOnDrawOverlay(void* userdata, void* args)
{
	const JER_HOST_API* api = ybHost();
	char buf[80];

	(void)userdata;
	(void)args;

	sprintf(buf, "Yaris bounce: %.2f  X:%.2f Y:%.2f  step:%d", sLastBounce, sLastScaleX, sLastScaleY, sPhaseStep);

	if (api != NULL && api->ui != NULL && api->ui->hud_message != NULL)
		api->ui->hud_message(buf, 2);

	return JER_RESULT_CONTINUE;
}
#endif

/*
 * JERICHO module entry — the runtime loader calls this as
 * jer_module_yarisbounce_entry() when the module is enabled.
 */
JER_MODULE_ENTRY(jer_module_yarisbounce_entry)(JERICHO_CONTEXT* ctx)
{
	/* pick up the bounce speed the player last tuned (D-pad), else the default */
	sPhaseStep = ybCfgGetInt("phase_step", YARIS_BOUNCE_PHASE_STEP);

	if (sPhaseStep < YARIS_BOUNCE_STEP_MIN)
		sPhaseStep = YARIS_BOUNCE_STEP_MIN;

	if (sPhaseStep > YARIS_BOUNCE_STEP_MAX)
		sPhaseStep = YARIS_BOUNCE_STEP_MAX;

	ctx->jer_register_module(ctx,
		"yarisbounce",		/* id */
		"Yaris Bounce",		/* name */
		"1.0.0",		/* version */
		"Nattdy",		/* author */
		"Squishy-Yaris squash & stretch on the car body AND wheels (render-only). D-pad Up/Down sets the speed.",	/* description */
		"",			/* dependencies */
		JERICHO_SDK_VERSION);	/* SDK this module was built against */

	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW, yarisBounceOnCarDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WHEEL, yarisBounceOnDrawWheel, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, yarisBounceOnFrame, NULL, 0);
#if YARIS_DEBUG_HUD
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, yarisBounceOnDrawOverlay, NULL, 0);
#endif

	ctx->jer_log(ctx, "[yarisbounce] registered (SDK v%d, bounce speed step=%d)\n", ctx->sdkVersion, sPhaseStep);
}
