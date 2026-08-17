/*
 * jer_compile.c — host-side helper: run the JERICHO module build script.
 *
 * The in-game "Compile Mods" action (Options -> JERICHO frontend) calls
 * this: it spawns JERICHO/build_mods.bat (hidden console), waits for the
 * build, and returns success. The build compiles every runtime "dll"
 * addon (JERICHO/MODS/<id>/mod.toml declares runtime = "dll") using the
 * game's own toolchain; the game exe is never rebuilt. After a successful
 * build the caller reloads the manager so fresh DLLs load immediately.
 *
 * Windows only: this is where the WinAPI lives so game files never see
 * windows.h (the frontend defines its own LoadImage and would collide).
 */
#include "jericho.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int jer_compile_mods(void)
{
	char cmdline[512];
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	/* CreateProcess cannot launch a .bat directly — go through cmd.exe.
	 * CREATE_NO_WINDOW keeps the console from flashing during the build.
	 * The game resolves JERICHO/ relative to its working directory (same
	 * convention as jer_init's root), so a relative path is correct. */
	snprintf(cmdline, sizeof(cmdline), "cmd.exe /c JERICHO\\build_mods.bat");

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));

	if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
		CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
	{
		WaitForSingleObject(pi.hProcess, INFINITE);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}

	return (code == 0) ? 1 : 0;
}
#else
int jer_compile_mods(void)
{
	return 0;	/* runtime loading is Windows/Linux-desktop only */
}
#endif
