/*
 * test_joinmods.c -- a server's "you need these mods" offer, driven headless.
 *
 * Links against the runtime library the game build produces (like
 * test_dispatch.c) and runs jer_joinmods for real: the module system, the mod
 * store (no helper script here, so no repo can be fetched and nothing can be
 * downloaded), the prompt, and modlist.ini in a throwaway root. Run by
 * test/run_tests.sh with JERICHO_TEST_JOINMODS=nolaunch, which logs the relaunch
 * line instead of starting a second process. It checks:
 *
 *   1. an installed mod that is off: the player is asked, and "No" changes
 *      nothing (no modlist.ini written, no relaunch);
 *   2. the same offer answered "Keep Them On": the mod is enabled in
 *      modlist.ini and the relaunch line carries -join, -mod and -joinmods;
 *   3. a mod no repo offers, and a core mod the server does not run: no
 *      question, just a notice naming it;
 *   4. a -joinmods boot tells the player what is on for this session only;
 *   5. with a repo (run_tests.sh builds one on disk and sets
 *      JERICHO_TEST_MODSTORE_URL): a mod only the repo has is offered, really
 *      DOWNLOADED by the store's helper on "Download & Join", kept off in
 *      modlist.ini for later starts, and switched on in the relaunch line.
 *
 * Frames are the frontend's own (jer_modstore_frontend_tick), which is what
 * polls a running store job, so a fetch or a download completes as in the game.
 */
#include "jericho.h"
#include "jer_joinmods.h"
#include "jer_modstore.h"
#include "jer_prompt.h"

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
static int gQuit;
void jer_frontend_quit(void) { gQuit++; }
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
static char gNotice[512];
int jer_notify(const char* s, int priority, int seconds)
{
	(void)priority; (void)seconds;
	snprintf(gNotice, sizeof(gNotice), "%s", s != NULL ? s : "");
	return 1;
}
void jer_text_draw_at(int x, int y, const char* s) { (void)x; (void)y; (void)s; }
int jer_text_width(const char* s) { (void)s; return 0; }
void PsyX_GetPSXWidescreenMappedViewport(void* r) { (void)r; }

