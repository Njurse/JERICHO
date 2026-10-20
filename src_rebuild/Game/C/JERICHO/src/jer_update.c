/* ------------------------------------------------------------------
 * jer_update.c -- the "a new version is available" screen.
 *
 * HOW THE CHECK RUNS (and why this way)
 *   jer_update_start() (from jer_init, i.e. at boot) spawns ONE `curl` for
 *   https://api.github.com/repos/Njurse/JERICHO/releases/latest and returns at
 *   once. curl writes CONFIG/update_check.part; when it exits 0 the file is
 *   renamed to CONFIG/update_check.json, which is also next launch's cache.
 *     - curl ships with Windows 10+ and every desktop Linux, and the update
 *       scripts already need it; the engine has no TLS stack of its own and
 *       should not grow one for this. PowerShell is slower to start and is
 *       Windows-only; the update scripts in a --check mode would mean JSON
 *       parsing in cmd.exe.
 *     - It is a child PROCESS, polled without waiting, so boot never blocks:
 *       offline, curl gives up on its own (--connect-timeout 3, --max-time 8).
 *     - ONE API request per launch, unauthenticated (60/hour/IP is plenty).
 *     - releases/latest never returns a pre-release, so the rolling "alpha"
 *       never counts as an update; a tag carrying a "-suffix" is ignored too.
 *   jer_update_frontend_tick() (FEmain.c, every frontend frame) decides ONCE, on
 *   the main menu: this launch's answer if it is in, else -- once
 *   JER_UPD_WAIT_S have passed since boot, or curl failed -- the cached answer
 *   from a previous launch, else nothing. It never opens over another screen.
 *
 * WHAT COUNTS AS NEWER
 *   The local version is JERICHO_BUILD_VERSION (git describe --match 'v*'):
 *   "0.10.4" on the tag, "0.10.4-12-gabc1234[-dirty]" past it. Only the leading
 *   X.Y.Z is compared, so an alpha / local build compares as the release it
 *   descends from: it is offered v0.10.5 when that ships, never "v0.10.4" (which
 *   would be a downgrade). A build with no v* tag behind it (a bare hash) has no
 *   version to compare and never checks.
 *
 * RE-CUT (same tag, rebuilt)
 *   update_jericho writes CONFIG/installed.json (tag, asset, sha256) after an
 *   update. When the release's version EQUALS ours and this build sits exactly on
 *   the tag, the SHA256 GitHub reports for our flavour's asset ("digest") is
 *   compared with the one recorded; a difference means the tag was re-cut. No
 *   installed.json (a hand-unzipped install) = unknown, so nothing is shown.
 *
 * FLAVOUR: a Release build looks at (and updates to) JERICHO_Release_*, any
 * other build (Release_dev, Debug -- the "-dev" ones, version.h) at
 * JERICHO_Release_dev_*.
 *
 * Off switch: CONFIG/jericho.ini  update_check=0 (the screen's third option).
 * ------------------------------------------------------------------ */
#include "jericho.h"
#include "jer_config.h"
#include "jer_frontend.h"
#include "jer_notify.h"
#include "jer_text.h"
#include "jer_prompt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef JERICHO_BUILD_VERSION
#define JERICHO_BUILD_VERSION "0.0.0"
#endif

#ifdef __cplusplus
extern "C" {
#endif
const char* jer_root_dir(void);
int jer_build_is_dev(void);	/* FEmain.c: the EXE knows its flavour, this library does not */
void jer_update_start(void);
void jer_update_frontend_tick(int onMainMenu);
void jer_frontend_dim_rect(int x, int y, int w, int h, int shade);	/* FEmain.c */
extern int gJerUpdateForce;	/* -forceupdatecheck (redriver2_psxpc.cpp) */
#ifdef __cplusplus
}
#endif

#define JER_UPD_URL	"https://api.github.com/repos/Njurse/JERICHO/releases/latest"
#define JER_UPD_WAIT_S	5	/* how long the main menu waits for THIS launch's answer */
#define JER_UPD_LINES	96	/* changelog lines kept (after wrapping) */
#define JER_UPD_LINE_W	120

#define JER_UPD_DEV	(jer_build_is_dev() != 0)

#ifdef _WIN32
#define JER_UPD_ASSET	(JER_UPD_DEV ? "JERICHO_Release_dev_win64.zip" : "JERICHO_Release_win64.zip")
#else
#define JER_UPD_ASSET	(JER_UPD_DEV ? "JERICHO_Release_dev_linux-x64.tar.gz" : "JERICHO_Release_linux-x64.tar.gz")
#endif

