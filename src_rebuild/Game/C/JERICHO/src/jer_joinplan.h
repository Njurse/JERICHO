#ifndef JER_JOINPLAN_H
#define JER_JOINPLAN_H

/* ------------------------------------------------------------------
 * jer_joinplan.h — the engine-free half of jer_joinmods.
 *
 * What would let this machine into a server, worked out from plain tables:
 * the mods the server wants on and off, what is installed here, and what the
 * configured repos offer. No engine, no files, no network, so
 * test/test_joinplan.c compiles it on its own (as C and as C++).
 * ------------------------------------------------------------------ */

#include "jer_joinmods.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What one step of a plan does. */
enum
{
	JER_JOINPLAN_ENABLE = 1,	/* installed here, off: on for this session (-mod) */
	JER_JOINPLAN_DISABLE,		/* on here, the server does not run it (-nomod) */
	JER_JOINPLAN_DOWNLOAD,		/* fetch from a repo, then on for this session */
	JER_JOINPLAN_BLOCK		/* cannot be fixed from here: `why` says what */
};

/* What a repo row is, when its index says (`runtime`); unknown otherwise. */
enum
{
	JER_JOINPLAN_KIND_UNKNOWN = 0,
	JER_JOINPLAN_KIND_LOADABLE,	/* a DLL addon or a data pack: live after a restart */
	JER_JOINPLAN_KIND_BUILTIN	/* linked into the exe: a download cannot add it */
};

/* One module installed here. */
typedef struct JER_JOINPLAN_LOCAL
{
	const char* id;
	const char* version;
	int enabled;
	int core;		/* mandatory: cannot be switched off */
	const char* refusal;	/* non-NULL: this boot refused it, and why */
} JER_JOINPLAN_LOCAL;

/* One row a configured repo offers. */
typedef struct JER_JOINPLAN_OFFER
{
	const char* id;
	const char* name;
	const char* version;
	int repo;		/* which configured repo */
	int canInstall;		/* the store would install it here */
	const char* note;	/* why not, when it would not */
	int kind;		/* JER_JOINPLAN_KIND_* */
} JER_JOINPLAN_OFFER;

typedef struct JER_JOINPLAN_STEP
{
	int action;			/* JER_JOINPLAN_* */
	char id[JER_JOIN_ID_MAX];
	char name[48];
	char version[JER_JOIN_VER_MAX];	/* what a download brings; what a block needed */
	int repo;			/* DOWNLOAD: the repo it comes from */
	char why[160];			/* BLOCK: the player-facing reason */
} JER_JOINPLAN_STEP;

typedef struct JER_JOINPLAN
{
	JER_JOINPLAN_STEP steps[JER_JOIN_MAX * 2];
	int count;
	int downloads;
	int enables;
	int disables;
	int blocks;
} JER_JOINPLAN;

/* What a server's list means here: `need` = the server's mods that are not on in
 * `local` (or, under JER_JOIN_EXACT, not at the server's version - an empty one
 * means any); `extra` = with JER_JOIN_EXACT_SET, the mods on in `local` that the
 * server does not run. `ignore` ids (the multiplayer module's tooling) count
 * neither way, and a module this boot refused is never "on". `local` is the
 * SAVED mod set - what a relaunch starts from - so a session that an earlier
 * offer started can still join a different server. Both outputs hold at most
 * JER_JOIN_MAX rows; the counts are written back. */
void jer_joinplan_diff(const JER_JOIN_MOD* server, int serverCount,
	const char* const* ignore, int ignoreCount, int flags,
	const JER_JOINPLAN_LOCAL* local, int localCount,
	JER_JOIN_MOD* need, int* needCount, JER_JOIN_MOD* extra, int* extraCount);

/* Work out the plan. `flags` are jer_joinmods_offer's (JER_JOIN_EXACT). */
void jer_joinplan_build(JER_JOINPLAN* plan,
	const JER_JOIN_MOD* need, int needCount,
	const JER_JOIN_MOD* extra, int extraCount, int flags,
	const JER_JOINPLAN_LOCAL* local, int localCount,
	const JER_JOINPLAN_OFFER* offers, int offerCount);

/* The consent question for a plan with no blocks. `repoNames` names repos by
 * index (NULL entries read "a mod repo"). */
void jer_joinplan_question(const JER_JOINPLAN* plan, const char* const* repoNames,
	int repoCount, char* out, int max);

/* One line naming why a plan with blocks cannot work. */
void jer_joinplan_refusal(const JER_JOINPLAN* plan, char* out, int max);

/* The relaunch command line: the exe, the original arguments that are safe to
 * keep (-ini, -cdimage, -console, -nointro, -nofmv), -join <address>, one -mod
 * per ENABLE/DOWNLOAD, one -nomod per DISABLE, and -joinmods. `posix` picks the
 * shell the line is for (sh -c, else cmd.exe /c), which decides what may sit
 * inside quotes. Returns 0, writing nothing usable, when any piece is unsafe or
 * the line does not fit. */
int jer_joinplan_cmdline(const JER_JOINPLAN* plan, const char* exe, const char* address,
	int argc, char** argv, int posix, char* out, int max);

/* A module id the store and the loader accept: [A-Za-z_][A-Za-z0-9_]*. */
int jer_joinplan_valid_id(const char* id);

/* host[:port] made only of [A-Za-z0-9.:-], never starting with '-'. */
int jer_joinplan_valid_address(const char* address);

#ifdef __cplusplus
}
#endif

#endif /* JER_JOINPLAN_H */
