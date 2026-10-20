#ifndef JER_MODSTORE_H
#define JER_MODSTORE_H

/* ------------------------------------------------------------------
 * jer_modstore.h — the JERICHO mod store: browse a mod repo, install one,
 * update one, remove one.
 *
 * The counterpart of the whole-game updater (jer_update.c), one level down: the
 * updater moves the GAME between releases, this moves ONE MOD between versions,
 * without touching the exe. The split of responsibility is the updater's:
 *
 *   jer_modstore.c   decides WHAT to do, parses the repo index, owns the menus
 *                    and the job state. It never writes into MODS/ itself.
 *   modstore.sh/.bat does the work: fetch, verify the SHA256, unpack, copy into
 *                    MODS/<id>/ (overwrite only), record what it installed.
 *
 * That boundary is deliberate. Every operation that can destroy a player's work
 * is one reviewed script, next to the same refusals update_jericho already
 * applies, and this side cannot get it wrong behind its back.
 *
 * WHAT THE STORE WILL NOT DO
 *   - install a package whose checksum does not match, or whose mod.toml names
 *     an id other than the one asked for;
 *   - delete a mod it did not install (a hand-made one, or one the release
 *     ships) -- removal needs the provenance record the script wrote;
 *   - downgrade a bundled mod below what the release shipped.
 *
 * This API is CORE-INTERNAL: the store is part of the game, not an addon, and the
 * engine never exposes its own state through it. The copy in JERICHO/sdk/include
 * exists because every engine header is mirrored there (tools/doccheck.py enforces
 * a 1:1 mirror) — an addon should not call these functions, and nothing here is
 * promised to stay put the way the jer_*.h API is.
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

/* The helper script's protocol version. jer_modstore asks the script for its
 * version at boot and says so in the log when they disagree, so a new exe beside
 * an old script (or the reverse) is a message rather than a strange failure. */
#define JER_MODSTORE_SCRIPT_VERSION 1

/* Return codes shared by the action calls. */
enum
{
	JER_MODSTORE_OK = 0,
	JER_MODSTORE_BUSY = 1,			/* one job at a time */
	JER_MODSTORE_NO_SCRIPT = 2,		/* modstore.sh/.bat is not beside the exe */
	JER_MODSTORE_BAD_ID = 3,		/* not a valid mod id */
	JER_MODSTORE_NOT_OURS = 4,		/* not installed by the store */
	JER_MODSTORE_UNAVAILABLE = 5	/* nothing to do (no repo, no index, ...) */
};

/* How a row in the repo browser reads. */
enum
{
	JER_MODSTORE_ST_AVAILABLE = 0,	/* not installed here */
	JER_MODSTORE_ST_INSTALLED,		/* installed, same version */
	JER_MODSTORE_ST_UPDATE,			/* installed, the repo has a newer one */
	JER_MODSTORE_ST_REPLACED,		/* installed, then the release overwrote it */
	JER_MODSTORE_ST_BUNDLED_NEWER,	/* installed newer than the repo offers */
	JER_MODSTORE_ST_NEEDS_CORE,		/* needs a newer JERICHO */
	JER_MODSTORE_ST_NEEDS_DEP		/* needs another mod that is not here */
};

/* One row of the repo browser: what the repo offers, and what this install says
 * about it. Strings point into the store's own storage and stay valid until the
 * next refresh. */
typedef struct JER_MODSTORE_ITEM
{
	const char* id;
	const char* name;
	const char* version;		/* the version the repo offers */
	const char* author;
	const char* description;
	int state;					/* JER_MODSTORE_ST_* */
	const char* note;			/* why it cannot be installed, else NULL */
	int canInstall;				/* 0 = offer the row, but refuse the action */
	int canRemove;				/* the store installed it, so it may delete it */
	int installedVersion;		/* installed somewhere (any version) */
} JER_MODSTORE_ITEM;

