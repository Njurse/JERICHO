/*
 * jer_compile.c — host-side build helpers.
 *
 * Two builds live here:
 *
 *   jer_compile_mods()      the fast one. Compiles every runtime "dll" addon
 *                           into a loadable DLL (JERICHO/build_mods.bat). The
 *                           game exe is never touched, so this is immediate.
 *
 *   jer_build_begin/finish  the slow one. Rebuilds the game exe with the DEEP
 *                           mods (premake + MSBuild, one step per module), so
 *                           the player sees "Compiling JERICHO addons..." /
 *                           "<name> [i/n]" on a presentation screen instead of
 *                           a frozen window. Deep mods are compiled INTO the
 *                           exe, so this cannot happen while the exe runs: the
 *                           build is deferred to the next boot, and the running
 *                           exe is moved aside (Windows locks a running image)
 *                           so the new one can land under the normal name.
 *
 * Windows only: this is where the WinAPI lives so game files never see
 * windows.h (the frontend defines its own LoadImage and would collide).
 */
#include <stdio.h>
#include <string.h>

#include "jericho.h"
#include "jer_config.h"
#include "jer_screen.h"

/* ------------------------------------------------------------------ */
/* The pending-build marker (plain file, so it survives a restart)     */
/* ------------------------------------------------------------------ */

#define JER_BUILD_MARKER "needs-build"
#define JER_BUILD_LOG "build.log"

static void jerBuildPath(char* out, int max, const char* leaf)
{
	const char* root = jer_root_dir();

	snprintf(out, max, "%s/%s/%s",
		(root != NULL && root[0] != 0) ? root : "JERICHO", JER_CONFIG_PATH, leaf);
}

int jer_build_pending(void)
{
	char path[640];
	FILE* f;

	jerBuildPath(path, sizeof(path), JER_BUILD_MARKER);
	f = fopen(path, "rb");

	if (f == NULL)
		return 0;

	fclose(f);
	return 1;
}

void jer_build_mark_pending(void)
{
	char path[640];
	FILE* f;

	jerBuildPath(path, sizeof(path), JER_BUILD_MARKER);
	f = fopen(path, "wb");

	if (f == NULL)
		return;

	/* The marker is a request, not data: the next boot compiles the deep
	 * modules and deletes this file. */
	fprintf(f, "A deep-mod rebuild was requested (Options -> JERICHO -> Compile Mods).\n"
	           "The next boot compiles the deep modules under JERICHO/MODS and deletes\n"
	           "this file.\n");
	fclose(f);
}

void jer_build_clear_pending(void)
{
	char path[640];

	jerBuildPath(path, sizeof(path), JER_BUILD_MARKER);
	remove(path);
}

/* ------------------------------------------------------------------ */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define JER_BUILD_MAX_MODS 32
#define JER_BUILD_CMDLINE 2048

/* Steps the build walks through, one child process each. */
enum { JER_STEP_PREMAKE = 0, JER_STEP_FIRST_MOD = 1 };

static struct
{
	int attempted;			/* jer_build_begin() ran */
	int step;				/* 0 premake, 1..count mods, count+1 exe */
	int count;				/* number of deep mods */
	int failed;				/* 1 = a step failed */
	int done;				/* 1 = the whole build finished */
	int renamed;			/* the running exe was moved aside */

	JER_DEEP_MOD mods[JER_BUILD_MAX_MODS];

	char srcDir[MAX_PATH];	/* the src_rebuild tree (premake5.lua lives here) */
	char config[64];		/* e.g. Release_dev (BUILD_CONFIGURATION_STRING) */
	char batPath[MAX_PATH];	/* JERICHO/build_game.bat */
	char logPath[640];		/* JERICHO/CONFIG/build.log */
	char exePath[MAX_PATH];	/* the running exe */
	char exeOld[MAX_PATH];	/* ...moved here while it is relinked */

	PROCESS_INFORMATION pi;
	int running;
} gBuild;

static int jerBuildFileExists(const char* path)
{
	FILE* f = fopen(path, "rb");

	if (f == NULL)
		return 0;

	fclose(f);
	return 1;
}

