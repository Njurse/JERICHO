// yarisbounce.c — the squishy-Yaris meme effect, as a JERICHO module.
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
//   over the car's per-car draw buffer (gTempCarVertDump[cp->id] — the buffer
//   DrawCar uses as the body's vlist), then squashes and stretches them: vx by
//   scaleX, vy by scaleY, plus a vertical lift.
//
//   The WHEELS get the same transform, on their own hook: JER_EVENT_DRAW_WHEEL
//   hands the module the per-wheel vertex copy DrawCarWheels is about to draw,
//   so the tyres squash and ride up with the body instead of being left behind
//   rigid. (The body's scale is remembered per car for the frame so the wheel
//   handler uses exactly the same transform.)
//
//   The "bounce" value swings a full circle once per second (RSIN over the
//   frame counter) and is passed through a quartic ease so the squash whips in
//   and settles. Holding D-pad Up/Down changes how fast that circle turns (the
//   phase step), saved to CONFIG/yarisbounce.ini when released.
//
//   This is render-only: the physics, the collision, and the clean model
//   itself are never touched.
//
// KNOWN INTERACTION — on the record
//   The vertex source is the CLEAN model, copied fresh every frame (exactly as
//   the WIP did, to avoid a self-feeding distortion). That buffer is shared
//   with the deformation system: crumple (and the stock denter) write dents
//   into gTempCarVertDump[cp->id], and this module overwrites it from the
//   clean model for the frame. So while yarisbounce is active, damaged cars
//   do not show their dents. The upgrade — keep a per-car baseline instead of
//   the clean model — is isolated in yarisBounceWriteBody() so it is a
//   one-function change when wanted.

#include "driver2.h"
#include "cars.h"		// gCarCleanModelPtr, gTempCarVertDump, CAR_DATA
#include "main.h"		// FrameCnt
#include "pad.h"		// Pads[0].direct, MPAD_D_*
#include "jericho.h"		// JERICHO: module API
#include "jer_events.h"		// JERICHO: event argument structs
#include "jer_math.h"		// interpolate_quartic_ease_out
#include "jer_config.h"		// persist the tuned bounce speed
#include "jer_hud.h"		// debug readout (YARIS_DEBUG_HUD only)

#include "yarisbounce.h"

/* The live bounce speed (frames are 1/30 s at the fixed step). The player
 * changes it with the D-pad; it is the module's own copy of the knob. */
static int sPhaseStep = YARIS_BOUNCE_PHASE_STEP;
static int sPhaseStepAdjusting = 0;	/* d-pad was held last frame */

/* Per-car transform for the current frame, so the wheel hook applies exactly
 * what the body got (indexed by cp->id, like gTempCarVertDump). */
static float sCarLift[MAX_CARS];
static int sCarLiftFrame[MAX_CARS];

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

/* The bounce for this frame: -1..1-ish, one circle per (4096/step) frames. */
static void yarisBounceCompute(float* outBounce, float* outScaleX, float* outScaleY)
{
	int phase = FrameCnt & 4095;
	int sinFixed = (int)interpolate_quartic_ease_out((float)RSIN(phase * sPhaseStep),
					YARIS_BOUNCE_SIN_MIN, YARIS_BOUNCE_SIN_MAX, YARIS_BOUNCE_MARGIN);
	float bounce = (float)sinFixed / 4096.0f;

	*outBounce = bounce;
	*outScaleX = 1.0f - bounce * YARIS_BOUNCE_AMP;	/* shorter/wider */
	*outScaleY = 1.0f + bounce * YARIS_BOUNCE_AMP;	/* taller/narrower */
}

/* How far the car's vertices are lifted so the squash doesn't sink it into the
 * road. Depends only on the car's box height and the frame's scale. */
static float yarisBounceLift(CAR_DATA* cp, float bounce, float scaleX)
{
	float verticalOffsetScaled = YARIS_BOUNCE_LIFT * (bounce < 0 ? -bounce : bounce);

	return YARIS_BOUNCE_LIFT + scaleX * (cp->hd.oBox.length[1] / 2) - verticalOffsetScaled;
}

/* Squash and stretch one car's body vertices in place.
 *
 * Ported from the engine's fiddleWithTheModel(). The one structural change:
 * the WIP printed a line per vertex (thousands of lines a frame) and wrote
 * two dead locals — both dropped. The maths, the vertex source, and the
 * vertical-offset formula are the original's. */
