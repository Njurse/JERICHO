/*
 * test_nomods.c -- "a player can disable every mod and still play the original
 * game": what the runtime guarantees for that, tested without the game.
 *
 * Links against the runtime library a game build produces (like
 * test_dispatch.c). Its registry carries the core suite and one optional module
 * whose entry would make itself felt everywhere - a hook on every event, an
 * engine override slot, a pause menu - so any code of it that runs shows. Two
 * runs, one per mode (each a fresh process, the way the game boots):
 *
 *   nomods        -nomods (jer_disable_all_modules): no module entry runs at
 *                 all, every module lists as disabled, no hook, no override,
 *                 no pause menu - the engine runs its own code only;
 *   optional-off  every optional mod switched off in modlist.ini (what a player
 *                 can do from the Mods menu; the core suite is mandatory): the
 *                 optional module's entry never runs and leaves nothing behind.
 */
#include "jericho.h"
#include "jer_pause_menu.h"

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
int jer_notify(const char* s, int priority, int seconds) { (void)s; (void)priority; (void)seconds; return 0; }
void jer_text_draw_at(int x, int y, const char* s) { (void)x; (void)y; (void)s; }
int jer_text_width(const char* s) { (void)s; return 0; }
void PsyX_GetPSXWidescreenMappedViewport(void* r) { (void)r; }

/* ---- the registry ---- */
static int gCoreRan, gLoudRan, gLoudHookRan;

#define CORE_STANDIN(id) \
	static void jer_module_##id##_standin(JERICHO_CONTEXT* ctx) \
	{ gCoreRan++; ctx->jer_register_module(ctx, #id, #id, "1.0.0", "tests", "core stand-in", "", JERICHO_SDK_VERSION); }
CORE_STANDIN(mp)
CORE_STANDIN(carhacks)
CORE_STANDIN(bake)
CORE_STANDIN(pedhacks)

static int LoudHook(void* ud, void* args)
{
	(void)ud; (void)args;
	gLoudHookRan++;
	return JER_RESULT_CONTINUE;
}

static void LoudStep(void) {}

static const JER_PAUSE_MENU_ITEM kLoudItems[] = {
	{ "Loud row", NULL, NULL, NULL },
};

static JER_PAUSE_MENU gLoudMenu;

/* An optional module that makes itself felt everywhere it can. */
static void jer_module_loud_entry(JERICHO_CONTEXT* ctx)
{
	int ev;

	gLoudRan++;
	ctx->jer_register_module(ctx, "loud", "Loud", "1.0.0", "tests", "touches everything", "",
		JERICHO_SDK_VERSION);

	for (ev = 0; ev < 64; ev++)
		ctx->jer_register_hook(ctx, ev, LoudHook, NULL, 0);

	jer_override(JER_OVERRIDE_SLOT_STEP_SIM, (void*)LoudStep);

	memset(&gLoudMenu, 0, sizeof(gLoudMenu));
	gLoudMenu.title = "Loud";
	gLoudMenu.items = kLoudItems;
	gLoudMenu.item_count = 1;
	gLoudMenu.id = "loud.menu";
	jer_pause_menu_register(&gLoudMenu);
}

const JER_REGISTRY_ENTRY jer_registry_modules[] = {
	{ "mp", jer_module_mp_standin, 1 },
	{ "carhacks", jer_module_carhacks_standin, 1 },
	{ "bake", jer_module_bake_standin, 1 },
	{ "pedhacks", jer_module_pedhacks_standin, 1 },
	{ "loud", jer_module_loud_entry, 1 },		/* default ON: only modlist/-nomods turns it off */
};
const int jer_registry_module_count = 5;

static char gLog[64 * 1024];

static void TestLogger(const char* msg)
{
	size_t n = strlen(gLog);

	if (n + strlen(msg) + 1 < sizeof(gLog))
		strcat(gLog, msg);
}

static int gFailures;

#define CHECK(cond, ...) do { if (!(cond)) { gFailures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
	else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

static int Listed(const char* id, int* enabled)
{
	JER_MODULE_INFO info[64];
	int i, n = jer_module_list(info, 64);

	for (i = 0; i < n; i++)
	{
		if (info[i].id != NULL && strcmp(info[i].id, id) == 0)
		{
			*enabled = info[i].enabled;
			return 1;
		}
	}

	return 0;
}

int main(int argc, char** argv)
{
	const char* root = argc > 1 ? argv[1] : ".";
	const char* mode = argc > 2 ? argv[2] : "nomods";
	char path[600];
	int ev, slot, en = -1;
	FILE* f;

	jer_set_logger(TestLogger);
	snprintf(path, sizeof(path), "%s/CONFIG/modlist.ini", root);
	remove(path);

	if (strcmp(mode, "nomods") == 0)
	{
		jer_disable_all_modules();
		jer_init(root);

		CHECK(jer_modules_disabled(), "-nomods is in force");
		CHECK(gCoreRan == 0 && gLoudRan == 0, "no module entry ran (core %d, optional %d)", gCoreRan, gLoudRan);
		CHECK(!jer_module_running("carhacks") && !jer_module_running("mp"), "no core module runs");
		CHECK(!jer_module_running("loud"), "the optional module does not run");
		/* jer_module_list is the Mods menu's view - what modlist.ini says - and
		 * -nomods does not rewrite the player's file: */
		CHECK(Listed("loud", &en) && en == 1, "the Mods menu still shows the saved choice");
	}
	else
	{
		f = fopen(path, "wb");
		if (f != NULL)
		{
			fputs("loud = 0\n", f);		/* the Mods menu's "off" */
			fclose(f);
		}

		jer_init(root);

		CHECK(gCoreRan == 4, "the core suite still runs (%d of 4)", gCoreRan);
		CHECK(gLoudRan == 0, "the switched-off module's entry never ran");
		CHECK(Listed("loud", &en) && en == 0, "it lists as disabled");
		CHECK(!jer_module_running("loud") && jer_module_running("carhacks"),
			"it does not run; the core does");
	}

	for (slot = 0; slot < JER_OVERRIDE_SLOT_COUNT; slot++)
		CHECK(jer_get_override(slot) == NULL, "override slot %d is the engine's own", slot);

	CHECK(jer_pause_menu_count() == 0, "no pause menu was added (%d)", jer_pause_menu_count());
	CHECK(strstr(gLog, "hook inventory (0 handler(s))") != NULL, "no hook is registered");

	for (ev = 0; ev < 64; ev++)
		jer_fire(ev, NULL);
	CHECK(gLoudHookRan == 0, "firing every event runs no module code (%d)", gLoudHookRan);

	if (gFailures)
		printf("--- runtime log ---\n%s", gLog);

	printf(gFailures ? "test_nomods (%s): %d FAILED\n" : "test_nomods (%s): all passed\n", mode, gFailures);
	return gFailures ? 1 : 0;
}