/* ---- a registry: the core suite, one optional mod that is off ---- */
#define STANDIN(id) \
	static void jer_module_##id##_standin(JERICHO_CONTEXT* ctx) \
	{ ctx->jer_register_module(ctx, #id, #id, "1.0.0", "tests", "stand-in", "", JERICHO_SDK_VERSION); }
STANDIN(mp)
STANDIN(carhacks)
STANDIN(bake)
STANDIN(pedhacks)
STANDIN(offmod)

const JER_REGISTRY_ENTRY jer_registry_modules[] = {
	{ "mp", jer_module_mp_standin, 1 },
	{ "carhacks", jer_module_carhacks_standin, 1 },
	{ "bake", jer_module_bake_standin, 1 },
	{ "pedhacks", jer_module_pedhacks_standin, 1 },
	{ "offmod", jer_module_offmod_standin, 0 },		/* installed, default off */
};
const int jer_registry_module_count = 5;

static char gLog[128 * 1024];

static void TestLogger(const char* msg)
{
	size_t n = strlen(gLog);

	if (n + strlen(msg) + 1 < sizeof(gLog))
		strcat(gLog, msg);
}

static int gFailures;

#define CHECK(cond, ...) do { if (!(cond)) { gFailures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
	else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

static JER_JOIN_MOD Mod(const char* id, const char* version)
{
	JER_JOIN_MOD m;

	memset(&m, 0, sizeof(m));
	snprintf(m.id, sizeof(m.id), "%s", id);
	snprintf(m.version, sizeof(m.version), "%s", version);
	return m;
}

void jer_host_sleep_ms(int ms);		/* the runtime's own (jer_host.c) */

/* Frontend frames until the flow waits on the player or is over (60 s at most:
 * a fetch or a download is a child process). */
static void Settle(void)
{
	int i;

	for (i = 0; i < 3000; i++)
	{
		jer_modstore_frontend_tick(0);

		if (jer_prompt_active() || !jer_joinmods_busy())
			return;

		jer_host_sleep_ms(20);
	}
}

static int FileExists(const char* root, const char* leaf)
{
	char path[600];
	FILE* f;

	snprintf(path, sizeof(path), "%s/%s", root, leaf);
	f = fopen(path, "rb");
	if (f == NULL)
		return 0;
	fclose(f);
	return 1;
}

static int ModlistSays(const char* root, const char* line)
{
	char path[600], buf[4096];
	FILE* f;
	size_t got;

	snprintf(path, sizeof(path), "%s/CONFIG/modlist.ini", root);
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;	/* no file */
	got = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[got] = 0;
	return strstr(buf, line) != NULL;
}

int main(int argc, char** argv)
{
	const char* root = argc > 1 ? argv[1] : ".";
	char* bootArgv[] = { (char*)"jericho", (char*)"-console", (char*)"-joinmods",
		(char*)"-mod", (char*)"offmod", (char*)"-nomod", (char*)"crossfire" };
	JER_JOIN_MOD need[1], core[3];
	static const char* const tooling[] = { "bake" };
	char modlist[600];

	jer_set_logger(TestLogger);
	snprintf(modlist, sizeof(modlist), "%s/CONFIG/modlist.ini", root);
	remove(modlist);

	/* 4 first: the boot note is shown on the first frontend frame */
	jer_joinmods_boot((int)(sizeof(bootArgv) / sizeof(bootArgv[0])), bootArgv);
	jer_force_module("offmod", 1);		/* this boot's -mod offmod: on now, saved off */
	jer_init(root);
	CHECK(jer_module_running("offmod"), "offmod runs this boot (forced)");
	jer_modstore_frontend_tick(0);
	CHECK(strstr(gNotice, "offmod on") != NULL && strstr(gNotice, "crossfire off") != NULL,
		"a -joinmods boot says what is on and off for this session (\"%s\")", gNotice);

	/* 1. installed but off: ask, and No changes nothing */
	need[0] = Mod("offmod", "1.0.0");
	CHECK(jer_joinmods_offer("192.168.1.20:1318", need, 1, tooling, 1, 0) == 1, "an offer is taken");
	CHECK(jer_joinmods_offer("192.168.1.20:1318", need, 1, tooling, 1, 0) == 0, "a second offer waits for the first");
	Settle();
	CHECK(jer_prompt_active(), "the player is asked");
	CHECK(jer_prompt_question() != NULL && strstr(jer_prompt_question(), "offmod") != NULL,
		"the question names the mod (\"%s\")", jer_prompt_question() ? jer_prompt_question() : "");
	CHECK(!strcmp(jer_prompt_yes_label(), "Restart & Join") && !strcmp(jer_prompt_third_label(), "Keep Them On"),
		"the answers are Restart & Join / No / Keep Them On");
	jer_prompt_tick(0, 0, 0, 1);		/* Circle: No */
	Settle();
	CHECK(!jer_joinmods_busy() && !jer_prompt_active(), "No ends the offer");
	CHECK(ModlistSays(root, "offmod") != 1, "No writes nothing to modlist.ini");
	CHECK(strstr(gLog, "would relaunch") == NULL && gQuit == 0, "No does not relaunch");

	/* 1b. this boot's own flags are what the server refused: the saved set fits */
	gLog[0] = 0;
	core[0] = Mod("mp", "");
	core[1] = Mod("carhacks", "");
	core[2] = Mod("pedhacks", "");
	CHECK(jer_joinmods_offer("192.168.1.30", core, 3, tooling, 1, JER_JOIN_EXACT_SET) == 1,
		"an exact-set offer for the saved set is taken");
	Settle();
	CHECK(jer_prompt_active() && strstr(jer_prompt_question(), "your own mod set") != NULL,
		"it offers a restart on the saved set (\"%s\")", jer_prompt_question() ? jer_prompt_question() : "");
	CHECK(jer_prompt_third_label()[0] == 0, "with two answers: there is nothing to keep");
	jer_prompt_tick(0, 0, 1, 0);		/* Cross: Restart & Join */
	Settle();
	CHECK(strstr(gLog, "would relaunch: ") != NULL && strstr(gLog, "-join 192.168.1.30 -joinmods") != NULL &&
		strstr(gLog, "-mod offmod") == NULL, "the relaunch drops this boot's -mod and joins");

	/* 2. Keep Them On: enabled for good, and the relaunch line is right */
	gLog[0] = 0;
	CHECK(jer_joinmods_offer("192.168.1.20:1318", need, 1, tooling, 1, 0) == 1, "the offer can be made again");
	Settle();
	CHECK(jer_prompt_active(), "asked again");
	jer_prompt_tick(0, 1, 0, 0);		/* right: No */
	jer_prompt_tick(0, 1, 0, 0);		/* right: Keep Them On */
	CHECK(jer_prompt_selected() == 2, "the third answer is highlighted (%d)", jer_prompt_selected());
	jer_prompt_tick(0, 0, 1, 0);		/* Cross */
	Settle();
	CHECK(ModlistSays(root, "offmod") == 1, "Keep Them On enables the mod in modlist.ini");
	CHECK(strstr(gLog, "would relaunch: ") != NULL, "the flow reaches the relaunch");
	CHECK(strstr(gLog, "-join 192.168.1.20:1318 -mod offmod -joinmods") != NULL,
		"the relaunch joins that server with that mod on");
	CHECK(strstr(gLog, " -console ") != NULL && strstr(gLog, "-nomod crossfire") == NULL,
		"it keeps -console and drops the boot's own session flags");
	CHECK(!jer_joinmods_busy(), "and the offer is over");

	/* 3. what cannot be fixed from here: a notice, no question */
	gNotice[0] = 0;
	need[0] = Mod("ghost", "1.0.0");
	CHECK(jer_joinmods_offer("10.0.0.5", need, 1, tooling, 1, 0) == 1, "an offer for an unknown mod is taken");
	Settle();
	CHECK(!jer_prompt_active() && !jer_joinmods_busy(), "nothing is asked");
	CHECK(strstr(gNotice, "none of your mod repos offers ghost") != NULL, "the notice names it (\"%s\")", gNotice);

	gNotice[0] = 0;
	CHECK(jer_joinmods_offer("10.0.0.5", core, 1, tooling, 1, JER_JOIN_EXACT_SET) == 1,
		"an exact set without a core mod is taken");
	Settle();
	CHECK(!jer_prompt_active() && strstr(gNotice, "cannot switch off") != NULL,
		"a core mod is never switched off (\"%s\")", gNotice);

	CHECK(jer_joinmods_offer("10.0.0.5;reboot", need, 1, tooling, 1, 0) == 0, "a bad address is refused");
	CHECK(jer_joinmods_offer("10.0.0.5", NULL, 0, NULL, 0, 0) == 0, "an empty offer is refused");
	CHECK(jer_joinmods_offer("10.0.0.5", need, 1, NULL, 1, 0) == 0, "an ignore count without a list is refused");

	/* 5. a mod only a repo has: asked, downloaded, kept off for later, joined */
	if (getenv("JERICHO_TEST_MODSTORE_URL") == NULL)
	{
		printf("skip: no JERICHO_TEST_MODSTORE_URL - run_tests.sh sets one for the download path\n");
	}
	else
	{
		gLog[0] = 0;
		need[0] = Mod("packmod", "1.0.0");
		CHECK(!FileExists(root, "MODS/packmod/mod.toml"), "packmod starts out not installed");
		CHECK(jer_joinmods_offer("10.1.1.1:1318", need, 1, tooling, 1, JER_JOIN_EXACT) == 1, "a download offer is taken");
		Settle();
		CHECK(jer_prompt_active() && jer_prompt_question() != NULL &&
			strstr(jer_prompt_question(), "Pack Mod") != NULL,
			"the repo was fetched and the question names its mod (\"%s\")",
			jer_prompt_question() ? jer_prompt_question() : "");
		CHECK(!strcmp(jer_prompt_yes_label(), "Download & Join"), "the first answer is Download & Join");
		jer_prompt_tick(0, 0, 1, 0);		/* Cross on the highlighted Yes */
		Settle();
		CHECK(FileExists(root, "MODS/packmod/mod.toml") && FileExists(root, "MODS/packmod/banks/README.txt"),
			"the store's helper downloaded and installed it");
		CHECK(ModlistSays(root, "packmod = 0") == 1 || ModlistSays(root, "packmod=0") == 1,
			"a download for one session stays off at later starts (its mod.toml says default-enabled)");
		CHECK(strstr(gLog, "-join 10.1.1.1:1318 -mod packmod -joinmods") != NULL,
			"the relaunch joins with it on");
		CHECK(!jer_joinmods_busy(), "and the offer is over");
	}

	if (gFailures)
		printf("--- runtime log ---\n%s", gLog);

	printf(gFailures ? "test_joinmods: %d FAILED\n" : "test_joinmods: all passed\n", gFailures);
	return gFailures ? 1 : 0;
}
