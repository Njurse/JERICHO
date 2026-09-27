/*
 * jer_compile.c — host-side build helpers.
 *
 * Two builds live here:
 *
 *   jer_compile_mods()      the fast one. Compiles every runtime "dll" addon
 *                           into a loadable binary (JERICHO/build_mods.<ext>).
 *                           The game exe is never touched, so this is quick.
 *
 *   jer_build_begin/finish  the slow one. Rebuilds the game exe with the DEEP
 *                           mods (one build step per module), so the player
 *                           sees "Compiling JERICHO addons..." / "<name>
 *                           [i/n]" on a presentation screen instead of a
 *                           frozen window. Deep mods are compiled INTO the
 *                           exe, so this cannot happen while it runs: the
 *                           build is deferred to the next boot. On Windows the
 *                           running exe is moved aside first (the OS locks a
 *                           running image); on POSIX the linker relinks it in
 *                           place. Either way the *new* build only runs on the
 *                           next start.
 *
 * Platform-neutral: every OS operation goes through jer_host (jer_host.h), so
 * this file has no platform branches and includes no host SDK header (which
 * matters — the frontend defines its own LoadImage and would collide with
 * <windows.h>). Each step runs the same shim
 * (JERICHO/build_game.bat on Windows, build_game.sh elsewhere), which forwards
 * to the shared cross-platform driver JERICHO/build.py.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "jericho.h"
#include "jer_config.h"
#include "jer_screen.h"
#include "jer_host.h"

/* ------------------------------------------------------------------ */
/* The pending-build marker (plain file, so it survives a restart)     */
/* ------------------------------------------------------------------ */

#define JER_BUILD_MARKER "needs-build"
#define JER_BUILD_LOG "build.log"

#define JER_BUILD_MAX_MODS 32
#define JER_BUILD_CMDLINE 2048
#define JER_BUILD_PATH 1024