/* One row of the Mods table: the engine's module inventory plus what the store
 * knows about it. Strings stay valid until the next call. */
typedef struct JER_MODSTORE_MODULE
{
	const char* id;
	const char* name;
	const char* version;		/* the version the folder carries */
	int enabled;
	const char* refusal;		/* non-NULL when this boot refused it */
	int fromStore;				/* the store installed what is here today */
	const char* storeVersion;	/* the version the store installed, or NULL */
	int replaced;				/* installed by the store, since replaced */
} JER_MODSTORE_MODULE;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Find the helper script beside the exe, create CONFIG/modstore/, and load the
 * cached index. Called from jer_init BEFORE the module scan, so a deferred
 * removal cannot race the loader. */
void jer_modstore_init(const char* rootDir);

/* Every frontend frame, from FEmain.c next to the updater's own tick: polls a
 * running job, redraws the tables, and opens the Mods menu in the mods screen's
 * place. */
/* onMainMenu: non-zero when the MAIN MENU is the current screen and nothing else
 * is up (the frontend passes what it passes the updater). A prompt must not be
 * raised over the update changelog or another presentation screen, so the store
 * waits for this rather than firing on the first free frame. */
void jer_modstore_frontend_tick(int onMainMenu);

/* Non-zero while a job is running (install/remove/index/rebake). The frontend
 * keeps running meanwhile: the helper is a child process, never a block. */
int jer_modstore_busy(void);

/* The last thing the store has to say (a job's result, or why it refused). */
const char* jer_modstore_status(void);

/* Non-zero when the mods menu is the screen on display (FEmain.c uses it to keep
 * the Credits overlay on the right screen). */
int jer_modstore_menu_active(void);

/* Open the Mods menu in place of the frontend screen that asked for it. */
void jer_modstore_open_menu(void);

/* ------------------------------------------------------------------ */
/* Repos                                                               */
/* ------------------------------------------------------------------ */

/* Configured repos: the official one is compiled in, more come from
 * CONFIG/modstore.ini as repo2..repo8 with repoN_name. */
int jer_modstore_repo_count(void);
const char* jer_modstore_repo_name(int index);
const char* jer_modstore_repo_url(int index);

/* Which repo the browser is showing (left/right on the repo row). */
int jer_modstore_repo_selected(void);
void jer_modstore_repo_select(int index);

/* How a repo reads in the browser: "fetched", "not fetched" or "failed", and how
 * old its cached index is ("just now", "5m", "3h", "2d"). Both return a static
 * string that stays valid until the next call, so draw one row at a time. */
const char* jer_modstore_repo_state(int index);
const char* jer_modstore_repo_age(int index);

/* Non-zero when the helper script beside the exe speaks another protocol version
 * than this build (a game update and its script have drifted apart). Reports both
 * versions through the out-params when they are not NULL. */
int jer_modstore_script_mismatch(int* seen, int* expected);

/* Re-read CONFIG/modstore.ini, so an added repo appears without a restart. */
void jer_modstore_repos_reload(void);

/* Fetch the selected repo's index. Returns JER_MODSTORE_OK when the job started;
 * the browser shows the result. */
int jer_modstore_refresh(void);

/* How many rows the selected repo's index has, and one of them. */
int jer_modstore_item_count(void);
const JER_MODSTORE_ITEM* jer_modstore_item(int index);

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

int jer_modstore_install(const char* id);
int jer_modstore_remove(const char* id);
int jer_modstore_rebake(void);

/* ------------------------------------------------------------------ */
/* The installed side (what THIS install has, from the link table)      */
/* ------------------------------------------------------------------ */

/* The module inventory the Mods table draws, straight from jer_module_list so
 * there is one source of truth for enabled/refused state. */
int jer_modstore_module_count(void);
int jer_modstore_module_at(int index, JER_MODSTORE_MODULE* out);

#ifdef __cplusplus
}
#endif

#endif /* JER_MODSTORE_H */