/* Walk up from the running exe looking for the premake tree (src_rebuild): the
 * exe sits in <tree>/bin/<config>/, so this is three levels up in the dev
 * tree. Returns 0 when the game is running from a tree that has no sources
 * (a shipped install), which simply means there is nothing to compile. */
static int jerBuildFindSrcDir(char* out, int max)
{
	char dir[MAX_PATH];
	char* slash;
	int depth;

	if (GetModuleFileNameA(NULL, dir, sizeof(dir)) == 0)
		return 0;

	slash = strrchr(dir, '\\');

	if (slash == NULL)
		return 0;

	*slash = 0;		/* <...>/bin/<config> */

	for (depth = 0; depth < 6; depth++)
	{
		char probe[MAX_PATH];

		snprintf(probe, sizeof(probe), "%s\\premake5.lua", dir);

		if (jerBuildFileExists(probe))
		{
			snprintf(out, max, "%s", dir);
			return 1;
		}

		slash = strrchr(dir, '\\');

		if (slash == NULL || slash == dir + 2)	/* "C:" — stop at the root */
			break;

		*slash = 0;
	}

	return 0;
}

/* The build script: next to the exe when the game ships with one, otherwise
 * in the repo (JERICHO/ next to src_rebuild/). */
static int jerBuildFindBat(char* out, int max)
{
	char dir[MAX_PATH];
	char* slash;

	/* 1) the runtime JERICHO next to the exe */
	if (GetModuleFileNameA(NULL, dir, sizeof(dir)) != 0)
	{
		slash = strrchr(dir, '\\');

		if (slash != NULL)
		{
			*slash = 0;
			snprintf(out, max, "%s\\JERICHO\\build_game.bat", dir);

			if (jerBuildFileExists(out))
				return 1;
		}
	}

	/* 2) the repo: <srcDir>/../JERICHO/build_game.bat */
	{
		char repo[MAX_PATH];
		int i;

		snprintf(repo, sizeof(repo), "%s", gBuild.srcDir);
		slash = strrchr(repo, '\\');

		if (slash == NULL)
			return 0;

		*slash = 0;
		snprintf(out, max, "%s\\JERICHO\\build_game.bat", repo);

		/* srcDir is <repo>/src_rebuild */
		(void)i;
		return jerBuildFileExists(out);
	}
}

static void jerBuildBody(void* ud, char* out, int max)
{
	(void)ud;

	if (!gBuild.attempted)
	{
		snprintf(out, max, "Preparing...");
		return;
	}

	if (gBuild.failed)
	{
		snprintf(out, max, "Build failed - see JERICHO\\%s\\%s", JER_CONFIG_PATH, JER_BUILD_LOG);
		return;
	}

	if (gBuild.step <= 0)
		snprintf(out, max, "Scanning modules... [0/%d]", gBuild.count);
	else if (gBuild.step <= gBuild.count)
		snprintf(out, max, "%s [%d/%d]", gBuild.mods[gBuild.step - 1].name, gBuild.step, gBuild.count);
	else if (gBuild.step == gBuild.count + 1)
		snprintf(out, max, "Linking the game [%d/%d]", gBuild.count, gBuild.count);
	else
		snprintf(out, max, "Done");
}

/* Launch one step of JERICHO/build_game.bat, appending its output to
 * CONFIG/build.log. One child per step is what makes "<name> [i/n]" exact:
 * no MSBuild output parsing, we simply know which step is running. */
