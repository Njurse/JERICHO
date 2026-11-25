/*
 * jer_joinmods.c — fetch the addons a server needs, with the player's say-so.
 *
 * See jer_joinmods.h for the player's side of it. This file is the driver: it
 * gathers the tables jer_joinplan.c plans from (the module inventory and every
 * configured repo's rows), asks, has the mod store download, and relaunches.
 *
 *   offer -> REFRESH  fetch each configured repo's index in turn (one store
 *                     job at a time; a repo that cannot be fetched is read
 *                     from its cache)
 *         -> PLAN     jer_joinplan_build; a plan with a block is a notice and
 *                     the end of it
 *         -> ASK      one prompt: Download & Join / No / Keep Them On
 *         -> INSTALL  each download, in turn, through jer_modstore_install
 *         -> LAUNCH   persist the answer, relaunch with -join and the flags,
 *                     quit
 *
 * TEMPORARY MEANS TEMPORARY. The session's changes are boot flags (-mod,
 * -nomod), so they last one run. A mod downloaded for a session is also written
 * to modlist.ini as OFF, because a mod.toml that says default-enabled would
 * otherwise switch it on at every later start. "Keep Them On" writes ON instead.
 */
#include "jericho.h"
#include "jer_internal.h"
#include "jer_host.h"
#include "jer_joinmods.h"
#include "jer_joinplan.h"
#include "jer_modstore.h"
#include "jer_notify.h"
#include "jer_prompt.h"
#include "jer_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void jer_frontend_quit(void);	/* FEmain.c: leave through the engine's own exit */

enum
{
	JM_IDLE = 0,
	JM_REFRESH,
	JM_PLAN,
	JM_ASK,
	JM_INSTALL,
	JM_LAUNCH
};

static struct
{
	int  state;
	char address[JER_JOIN_ADDR_MAX];
	JER_JOIN_MOD server[JER_JOIN_MAX];	/* what the server runs */
	int  serverCount;
	char ignore[8][JER_JOIN_ID_MAX];	/* counted neither way (mp's tooling) */
	int  ignoreCount;
	int  flags;
	JER_JOIN_MOD need[JER_JOIN_MAX];	/* the diff against the saved set (PLAN) */
	int  needCount;
	JER_JOIN_MOD extra[JER_JOIN_MAX];
	int  extraCount;
	int  plainRestart;		/* the saved set already fits: only this boot's flags were wrong */

	int  repo;		/* REFRESH: the repo being fetched */
	int  fetching;		/* REFRESH: its fetch is running */
	int  savedRepo;		/* the browser's own selection, put back after */

	JER_JOINPLAN plan;
	char question[200];
	int  asked;		/* ASK: the prompt is up */
	int  keep;		/* the player chose Keep Them On */

	int  step;		/* INSTALL: the plan step being downloaded */
	int  installing;	/* INSTALL: its job is running */

	/* the boot */
	int  argc;
	char** argv;
	char sessionNote[200];	/* -joinmods: what this boot runs for a server only */
	int  sessionNoted;
} gJm;

static void JmEnd(void)
{
	if (gJm.savedRepo >= 0 && gJm.savedRepo < jer_modstore_repo_count())
		jer_modstore_repo_select(gJm.savedRepo);

	gJm.state = JM_IDLE;
	gJm.asked = 0;
	gJm.fetching = 0;
	gJm.installing = 0;
}

static void JmFail(const char* text)
{
	jer_log("[joinmods] %s\n", text);
	jer_notify(text, 1, 8);
	JmEnd();
}

