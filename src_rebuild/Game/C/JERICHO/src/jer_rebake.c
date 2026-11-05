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
 * starts.
 *
 * First launch: when there is no readable synthetic LEV yet -- no
 * DRIVER2/LEVELS/JMPPED.LEV (peds: stock Tanner + Jericho, built from the
 * player's own HAVANA.LEV) and/or no baked Driver 1 cars -- the same prompt
 * explains why baking is needed and offers Continue / No. No only skips this
 * launch; first_bake_prompt=0 in JERICHO/CONFIG/rebake.ini (written on the
 * first No) stops it asking. The version re-bake question is unaffected.
 *
 * Both questions are short lines (a newline is a paragraph break): drop
 * Driver 1 files in DRIVER/, and if you have the 16 Dec 1998 proto (the one
 * with the Buick GSX) put LEVELS/ and SOUND/ in EURO43/ so the bake can pull
 * those cars. */
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
#include "../include/jer_rebake.h"	/* the re-arm hook jer_modstore.c calls */

#define RB_PATH_MAX 1024

static char sRebakeScript[RB_PATH_MAX];
static int sRebakePortraitsOnly;	/* encode MIAMI.RAW; do not rebuild the cars */

/* The prompt is one-shot per run. A mod install can bring newer bake tooling
 * after it has already run, so jer_modstore.c calls jer_rebake_rearm() then. */
static int sRebakeAsked;
static int sRebakeSettled;
static int sRebakeAccepted;	/* the player answered Yes: never re-ask this run */

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

/* 1 when the page at model*0x8000 is missing or all zero. That is the
 * sibling RAW the menu reads first. A BAKE_VERSION 11 bank has the cars
 * and does not have this file. */
static int RbPageBlank(const char* path, int model)
{
	FILE* f;
	unsigned char buf[256];
	long need;
	int left, n, i, any;

	if (model < 0)
		return 1;

	need = ((long)model + 1L) * 0x8000L;
	f = fopen(path, "rb");
	if (f == NULL)
		return 1;
	if (fseek(f, 0, SEEK_END) != 0 || ftell(f) < need ||
		fseek(f, (long)model * 0x8000L, SEEK_SET) != 0)
	{
		fclose(f);
		return 1;
	}

	any = 0;
	left = 0x8000;
	while (left > 0 && !any)
	{
		n = left > (int)sizeof(buf) ? (int)sizeof(buf) : left;
		if ((int)fread(buf, 1, (size_t)n, f) != n)
		{
			fclose(f);
			return 1;
		}
		for (i = 0; i < n; i++)
		{
			if (buf[i] != 0)
			{
				any = 1;
				break;
			}
		}
		left -= n;
	}
	fclose(f);
	return !any;
}

/* Forward decl: RbMiamiPortraitMissing (and the ped check below) call this
 * before the definition later in the file. GCC rejects an implicit then
 * static definition; MSVC was more forgiving. */
static int RbLevReadable(const char* path);

/* Cars are already baked, the Miami source PNG is in the package, and the
 * sibling RAW the menu opens was never written. Encoding that file does not
 * change BAKE_VERSION and does not rebuild the LEV. */
static int RbMiamiPortraitMissing(void)
{
	char path[RB_PATH_MAX];
	char raw[RB_PATH_MAX];

	snprintf(path, sizeof(path),
		"%s/MODS/carhacks/assets/portraits/MIAMI/portraits/MCARS.RAW_0.RAW.png",
		jer_root_dir());
	{
		FILE* f = fopen(path, "rb");

		if (f == NULL)
			return 0;
		fclose(f);
	}

	snprintf(path, sizeof(path), "%s/MODS/carhacks/D1CARS/MIAMI.LEV", jer_root_dir());
	if (!RbLevReadable(path))
		return 0;

	snprintf(raw, sizeof(raw), "%s/MODS/carhacks/D1CARS/MIAMI.RAW", jer_root_dir());
	return RbPageBlank(raw, 1);
}

/* The running exe, so the script can relaunch THIS game even where the script
 * does not sit beside it (the dev tree keeps BAKE_D1_CARS at the repo root). */
static void RbExePath(char* out, int outSize)
{
	out[0] = 0;
#if defined(_WIN32)
	if (GetModuleFileNameA(NULL, out, outSize) == 0)
		out[0] = 0;
#else
	{
		ssize_t k = readlink("/proc/self/exe", out, outSize - 1);
		if (k > 0)
			out[k] = 0;
		else
			out[0] = 0;
	}
#endif
}

static void RbStartPortraits(void)
{
	sRebakePortraitsOnly = 1;
	jer_rebake_start();
}

void jer_rebake_start(void)
{
	char gameExe[RB_PATH_MAX];
	int portraitsOnly = sRebakePortraitsOnly;

	/* The options-menu Bake button calls this directly. Only the portrait
	 * prompt sets the flag, and it is consumed here so the next bake is a
	 * full one. */
	sRebakePortraitsOnly = 0;

	/* The prompt tick is what normally remembers the script. Bake on the options
	 * menu can run before that tick, so look it up here too. Same function. */
	if (sRebakeScript[0] == 0)
		RbToolingVersion();

	if (sRebakeScript[0] == 0)
	{
		jer_log("[rebake] " RB_SCRIPT " is not beside the game; run it from your JERICHO folder\n");
		return;
	}

	/* Accepted: a later re-arm (a mod install) must not ask again this run. */
	sRebakeAccepted = 1;

	RbExePath(gameExe, sizeof(gameExe));

#if defined(_WIN32)
	{
		char cmd[RB_PATH_MAX * 2 + 64];
		STARTUPINFOA si;
		PROCESS_INFORMATION pi;

		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		/* --from-game: the script bakes, then relaunches the exe passed after it.
		 * A manual run never passes the flag, so it never restarts the game. */
		snprintf(cmd, sizeof(cmd), "cmd.exe /c \"\"%s\" --from-game \"%s\"%s\"",
			sRebakeScript, gameExe, portraitsOnly ? " --portraits-only" : "");
		if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
		{
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			jer_log("[rebake] started %s --from-game; the game closes and reopens after\n", sRebakeScript);
			exit(0);	/* the engine's own way out (atexit finalises the log) */
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
			if (portraitsOnly)
				execl("/bin/sh", "sh", sRebakeScript, "--from-game", gameExe, "--portraits-only", (char*)NULL);
			else
				execl("/bin/sh", "sh", sRebakeScript, "--from-game", gameExe, (char*)NULL);
			_exit(127);
		}
		jer_log(pid > 0 ? "[rebake] started %s --from-game (log: JERICHO/CONFIG/rebake.log); the game closes and reopens after\n"
			: "[rebake] could not start %s\n", sRebakeScript);

		if (pid > 0)
			exit(0);
	}
#endif
}

/* A synthetic LEV is "readable" when it opens and is more than a stub. */
static int RbLevReadable(const char* path)
{
	long size;
	FILE* f = fopen(path, "rb");

	if (f == NULL)
		return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fclose(f);
	return size >= 2048;
}

static void RbIniPath(char* out, int outSize)
{
	snprintf(out, outSize, "%s/CONFIG/rebake.ini", jer_root_dir());
}

/* first_bake_prompt=0 in CONFIG/rebake.ini: never show the first-launch prompt */
static int RbFirstPromptOptedOut(void)
{
	char path[RB_PATH_MAX];

	RbIniPath(path, sizeof(path));
	return RbFileInt(path, "first_bake_prompt=") == 0;
}

/* No: skip this launch, and leave the switch where the prompt said it is */
static void RbFirstNo(void)
{
	char path[RB_PATH_MAX];
	FILE* f;

	RbIniPath(path, sizeof(path));
	f = fopen(path, "rb");
	if (f != NULL)
	{
		fclose(f);
		return;
	}
	f = fopen(path, "wb");
	if (f == NULL)
		return;
	fputs("; JERICHO first-launch bake prompt (cars + peds).\n"
		"; 1 = ask at startup while JMPPED.LEV / the Driver 1 cars are not baked\n"
		"; 0 = never ask (bake any time with BAKE_D1_CARS beside the game)\n"
		"first_bake_prompt=1\n", f);
	fclose(f);
	jer_log("[rebake] first-launch bake skipped; set first_bake_prompt=0 in %s to stop asking\n", path);
}

/* Frontend, once per frame on the main menu. Asks at most once per run. */
void jer_rebake_frontend_tick(int onMainMenu)
{
	int upd = jer_frontend_find("jericho.update");
	int baked, wanted;

	if (sRebakeAsked)
		return;

	/* The store's startup update question comes first, and may still be fetching
	 * the repo index or applying an update. Do not raise "re-bake your D1 cars?"
	 * on top of it: an update can bring the newer bake tooling this is about to
	 * ask about. */
	if (jer_modstore_startup_update_pending())
	{
		sRebakeSettled = 0;
		return;
	}

	/* let the updater have its say first: its notes page owns the screen, and a
	 * prompt over it is two screens at once. Ask after a few quiet main-menu
	 * frames, i.e. once the player is back on the main menu. */
	if (!onMainMenu || jer_prompt_active() || (upd >= 0 && jer_frontend_current_menu() == upd))
	{
		sRebakeSettled = 0;
		return;
	}
	if (++sRebakeSettled < 30)
		return;
	sRebakeAsked = 1;

	baked = RbBakedVersion();
	wanted = RbToolingVersion();

	/* first launch: no readable synthetic LEV yet (peds and/or cars) */
	if (wanted >= 0 && !RbFirstPromptOptedOut())
	{
		char ped[RB_PATH_MAX];
		int pedsOk, carsOk;

		snprintf(ped, sizeof(ped), "%s/../DRIVER2/LEVELS/JMPPED.LEV", jer_root_dir());
		pedsOk = RbLevReadable(ped);
		carsOk = baked >= 0;
		if (!pedsOk || !carsOk)
		{
			jer_log("[rebake] first launch: peds %s, Driver 1 cars %s\n",
				pedsOk ? "ok" : "missing", carsOk ? "ok" : "missing");
			jer_prompt_begin_ex("First start: bake cars and peds from your files.\n"
				"The game restarts after.\n"
				"Drop Driver 1 files in DRIVER/.\n"
				"If you have the 16 Dec 1998 proto with the Buick GSX,\n"
				"put LEVELS/ and SOUND/ in EURO43/\n"
				"so the bake can pull those cars.",
				"Continue", "No", jer_rebake_start, RbFirstNo);
			return;
		}
	}

	if (baked < 0 || wanted < 0 || baked >= wanted)
	{
		/* The cars are current. BAKE_VERSION does not know about MIAMI.RAW,
		 * so a version-11 bank is not stale as a car bank. The pictures are
		 * a separate file. Encoding them does not rebuild the LEV. */
		if (baked >= 0 && RbMiamiPortraitMissing())
		{
			jer_log("[rebake] Driver 1 cars are current; MIAMI.RAW is missing\n");
			jer_prompt_begin("Add Driver 1 car-select pictures?\n"
				"The cars stay as they are.\n"
				"The pictures are encoded into MIAMI.RAW\n"
				"beside the baked level.",
				RbStartPortraits);
		}
		return;
	}
	sRebakePortraitsOnly = 0;

	jer_log("[rebake] Driver 1 bake is older than the tooling (baked %d, expected %d)\n", baked, wanted);
	jer_prompt_begin("Re-bake Driver 1 cars to pick up body colours?\n"
		"An older bake left those shades out of the palette,\n"
		"so the paint swap cannot show them.\n"
		"Drop Driver 1 files in DRIVER/.\n"
		"If you have the 16 Dec 1998 proto with the Buick GSX,\n"
		"put LEVELS/ and SOUND/ in EURO43/\n"
		"so the bake can pull those cars.",
		jer_rebake_start);
}

/* The one-shot prompt already ran (or the player declined). A mod install may
 * have brought newer bake tooling, so allow the question again. */
void jer_rebake_rearm(void)
{
	/* A mod install can bring newer bake tooling, but never re-ask a player who
	 * already said Yes this run -- that was the "asked twice" report. */
	if (sRebakeAccepted)
		return;

	sRebakeAsked = 0;
	sRebakeSettled = 0;
}