static int jerBuildSpawnStep(void)
{
	char cmdline[JER_BUILD_CMDLINE];
	STARTUPINFOA si;
	SECURITY_ATTRIBUTES sa;
	HANDLE hLog;
	HANDLE hNul = INVALID_HANDLE_VALUE;
	DWORD flags;

	if (gBuild.step == gBuild.count + 1 && !gBuild.renamed)
	{
		/* Windows locks the running image, so the linker cannot overwrite it.
		 * Move it aside; the fresh exe then lands under the normal name and is
		 * what runs next time. The stale .old is deleted on the next boot. */
		DeleteFileA(gBuild.exeOld);

		if (MoveFileA(gBuild.exePath, gBuild.exeOld))
			gBuild.renamed = 1;
		else
			jer_error("JERICHO: could not move the running exe aside - the build will fail");
	}

	if (gBuild.step <= 0)
		snprintf(cmdline, sizeof(cmdline), "cmd.exe /c \"\"%s\" \"%s\" \"%s\" premake\"",
			gBuild.batPath, gBuild.srcDir, gBuild.config);
	else if (gBuild.step <= gBuild.count)
		snprintf(cmdline, sizeof(cmdline), "cmd.exe /c \"\"%s\" \"%s\" \"%s\" mod %s\"",
			gBuild.batPath, gBuild.srcDir, gBuild.config, gBuild.mods[gBuild.step - 1].id);
	else
		snprintf(cmdline, sizeof(cmdline), "cmd.exe /c \"\"%s\" \"%s\" \"%s\" exe\"",
			gBuild.batPath, gBuild.srcDir, gBuild.config);

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	hLog = CreateFileA(gBuild.logPath, FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	if (hLog != INVALID_HANDLE_VALUE)
	{
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = hLog;
		si.hStdError = hLog;

		/* cmd.exe must not inherit the game's stdin (or none at all): it can
		 * block on it and then the step would never finish. Give it NUL. */
		hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			&sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		si.hStdInput = hNul;
	}

	memset(&gBuild.pi, 0, sizeof(gBuild.pi));

	flags = CREATE_NO_WINDOW;

	if (!CreateProcessA(NULL, cmdline, NULL, NULL, hLog != INVALID_HANDLE_VALUE, flags,
		NULL, NULL, &si, &gBuild.pi))
	{
		if (hLog != INVALID_HANDLE_VALUE)
			CloseHandle(hLog);

		jer_log("[build] cannot run step %d (err %lu)\n", gBuild.step, GetLastError());
		return 0;
	}

	if (hLog != INVALID_HANDLE_VALUE)
		CloseHandle(hLog);

	if (hNul != INVALID_HANDLE_VALUE)
		CloseHandle(hNul);

	gBuild.running = 1;
	return 1;
}

/* 1 = the child finished (code set), 0 = still building */
static int jerBuildChildDone(int* code)
{
	DWORD rc = 1;

	if (!gBuild.running)
	{
		*code = -1;
		return 1;
	}

	if (WaitForSingleObject(gBuild.pi.hProcess, 0) != WAIT_OBJECT_0)
		return 0;

	if (!GetExitCodeProcess(gBuild.pi.hProcess, &rc))
		rc = 1;

	CloseHandle(gBuild.pi.hThread);
	CloseHandle(gBuild.pi.hProcess);
	gBuild.running = 0;
	*code = (int)rc;
	return 1;
}

/* 1 = the running exe was moved aside and a newer one has landed in its place,
 * i.e. the link succeeded even though the step reported a failure. */
static int jerBuildExeRebuilt(void)
{
	WIN32_FILE_ATTRIBUTE_DATA fresh, old;

	if (!gBuild.renamed)
		return 0;

	if (!GetFileAttributesExA(gBuild.exePath, GetFileExInfoStandard, &fresh))
		return 0;

	if (!GetFileAttributesExA(gBuild.exeOld, GetFileExInfoStandard, &old))
		return 0;

	return CompareFileTime(&fresh.ftLastWriteTime, &old.ftLastWriteTime) > 0;
}

/* One frame of the compile screen: advance the build by one step when the
 * current child finishes. Returns non-zero when the build is over. */
static int jerBuildTick(void* ud)
{
	int code = 0;

	(void)ud;

	if (!gBuild.attempted || gBuild.done || gBuild.failed)
		return 1;

	if (!gBuild.running)
	{
		if (gBuild.step > gBuild.count + 1)
		{
			gBuild.done = 1;
			return 1;
		}

		{
			int spawned = jerBuildSpawnStep();

			return spawned ? 0 : 1;
		}
	}

	if (!jerBuildChildDone(&code))
		return 0;

	if (code != 0)
	{
		/* The exe step also mirrors JERICHO/MODS next to the exe, which cannot
		 * work while this process has the module DLLs loaded. The link itself
		 * comes first: if the exe was rebuilt, the build did its job. */
		if (gBuild.step == gBuild.count + 1 && jerBuildExeRebuilt())
		{
			jer_log("[build] the exe was relinked (step exit %d was the MODS mirror, which is skipped while the game runs)\n", code);
			gBuild.step++;
			return 0;
		}

		jer_log("[build] step %d failed (exit %d) - see JERICHO/%s/%s\n", gBuild.step, code, JER_CONFIG_PATH, JER_BUILD_LOG);
		gBuild.failed = 1;
		return 1;
	}

	gBuild.step++;
	return 0;
}

static const JER_SCREEN jerBuildScreen =
{
	"jer-build",
	"Compiling JERICHO addons...",
	jerBuildBody,
	jerBuildTick,
	NULL,
	NULL,
	NULL
};

int jer_build_begin(void)
{
	char exePath[MAX_PATH];
	char exeOld[MAX_PATH];

	GetModuleFileNameA(NULL, exePath, sizeof(exePath));
	snprintf(exeOld, sizeof(exeOld), "%s.old", exePath);

	/* A previous boot-time build moved the then-running exe aside. It is
	 * nobody's now (this process runs from the fresh one), so drop it. This
	 * runs on every boot, whether or not a build is pending. */
	DeleteFileA(exeOld);

	if (!jer_build_pending())
		return 0;

	/* Consume the request first: a build that fails must not be retried on
	 * every boot (that would wedge startup). Press Compile Mods again. */
	jer_build_clear_pending();

	memset(&gBuild, 0, sizeof(gBuild));
	snprintf(gBuild.exePath, sizeof(gBuild.exePath), "%s", exePath);
	snprintf(gBuild.exeOld, sizeof(gBuild.exeOld), "%s", exeOld);
	jerBuildPath(gBuild.logPath, sizeof(gBuild.logPath), JER_BUILD_LOG);
	snprintf(gBuild.config, sizeof(gBuild.config), "%s", BUILD_CONFIGURATION_STRING);

	DeleteFileA(gBuild.logPath);

	jer_log("[build] ---- deep-mod build requested ----\n");

	if (!jerBuildFindSrcDir(gBuild.srcDir, sizeof(gBuild.srcDir)) ||
		!jerBuildFindBat(gBuild.batPath, sizeof(gBuild.batPath)))
	{
		jer_log("[build] no src_rebuild tree / JERICHO/build_game.bat found - cannot build here\n");
		gBuild.attempted = 1;
		gBuild.failed = 1;
		return 1;
	}

	gBuild.count = jer_deep_mod_list(gBuild.mods, JER_BUILD_MAX_MODS);
	jer_log("[build] tree %s, config %s, %d deep module(s)\n", gBuild.srcDir, gBuild.config, gBuild.count);

	if (gBuild.count == 0)
	{
		jer_log("[build] no deep modules installed; nothing to compile\n");
		gBuild.attempted = 1;
		gBuild.done = 1;
		return 0;		/* nothing to show */
	}

	gBuild.attempted = 1;
	gBuild.step = 0;

	jer_screen_register(&jerBuildScreen);
	jer_screen_show("jer-build");

	return 1;
}

void jer_build_finish(void)
{
	char leaf[128];
	FILE* f;

	if (!gBuild.attempted)
		return;

	if (gBuild.failed)
	{
		jer_log("[build] FAILED - see JERICHO/%s/%s\n", JER_CONFIG_PATH, JER_BUILD_LOG);
		jer_error("JERICHO: compiling the deep mods failed - see JERICHO\\%s\\%s", JER_CONFIG_PATH, JER_BUILD_LOG);
		return;
	}

	/* the freshly linked exe is on disk, but this process is still the old
	 * image: the new build runs on the next start */
	jer_log("[build] OK - the new exe is in place; restart to run the compiled mods\n");

	snprintf(leaf, sizeof(leaf), "JERICHO\\%s\\%s", JER_CONFIG_PATH, JER_BUILD_LOG);
	f = fopen(gBuild.logPath, "ab");

	if (f != NULL)
	{
		fprintf(f, "JERICHO: build finished successfully.\n");
		fclose(f);
	}

	jer_error("JERICHO: deep mods compiled - restart to run them");
}

#else
/* The deep build is a Windows/MSBuild flow. */
int jer_build_begin(void)
{
	return 0;
}

void jer_build_finish(void)
{
}
#endif

/* ------------------------------------------------------------------ */
/* Addons: compile every runtime "dll" module into a loadable DLL      */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

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