int jer_joinmods_offer(const char* address, const JER_JOIN_MOD* server, int serverCount,
	const char* const* ignore, int ignoreCount, int flags)
{
	int i;

	if (gJm.state != JM_IDLE)
		return 0;

	if (!jer_joinplan_valid_address(address))
	{
		jer_log("[joinmods] offer refused: the server address is not one -join can take\n");
		return 0;
	}

	if (serverCount < 0 || serverCount > JER_JOIN_MAX || (serverCount > 0 && server == NULL) ||
		ignoreCount < 0 || (ignoreCount > 0 && ignore == NULL))
		return 0;

	/* an empty list is still an answer: with JER_JOIN_EXACT_SET it says "no mods" */
	if (serverCount == 0 && !(flags & JER_JOIN_EXACT_SET))
		return 0;

	memset(gJm.server, 0, sizeof(gJm.server));
	memset(gJm.ignore, 0, sizeof(gJm.ignore));
	snprintf(gJm.address, sizeof(gJm.address), "%s", address);

	for (i = 0; i < serverCount; i++)
		gJm.server[i] = server[i];

	gJm.ignoreCount = 0;
	for (i = 0; i < ignoreCount && gJm.ignoreCount < 8; i++)
	{
		if (ignore[i] != NULL && ignore[i][0] != 0)
			snprintf(gJm.ignore[gJm.ignoreCount++], JER_JOIN_ID_MAX, "%s", ignore[i]);
	}

	gJm.serverCount = serverCount;
	gJm.needCount = 0;
	gJm.extraCount = 0;
	gJm.plainRestart = 0;
	gJm.flags = flags;
	gJm.savedRepo = jer_modstore_repo_selected();
	gJm.repo = 0;
	gJm.fetching = 0;
	gJm.asked = 0;
	gJm.keep = 0;
	gJm.step = 0;
	gJm.installing = 0;
	gJm.state = JM_REFRESH;

	jer_log("[joinmods] %s refused this machine for its mods; it runs %d%s%s - checking %d repo(s)\n",
		gJm.address, serverCount, (flags & JER_JOIN_EXACT) ? ", versions must match" : "",
		(flags & JER_JOIN_EXACT_SET) ? ", and no others" : "", jer_modstore_repo_count());
	jer_notify("This server runs other mods. Checking your mod repos...", 1, 4);
	return 1;
}

int jer_joinmods_busy(void)
{
	return gJm.state != JM_IDLE;
}

void jer_joinmods_boot(int argc, char** argv)
{
	int i, on = 0, off = 0;
	char onText[60], offText[60];	/* sized so the longest note fits sessionNote */

	gJm.argc = argc;
	gJm.argv = argv;
	gJm.savedRepo = -1;
	onText[0] = offText[0] = 0;

	for (i = 1; argv != NULL && i < argc; i++)
	{
		if (argv[i] != NULL && !strcmp(argv[i], "-joinmods"))
			break;
	}

	if (argv == NULL || i >= argc)
		return;		/* an ordinary start */

	for (i = 1; i + 1 < argc; i++)
	{
		char* list = NULL;
		int* count = NULL;

		if (!strcmp(argv[i], "-mod"))
		{
			list = onText;
			count = &on;
		}
		else if (!strcmp(argv[i], "-nomod"))
		{
			list = offText;
			count = &off;
		}

		if (list != NULL && jer_joinplan_valid_id(argv[i + 1]))
		{
			size_t used = strlen(list);

			snprintf(list + used, sizeof(onText) - used, "%s%s", *count ? ", " : "", argv[i + 1]);
			(*count)++;
			i++;
		}
	}

	if (on > 0 && off > 0)
		snprintf(gJm.sessionNote, sizeof(gJm.sessionNote),
			"For this server only: %s on, %s off. Turn mods on for good in the Mods menu.",
			onText, offText);
	else if (on > 0)
		snprintf(gJm.sessionNote, sizeof(gJm.sessionNote),
			"On for this server only: %s. Turn them on for good in the Mods menu.", onText);
	else if (off > 0)
		snprintf(gJm.sessionNote, sizeof(gJm.sessionNote),
			"Off for this server only: %s.", offText);
}

/* The repos' rows and the module inventory, as the planner wants them. The
 * strings point into the store's own storage, so the plan is built before the
 * next selection changes it. */
