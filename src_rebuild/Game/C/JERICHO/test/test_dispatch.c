/*
 * test_dispatch.c -- the JERICHO hook dispatcher, tested without the game.
 *
 * Links against the runtime library the game build produces
 * (bin/<cfg>/libJERICHO_runtime.a) with a one-module registry of its own and
 * no-op stand-ins for the frontend/engine functions the runtime references.
 * Run by test/run_tests.sh. It checks:
 *
 *   1. a hook registered WHILE an event is being dispatched is not run in that
 *      dispatch, does not make another handler run twice, and runs from the
 *      next dispatch on (it used to be inserted in place, shifting the array
 *      under the loop);
 *   2. hooks past the handler table are dropped with a log line naming the
 *      module and event (they used to vanish silently), and dispatch still works.
 */
#include "jericho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stand-ins for what the runtime library references outside itself ---- */
int jer_build_is_dev(void) { return 0; }
void jer_console_log(const char* s) { (void)s; }
void* jer_frontend_current_menu(void) { return 0; }
void jer_frontend_dim_rect(int x, int y, int w, int h, int s) { (void)x; (void)y; (void)w; (void)h; (void)s; }
void* jer_frontend_find(const char* id) { (void)id; return 0; }
int jer_frontend_list_top(void) { return 0; }
void jer_frontend_open(const char* id) { (void)id; }
void jer_frontend_open_replace(const char* id) { (void)id; }
void jer_frontend_quit(void) {}
void jer_frontend_refresh(void) {}
void jer_frontend_refresh_at(int i) { (void)i; }
int jer_frontend_register_menu(void* m) { (void)m; return 0; }
void jer_frontend_replace(const char* id) { (void)id; }
void jer_frontend_reset(void) {}
int jer_frontend_row_rect(int i, int* x, int* y, int* w, int* h) { (void)i; (void)x; (void)y; (void)w; (void)h; return 0; }
int jer_frontend_selected(void) { return 0; }
int jer_mods_compile_available(void) { return 0; }
void jer_mods_open_credits(void) {}
void jer_mods_open_screen(void) {}
void jer_mods_request_compile(void) {}
void jer_notify(const char* s, int f) { (void)s; (void)f; }
void jer_text_draw_at(int x, int y, const char* s) { (void)x; (void)y; (void)s; }
int jer_text_width(const char* s) { (void)s; return 0; }
void PsyX_GetPSXWidescreenMappedViewport(void* r) { (void)r; }

/* ---- the test module ---- */
#define EV_DEFER	(JER_EVENT_MODULE_CUSTOM + 900)
#define EV_FILL		(JER_EVENT_MODULE_CUSTOM + 901)
#define FILL_HOOKS	300	/* more than the 256-entry table */

static JERICHO_CONTEXT* gTestCtx;
static int gPhase;		/* 1 = deferral test, 2 = overflow test */
static int gRanFirst, gRanLate, gRanFill;
static int gLateRegistered;
static char gLog[64 * 1024];

static void TestLogger(const char* msg)
{
	size_t n = strlen(gLog);

	if (n + strlen(msg) + 1 < sizeof(gLog))
		strcat(gLog, msg);
}

static int LateHook(void* ud, void* args)
{
	(void)ud; (void)args;
	gRanLate++;
	return JER_RESULT_CONTINUE;
}

/* Registers LateHook at a LOWER priority, i.e. in front of itself: in place,
 * that shifted this handler one slot up and the loop ran it again. */
static int FirstHook(void* ud, void* args)
{
	(void)ud; (void)args;
	gRanFirst++;

	if (!gLateRegistered)
	{
		gLateRegistered = 1;
		gTestCtx->jer_register_hook(gTestCtx, EV_DEFER, LateHook, NULL, -10);
	}

	return JER_RESULT_CONTINUE;
}

static int FillHook(void* ud, void* args)
{
	(void)ud; (void)args;
	gRanFill++;
	return JER_RESULT_CONTINUE;
}

static void jer_module_dtest_entry(JERICHO_CONTEXT* ctx)
{
	int i;

	gTestCtx = ctx;
	ctx->jer_register_module(ctx, "dtest", "Dispatch test", "1.0.0", "tests",
		"dispatcher test", "", JERICHO_SDK_VERSION);

	if (gPhase == 1)
	{
		ctx->jer_register_hook(ctx, EV_DEFER, FirstHook, NULL, 0);
	}
	else
	{
		for (i = 0; i < FILL_HOOKS; i++)
			ctx->jer_register_hook(ctx, EV_FILL, FillHook, NULL, i);
	}
}

/* The runtime refuses optional modules unless the core suite is present, so the
 * registry carries no-op stand-ins under the core ids. */
#define CORE_STANDIN(id) \
	static void jer_module_##id##_standin(JERICHO_CONTEXT* ctx) \
	{ ctx->jer_register_module(ctx, #id, #id, "1.0.0", "tests", "core stand-in", "", JERICHO_SDK_VERSION); }
CORE_STANDIN(mp)
CORE_STANDIN(carhacks)
CORE_STANDIN(bake)
CORE_STANDIN(pedhacks)

const JER_REGISTRY_ENTRY jer_registry_modules[] = {
	{ "mp", jer_module_mp_standin, 1 },
	{ "carhacks", jer_module_carhacks_standin, 1 },
	{ "bake", jer_module_bake_standin, 1 },
	{ "pedhacks", jer_module_pedhacks_standin, 1 },
	{ "dtest", jer_module_dtest_entry, 1 },
};
const int jer_registry_module_count = 5;

static int gFailures;

#define CHECK(cond, ...) do { if (!(cond)) { gFailures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
	else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv)
{
	const char* root = argc > 1 ? argv[1] : ".";

	jer_set_logger(TestLogger);

	/* 1. registration during dispatch */
	gPhase = 1;
	jer_init(root);

	jer_fire(EV_DEFER, NULL);
	CHECK(gRanFirst == 1, "a handler runs once even when it registers a hook mid-dispatch (ran %d)", gRanFirst);
	CHECK(gRanLate == 0, "a hook registered mid-dispatch does not run in that dispatch (ran %d)", gRanLate);

	jer_fire(EV_DEFER, NULL);
	CHECK(gRanLate == 1, "the queued hook runs from the next dispatch on (ran %d)", gRanLate);
	CHECK(gRanFirst == 2, "the original handler still runs once per dispatch (total %d)", gRanFirst);

	/* 2. table overflow */
	gPhase = 2;
	gLog[0] = 0;
	jer_manager_reload(root);

	jer_fire(EV_FILL, NULL);
	CHECK(gRanFill > 0 && gRanFill < FILL_HOOKS, "dispatch survives a full table (%d of %d hooks ran)", gRanFill, FILL_HOOKS);
	CHECK(strstr(gLog, "handler table full") != NULL && strstr(gLog, "'dtest'") != NULL,
		"the dropped hook is logged with the module id");
	CHECK(strstr(strstr(gLog, "handler table full") ? strstr(gLog, "handler table full") + 1 : gLog,
		"handler table full") == NULL, "the overflow is logged once per pass, not per hook");

	if (gFailures)
		printf("--- runtime log (last pass) ---\n%s", gLog);

	printf(gFailures ? "test_dispatch: %d FAILED\n" : "test_dispatch: all passed\n", gFailures);
	return gFailures ? 1 : 0;
}
