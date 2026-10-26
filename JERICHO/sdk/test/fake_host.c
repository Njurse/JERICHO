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

static const JER_HOST_API gApi =
{
	sizeof(JER_HOST_API), JER_HOST_API_VERSION, JER_HOST_ABI_V1,
	&gCore, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

const JER_HOST_API* jer_host_api(void)
{
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