static void JmPlan(void)
{
	static JER_JOINPLAN_LOCAL local[JER_MAX_MODULES];
	static char localId[JER_MAX_MODULES][JER_JOIN_ID_MAX];
	static char localVer[JER_MAX_MODULES][JER_JOIN_VER_MAX];
	static char localWhy[JER_MAX_MODULES][96];
	static JER_JOINPLAN_OFFER offers[JER_JOIN_MAX * 4];
	static char offerId[JER_JOIN_MAX * 4][JER_JOIN_ID_MAX];
	static char offerName[JER_JOIN_MAX * 4][48];
	static char offerVer[JER_JOIN_MAX * 4][JER_JOIN_VER_MAX];
	static char offerNote[JER_JOIN_MAX * 4][96];
	const char* repoNames[16];
	int localCount = 0, offerCount = 0, r, i, n;

	n = jer_modstore_module_count();

	/* jer_modstore_module_at's strings last until its next call: copy them */
	for (i = 0; i < n && localCount < JER_MAX_MODULES; i++)
	{
		JER_MODSTORE_MODULE m;

		if (!jer_modstore_module_at(i, &m) || m.id == NULL)
			continue;

		snprintf(localId[localCount], sizeof(localId[0]), "%s", m.id);
		snprintf(localVer[localCount], sizeof(localVer[0]), "%s", m.version ? m.version : "");
		snprintf(localWhy[localCount], sizeof(localWhy[0]), "%s", m.refusal ? m.refusal : "");
		local[localCount].id = localId[localCount];
		local[localCount].version = localVer[localCount];
		local[localCount].enabled = m.enabled;
		local[localCount].core = m.core;
		local[localCount].refusal = m.refusal ? localWhy[localCount] : NULL;
		localCount++;
	}

	/* The diff is against the SAVED set: it is what the relaunch starts from. */
	{
		const char* ignore[8];
		int k, differs = 0;

		for (k = 0; k < gJm.ignoreCount; k++)
			ignore[k] = gJm.ignore[k];

		jer_joinplan_diff(gJm.server, gJm.serverCount, ignore, gJm.ignoreCount, gJm.flags,
			local, localCount, gJm.need, &gJm.needCount, gJm.extra, &gJm.extraCount);

		/* ...and when that already fits, the refusal was this boot's own -mod /
		 * -nomod flags: a restart on the saved set is the whole fix */
		for (k = 0; k < localCount && !differs; k++)
		{
			int saved = local[k].enabled && local[k].refusal == NULL;

			differs = saved != (jer_module_running(local[k].id) != 0);
		}

		gJm.plainRestart = gJm.needCount + gJm.extraCount == 0 && differs;
	}

	/* Only the rows that name a mod the server wants: the store's strings move
	 * with the selection, so the ones the plan needs are copied out here. */
	for (r = 0; r < jer_modstore_repo_count() && r < 16; r++)
	{
		repoNames[r] = jer_modstore_repo_name(r);
		jer_modstore_repo_select(r);

		for (i = 0; i < jer_modstore_item_count() && offerCount < JER_JOIN_MAX * 4; i++)
		{
			const JER_MODSTORE_ITEM* it = jer_modstore_item(i);
			int k, wanted = 0;

			if (it == NULL || it->id == NULL)
				continue;

			for (k = 0; k < gJm.needCount && !wanted; k++)
				wanted = strncmp(gJm.need[k].id, it->id, JER_JOIN_ID_MAX) == 0;

			if (!wanted)
				continue;

			snprintf(offerId[offerCount], sizeof(offerId[0]), "%s", it->id);
			snprintf(offerName[offerCount], sizeof(offerName[0]), "%s", it->name ? it->name : it->id);
			snprintf(offerVer[offerCount], sizeof(offerVer[0]), "%s", it->version ? it->version : "");
			snprintf(offerNote[offerCount], sizeof(offerNote[0]), "%s", it->note ? it->note : "");
			offers[offerCount].id = offerId[offerCount];
			offers[offerCount].name = offerName[offerCount];
			offers[offerCount].version = offerVer[offerCount];
			offers[offerCount].note = offerNote[offerCount];
			offers[offerCount].repo = r;
			offers[offerCount].canInstall = it->canInstall;
			offers[offerCount].kind = (it->runtime != NULL && !strcmp(it->runtime, "dll")) ?
				JER_JOINPLAN_KIND_LOADABLE : JER_JOINPLAN_KIND_UNKNOWN;
			offerCount++;
		}
	}

	jer_joinplan_build(&gJm.plan, gJm.need, gJm.needCount, gJm.extra, gJm.extraCount,
		gJm.flags, local, localCount, offers, offerCount);

	for (i = 0; i < gJm.plan.count; i++)
	{
		const JER_JOINPLAN_STEP* s = &gJm.plan.steps[i];

		jer_log("[joinmods]   %-8s %-16s %s%s%s\n",
			s->action == JER_JOINPLAN_ENABLE ? "on" :
			s->action == JER_JOINPLAN_DISABLE ? "off" :
			s->action == JER_JOINPLAN_DOWNLOAD ? "download" : "BLOCKED",
			s->id, s->version, s->action == JER_JOINPLAN_BLOCK ? ": " : "",
			s->action == JER_JOINPLAN_BLOCK ? s->why : "");
	}

	if (gJm.plan.blocks > 0)
	{
		char text[200];

		jer_joinplan_refusal(&gJm.plan, text, (int)sizeof(text));
		JmFail(text);
		return;
	}

	if (gJm.plan.downloads + gJm.plan.enables + gJm.plan.disables == 0)
	{
		if (!gJm.plainRestart)
		{
			JmFail("This server refused this machine's mods, but nothing here would change that.");
			return;
		}

		snprintf(gJm.question, sizeof(gJm.question),
			"This server runs your own mod set; this session has other mods on. Restart JERICHO with yours and join?");
		gJm.state = JM_ASK;
		return;
	}

	jer_joinplan_question(&gJm.plan, repoNames, r, gJm.question, (int)sizeof(gJm.question));
	gJm.state = JM_ASK;
}