static void jerBuildPath(char* out, int max, const char* leaf)
{
	const char* root = jer_root_dir();

	snprintf(out, (size_t)max, "%s/%s/%s",
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
/* The stepped deep build                                             */
/* ------------------------------------------------------------------ */

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

	char srcDir[JER_BUILD_PATH];	/* the src_rebuild tree (premake5.lua lives here) */
	char config[64];				/* e.g. Release_dev (BUILD_CONFIGURATION_STRING) */
	char shimPath[JER_BUILD_PATH];	/* JERICHO/build_game.bat | .sh */
	char logPath[640];				/* JERICHO/CONFIG/build.log */
	char exePath[JER_BUILD_PATH];	/* the running exe */
	char exeOld[JER_BUILD_PATH];	/* ...moved here while it is relinked */

	JER_HOST_PROC* proc;			/* the current step's child, or NULL */
} gBuild;

/* the last '/' or '\\' in s, or NULL (handles both separators so a Windows
 * exe path and a POSIX one are treated the same) */
static const char* jerLastSep(const char* s)
{
	const char* a = strrchr(s, '/');
	const char* b = strrchr(s, '\\');

	if (a == NULL)
		return b;
	if (b == NULL)
		return a;

	return (a > b) ? a : b;
}

/* Walk up from the running exe looking for the premake tree (src_rebuild): the
 * exe sits in <tree>/bin/<config>/, so this is three levels up in the dev
 * tree. Returns 0 when the game runs from a tree with no sources (a shipped
 * install), which simply means there is nothing to compile. */
static int jerBuildFindSrcDir(char* out, int max)
{
	char dir[JER_BUILD_PATH];
	char* slash;
	int depth;

	if (!jer_host_exe_path(dir, sizeof(dir)))
		return 0;

	slash = (char*)jerLastSep(dir);

	if (slash == NULL)
		return 0;

	*slash = 0;		/* <...>/bin/<config> */

	for (depth = 0; depth < 6; depth++)
	{
		char probe[JER_BUILD_PATH];

		snprintf(probe, sizeof(probe), "%s/premake5.lua", dir);

		if (jer_host_file_exists(probe))
		{
			snprintf(out, (size_t)max, "%s", dir);
			return 1;
		}

		slash = (char*)jerLastSep(dir);

		if (slash == NULL || slash == dir)	/* root */
			break;

		*slash = 0;
	}

	return 0;
}

/* The build shim: next to the exe when the game ships with one, otherwise in
 * the repo (JERICHO/ next to src_rebuild/). `base` is "build_game" or
 * "build_mods"; the extension comes from the host (.bat / .sh). */
static int jerBuildFindShim(const char* srcDir, const char* base, char* out, int max)
{
	const char* ext = jer_host_script_ext();
	char dir[JER_BUILD_PATH];
	char* slash;

	/* 1) the runtime JERICHO next to the exe */
	if (jer_host_exe_path(dir, sizeof(dir)))
	{
		slash = (char*)jerLastSep(dir);

		if (slash != NULL)
		{
			*slash = 0;
			snprintf(out, (size_t)max, "%s/JERICHO/%s.%s", dir, base, ext);

			if (jer_host_file_exists(out))
				return 1;
		}
	}

	/* 2) the repo: srcDir is <repo>/src_rebuild, so the shim is one up */
	snprintf(out, (size_t)max, "%s/../JERICHO/%s.%s", srcDir, base, ext);

	return jer_host_file_exists(out);
}

static void jerBuildBody(void* ud, char* out, int max)
{
	(void)ud;

	if (!gBuild.attempted)
	{
		snprintf(out, (size_t)max, "Preparing...");
		return;
	}

	if (gBuild.failed)
	{
		snprintf(out, (size_t)max, "Build failed - see JERICHO/%s/%s", JER_CONFIG_PATH, JER_BUILD_LOG);
		return;
	}

	if (gBuild.step <= 0)
		snprintf(out, (size_t)max, "Scanning modules... [0/%d]", gBuild.count);
	else if (gBuild.step <= gBuild.count)
		snprintf(out, (size_t)max, "%s [%d/%d]", gBuild.mods[gBuild.step - 1].name, gBuild.step, gBuild.count);
	else if (gBuild.step == gBuild.count + 1)
		snprintf(out, (size_t)max, "Linking the game [%d/%d]", gBuild.count, gBuild.count);
	else
		snprintf(out, (size_t)max, "Done");
}

/* Launch one step of the build shim, appending its output to CONFIG/build.log.
 * One child per step is what makes "<name> [i/n]" exact: no compiler output
 * parsing, we simply know which step is running. */
static int jerBuildSpawnStep(void)
{
	char cmdline[JER_BUILD_CMDLINE];

	if (gBuild.step == gBuild.count + 1 && !gBuild.renamed &&
		jer_host_exe_locks_while_running())
	{
		/* Windows locks the running image, so the linker cannot overwrite it.
		 * Move it aside; the fresh exe then lands under the normal name and is
		 * what runs next time. The stale .old is deleted on the next boot.
		 * (On POSIX the linker relinks in place, so none of this happens.) */
		remove(gBuild.exeOld);

		if (rename(gBuild.exePath, gBuild.exeOld) == 0)
			gBuild.renamed = 1;
		else
			jer_error("JERICHO: could not move the running exe aside - the build will fail");
	}

	if (gBuild.step <= 0)
		snprintf(cmdline, sizeof(cmdline), "\"%s\" \"%s\" \"%s\" premake",
			gBuild.shimPath, gBuild.srcDir, gBuild.config);
	else if (gBuild.step <= gBuild.count)
		snprintf(cmdline, sizeof(cmdline), "\"%s\" \"%s\" \"%s\" mod %s",
			gBuild.shimPath, gBuild.srcDir, gBuild.config, gBuild.mods[gBuild.step - 1].id);
	else
		snprintf(cmdline, sizeof(cmdline), "\"%s\" \"%s\" \"%s\" exe",
			gBuild.shimPath, gBuild.srcDir, gBuild.config);

	gBuild.proc = jer_host_spawn(cmdline, gBuild.logPath);

	if (gBuild.proc == NULL)
	{
		jer_log("[build] cannot run step %d (%s)\n", gBuild.step, jer_host_last_error());
		return 0;
	}

	return 1;
}

/* 1 = the child finished (code set), 0 = still building */
static int jerBuildChildDone(int* code)
{
	int r;

	if (gBuild.proc == NULL)
	{
		*code = -1;
		return 1;
	}

	r = jer_host_proc_poll(gBuild.proc, code);

	if (r == 0)
		return 0;

	if (r < 0)
		*code = -1;

	return 1;
}

/* 1 = the running exe was moved aside and a newer one has landed in its place,
 * i.e. the link succeeded even though the step reported a failure. Only ever
 * true on a platform that moves the exe aside (Windows). */
static int jerBuildExeRebuilt(void)
{
	long long fresh, old;

	if (!gBuild.renamed)
		return 0;

	fresh = jer_host_file_mtime(gBuild.exePath);

	if (fresh == 0)
		return 0;

	old = jer_host_file_mtime(gBuild.exeOld);

	if (old == 0)
		return 0;

	return fresh > old;
}

/* One frame of the compile screen: advance the build by one step when the
 * current child finishes. Returns non-zero when the build is over. */
static int jerBuildTick(void* ud)
{
	int code = 0;

	(void)ud;

	if (!gBuild.attempted || gBuild.done || gBuild.failed)
		return 1;

	if (gBuild.proc == NULL)
	{
		if (gBuild.step > gBuild.count + 1)
		{
			gBuild.done = 1;
			return 1;
		}

		{
			int spawned = jerBuildSpawnStep();

			if (!spawned)
				gBuild.failed = 1;	/* nothing to show, but do not claim success */

			return spawned ? 0 : 1;
		}
	}

	if (!jerBuildChildDone(&code))
		return 0;

	jer_host_proc_free(gBuild.proc);
	gBuild.proc = NULL;

	if (code != 0)
	{
		/* The exe step also mirrors JERICHO/MODS next to the exe, which cannot
		 * work while this process has the module binaries loaded (Windows).
		 * The link itself comes first: if the exe was rebuilt, the build did
		 * its job. */
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
	char exePath[JER_BUILD_PATH];
	char exeOld[JER_BUILD_PATH];

	if (jer_host_exe_path(exePath, sizeof(exePath)))
	{
		snprintf(exeOld, sizeof(exeOld), "%s.old", exePath);

		/* A previous boot-time build moved the then-running exe aside. It is
		 * nobody's now (this process runs from the fresh one), so drop it — but
		 * only once the normal exe is really there, or we would delete the last
		 * good image after a build whose *link* failed. */
		if (jer_host_file_exists(exePath))
			remove(exeOld);
	}

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

	remove(gBuild.logPath);

	jer_log("[build] ---- deep-mod build requested (host %s) ----\n", jer_host_name());

	if (!jerBuildFindSrcDir(gBuild.srcDir, sizeof(gBuild.srcDir)) ||
		!jerBuildFindShim(gBuild.srcDir, "build_game", gBuild.shimPath, sizeof(gBuild.shimPath)))
	{
		jer_log("[build] no src_rebuild tree / JERICHO/build_game.%s found - cannot build here\n", jer_host_script_ext());
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
	FILE* f;

	if (!gBuild.attempted)
		return;

	/* one shot: a second LoadFrontendScreens() in this process must not re-report */
	gBuild.attempted = 0;

	if (gBuild.count == 0 && gBuild.done && !gBuild.failed)
	{
		/* nothing to compile: not an error, and nothing to announce */
		jer_log("[build] no deep mods installed - nothing to compile\n");
		return;
	}

	/* !done here means the screen loop gave up at its frame cap without the
	 * build finishing, which must not be reported as success */
	if (gBuild.failed || !gBuild.done)
	{
		/* stop pumping it: the loop has given up, so a build that is somehow
		 * still running must not keep driving a screen nobody is drawing */
		jer_screen_dismiss();

		/* the exe may still be sitting aside (the link is what failed). Put it
		 * back rather than leaving the install with no exe at all. */
		if (gBuild.renamed && !jer_host_file_exists(gBuild.exePath))
		{
			if (rename(gBuild.exeOld, gBuild.exePath) == 0)
				jer_log("[build] restored the previous exe\n");
			else
				jer_log("[build] WARNING: the previous exe is at %s\n", gBuild.exeOld);
		}

		jer_log("[build] FAILED - see JERICHO/%s/%s\n", JER_CONFIG_PATH, JER_BUILD_LOG);
		jer_error("JERICHO: compiling the deep mods failed - see JERICHO/%s/%s", JER_CONFIG_PATH, JER_BUILD_LOG);
		return;
	}

	/* the freshly linked exe is on disk, but this process is still the old
	 * image: the new build runs on the next start */
	jer_log("[build] OK - the new exe is in place; restart to run the compiled mods\n");

	f = fopen(gBuild.logPath, "ab");

	if (f != NULL)
	{
		fprintf(f, "JERICHO: build finished successfully.\n");
		fclose(f);
	}

	jer_error("JERICHO: deep mods compiled - restart to run them");
}

/* ------------------------------------------------------------------ */
/* Addons: compile every runtime "dll" module into a loadable binary   */
/* ------------------------------------------------------------------ */

int jer_compile_mods(void)
{
	char shim[JER_BUILD_PATH];
	char cmdline[1200];
	JER_HOST_PROC* proc;
	int code = 1;
	int ok = 0;

	/* find JERICHO/build_mods.<ext>: beside the exe, else relative to cwd */
	{
		char dir[JER_BUILD_PATH];

		if (jer_host_exe_path(dir, sizeof(dir)))
		{
			char* slash = (char*)jerLastSep(dir);

			if (slash != NULL)
			{
				*slash = 0;
				snprintf(shim, sizeof(shim), "%s/JERICHO/build_mods.%s", dir, jer_host_script_ext());
				ok = jer_host_file_exists(shim);
			}
		}
	}

	if (!ok)
		snprintf(shim, sizeof(shim), "JERICHO/build_mods.%s", jer_host_script_ext());

	snprintf(cmdline, sizeof(cmdline), "\"%s\"", shim);

	proc = jer_host_spawn(cmdline, NULL);

	if (proc == NULL)
	{
		jer_log("[compile] cannot run %s (%s)\n", shim, jer_host_last_error());
		return 0;
	}

	/* The caller is the blocking Compile Mods prompt, so wait here — but poll
	 * with a sleep so the game's core is not pegged while the build runs. */
	for (;;)
	{
		if (jer_host_proc_poll(proc, &code) != 0)
			break;

		jer_host_sleep_ms(50);
	}

	jer_host_proc_free(proc);

	return (code == 0) ? 1 : 0;
}

/*
 * What "Compile Mods" does once the player confirms (see jer_prompt.h): the
 * runtime addons build immediately, the deep mods are deferred to the next
 * boot because they relink the exe. Platform-neutral — the shims and the
 * driver resolve the platform.
 */
void jer_compile_request(void)
{
	int built = jer_compile_mods();

	jer_build_mark_pending();

	/* the runtime addons that just built are loadable now, so pick them up
	 * (the deep mods still wait for the restart) */
	jer_manager_reload(jer_root_dir());

	jer_log("[compile] Compile Mods: runtime addons built=%d, deep mods deferred to the next boot\n", built);

	jer_error("JERICHO: restart to compile the deep mods");
}