static void yarisBounceWriteBody(CAR_DATA* cp)
{
	MODEL* srcModel;
	SVECTOR* srcVerts;
	SVECTOR* dstVerts;
	int numVerts, i;
	float bounce, scaleX, scaleY, lift;

	if (cp == NULL)
		return;

	/* speed 0 = the effect is off (stock geometry), which also makes the
	 * D-pad's lowest setting a clean "no bounce" */
	if (sPhaseStep == 0)
		return;

	/* the pristine per-slot model; NULL in the frontend / before a level's
	 * car models are resident, so there is nothing to squish yet */
	srcModel = gCarCleanModelPtr[cp->ap.model];

	if (srcModel == NULL)
		return;

	numVerts = srcModel->num_vertices;

	/* gTempCarVertDump[cp->id] is exactly MAX_DENTING_VERTS long — never write
	 * past it. (The WIP looped over num_vertices unclamped.) */
	if (numVerts > MAX_DENTING_VERTS)
		numVerts = MAX_DENTING_VERTS;

	if (numVerts <= 0)
		return;

	srcVerts = GET_MODEL_DATA(SVECTOR, srcModel, vertices);
	dstVerts = gTempCarVertDump[cp->id];

	/* refresh from the clean model, so the squash never compounds frame to
	 * frame (the WIP's "feedback loop as it keeps reading the distorted
	 * model") */
	for (i = 0; i < numVerts; i++)
		dstVerts[i] = srcVerts[i];

	yarisBounceCompute(&bounce, &scaleX, &scaleY);
	lift = yarisBounceLift(cp, bounce, scaleX);

	if (!sLoggedFirstSquish)
	{
		short vyMin = dstVerts[0].vy, vyMax = dstVerts[0].vy;

		for (i = 1; i < numVerts; i++)
		{
			if (dstVerts[i].vy < vyMin)
				vyMin = dstVerts[i].vy;

			if (dstVerts[i].vy > vyMax)
				vyMax = dstVerts[i].vy;
		}

		sLoggedFirstSquish = 1;
		jer_log("[yarisbounce] first squish: car=%d model=%d verts=%d scaleX=%.2f scaleY=%.2f step=%d boxY=%d vy=[%d..%d] lift=%.1f\n",
			cp->id, (int)cp->ap.model, numVerts, scaleX, scaleY, sPhaseStep,
			(int)cp->hd.oBox.length[1], vyMin, vyMax, lift);
	}

	for (i = 0; i < numVerts; i++)
	{
		dstVerts[i].vx = (short)(dstVerts[i].vx * scaleX);
		dstVerts[i].vy = (short)(dstVerts[i].vy * scaleY + lift);
	}

	/* hand the transform to the wheel hook for this car, this frame */
	if (cp->id >= 0 && cp->id < MAX_CARS)
	{
		sCarLift[cp->id] = lift;
		sCarLiftFrame[cp->id] = FrameCnt;
	}

#if YARIS_DEBUG_HUD
	sLastBounce = bounce;
	sLastScaleX = scaleX;
	sLastScaleY = scaleY;
#endif
}

/* JER_EVENT_CAR_DRAW — fired per drawn car in DrawCar(), before the body
 * matrix is built and before CarModelPtr->vlist is pointed at the per-car
 * vertex buffer, so writing that buffer here is what gets drawn. */
static int yarisBounceOnCarDraw(void* userdata, void* args)
{
	JER_ARGS_CAR_DRAW* a = (JER_ARGS_CAR_DRAW*)args;

	(void)userdata;

	if (a != NULL)
		yarisBounceWriteBody((CAR_DATA*)a->car);

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_DRAW_WHEEL — fired per wheel in DrawCarWheels() with the module's
 * own per-wheel vertex copy (wheel-local coordinates, same 1:1 scale as the
 * body). Apply the SAME squash and lift the body got, so the tyres deform with
 * the car instead of staying rigid. */
static int yarisBounceOnDrawWheel(void* userdata, void* args)
{
	JER_ARGS_DRAW_WHEEL* a = (JER_ARGS_DRAW_WHEEL*)args;
	SVECTOR* verts;
	float bounce, scaleX, scaleY, lift;
	int i;

	(void)userdata;

	if (a == NULL || a->verts == NULL || a->numVerts <= 0)
		return JER_RESULT_CONTINUE;

	if (a->carId < 0 || a->carId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	/* only if the body was squished this frame (the transform lives there) */
	if (sCarLiftFrame[a->carId] != FrameCnt)
		return JER_RESULT_CONTINUE;

	yarisBounceCompute(&bounce, &scaleX, &scaleY);
	lift = sCarLift[a->carId];

	verts = (SVECTOR*)a->verts;

	if (!sLoggedFirstWheel)
	{
		sLoggedFirstWheel = 1;
		jer_log("[yarisbounce] first wheel: car=%d wheel=%d verts=%d scaleX=%.2f scaleY=%.2f lift=%.1f\n",
			a->carId, a->wheelnum, a->numVerts, scaleX, scaleY, lift);
	}

	for (i = 0; i < a->numVerts; i++)
	{
		verts[i].vx = (short)(verts[i].vx * scaleX);
		verts[i].vy = (short)(verts[i].vy * scaleY + lift);
	}

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_FRAME — D-pad Up/Down tunes the bounce speed. The step moves every
 * frame the pad is held (clamped), and is saved + reported once, on release. */
static int yarisBounceOnFrame(void* userdata, void* args)
{
	int dpad = Pads[0].direct;
	int dir = 0;

	(void)userdata;
	(void)args;

	if (dpad & MPAD_D_UP)
		dir = 1;
	else if (dpad & MPAD_D_DOWN)
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

		jer_config_set_int("yarisbounce", "phase_step", sPhaseStep);
		jer_log("[yarisbounce] bounce speed: phase step -> %d (%.2f cycles/sec)\n",
			sPhaseStep, (float)sPhaseStep * 30.0f / 4096.0f);
	}

	return JER_RESULT_CONTINUE;
}

#if YARIS_DEBUG_HUD
/* JER_EVENT_DRAW_OVERLAY — the live phase/scale readout (a port of the old
 * UpdateBounceDisplay() HUD, now a module-owned line via jer_hud). */
static int yarisBounceOnDrawOverlay(void* userdata, void* args)
{
	char buf[80];

	(void)userdata;
	(void)args;

	sprintf(buf, "Yaris bounce: %.2f  X:%.2f Y:%.2f  step:%d", sLastBounce, sLastScaleX, sLastScaleY, sPhaseStep);
	jer_hud_message_replace(buf, 2);

	return JER_RESULT_CONTINUE;
}
#endif

/*
 * JERICHO module entry — the generated registry calls this as
 * jer_module_yarisbounce_entry() when the module is enabled.
 */
JER_MODULE_ENTRY(jer_module_yarisbounce_entry)(JERICHO_CONTEXT* ctx)
{
	/* pick up the bounce speed the player last tuned (D-pad), else the default */
	sPhaseStep = jer_config_get_int("yarisbounce", "phase_step", YARIS_BOUNCE_PHASE_STEP);

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