static void JmYes(void)
{
	gJm.keep = 0;
	gJm.state = gJm.plan.downloads > 0 ? JM_INSTALL : JM_LAUNCH;
}

static void JmKeep(void)
{
	gJm.keep = 1;
	gJm.state = gJm.plan.downloads > 0 ? JM_INSTALL : JM_LAUNCH;
}

static void JmNo(void)
{
	jer_log("[joinmods] the player declined; nothing was fetched or changed\n");
	JmEnd();
}

static int JmInstalledHere(const char* id)
{
	char path[640];

	snprintf(path, sizeof(path), "%s/MODS/%s/mod.toml", jer_root_dir(), id);
	return jer_host_file_exists(path);
}

static void JmInstallTick(void)
{
	char text[200];

	if (jer_modstore_busy())
		return;

	if (gJm.installing)
	{
		const JER_JOINPLAN_STEP* s = &gJm.plan.steps[gJm.step];

		gJm.installing = 0;

		if (!JmInstalledHere(s->id))
		{
			snprintf(text, sizeof(text), "Could not download %s: %s", s->name,
				jer_modstore_status() != NULL ? jer_modstore_status() : "the store did not finish");
			JmFail(text);
			return;
		}

		if (jer_modstore_installed_kind(s->id) == JER_MODSTORE_KIND_BUILTIN)
		{
			snprintf(text, sizeof(text),
				"Can't join: %s is built into the game; it needs a JERICHO build that has it", s->name);
			JmFail(text);
			return;
		}

		/* A download for one session stays off at every later start, whatever
		 * its mod.toml says, until the player turns it on. */
		jer_manager_set_enabled(jer_root_dir(), s->id, gJm.keep ? 1 : 0);
		gJm.step++;
	}

	while (gJm.step < gJm.plan.count && gJm.plan.steps[gJm.step].action != JER_JOINPLAN_DOWNLOAD)
		gJm.step++;

	if (gJm.step < gJm.plan.count)
	{
		const JER_JOINPLAN_STEP* s = &gJm.plan.steps[gJm.step];
		int r;

		jer_modstore_repo_select(s->repo);
		r = jer_modstore_install(s->id);

		if (r != JER_MODSTORE_OK)
		{
			snprintf(text, sizeof(text), "Could not download %s: %s", s->name,
				r == JER_MODSTORE_NO_SCRIPT ? "the mod store helper is not beside the game" :
				(jer_modstore_status() != NULL ? jer_modstore_status() : "the store refused"));
			JmFail(text);
			return;
		}

		gJm.installing = 1;
		snprintf(text, sizeof(text), "Downloading %s %s...", s->name, s->version);
		jer_notify(text, 1, 4);
		jer_log("[joinmods] downloading %s %s from repo %d\n", s->id, s->version, s->repo);
		return;
	}

	gJm.state = JM_LAUNCH;
}

