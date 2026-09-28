/* carhacks/net.h — the MULTIPLAYER ADAPTER: carhacks' car identity + import set
 * over the JERICHO addon net bridge (jer_net.h).
 *
 * Why the bridge and not mp's own wire: mp's protocol carries a car as a bare
 * MODEL NUMBER (MP_ROSTER_ENTRY.model / MP_CARSTATE_ENTRY.model), which cannot
 * express "Rio's model 9" - a model number is ambiguous between cities. Rather
 * than change mp's protocol (out of scope here), carhacks multiplexes its own
 * channel over the bridge mp already exposes, so a session can agree WHICH CITY
 * each machine's car data comes from.
 *
 * The thing this exists to fix: each machine reads a level's car data from ONE
 * foreign city (models.c), and today that is whatever each machine's own
 * carhacks.ini happens to say. With this the HOST is authoritative - it folds its
 * own import set together with every peer's carhacks PICK, broadcasts the result
 * as one SET, and clients adopt it, so every machine loads the same cars instead
 * of trusting that the configs happen to match.
 *
 * With no session live every call here is a no-op (jer_net_is_active() == 0), so
 * the single-player path is untouched.
 *
 * WIRE FORMAT (chkNetSendPacket / chkNetOnRecv):
 *
 *   byte 0  wire version (CHK_NET_WIRE_VERSION) - a peer that does not know it
 *           ignores the message instead of guessing
 *   byte 1  tag (CHK_NET_REQ / CHK_NET_PICK / CHK_NET_SET)
 *   rest    payload
 *
 *   REQ   no payload                  - "send me the set and the car table"
 *   PICK  [city][model]              - "this is the car I am driving"
 *   CARS  [count] then count x        - the host's per-player car table, so EVERY
 *         [playerId][city][model]       machine knows who drives what
 *   SET   [guestCity][count][version] - the host's agreed import set
 *         then count x [slot][city][model]
 *
 * 2 + max(1 + 8*3) = 27 bytes at most, against the bridge's 1024-byte cap.
 */
#ifndef CHK_NET_H
#define CHK_NET_H

#include "driver2.h"

#include "carid.h"

/* The bridge channel. <= 15 chars (jer_net.h). */
#define CHK_NET_CHANNEL		"carhacks.car"

#define CHK_NET_WIRE_VERSION	1

enum
{
	CHK_NET_REQ  = 1,	/* a client asks the host for the set and the car table */
	CHK_NET_PICK = 2,	/* a peer reports the car it is driving */
	CHK_NET_SET  = 3,	/* the host's agreed import set */
	CHK_NET_CARS = 4	/* the host's per-player car table */
};

/* Register the channel and its inbound hook. Called from carhacks_register.
 * Safe to call more than once (a module reload re-runs it). */
void chkNetRegister(JERICHO_CONTEXT* ctx);

/* Tell the session what this machine wants to drive. No-op with no session. */
void chkNetAdvertisePick(int city, int model);

/* Ask the host for the agreed set (a client does this when it joins). */
void chkNetRequestSet(void);

/* Host: recompute the agreed set from the level's set plus every peer's claim,
 * and broadcast it. */
void chkNetPublishSet(void);

/* Host: this machine has just BUILT its level's import set - publish it now so
 * the clients still in the menus adopt it before their own level load. (The
 * level-launch event fires BEFORE the set exists, so it cannot be the moment.) */
void chkNetNotifySetBuilt(void);

/* Apply the session's agreed set into the local import set. Returns the number
 * of entries taken. Called from carhacks.c right before the engine write, and
 * it makes the local config fallback stand down while a session has agreed a
 * set - the host is authoritative. */
int chkNetApplyAgreedSet(void);

/* Is a session's agreed set in force? */
int chkNetHasAgreedSet(void);

/* The guest city the session agreed, or -1. */
int chkNetAgreedGuestCity(void);

/* ---- who is driving what ------------------------------------------------- */

/* Player ids are the bridge's: 0 = host, 1.. = clients. */
#define CHK_NET_MAX_PLAYERS	8

/* What the LOCAL player is driving, read from the engine (the car's resident
 * slot, the city that slot's data came from - CHK_CITY_NATIVE for the level's
 * own - and the model it holds). CHK_MODEL_NONE when there is no car yet. */
CHK_CAR_ID chkNetLocalCar(void);

/* What player `id` (0 = the host) is driving, as that player reported it.
 * Returns 0 when nothing is known about that player yet. */
int chkNetPeerCar(int id, CHK_CAR_ID* out);

/* How many players we have a car identity for (0 while nothing is known). */
int chkNetPeerCount(void);

/* A city index as a log-friendly name ("level" for CHK_CITY_NATIVE/-1). */
const char* chkNetCityName(int city);

#endif /* CHK_NET_H */
