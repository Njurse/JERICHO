/*
 * example.c — the JERICHO sample module.
 *
 * A minimal, self-contained module that only uses the public SDK
 * (jericho.h — no game headers), proving the pipeline end to end:
 * premake links it in, the generated registry lists it, the runtime
 * activates it at boot, and its hooks fire every frame.
 */
#include "jericho.h"

#include <string.h>

/* one custom event defined by this module */
#define EXAMPLE_EVENT_TICK (JER_EVENT_MODULE_CUSTOM + 1)

typedef struct EXAMPLE_STATE
{
	JERICHO_CONTEXT* ctx;	/* saved so the module can fire custom events */
	int frames;
	int ticks;
} EXAMPLE_STATE;

static int exampleOnBoot(void* userdata, void* args)
{
	EXAMPLE_STATE* st = (EXAMPLE_STATE*)userdata;

	(void)args;

	st->ctx->jer_log(st->ctx, "[example] booted! (frames will tick in the frame hook)\n");
	return JER_RESULT_CONTINUE;
}

static int exampleOnFrame(void* userdata, void* args)
{
	EXAMPLE_STATE* st = (EXAMPLE_STATE*)userdata;

	(void)args;

	st->frames++;

	/* fire our custom event once per 60 frames to prove custom ids work */
	if ((st->frames % 60) == 0)
	{
		int result = st->ctx->jer_fire(st->ctx, EXAMPLE_EVENT_TICK, NULL);

		if (result == JER_RESULT_STOP)
			st->ticks++;

		st->ctx->jer_log(st->ctx, "[example] fired custom event %d (frame %d)\n", EXAMPLE_EVENT_TICK, st->frames);
	}

	return JER_RESULT_CONTINUE;
}

/*
 * Module entry point — the generated registry calls this as
 * jer_module_example_entry() when the module is enabled.
 */
JER_MODULE_ENTRY(jer_module_example_entry)(JERICHO_CONTEXT* ctx)
{
	static EXAMPLE_STATE state;

	memset(&state, 0, sizeof(state));
	state.ctx = ctx;

	/* metadata: id must match the build id ("example") */
	ctx->jer_register_module(ctx,
		"example",			/* id */
		"Example Module",	/* name */
		"0.1.0",			/* version */
		"DeepSeek",			/* author */
		"JERICHO smoke-test: logs at boot and fires a custom event every 60 frames.",	/* description */
		"",					/* dependencies */
		JERICHO_SDK_VERSION);	/* SDK this module was built against */

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, exampleOnBoot, &state, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, exampleOnFrame, &state, 0);

	ctx->jer_log(ctx, "[example] registered (SDK v%d)\n", ctx->sdkVersion);
}