static void JmLaunch(void)
{
	char exe[512];
	char line[1600];
	char cmd[1700];
	JER_HOST_PROC* p;
	int i;
#if defined(_WIN32)
	const int posix = 0;
#else
	const int posix = 1;
#endif

	if (gJm.keep)
	{
		for (i = 0; i < gJm.plan.count; i++)
		{
			const JER_JOINPLAN_STEP* s = &gJm.plan.steps[i];

			if (s->action == JER_JOINPLAN_ENABLE || s->action == JER_JOINPLAN_DOWNLOAD)
				jer_manager_set_enabled(jer_root_dir(), s->id, 1);
		}

		jer_log("[joinmods] kept on: the server's mods are enabled in modlist.ini\n");
	}

	if (!jer_host_exe_path(exe, sizeof(exe)))
	{
		JmFail("The mods are ready. Start JERICHO again to join the server.");
		return;
	}

	if (!jer_joinplan_cmdline(&gJm.plan, exe, gJm.address, gJm.argc, gJm.argv, posix,
		line, (int)sizeof(line)))
	{
		JmFail("The mods are ready, but JERICHO could not restart itself. Start it again to join.");
		return;
	}

	/* A short pause first: the old instance is still closing its log and its
	 * sockets when the new one starts. */
	if (posix)
		snprintf(cmd, sizeof(cmd), "sleep 1; %s", line);
	else
		snprintf(cmd, sizeof(cmd), "ping -n 2 127.0.0.1 >nul & %s", line);

	/* JERICHO_TEST_JOINMODS=nolaunch: the whole flow, headless, without the
	 * relaunch itself (test/test_joinmods.c reads this line). The store's own
	 * JERICHO_TEST_* levers work the same way, and an unset one changes nothing. */
	{
		const char* lever = getenv("JERICHO_TEST_JOINMODS");

		if (lever != NULL && strcmp(lever, "nolaunch") == 0)
		{
			jer_log("[joinmods] test: would relaunch: %s\n", line);
			JmEnd();
			return;
		}
	}

	jer_log("[joinmods] relaunching: %s\n", line);
	p = jer_host_spawn(cmd, NULL);

	if (p == NULL)
	{
		JmFail("The mods are ready, but JERICHO could not restart itself. Start it again to join.");
		return;
	}

	jer_host_proc_free(p);
	JmEnd();
	jer_frontend_quit();
}

void jer_joinmods_tick(void)
{
	if (gJm.sessionNote[0] != 0 && !gJm.sessionNoted)
	{
		gJm.sessionNoted = 1;
		jer_log("[joinmods] %s\n", gJm.sessionNote);
		jer_notify(gJm.sessionNote, 1, 8);
	}

	switch (gJm.state)
	{
	case JM_REFRESH:
		if (jer_modstore_busy())
			return;		/* a fetch is running: ours, or a job the player started */

		if (gJm.fetching)
		{
			gJm.fetching = 0;
			gJm.repo++;
		}

		if (gJm.repo < jer_modstore_repo_count())
		{
			jer_modstore_repo_select(gJm.repo);

			if (jer_modstore_refresh() == JER_MODSTORE_OK)
				gJm.fetching = 1;
			else
				gJm.repo++;	/* cannot fetch it: its cached index still counts */

			return;
		}

		gJm.state = JM_PLAN;
		JmPlan();
		return;

	case JM_ASK:
		if (gJm.asked)
		{
			if (!jer_prompt_active() && gJm.state == JM_ASK)
				JmNo();		/* dismissed without an answer */
			return;
		}

		if (jer_prompt_active() || jer_screen_active())
			return;		/* never over another question or a presentation screen */

		/* a plain restart changes nothing that could be kept: two answers */
		if (jer_prompt_begin3(gJm.question,
			gJm.plan.downloads > 0 ? "Download & Join" : "Restart & Join", "No",
			gJm.plainRestart ? "" : "Keep Them On", JmYes, JmNo, JmKeep))
			gJm.asked = 1;
		return;

	case JM_INSTALL:
		JmInstallTick();
		return;

	case JM_LAUNCH:
		JmLaunch();
		return;

	default:
		return;
	}
}
