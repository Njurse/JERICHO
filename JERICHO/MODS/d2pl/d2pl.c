/* d2pl.c — module entry for the on-foot ped addon.
 *
 * Runtime DLL addon: every symbol used here is the JERICHO SDK, an exported
 * jer_* helper, or the host API. See API_NOTES.md for the hooks that could
 * be collapsed and the API gaps that still force workarounds.
 */

#include "d2pl.h"

JER_MODULE_ENTRY(jer_module_d2pl_entry)(JERICHO_CONTEXT* ctx)
{
	D2plInit();
	D2plHoldtypeInit();
	D2plWeaponInit();

	ctx->jer_register_module(ctx,
		D2PL_MOD_ID,
		"On Foot",
		D2PL_VERSION,
		"JERICHO",
		"Camera-relative on-foot movement, holdtype aim pose, laser sight and three test weapons.",
		"",
		JERICHO_SDK_VERSION);

	/* look / camera */
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA_LOOK, D2plOnLook, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, D2plOnCamera, NULL, 0);

	/* movement */
	ctx->jer_register_hook(ctx, JER_EVENT_PED_INPUT, D2plOnPedInput, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_MOVE, D2plOnPedMove, NULL, 0);

	/* pose */
	ctx->jer_register_hook(ctx, JER_EVENT_PED_POSE, D2plOnPedPose, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_SKELETON, D2plOnPedSkeleton, NULL, 0);

	/* weapon tick + HUD */
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, D2plOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, D2plOnOverlay, NULL, 0);

	/* a new level means a new ped pointer */
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, D2plOnGameStart, NULL, 0);
}
