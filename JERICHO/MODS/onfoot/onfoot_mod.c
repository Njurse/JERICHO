/* onfoot_mod.c — module entry for the on-foot ped addon.
 *
 * Runtime DLL addon: every symbol used here is the JERICHO SDK, an exported
 * jer_* helper, or the host API. See API_NOTES.md for the hooks that could
 * be collapsed and the API gaps that still force workarounds.
 */

#include "onfoot.h"

JER_MODULE_ENTRY(jer_module_onfoot_entry)(JERICHO_CONTEXT* ctx)
{
	OnfootInit();
	OnfootWeaponInit();

	ctx->jer_register_module(ctx,
		ONFOOT_MOD_ID,
		"On Foot",
		ONFOOT_VERSION,
		"JERICHO",
		"Camera-relative on-foot movement, aim arm pose, laser sight and a debug pistol.",
		"",
		JERICHO_SDK_VERSION);

	/* look / camera */
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA_LOOK, OnfootOnLook, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, OnfootOnCamera, NULL, 0);

	/* movement */
	ctx->jer_register_hook(ctx, JER_EVENT_PED_INPUT, OnfootOnPedInput, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_MOVE, OnfootOnPedMove, NULL, 0);

	/* pose */
	ctx->jer_register_hook(ctx, JER_EVENT_PED_POSE, OnfootOnPedPose, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_SKELETON, OnfootOnPedSkeleton, NULL, 0);

	/* weapon tick + HUD */
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, OnfootOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, OnfootOnOverlay, NULL, 0);

	/* a new level means a new ped pointer */
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, OnfootOnGameStart, NULL, 0);
}