enum { UPD_IDLE, UPD_RUNNING, UPD_OK, UPD_FAILED };

static int gUpdState = UPD_IDLE;
static int gUpdDecided;
static time_t gUpdStart;
#ifdef _WIN32
static HANDLE gUpdProc;
#else
static pid_t gUpdPid;
#endif

static char gUpdTag[32];		/* "v0.10.5" */
static int gUpdRecut;
static char gUpdLines[JER_UPD_LINES][JER_UPD_LINE_W];
static int gUpdLineCount;
static int gUpdScroll;
static int gUpdForced;		/* this check runs as if the installed build were 0.0.0 */
int gJerUpdateForce;

/* LAYOUT, in the frontend's 640x480 draw space. The left column carries the text,
 * the options sit bottom-right in the stock box (right edge 590, the anchor every
 * frontend list uses), so the notes wrap to UPD_NOTES_W and stop well short of
 * the widest option (~300 wide in the menu font, i.e. from x ~290). */
#define UPD_X		40
#define UPD_NOTES_W	236
#define UPD_TITLE_Y	104
#define UPD_VER_Y	146
#define UPD_NOTES_Y	172
#define UPD_LINE_STEP	18
#define UPD_NOTES_END	436	/* no notes line STARTS below this */
#define UPD_FOOT_Y	446
#define UPD_ROW_BOTTOM	460	/* the options' lower edge */
#define UPD_ROW_STEP	32

static int UpdVisibleLines(void);
static char gUpdHeader[160];

static void UpdPath(char* out, size_t n, const char* leaf)
{
	snprintf(out, n, "%s/CONFIG/%s", jer_root_dir(), leaf);
}

static int UpdEnabled(void)
{
	return jer_config_get_int("jericho", "update_check", 1) != 0;
}

/* ---------------------------------------------------------------- spawn */

void jer_update_start(void)
{
	char part[600];

	if (gUpdState != UPD_IDLE)
		return;
	/* DEV builds only: -forceupdatecheck or jericho.ini update_force=1 shows the
	 * screen against the real latest release, whatever this build is. */
	gUpdForced = jer_build_is_dev() &&
		(gJerUpdateForce || jer_config_get_int("jericho", "update_force", 0) != 0);
	if (!gUpdForced && !UpdEnabled())
		return;

	UpdPath(part, sizeof(part), "update_check.part");
	remove(part);
	gUpdStart = time(NULL);

#ifdef _WIN32
	{
		char cmd[1024];
		STARTUPINFOA si;
		PROCESS_INFORMATION pi;

		snprintf(cmd, sizeof(cmd),
			"curl.exe -fsSL --connect-timeout 3 --max-time 8 -H \"User-Agent: JERICHO\" "
			"-H \"Accept: application/vnd.github+json\" -o \"%s\" " JER_UPD_URL, part);
		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		memset(&pi, 0, sizeof(pi));
		if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
		{
			gUpdState = UPD_FAILED;
			return;
		}
		CloseHandle(pi.hThread);
		gUpdProc = pi.hProcess;
	}
#else
	{
		pid_t pid = fork();

		if (pid < 0)
		{
			gUpdState = UPD_FAILED;
			return;
		}
		if (pid == 0)
		{
			int devnull = open("/dev/null", O_RDWR);

			if (devnull >= 0)
			{
				dup2(devnull, 0);
				dup2(devnull, 1);
				dup2(devnull, 2);
			}
			execlp("curl", "curl", "-fsSL", "--connect-timeout", "3", "--max-time", "8",
				"-H", "User-Agent: JERICHO", "-H", "Accept: application/vnd.github+json",
				"-o", part, JER_UPD_URL, (char*)NULL);
			_exit(127);
		}
		gUpdPid = pid;
	}
#endif
	gUpdState = UPD_RUNNING;
	jer_log("[update] checking %s (%s)\n", JER_UPD_URL, JER_UPD_ASSET);
}

