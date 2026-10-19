/*
 * testmod.c — the mod store's own fixture addon.
 *
 * Not a mod anybody plays. It exists so the store's install path can be
 * proven end to end on a real addon: package it with
 * test/make_test_repo.sh, install it from the store, and check the log for
 * this module's boot line. SDK-only (jericho.h and nothing else), exactly
 * like MODS/example, so it loads under any build flavour.
 */
#include "jericho.h"

typedef struct TESTMOD_STATE
{
	JERICHO_CONTEXT* ctx;
	int frames;
} TESTMOD_STATE;

static int testmodOnBoot(void* userdata, void* args)
{
	TESTMOD_STATE* st = (TESTMOD_STATE*)userdata;

	(void)args;

	st->ctx->jer_log(st->ctx, "[testmod] installed by the mod store and booted\n");
	return JER_RESULT_CONTINUE;
}

static int testmodOnFrame(void* userdata, void* args)
{
	TESTMOD_STATE* st = (TESTMOD_STATE*)userdata;

	(void)args;

	/* one line ever, so a long run's log stays readable */
	if (st->frames++ == 0)
		st->ctx->jer_log(st->ctx, "[testmod] first frame after install\n");

	return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_testmod_entry)(JERICHO_CONTEXT* ctx)
{
	static TESTMOD_STATE state;

	state.ctx = ctx;
	state.frames = 0;

	ctx->jer_register_module(ctx,
		"testmod",
		"Test Mod",
		"0.1.0",
		"JERICHO",
		"Mod-store fixture: installs from a repo, logs at boot, does nothing else.",
		"",
		JERICHO_SDK_VERSION);

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, testmodOnBoot, &state, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, testmodOnFrame, &state, 0);

	ctx->jer_log(ctx, "[testmod] registered (SDK v%d)\n", ctx->sdkVersion);
}
