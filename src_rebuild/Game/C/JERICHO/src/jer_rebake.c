/* jer_rebake.c -- after an update, ask once whether to re-bake the Driver 1 cars.
 *
 * Ported from feature/mod-store (77be5b84) without the store: an update that
 * ships a BAKE_D1_CARS script with a higher BAKE_VERSION than the one stamped in
 * JERICHO/MODS/carhacks/D1CARS/BAKE_VERSION leaves the cars baked by the old
 * tooling. On the first main-menu frame we compare the two and raise a
 * jer_prompt. An unstamped bake counts as version 0 (older); no bake at all asks
 * nothing.
 *
 * Yes starts the BAKE_D1_CARS script beside the game (the script cds to its own
 * folder and writes JERICHO/MODS/carhacks/D1CARS there). Windows: a console
 * window running BAKE_D1_CARS.bat. Linux: sh BAKE_D1_CARS.sh in the background,
 * output to JERICHO/CONFIG/rebake.log. The new bake is read next time the game
 * starts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif
#include "../include/jericho.h"
#include "../include/jer_prompt.h"
#include "../include/jer_frontend.h"
#include "../include/jer_modstore.h"	/* the startup mod-update check runs first */

#define RB_PATH_MAX 1024

static char sRebakeScript[RB_PATH_MAX];

static int RbFileInt(const char* path, const char* afterKey)
{
	char buf[8192];
	const char* p;
	size_t n;
	FILE* f = fopen(path, "rb");

	if (f == NULL)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;

	p = buf;
	if (afterKey != NULL)
	{
		p = strstr(buf, afterKey);
		if (p == NULL)
			return -1;
		p += strlen(afterKey);
	}
	while (*p != 0 && !isdigit((unsigned char)*p))
		p++;
	return *p ? atoi(p) : -1;
}

static int RbDirHasEntries(const char* path)
{
#if defined(_WIN32)
	char pattern[RB_PATH_MAX + 8];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	int found = 0;

	snprintf(pattern, sizeof(pattern), "%s/*", path);
	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) { found = 1; break; }
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return found;
#else
	DIR* d = opendir(path);
	struct dirent* e;
	int found = 0;

	if (d == NULL)
		return 0;
	while ((e = readdir(d)) != NULL)
	{
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { found = 1; break; }
	}
	closedir(d);
	return found;
#endif
}

#if defined(_WIN32)
#define RB_SCRIPT "BAKE_D1_CARS.bat"
#else
#define RB_SCRIPT "BAKE_D1_CARS.sh"
#endif

/* BAKE_VERSION the shipped tooling expects; also remembers the runnable script */
static int RbToolingVersion(void)
{
	const char* root = jer_root_dir();	/* <install>/JERICHO */
	char path[RB_PATH_MAX];
	int v;

	sRebakeScript[0] = 0;
	snprintf(path, sizeof(path), "%s/../%s", root, RB_SCRIPT);
	v = RbFileInt(path, "BAKE_VERSION=");
	if (v >= 0)
	{
		snprintf(sRebakeScript, sizeof(sRebakeScript), "%s", path);
		return v;
	}
	/* the dev tree keeps it here; it can tell us the version but must not be run
	 * from there (it bakes into its own folder) */
	snprintf(path, sizeof(path), "%s/MODS/mp/tools/pack_lan/%s", root, RB_SCRIPT);
	return RbFileInt(path, "BAKE_VERSION=");
}

static int RbBakedVersion(void)
{
	char dir[RB_PATH_MAX];
	char path[RB_PATH_MAX + 16];
	int v;

	/* carhacks owns the baked car data: JERICHO/MODS/carhacks/D1CARS */
	snprintf(dir, sizeof(dir), "%s/MODS/carhacks/D1CARS", jer_root_dir());
	snprintf(path, sizeof(path), "%s/BAKE_VERSION", dir);
	v = RbFileInt(path, NULL);
	if (v >= 0)
		return v;

	/* A bake from before the move lived beside DRIVER2. Report it too, so an
	 * update from that build still offers the re-bake (into the new folder). */
	snprintf(dir, sizeof(dir), "%s/../DRIVER/D1CARS", jer_root_dir());
	snprintf(path, sizeof(path), "%s/BAKE_VERSION", dir);
	v = RbFileInt(path, NULL);
	if (v >= 0)
		return v;

	return RbDirHasEntries(dir) ? 0 : -1;
}

static void RbYes(void)
{
	if (sRebakeScript[0] == 0)
	{
		jer_log("[rebake] " RB_SCRIPT " is not beside the game; run it from your JERICHO folder\n");
		return;
	}
#if defined(_WIN32)
	{
		char cmd[RB_PATH_MAX + 64];
		STARTUPINFOA si;
		PROCESS_INFORMATION pi;

		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		snprintf(cmd, sizeof(cmd), "cmd.exe /c \"\"%s\"\"", sRebakeScript);
		if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
		{
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			jer_log("[rebake] started %s\n", sRebakeScript);
		}
		else
			jer_log("[rebake] could not start %s\n", sRebakeScript);
	}
#else
	{
		pid_t pid = fork();

		if (pid == 0)
		{
			char logPath[RB_PATH_MAX];
			FILE* lf;

			snprintf(logPath, sizeof(logPath), "%s/CONFIG/rebake.log", jer_root_dir());
			lf = freopen(logPath, "w", stdout);
			if (lf != NULL)
				dup2(fileno(stdout), 2);
			freopen("/dev/null", "r", stdin);	/* the script's closing read returns at once */
			execl("/bin/sh", "sh", sRebakeScript, (char*)NULL);
			_exit(127);
		}
		jer_log(pid > 0 ? "[rebake] started %s (log: JERICHO/CONFIG/rebake.log)\n"
			: "[rebake] could not start %s\n", sRebakeScript);
	}
#endif
}

/* Frontend, once per frame on the main menu. Asks at most once per run. */
void jer_rebake_frontend_tick(int onMainMenu)
{
	static int asked = 0;
	static int settled = 0;
	int upd = jer_frontend_find("jericho.update");
	int baked, wanted;

	if (asked)
		return;

	/* The store's startup update question comes first, and may still be fetching
	 * the repo index or applying an update. Do not raise "re-bake your D1 cars?"
	 * on top of it: an update can bring the newer bake tooling this is about to
	 * ask about. */
	if (jer_modstore_startup_update_pending())
	{
		settled = 0;
		return;
	}

	/* let the updater have its say first: its notes page owns the screen, and a
	 * prompt over it is two screens at once. Ask after a few quiet main-menu
	 * frames, i.e. once the player is back on the main menu. */
	if (!onMainMenu || jer_prompt_active() || (upd >= 0 && jer_frontend_current_menu() == upd))
	{
		settled = 0;
		return;
	}
	if (++settled < 30)
		return;
	asked = 1;

	baked = RbBakedVersion();
	wanted = RbToolingVersion();
	if (baked < 0 || wanted < 0 || baked >= wanted)
		return;

	jer_log("[rebake] Driver 1 bake is older than the tooling (baked %d, expected %d)\n", baked, wanted);
	jer_prompt_begin("Your Driver 1 cars were baked with older tooling. Re-bake them now?", RbYes);
}
