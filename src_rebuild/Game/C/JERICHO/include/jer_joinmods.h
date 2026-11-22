#ifndef JER_JOINMODS_H
#define JER_JOINMODS_H

/* ------------------------------------------------------------------
 * jer_joinmods.h — get the addons a server needs, with the player's say-so.
 *
 * A join that a server refuses for its mods used to be a dead end ("mod
 * mismatch"). The server now says which mods it runs, and JERICHO works out
 * what would let the player in:
 *
 *   - a mod the server runs that is installed here but switched off is
 *     switched ON, for that session only;
 *   - one that is not installed, or not at the server's version, is fetched
 *     from a mod repo the player has configured (the official repo is built
 *     in; more come from CONFIG/modstore.ini) when one of them offers it, and
 *     switched on for that session;
 *   - a mod switched on here that the server does not run is switched OFF for
 *     that session.
 *
 * NOTHING IS FETCHED OR CHANGED WITHOUT ASKING. One prompt names what would be
 * downloaded and from which repo:
 *
 *   Download & Join   fetch, then relaunch JERICHO with -join <server> and one
 *                     -mod / -nomod per change. The changes are boot flags, so
 *                     the next ordinary start is the player's own mod set.
 *   No                nothing happens.
 *   Keep Them On      the same, and the server's mods are also enabled in
 *                     modlist.ini for good (the player's choice to make).
 *
 * A mod that no configured repo offers, one only at another version than the
 * server's, or one that is built INTO the game (a download alone cannot add
 * code to the exe) is named instead, and nothing happens.
 *
 * The multiplayer module calls jer_joinmods_offer when a join is refused for
 * mods; the rest runs in the frontend's own frame. Nothing here runs unless a
 * server refused this machine, so a game with every mod off is untouched.
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_JOIN_ID_MAX		24	/* = MP_MOD_ID_MAX: what the wire carries */
#define JER_JOIN_VER_MAX	16	/* = MP_MOD_VER_MAX */
#define JER_JOIN_MAX		32	/* = MP_MAX_MODS */
#define JER_JOIN_ADDR_MAX	128	/* host[:port] */

/* One mod, as a server or this machine names it. */
typedef struct JER_JOIN_MOD
{
	char id[JER_JOIN_ID_MAX];
	char version[JER_JOIN_VER_MAX];
} JER_JOIN_MOD;

/* jer_joinmods_offer flags */
#define JER_JOIN_EXACT		0x1	/* the server matches versions too (MP_MODCHECK_EXACT) */
#define JER_JOIN_EXACT_SET	0x2	/* the server refuses our OTHER mods too: its set is the set */

/* A server at `address` (host[:port], the -join form) refused this machine for
 * its mods and said which it runs: `server`, each with its version (binding only
 * under JER_JOIN_EXACT; an empty one means any). `ignore` names mods that count
 * neither way (the multiplayer module's tooling, such as bake).
 *
 * What to switch on, fetch and switch off is worked out against this install's
 * SAVED mod set (modlist.ini), because that is what the relaunch starts from - so
 * a session an earlier offer started, running with other -mod / -nomod flags,
 * can still join a different server in one go.
 *
 * Returns 1 when the offer was taken (the repos are checked, then the player is
 * asked or told why it cannot work), 0 when it was not (bad arguments, or an
 * offer is already running). */
int jer_joinmods_offer(const char* address, const JER_JOIN_MOD* server, int serverCount,
	const char* const* ignore, int ignoreCount, int flags);

/* Non-zero while an offer is being worked: checking repos, waiting on the
 * player, downloading, or about to relaunch. */
int jer_joinmods_busy(void);

/* The engine's main(), once, before jer_init: keeps the command line for the
 * relaunch, and notices a session this flow started (-joinmods) so the player
 * can be told which mods are on for this session only. */
void jer_joinmods_boot(int argc, char** argv);

/* Every frontend frame (jer_modstore_frontend_tick calls it). */
void jer_joinmods_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* JER_JOINMODS_H */