static void UpdPoll(void)
{
	int ok = 0, done = 0;

	if (gUpdState != UPD_RUNNING)
		return;
#ifdef _WIN32
	{
		DWORD code = 0;

		if (WaitForSingleObject(gUpdProc, 0) == WAIT_OBJECT_0)
		{
			done = 1;
			ok = GetExitCodeProcess(gUpdProc, &code) && code == 0;
			CloseHandle(gUpdProc);
			gUpdProc = NULL;
		}
	}
#else
	{
		int status = 0;

		if (waitpid(gUpdPid, &status, WNOHANG) == gUpdPid)
		{
			done = 1;
			ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		}
	}
#endif
	if (!done)
		return;

	if (ok)
	{
		char part[600], json[600];

		UpdPath(part, sizeof(part), "update_check.part");
		UpdPath(json, sizeof(json), "update_check.json");
		remove(json);
		ok = rename(part, json) == 0;
	}
	gUpdState = ok ? UPD_OK : UPD_FAILED;
	jer_log("[update] check %s\n", ok ? "finished" : "failed (offline? using the cached answer, if any)");
}

/* ---------------------------------------------------------------- json */

static char* UpdSlurp(const char* path, long cap)
{
	FILE* f = fopen(path, "rb");
	char* buf;
	long n;

	if (!f)
		return NULL;
	buf = (char*)malloc((size_t)cap + 1);
	if (!buf)
	{
		fclose(f);
		return NULL;
	}
	n = (long)fread(buf, 1, (size_t)cap, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

/* The value after "key": in s (from `from`), or NULL. Good enough for GitHub's
 * own JSON: keys are unique per object and we only read strings. */
static const char* UpdFindKey(const char* from, const char* key)
{
	char pat[64];
	const char* p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	for (p = strstr(from, pat); p; p = strstr(p + 1, pat))
	{
		const char* q = p + strlen(pat);

		while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
			q++;
		if (*q != ':')
			continue;
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
			q++;
		return q;
	}
	return NULL;
}

/* Decode a JSON string at p (which must point at its opening quote) into out.
 * \uXXXX outside ASCII becomes '?': the frontend font is ASCII. */
static const char* UpdString(const char* p, char* out, size_t n)
{
	size_t k = 0;

	if (n)
		out[0] = '\0';
	if (!p || *p != '"')
		return NULL;
	for (p++; *p && *p != '"'; p++)
	{
		char c = *p;

		if (c == '\\' && p[1])
		{
			p++;
			switch (*p)
			{
				case 'n': c = '\n'; break;
				case 'r': c = '\r'; break;
				case 't': c = ' '; break;
				case 'u':
				{
					unsigned v = 0;
					int i;

					for (i = 1; i <= 4 && p[i]; i++)
						v = v * 16 + (unsigned)((p[i] <= '9') ? p[i] - '0' : ((p[i] | 32) - 'a' + 10));
					p += (i - 1);
					c = (v >= 32 && v < 127) ? (char)v : (v == 0x2014 || v == 0x2013 ? '-' : '?');
					break;
				}
				default: c = *p; break;
			}
		}
		if (k + 1 < n)
			out[k++] = c;
	}
	if (n)
		out[k < n ? k : n - 1] = '\0';
	return p;
}

/* "0.10.4..." / "v0.10.4..." -> 3 ints and the remainder. 0 if not a version. */
static int UpdParseVer(const char* s, int v[3], const char** rest)
{
	int i;

	if (*s == 'v' || *s == 'V')
		s++;
	for (i = 0; i < 3; i++)
	{
		if (*s < '0' || *s > '9')
			return 0;
		v[i] = (int)strtol(s, (char**)&s, 10);
		if (i < 2)
		{
			if (*s != '.')
				return 0;
			s++;
		}
	}
	if (rest)
		*rest = s;
	return 1;
}

static int UpdCmp(const int a[3], const int b[3])
{
	int i;

	for (i = 0; i < 3; i++)
		if (a[i] != b[i])
			return a[i] < b[i] ? -1 : 1;
	return 0;
}

/* ------------------------------------------------------------- changelog */

static void UpdAddLine(const char* text, int indent)
{
	char line[JER_UPD_LINE_W];
	const char* p = text;
	int maxW = UPD_NOTES_W - (indent ? jer_text_width(JER_TEXT_FE, 100, "  ") : 0);

	while (*p && gUpdLineCount < JER_UPD_LINES)
	{
		/* greedy word wrap, measured in the font the screen draws with */
		int len = 0, lastSpace = -1;

		while (p[len] && len < JER_UPD_LINE_W - 2)
		{
			memcpy(line, p, (size_t)len + 1);
			line[len + 1] = '\0';
			if (jer_text_width(JER_TEXT_FE, 100, line) > maxW)
				break;
			if (p[len] == ' ')
				lastSpace = len;
			len++;
		}
		if (p[len] && lastSpace > 0)
			len = lastSpace;
		if (len == 0)
			len = 1;
		snprintf(gUpdLines[gUpdLineCount], JER_UPD_LINE_W, "%*s%.*s",
			p == text ? 0 : (indent ? 2 : 0), "", len, p);
		gUpdLineCount++;
		p += len;
		while (*p == ' ')
			p++;
	}
}

/* The release body's changelog: the "## What's new in vX.Y.Z" section build.yml
 * writes (CHANGELOG.md's section for the tag), else a plain "## vX.Y.Z" one, up to
 * the next "## " heading or "---" rule; the whole body if there is neither.
 * Markdown is flattened: headings lose their #s, bullets become "- ", and ** and
 * ` are dropped. */
static const char* UpdFindSection(const char* body, const char* want, const char** endOut)
{
	const char* p;
	const char* start = NULL;
	size_t wl = strlen(want);

	*endOut = NULL;
	for (p = body; *p; )
	{
		const char* eol = strchr(p, '\n');
		size_t n = eol ? (size_t)(eol - p) : strlen(p);

		if (!start)
		{
			if (n >= wl && strncmp(p, want, wl) == 0 && (n == wl || p[wl] == '\r' || p[wl] == ' '))
				start = eol ? eol + 1 : p + n;
		}
		else if ((n >= 3 && strncmp(p, "## ", 3) == 0) || (n >= 3 && strncmp(p, "---", 3) == 0))
		{
			*endOut = p;
			break;
		}
		p = eol ? eol + 1 : p + n;
	}
	return start;
}

static void UpdBuildNotes(const char* body, const char* tag)
{
	const char* p;
	const char* start;
	const char* end;
	char want[64];

	gUpdLineCount = 0;
	gUpdScroll = 0;
	snprintf(want, sizeof(want), "## What's new in %s", tag);
	start = UpdFindSection(body, want, &end);
	if (!start)
	{
		snprintf(want, sizeof(want), "## %s", tag);
		start = UpdFindSection(body, want, &end);
	}
	if (!start)
	{
		start = body;
		end = NULL;
	}
	if (!end)
		end = start + strlen(start);

	/* Markdown hard-wraps its source lines, so a paragraph or bullet is joined
	 * back up (para) and wrapped once, to the screen's width. */
	{
		static char para[4096];
		size_t pk = 0;
		int paraIndent = 0;

		for (p = start; p <= end && gUpdLineCount < JER_UPD_LINES; )
		{
			char raw[600], clean[600];
			const char* eol = (p < end) ? (const char*)memchr(p, '\n', (size_t)(end - p)) : NULL;
			size_t n = eol ? (size_t)(eol - p) : (size_t)(end - p);
			size_t i, k = 0;
			int bullet = 0, heading = 0;
			const char* s;

			if (n >= sizeof(raw))
				n = sizeof(raw) - 1;
			memcpy(raw, p, n);
			raw[n] = '\0';
			p = eol ? eol + 1 : end + 1;

			for (s = raw; *s == ' ' || *s == '\t'; s++)
				;
			if (*s == '#')
			{
				heading = 1;
				while (*s == '#')
					s++;
				while (*s == ' ')
					s++;
			}
			if ((s[0] == '-' || s[0] == '*') && s[1] == ' ')
			{
				s += 2;
				clean[k++] = '-';
				clean[k++] = ' ';
				bullet = 1;
			}
			for (i = 0; s[i] && k + 1 < sizeof(clean); i++)
			{
				if (s[i] == '\r' || s[i] == '`' || s[i] == '*')	/* bold, italics, code */
					continue;
				clean[k++] = s[i];
			}
			clean[k] = '\0';

			/* a blank line, a heading or a new bullet ends the paragraph so far */
			if (k == 0 || heading || bullet)
			{
				if (pk)
					UpdAddLine(para, paraIndent);
				pk = 0;
				paraIndent = bullet;
			}
			if (k == 0)
			{
				/* one blank line between paragraphs, none leading */
				if (gUpdLineCount > 0 && gUpdLineCount < JER_UPD_LINES && gUpdLines[gUpdLineCount - 1][0] != '\0')
					gUpdLines[gUpdLineCount++][0] = '\0';
				continue;
			}
			if (heading)
			{
				UpdAddLine(clean, 0);
				continue;
			}
			if (pk && pk + 1 < sizeof(para))
				para[pk++] = ' ';
			for (i = 0; clean[i] && pk + 1 < sizeof(para); i++)
				para[pk++] = clean[i];
			para[pk] = '\0';
		}
		if (pk && gUpdLineCount < JER_UPD_LINES)
			UpdAddLine(para, paraIndent);
	}
	while (gUpdLineCount > 0 && gUpdLines[gUpdLineCount - 1][0] == '\0')
		gUpdLineCount--;
	if (gUpdLineCount == JER_UPD_LINES)
		snprintf(gUpdLines[JER_UPD_LINES - 1], JER_UPD_LINE_W, "... (more on the release page)");
}

/* ---------------------------------------------------------------- decide */

/* 1 = there is something to show. */
static int UpdEvaluate(const char* path)
{
	char* json = UpdSlurp(path, 512 * 1024);
	char* body = NULL;
	const char* p;
	const char* rest;
	int rv[3], lv[3], cmp, show = 0;

	if (!json)
		return 0;

	UpdString(UpdFindKey(json, "tag_name"), gUpdTag, sizeof(gUpdTag));
	if (!UpdParseVer(gUpdTag, rv, &rest) || *rest != '\0')
		goto out;	/* not a plain vX.Y.Z: a pre-release name, never an update */
	if (gUpdForced)
	{
		lv[0] = lv[1] = lv[2] = 0;	/* pretend to be older than anything */
		rest = "-forced";
	}
	else if (!UpdParseVer(JERICHO_BUILD_VERSION, lv, &rest))
		goto out;	/* no release behind this build: nothing to compare */

	cmp = UpdCmp(rv, lv);
	gUpdRecut = 0;

	if (cmp == 0 && (*rest == '\0' || strcmp(rest, "-dirty") == 0))
	{
		/* exactly this tag: was it rebuilt since we installed it? */
		char inst[600], digest[100] = "", have[100] = "";
		char* ij;

		for (p = UpdFindKey(json, "name"); p; p = UpdFindKey(p + 1, "name"))
		{
			char name[96];

			UpdString(p, name, sizeof(name));
			if (strcmp(name, JER_UPD_ASSET) == 0)
			{
				UpdString(UpdFindKey(p, "digest"), digest, sizeof(digest));
				break;
			}
		}
		UpdPath(inst, sizeof(inst), "installed.json");
		if ((ij = UpdSlurp(inst, 4096)) != NULL)
		{
			UpdString(UpdFindKey(ij, "sha256"), have, sizeof(have));
			free(ij);
		}
		if (strncmp(digest, "sha256:", 7) == 0 && have[0] != '\0'
#ifdef _WIN32
		    && _stricmp(digest + 7, have) != 0)
#else
		    && strcasecmp(digest + 7, have) != 0)
#endif
			gUpdRecut = 1;
	}

	if (cmp <= 0 && !gUpdRecut)
		goto out;

	body = (char*)malloc(64 * 1024);
	if (body)
	{
		UpdString(UpdFindKey(json, "body"), body, 64 * 1024);
		UpdBuildNotes(body, gUpdTag);
		free(body);
	}
	if (gUpdRecut)
		snprintf(gUpdHeader, sizeof(gUpdHeader), "%s was re-cut: it has been rebuilt since you installed it", gUpdTag);
	else if (gUpdForced)
		snprintf(gUpdHeader, sizeof(gUpdHeader), "JERICHO %s  (you have %s)", gUpdTag + 1, JERICHO_BUILD_VERSION);
	else
		snprintf(gUpdHeader, sizeof(gUpdHeader), "JERICHO %s  (you have %d.%d.%d%s)", gUpdTag + 1, lv[0], lv[1], lv[2],
			JER_UPD_DEV ? "-dev" : "");
	show = 1;
	jer_log("[update] %s available: %s\n", gUpdTag, gUpdRecut ? "re-cut" : "newer");
out:
	free(json);
	return show;
}

/* ---------------------------------------------------------------- screen */

static void UpdLeave(void)
{
	jer_frontend_replace(0);	/* the main menu, in this screen's place */
}

#ifndef _WIN32
static void UpdExeDir(char* exe, size_t n, char* dir, size_t dn)
{
	ssize_t k = readlink("/proc/self/exe", exe, n - 1);
	char* slash;

	exe[k > 0 ? k : 0] = '\0';
	snprintf(dir, dn, "%s", exe);
	slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';
	else
		snprintf(dir, dn, ".");
}
#endif

static int UpdDoUpdate(void* ud)
{
	(void)ud;
#ifdef _WIN32
	{
		char exe[MAX_PATH], dir[MAX_PATH], script[MAX_PATH + 32], cmd[2048];
		char* slash;
		STARTUPINFOA si;
		PROCESS_INFORMATION pi;
		FILE* f;

		GetModuleFileNameA(NULL, exe, sizeof(exe));
		snprintf(dir, sizeof(dir), "%s", exe);
		slash = strrchr(dir, '\\');
		if (slash)
			*slash = '\0';
		snprintf(script, sizeof(script), "%s\\update_jericho.bat", dir);
		if ((f = fopen(script, "rb")) == NULL)
		{
			jer_notify("update_jericho.bat is not next to the game - update by hand", 2, 6);
			return 1;
		}
		fclose(f);
		/* cmd /c "<the whole line>": the outer quotes are stripped by cmd itself */
		snprintf(cmd, sizeof(cmd), "cmd.exe /c \"\"%s\" %s --tag %s --wait %lu --relaunch \"%s\"\"",
			script, JER_UPD_DEV ? "dev" : "release", gUpdTag, (unsigned long)GetCurrentProcessId(), exe);
		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir, &si, &pi))
		{
			jer_notify("Could not start update_jericho.bat", 2, 6);
			return 1;
		}
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
#else
	{
		char exe[1024], dir[1024], script[1100], log[1100], pidStr[16];
		pid_t pid;

		UpdExeDir(exe, sizeof(exe), dir, sizeof(dir));
		snprintf(script, sizeof(script), "%s/update_jericho.sh", dir);
		if (access(script, R_OK) != 0)
		{
			jer_notify("update_jericho.sh is not next to the game - update by hand", 2, 6);
			return 1;
		}
		snprintf(log, sizeof(log), "%s/CONFIG/update.log", jer_root_dir());
		snprintf(pidStr, sizeof(pidStr), "%d", (int)getpid());
		pid = fork();
		if (pid < 0)
		{
			jer_notify("Could not start update_jericho.sh", 2, 6);
			return 1;
		}
		if (pid == 0)
		{
			int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			setsid();	/* outlive the game */
			if (fd >= 0)
			{
				dup2(fd, 1);
				dup2(fd, 2);
			}
			execl("/bin/sh", "sh", script, JER_UPD_DEV ? "dev" : "release", "--tag", gUpdTag,
				"--wait", pidStr, "--relaunch", exe, (char*)NULL);
			_exit(127);
		}
	}
#endif
	jer_log("[update] update_jericho started for %s, quitting so it can replace the game\n", gUpdTag);
	fflush(stdout);
	fflush(stderr);
	exit(0);	/* the engine's own way out (atexit PsyX_Shutdown finalises the log) */
	return 1;
}

