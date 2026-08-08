/*
 * camera.c — JERICHO example: over-the-shoulder camera for Tanner on foot.
 *
 * The game's chase cam sits behind + above the ped at ~1500-2000 units. This
 * module pulls it in toward Tanner and shifts it to the left of his facing
 * (seen from behind), so we look over his shoulder. The camera still aims at
 * the look target ahead of him, so his view direction stays lined up with the
 * screen centre.
 *
 * The offset calculation (world x/z plane):
 *   facing  f = (RSIN(h), RCOS(h))     Driver 2's heading vector
 *   left    l = (RCOS(h), -RSIN(h))    perpendicular to f
 *   offset  = DIST * f  +  LATERAL * l
 * with DIST = 1000 (pull-in) and LATERAL = 450. Tanner's on-screen offset
 * from centre ~ atan(LATERAL / DIST) ~ 24 degrees — about a third of the
 * half-width — classic over-the-shoulder framing. Flip CAM_LEFT_SIGN for
 * the other shoulder.
 */
#include "jericho.h"
#include "jer_events.h"

#include "driver2.h"
#include "players.h"
#include "camera.h"
#include "dr2math.h"

#define CAM_DIST 900		/* pull the camera in toward Tanner */
#define CAM_LATERAL 400		/* shift it left of his facing */
#define CAM_LEFT_SIGN 1		/* -1 for the other shoulder */

static int CameraOnCameraEvent(void* userdata, void* args)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	PLAYER* lp = (PLAYER*)a->player;
	VECTOR* camPos = (VECTOR*)a->cameraPosition;
	int heading;

	(void)userdata;

	/* only when Tanner is on foot (in a car the chase cam is fine) */
	if (lp->playerCarId >= 0)
		return JER_RESULT_CONTINUE;

	/* use the BODY direction (lp->dir), not the head/look angle, so the
	 * offset rotates with Tanner's movement */
	heading = lp->dir;

	/* The chase cam sits at basePos + cameraDist*f with f = (RSIN, RCOS)
	 * of the direction, so pulling in is -DIST*f; the lateral shift is
	 * along the perpendicular l = (RCOS, -RSIN). */
	camPos->vx += -FIXEDH(RSIN(heading) * CAM_DIST)
		+ FIXEDH(RCOS(heading) * CAM_LATERAL * CAM_LEFT_SIGN);
	camPos->vz += -FIXEDH(RCOS(heading) * CAM_DIST)
		- FIXEDH(RSIN(heading) * CAM_LATERAL * CAM_LEFT_SIGN);

	return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_camera_entry)(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_module(ctx,
		"camera",			/* id */
		"Over-the-Shoulder Camera",	/* name */
		"0.1.0",			/* version */
		"DeepSeek",			/* author */
		"Example: pulls the on-foot chase camera in and left of Tanner's facing so we look over his shoulder.",	/* description */
		"",				/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, CameraOnCameraEvent, NULL, 0);

	ctx->jer_log(ctx, "[camera] over-the-shoulder active (dist %d, lateral %d)\n",
		CAM_DIST, CAM_LATERAL);
}
