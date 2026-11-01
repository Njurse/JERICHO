#define JERICHO_HOST_BUILD 1

#include "jericho.h"
#include "jer_host_api.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef void (*TEST_ENTRY)(JERICHO_CONTEXT*);

static int gLogged;

static void mockLog(const char* fmt, ...)
{
	(void)fmt;
	gLogged = 1;
}

static int mockFire(int event, void* args)
{
	(void)event;
	(void)args;
	return 0;
}

static void mockOverride(int slot, void* fn)
{
	(void)slot;
	(void)fn;
}

static const JER_HOST_CORE_API gCore =
{
	sizeof(JER_HOST_CORE_API), JER_HOST_API_VERSION,
	mockLog, mockFire, mockOverride
};

static int mockCarCount(void)
{
	return 0;
}

/* jerHostApiGet() refuses a table whose subtables are missing or smaller than the
 * addon expects, so every subtable must be published with its size (function
 * pointers left NULL except the ones test_addon calls). */
static JER_HOST_CONFIG_API gConfig;
static JER_HOST_CAR_API gCar;
static JER_HOST_PLAYER_API gPlayer;
static JER_HOST_CAMERA_API gCamera;
static JER_HOST_MODEL_API gModel;
static JER_HOST_WORLD_API gWorld;
static JER_HOST_GAME_API gGame;
static JER_HOST_UI_API gUi;
static JER_HOST_DEBUG_API gDebug;
static JER_HOST_CARDATA_API gCardata;
static JER_HOST_API gApi;

#define FAKE_SUBTABLE(var, type) \
	((var).size = (unsigned int)sizeof(type), (var).version = JER_HOST_API_VERSION, &(var))

const JER_HOST_API* jer_host_api(void)
{
	if (gApi.size == 0)
	{
		gCar.count = mockCarCount;
		gApi.version = JER_HOST_API_VERSION;
		gApi.abi = JER_HOST_ABI_V1;
		gApi.core = &gCore;
		gApi.config = FAKE_SUBTABLE(gConfig, JER_HOST_CONFIG_API);
		gApi.car = FAKE_SUBTABLE(gCar, JER_HOST_CAR_API);
		gApi.player = FAKE_SUBTABLE(gPlayer, JER_HOST_PLAYER_API);
		gApi.camera = FAKE_SUBTABLE(gCamera, JER_HOST_CAMERA_API);
		gApi.model = FAKE_SUBTABLE(gModel, JER_HOST_MODEL_API);
		gApi.world = FAKE_SUBTABLE(gWorld, JER_HOST_WORLD_API);
		gApi.game = FAKE_SUBTABLE(gGame, JER_HOST_GAME_API);
		gApi.ui = FAKE_SUBTABLE(gUi, JER_HOST_UI_API);
		gApi.debug = FAKE_SUBTABLE(gDebug, JER_HOST_DEBUG_API);
		gApi.cardata = FAKE_SUBTABLE(gCardata, JER_HOST_CARDATA_API);
		gApi.size = (unsigned int)sizeof(JER_HOST_API);
	}

	return &gApi;
}

int main(int argc, char** argv)
{
	HMODULE addon;
	TEST_ENTRY entry;
	JERICHO_CONTEXT ctx;

	if (argc < 2)
	{
		printf("usage: fake_host <addon.dll>\n");
		return 2;
	}

	addon = LoadLibraryA(argv[1]);
	if (addon == 0)
	{
		printf("FAIL: LoadLibrary(%s) error %lu\n", argv[1], (unsigned long)GetLastError());
		return 1;
	}

	entry = (TEST_ENTRY)(void*)GetProcAddress(addon, "jer_module_testaddon_entry");
	if (entry == 0)
	{
		printf("FAIL: entry symbol missing\n");
		return 1;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.sdkVersion = JERICHO_SDK_VERSION;
	entry(&ctx);

	printf("%s: resolver %s\n", gLogged ? "PASS" : "FAIL",
		gLogged ? "resolved and called" : "was not reached");
	return gLogged ? 0 : 1;
}