static int UpdRemind(void* ud)
{
	(void)ud;
	UpdLeave();
	return 1;
}

static int UpdDisable(void* ud)
{
	(void)ud;
	jer_config_set_int("jericho", "update_check", 0);
	jer_notify("Update checks off (JERICHO/CONFIG/jericho.ini: update_check=1 turns them on)", 1, 6);
	UpdLeave();
	return 1;
}

/* Left/Right on any option pages through a long changelog. */
static int UpdScrollNotes(void* ud, int dir)
{
	(void)ud;
	int vis = UpdVisibleLines();

	gUpdScroll += dir * (vis - 1);
	if (gUpdScroll > gUpdLineCount - vis)
		gUpdScroll = gUpdLineCount - vis;
	if (gUpdScroll < 0)
		gUpdScroll = 0;
	return 1;
}

static int UpdBack(void* ud)
{
	(void)ud;
	UpdLeave();
	return 1;
}

static const JER_FE_ITEM kUpdItems[] = {
	{ "Update JERICHO (Restart)", NULL, NULL, UpdDoUpdate, UpdScrollNotes, -1, 0 },
	{ "Remind me next launch", NULL, NULL, UpdRemind, UpdScrollNotes, -1, 0 },
	{ "Disable update checks", NULL, NULL, UpdDisable, UpdScrollNotes, -1, 0 },
};

static const JER_FE_MENU kUpdMenu = {
	"jericho.update",
	kUpdItems, 3,
	NULL, NULL,
	NULL,	/* title: drawn by UpdDrawNotes, top-left, not at the stock (100,150) */
	NULL,
	UpdBack,
	0,		/* row_w: the stock box, so the labels right-align at x 590 */
	UPD_ROW_STEP,
	3,		/* bottom_items: every option pinned to the bottom edge */
	UPD_ROW_BOTTOM,
	0, 0
};

static int UpdVisibleLines(void)
{
	int top = UPD_NOTES_Y + (gUpdForced ? UPD_LINE_STEP : 0);

	return (UPD_NOTES_END - top) / UPD_LINE_STEP + 1;
}

static void UpdDrawNotes(void)
{
	static const char kTitle[] = "A NEW VERSION OF JERICHO IS AVAILABLE.";
	int top = UPD_NOTES_Y, vis, i, line, scale = 150;
	char foot[96];

	/* darken the busy background behind the text column (and only there) */
	jer_frontend_dim_rect(UPD_X - 12, UPD_TITLE_Y - 10, UPD_NOTES_W + 24 + 12,
		UPD_FOOT_Y + 22 - (UPD_TITLE_Y - 10), 96);

	/* the mockup's orange title, shrunk to the dimmed column so it never
	 * reaches the options column */
	while (scale > 100 && jer_text_width(JER_TEXT_FE, scale, kTitle) > UPD_NOTES_W + 12)
		scale -= 10;
	jer_text_draw_at(JER_TEXT_FE, scale, UPD_X, UPD_TITLE_Y, kTitle, 255, 150, 0);

	jer_text_draw_at(JER_TEXT_FE, 110, UPD_X, UPD_VER_Y, gUpdHeader,
		255, gUpdRecut ? 200 : 255, gUpdRecut ? 120 : 255);
	if (gUpdForced)
	{
		jer_text_draw_at(JER_TEXT_FE, 90, UPD_X, top,
			"FORCED CHECK (dev): Update installs this release over this folder", 255, 220, 0);
		top += UPD_LINE_STEP;
	}

	vis = UpdVisibleLines();
	for (i = gUpdScroll, line = top; i < gUpdLineCount && i < gUpdScroll + vis; i++, line += UPD_LINE_STEP)
		jer_text_draw_at(JER_TEXT_FE, 100, UPD_X, line, gUpdLines[i], 255, 255, 255);

	if (gUpdLineCount > vis)
		snprintf(foot, sizeof(foot), "Lines %d-%d of %d   LEFT / RIGHT: scroll", gUpdScroll + 1,
			(gUpdScroll + vis < gUpdLineCount) ? gUpdScroll + vis : gUpdLineCount, gUpdLineCount);
	else
		snprintf(foot, sizeof(foot), "%d line%s", gUpdLineCount, gUpdLineCount == 1 ? "" : "s");
	jer_text_draw_at(JER_TEXT_FE, 80, UPD_X, UPD_FOOT_Y, foot, 170, 170, 170);
}

/* Is the updater's own screen the one on display? FEmain.c holds the attract
 * demo's idle clock while it is: reading a changelog is not idling, and the
 * demo would otherwise start a level from under this screen. */
int jer_update_screen_active(void)
{
	int menu = jer_frontend_find("jericho.update");

	return menu >= 0 && jer_frontend_current_menu() == menu;
}

/* FEmain.c, every frontend frame after the screen is drawn. onMainMenu = the
 * stock main menu is the current screen. */
void jer_update_frontend_tick(int onMainMenu)
{
	int menu;

	if (jer_update_screen_active())
	{
		UpdDrawNotes();
		return;
	}

	UpdPoll();

	menu = jer_frontend_find("jericho.update");

	if (gUpdDecided || gUpdState == UPD_IDLE || !onMainMenu)
		return;

	if (gUpdState == UPD_RUNNING && time(NULL) - gUpdStart < JER_UPD_WAIT_S)
		return;	/* the main menu stays usable meanwhile */

	if (jer_prompt_active())
		return;	/* a host prompt owns the screen; open the notes after it */

	gUpdDecided = 1;
	{
		char path[600];

		UpdPath(path, sizeof(path), "update_check.json");
		if (!UpdEvaluate(path))	/* this launch's answer, or the last one's */
			return;
	}

	if (menu < 0)
	{
		jer_frontend_register_menu(&kUpdMenu);
		menu = jer_frontend_find("jericho.update");
	}
	if (menu >= 0)
		jer_frontend_open_replace(menu);
}
