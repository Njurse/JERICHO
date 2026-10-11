/* carhacks/net.c — the multiplayer adapter (see net.h).
 *
 * The rule it enforces is the engine's own: a level reads car data from ONE
 * foreign city, so the session has to agree which - and the HOST decides.
 *
 * gChkNetAgreed holds the SET PAYLOAD only (no wire version, no tag):
 *     [guestCity][count][version] then count x [slot][city][model]
 */

#include "driver2.h"

#include "jericho.h"
#include "jer_console.h"	/* the unified status console: carhacks' player-facing lines */
#include "jer_events.h"
#include "jer_config.h"
#include "jer_net.h"

#include "system.h"		/* LevelNames[] for the log */
#include "cars.h"		/* car_data[], where the local car's slot lives */
#include "players.h"		/* player[]: whose car is whose */
#include "mission.h"		/* residentCarModels[], GameLevel, GetCarModelSourceCity */

#include "carid.h"
#include "carimport.h"
#include "texture.h"	/* CarSlotResReport: the resource monitor */
#include "net.h"
#include "slotrelease.h"	/* the release decision (pure, tested off-engine) */

#include <string.h>
#include <stdlib.h>		/* atoi: the palette test lever */

/* Player ids are the bridge's (0 = host, 1.. = clients). CHK_NET_MAX_PLAYERS is
 * public (net.h) because carhacks.c sizes its own per-player tables with it. */

/* Payload cap: [guestCity][count][version] + 8 x [slot][city][model]. */
#define CHK_NET_SET_MAX		(3 + CHK_IMPORT_MAX_SLOTS * 3)

static int gChkNetRegistered;
static int gChkNetSession;		/* a session was live last frame */

/* What each player drives. Filled from the wire: a peer's own report of its car
 * (player 0 = the host). Peer rows are per-player, so a THIRD machine can learn
 * player 1's car from the host's broadcast - a client's PICK only reaches the
 * host. */
static CHK_CAR_ID gChkNetPeerPick[CHK_NET_MAX_PLAYERS];
static int gChkNetPeerPickSet[CHK_NET_MAX_PLAYERS];

/* The resident slot each player's car was folded into, -1 = none. A peer leaving is the unload
 * event for its car: the slot is given back (chkImportReleaseSlot) once nobody drives that car
 * any more, which is why the slot is remembered here and not only in the set. */
static int gChkNetPeerSlot[CHK_NET_MAX_PLAYERS];

/* Release decisions that are WAITING (chkNetReleaseSlotIfUnused): a slot nobody names any more
 * but that still has a car on it. Retried every frame until the car is gone (or somebody names
 * the car again). `Last*` is what the previous decision for the slot said, so a retry logs only
 * when something changed. */
static int gChkNetRelPending[CHK_IMPORT_MAX_SLOTS];
static char gChkNetRelWhy[CHK_IMPORT_MAX_SLOTS][48];
static int gChkNetRelLast[CHK_IMPORT_MAX_SLOTS];		/* CHK_REL_VERDICT */
static int gChkNetRelLastCars[CHK_IMPORT_MAX_SLOTS];

/* A peer's car that could not be BUILT here yet, and the frame to try again on. This is the
 * other half of "a load that heals by itself": a slot whose last car is still on it is
 * deferred and freed a frame or two later, and the hot-load pool is shared with every other
 * pick, so "not this time" is usually "not yet". See chkNetLoadRetryArm. `RetryCar` is the
 * car the attempt count belongs to, so a genuinely NEW pick starts over (and is worth
 * announcing) while a peer re-advertising the same car it cannot load stays quiet. */
static unsigned long gChkNetLoadRetryAt[CHK_NET_MAX_PLAYERS];
static int gChkNetLoadAttempts[CHK_NET_MAX_PLAYERS];
static CHK_CAR_ID gChkNetLoadRetryCar[CHK_NET_MAX_PLAYERS];

/* One tick per frame: the retry deadlines and the advert period are counted in frames. Not
 * milliseconds - carhacks takes no platform headers (mp keeps windows.h out of its mod files
 * for winsock ordering, and <time.h> is shadowed by the game's own on this include path) - and
 * the engine's fixed step makes a frame count a good enough heartbeat for both. */
static unsigned long gChkNetFrame;		/* ++ once per frame */
static unsigned long gChkNetAdvertFrame;	/* the frame we last announced on */

/* What WE are driving, as last advertised (mp settles the local car after the
 * session starts, so this changes under us). */
static CHK_CAR_ID gChkNetLocal;
static int gChkNetLocalSet;

/* The session's agreed set payload (see above) and the city it reads from. */
static unsigned char gChkNetAgreed[CHK_NET_SET_MAX];
static int gChkNetAgreedLen;
static int gChkNetAgreedGuest = -1;

static unsigned char gChkNetBuf[64];

/* Is the session's set AGREEMENT on? carhacks.ini: mp_agree_imports (default 1).
 * Off = every machine keeps its own import set, which is what a session where
 * the players deliberately want different cars needs - and what the three-city
 * stress test (tools/chk_mp_foreign.sh) uses. */
static int chkNetAgreeEnabled(void)
{
	return jer_config_get_int("carhacks", "mp_agree_imports", 1) != 0;
}

/* ---------------------------------------------------------------------------
 * Sending
 * ------------------------------------------------------------------------- */

static int chkNetSendPacket(int tag, const unsigned char* payload, int payloadLen)
{
	int len = 0;

	if (!jer_net_is_active())
		return 0;

	gChkNetBuf[len++] = (unsigned char)CHK_NET_WIRE_VERSION;
	gChkNetBuf[len++] = (unsigned char)tag;

	if (payload != NULL && payloadLen > 0)
	{
		if (payloadLen > (int)sizeof(gChkNetBuf) - len)
			payloadLen = (int)sizeof(gChkNetBuf) - len;

		memcpy(gChkNetBuf + len, payload, (size_t)payloadLen);
		len += payloadLen;
	}

	return jer_net_send(CHK_NET_CHANNEL, gChkNetBuf, len);
}

/* ---------------------------------------------------------------------------
 * The host's agreed set
 * ------------------------------------------------------------------------- */

const char* chkNetCityName(int city)
{
	return (city >= 0 && city < CITY_COUNT) ? LevelNames[city] : "level";
}

/* Fold every peer's car into this machine's set, so it LOADS what the other
 * players drive instead of drawing their SLOT as whatever this level happens to
 * hold there. A car already in the set is left alone; a new one takes a spare
 * resident slot. Returns how many were folded.
 *
 * Whether the engine can actually hold the car is the SET's call: it takes a spare
 * resident slot, from whatever city that car belongs to. A set may name several
 * cities (chkImportSetSlot in carimport.c), so a peer's car is no longer refused
 * for coming from a "second" city; only a full set of spare slots is a limit, and
 * then the peer keeps the clean fallback instead (ChkOnCarPeerDraw). */
static void chkNetReapDeparted(void);
static void chkNetBroadcastCars(void);

/* Does any player's car identity still name `car`? The table (every player still here, our own
 * row included), what WE drive now (chkNetLocalCar - the choice, which survives the level
 * consuming the pick; the old check read the consumed pick and so never saw our own car) and a
 * frontend pick not yet consumed. `who` gets the player id, or -1 for this machine's player.
 * Two players can pick the same car, and the canonical rule puts them in ONE slot - so a release
 * must ask this before giving a slot back, or the survivor's car disappears with the leaver. */
static int chkNetCarWantedBy(CHK_CAR_ID car, int* who)
{
	CHK_CAR_ID mine = chkNetLocalCar();
	int i;

	if (who != NULL)
		*who = -1;

	if (!chkCarIdIsSet(car))
		return 0;

	if (chkCarIdIsSet(mine) && chkCarIdEqual(mine, car))
		return 1;

	if (chkImportLocalPickCity() >= 0 &&
		chkCarIdEqual(chkCarId(chkImportLocalPickCity(), chkImportLocalPickModel()), car))
		return 1;

	for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
	{
		if (gChkNetPeerPickSet[i] && chkCarIdEqual(gChkNetPeerPick[i], car))
		{
			if (who != NULL)
				*who = i;

			return 1;
		}
	}

	return 0;
}

/* THE release routine. Every path that can leave a resident slot unused comes here - a peer
 * leaving (chkNetReleaseDeparted), a peer changing car (chkNetUnbindChangedPeers, which covers
 * a switch to a domestic car too), this machine's player changing car (chkNetLocalSwitched, from
 * mp's MP_CARQ_CHOSEN) and a load that did not end in a switch (mplive.c) - so the rule is
 * written once (slotrelease.h, chkReleaseVerdict):
 *
 *   free the slot only when NO player's identity names its car AND NO car is on it.
 *
 *   named          -> kept     (someone draws it, or will again)
 *   cars on it     -> deferred (freeing the mesh/pages a car is drawn with this frame is #12);
 *                                retried every frame by chkNetRetryPendingReleases
 *   neither        -> released (engine + module: chkImportReleaseSlot), every record pointing at
 *                                the slot forgotten, and the resource monitor printed
 *
 * One log line per decision; a retry logs only when its verdict or car count changes. Only the
 * spare slots (CHK_IMPORT_SPARE_FIRST..) are ever released: below them are the level's own
 * civilian slots, which a config import may have knocked and which no player owns.
 *
 * Returns the verdict. */
static int chkNetReleaseDecide(int slot, const char* why, int retry)
{
	CHK_CAR_ID car;
	CHK_REL_VERDICT v;
	int held, wanted, who = -1, cars, first = -1, logIt, k;

	if (slot < CHK_IMPORT_SPARE_FIRST || slot >= CHK_IMPORT_MAX_SLOTS)
		return CHK_REL_NOT_HELD;

	if (why == NULL)
		why = "?";

	held = chkImportSlotHeld(slot);
	car = chkImportSlotId(slot);
	wanted = held && chkNetCarWantedBy(car, &who);
	cars = held ? chkImportCarsOnSlot(slot, &first) : 0;
	v = chkReleaseVerdict(held, wanted, cars);

	logIt = retry ? chkReleaseShouldLog((CHK_REL_VERDICT)gChkNetRelLast[slot], gChkNetRelLastCars[slot], v, cars)
		: (v != CHK_REL_NOT_HELD);

	gChkNetRelLast[slot] = (int)v;
	gChkNetRelLastCars[slot] = cars;

	switch (v)
	{
		case CHK_REL_NOT_HELD:
			gChkNetRelPending[slot] = 0;
			break;

		case CHK_REL_KEEP_WANTED:
			gChkNetRelPending[slot] = 0;

			if (logIt)
			{
				if (who >= 0)
					printInfo("[carhacks] release: slot %d (%s model %d) kept - still named by player %d (%s)\n",
						slot, chkNetCityName(chkCarIdCity(car)), chkCarIdModel(car), who, why);
				else
					printInfo("[carhacks] release: slot %d (%s model %d) kept - still named by this machine's player (%s)\n",
						slot, chkNetCityName(chkCarIdCity(car)), chkCarIdModel(car), why);
			}
			break;

		case CHK_REL_DEFER_CARS:
			if (!gChkNetRelPending[slot] || !retry)
			{
				gChkNetRelPending[slot] = 1;
				snprintf(gChkNetRelWhy[slot], sizeof(gChkNetRelWhy[slot]), "%s", why);
			}

			if (logIt)
				printInfo("[carhacks] release: slot %d (%s model %d) deferred - car %d still on it (%d car%s) (%s); retrying each frame\n",
					slot, chkNetCityName(chkCarIdCity(car)), chkCarIdModel(car),
					first, cars, (cars == 1) ? "" : "s", why);
			break;

		case CHK_REL_FREE:
			gChkNetRelPending[slot] = 0;

			/* forget every record pointing at this slot, then give it back */
			for (k = 0; k < CHK_NET_MAX_PLAYERS; k++)
			{
				if (gChkNetPeerSlot[k] == slot)
					gChkNetPeerSlot[k] = -1;
			}

			printInfo("[carhacks] release: slot %d (%s model %d) released - nobody names it and no car is on it (%s)\n",
				slot, chkNetCityName(chkCarIdCity(car)), chkCarIdModel(car), why);

			chkImportReleaseSlot(slot);

			/* the monitor, after every release: what is still held, and the pool's state */
			CarSlotResReport();
			break;
	}

	return (int)v;
}

int chkNetReleaseSlotIfUnused(int slot, const char* why)
{
	return chkNetReleaseDecide(slot, why, 0);
}

/* Every frame: retry the deferred decisions (a slot whose last car may have gone). */
static void chkNetRetryPendingReleases(void)
{
	int slot;

	for (slot = CHK_IMPORT_SPARE_FIRST; slot < CHK_IMPORT_MAX_SLOTS; slot++)
	{
		char why[48];

		if (!gChkNetRelPending[slot])
			continue;

		memcpy(why, gChkNetRelWhy[slot], sizeof(why));
		why[sizeof(why) - 1] = '\0';

		chkNetReleaseDecide(slot, why, 1);
	}
}

/* ---------------------------------------------------------------------------
 * Retrying a peer's car that could not be loaded yet
 * -------------------------------------------------------------------------
 *
 * WHY THIS EXISTS. Folding a peer's car into the set needs a free spare resident slot and,
 * for a car this level did not build itself, a hot-load block. BOTH can be busy for a moment:
 * a slot whose last car is still drawn on it is DEFERRED (chkNetRetryPendingReleases frees it
 * a frame or two later), and the hot-load blocks are shared with every other pick. A refusal
 * used to be FINAL - the peer stayed drawn as this level's own car of the same number for the
 * rest of the match - and the only thing that ever retried it was the peer's own pick advert
 * seconds later, if it arrived at all. This is the retry that makes the load heal by itself.
 *
 * The delay is deliberate: the NEXT frame is too eager (each attempt walks the set and can log,
 * and the thing that frees a slot is the release pass, which itself runs every frame), and the
 * peer's advert is far too slow. A short beat is enough for the deferred release to land. It is
 * bounded - a slot can also be held for a reason that outlives the wait - and after
 * CHK_NET_LOAD_RETRY_MAX attempts the ask is dropped and the refusal is logged once as final. */
#define CHK_NET_LOAD_RETRY_FRAMES	10	/* ~0.4 s at this rate - see the note above */
#define CHK_NET_LOAD_RETRY_MAX		20	/* attempts before the ask is dropped (~8 s) */

enum
{
	CHK_NET_RETRY_DONE   = -2,	/* already gave up on this car: say nothing */
	CHK_NET_RETRY_GAVEUP = -1,	/* the wait just ran out: say so once */
	CHK_NET_RETRY_AGAIN  =  0,	/* retrying, quietly */
	CHK_NET_RETRY_FIRST  =  1	/* the first attempt: worth saying */
};

/* The car is loaded (or needs nothing): stop waiting for it, and let a later ask be "first"
 * again. */
static void chkNetLoadRetryClear(int p)
{
	gChkNetLoadRetryAt[p] = 0;
	gChkNetLoadAttempts[p] = 0;
	gChkNetLoadRetryCar[p] = chkCarId(CHK_CITY_NATIVE, CHK_MODEL_NONE);
}

/* Could not load `p`'s car this time: try again after the delay. Returns CHK_NET_RETRY_* so the
 * caller can decide what, if anything, to say - a first attempt is news, a retry is not, and
 * the give-up is said exactly once. A new car for this player (they picked something else)
 * resets the count, so it is announced again. */
static int chkNetLoadRetryArm(int p)
{
	if (!chkCarIdEqual(gChkNetPeerPick[p], gChkNetLoadRetryCar[p]))
	{
		gChkNetLoadRetryCar[p] = gChkNetPeerPick[p];
		gChkNetLoadAttempts[p] = 0;
	}

	if (gChkNetLoadAttempts[p] >= CHK_NET_LOAD_RETRY_MAX)
	{
		gChkNetLoadRetryAt[p] = 0;

		/* the transition out of the wait is the one time this is worth saying */
		if (gChkNetLoadAttempts[p] == CHK_NET_LOAD_RETRY_MAX)
		{
			gChkNetLoadAttempts[p]++;
			return CHK_NET_RETRY_GAVEUP;
		}

		return CHK_NET_RETRY_DONE;
	}

	gChkNetLoadAttempts[p]++;
	gChkNetLoadRetryAt[p] = gChkNetFrame + CHK_NET_LOAD_RETRY_FRAMES;

	return (gChkNetLoadAttempts[p] == 1) ? CHK_NET_RETRY_FIRST : CHK_NET_RETRY_AGAIN;
}

/* Every frame: run the fold again when any waiting car is due. One pass covers every waiting
 * player - the fold already walks them all, and skips a car that is now in a slot. */
static void chkNetRetryPendingLoads(void)
{
	int p, due = 0;

	for (p = 0; p < CHK_NET_MAX_PLAYERS; p++)
	{
		if (gChkNetLoadRetryAt[p] != 0 && gChkNetFrame >= gChkNetLoadRetryAt[p])
		{
			due = 1;
			break;
		}
	}

	if (due)
		chkNetFoldPeerCars();
}

/* CHK_REFUSE_LOAD=<n>: hold back the first n load attempts for EVERY player, so the retry above
 * is exercised on purpose. Without a lever like this the retry is only reachable by luck - a
 * slot has to be busy at the exact moment a pick arrives - which is the same reason the module
 * carries CHK_DROP_ADVERT and CHK_FORCE_CAR. Logged, so a run that used it says so. */
static int chkNetTestRefuseLoad(int p)
{
	static int want = -2;
	static int refused[CHK_NET_MAX_PLAYERS];

	if (want == -2)
	{
		const char* s = getenv("CHK_REFUSE_LOAD");

		want = (s != NULL) ? atoi(s) : -1;
	}

	if (want <= 0 || p < 0 || p >= CHK_NET_MAX_PLAYERS || refused[p] >= want)
		return 0;

	refused[p]++;

	printInfo("[carhacks/net] CHK_REFUSE_LOAD: refusing player %d's car (attempt %d of %d) to exercise the retry\n",
		p, refused[p], want);

	return 1;
}

/* A new level: the set was just reset (chkImportReset), so the slot records of the old level are
 * meaningless - and the level-build fold runs right after this, inside the car-data hook, where a
 * stale record must not trigger a release. Forget them, and any deferred decision with them. */
void chkNetOnLevelReset(void)
{
	int i;

	for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
	{
		gChkNetPeerSlot[i] = -1;
		chkNetLoadRetryClear(i);	/* the new level's build does this itself */
	}

	for (i = 0; i < CHK_IMPORT_MAX_SLOTS; i++)
	{
		gChkNetRelPending[i] = 0;
		gChkNetRelLast[i] = CHK_REL_NOT_HELD;
		gChkNetRelLastCars[i] = 0;
	}
}

/* A player whose car identity no longer names the car in the slot recorded for them has
 * CHANGED CAR (or left - see below): forget the record and let the release routine decide about
 * the old slot. This is the peer-repick half of the lifecycle, and it is deliberately not inside
 * the fold's import loop: that loop skips a player whose new car needs no import (a domestic car
 * the level holds, the `continue` near the top), which is exactly the switch that used to leave
 * the old import's slot held for the rest of the session. */
static void chkNetUnbindChangedPeers(void)
{
	int p;

	for (p = 0; p < CHK_NET_MAX_PLAYERS; p++)
	{
		int slot = gChkNetPeerSlot[p];
		char why[48];

		if (slot < 0)
			continue;

		if (gChkNetPeerPickSet[p] && chkCarIdEqual(chkImportSlotId(slot), gChkNetPeerPick[p]))
			continue;		/* still that car */

		gChkNetPeerSlot[p] = -1;

		snprintf(why, sizeof(why), "player %d changed car", p);
		chkNetReleaseSlotIfUnused(slot, why);
	}
}

/* The unload half of the car table: for every player the previous table named and the current one
 * does not, forget them and offer their car's slot to the release routine - which keeps it if
 * somebody else still drives that car, and defers it while their car is still in the world.
 *
 * This is "a peer leaves -> release its slot" from the resource-lifetime table in MP_ADAPTER.md.
 * Before it, a departed player's car stayed in its resident slot (and its lower-half pool pages,
 * baked page index and geometry stayed consumed) for the rest of the session; and the first
 * version freed it at once, while the leaver's car was still being drawn (#12). */
void chkNetReleaseDeparted(const int* seen)
{
	int i;

	for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
	{
		int slot;
		char why[48];

		if (seen[i] || !gChkNetPeerPickSet[i])
			continue;			/* still here, or never was */

		printInfo("[carhacks/net] player %d left the session\n", i);

		gChkNetPeerPickSet[i] = 0;
		gChkNetPeerPick[i] = chkCarId(CHK_CITY_NATIVE, CHK_MODEL_NONE);

		slot = gChkNetPeerSlot[i];
		gChkNetPeerSlot[i] = -1;

		if (slot < 0 || slot >= CHK_IMPORT_MAX_SLOTS)
			continue;

		snprintf(why, sizeof(why), "player %d left", i);
		chkNetReleaseSlotIfUnused(slot, why);
	}
}

/* This machine's player is now driving `now` (it was `was`) - a successful mid-match switch, told
 * by mp's MP_CARQ_CHOSEN after the car on the road really changed (mplive.c). The shared identity
 * moves here and ONLY here, after the load succeeded (an advert for a car that then failed to
 * load told every machine to build a car nobody drove):
 *
 *   the choice (chkImportSetChosen), our own row of the table, the advert dedupe of
 *   chkNetOnFrame, the advert itself, and on the host the table broadcast and the agreed set;
 *   then the old car's slot goes to the release routine. */
void chkNetLocalSwitched(CHK_CAR_ID was, CHK_CAR_ID now)
{
	int me = jer_net_local_player();

	if (!chkCarIdIsSet(now))
		return;

	chkImportSetChosen(now);

	if (me >= 0 && me < CHK_NET_MAX_PLAYERS)
	{
		gChkNetPeerPick[me] = now;
		gChkNetPeerPickSet[me] = 1;
	}

	gChkNetLocal = now;		/* chkNetOnFrame would otherwise advertise it a second time */
	gChkNetLocalSet = 1;

	chkNetAdvertisePick((int)now.city, (int)now.model);

	if (jer_net_is_host())
	{
		chkNetBroadcastCars();
		chkNetPublishSet();
	}

	if (chkCarIdIsSet(was) && !chkCarIdEqual(was, now))
	{
		int slot = chkImportSlotOfCar(was);

		if (slot >= 0)
			chkNetReleaseSlotIfUnused(slot, "we changed car");
	}
}

int chkNetFoldPeerCars(void)
{
	int folded = 0, p, slot;

	/* First the players who are no longer in the car their slot was recorded for: whatever
	 * the loop below does with their NEW car (import it, or skip it as domestic), the OLD
	 * slot has to be offered back. */
	chkNetUnbindChangedPeers();

	/* ON A CLIENT, the host's car (player 0) is folded HERE TOO, like any other peer's -
	 * this is #13.
	 *
	 * It used to be left to the host's agreed set, and a set is applied once per level
	 * (CHK_NET_SET above). So a host who changed car MID-MATCH stayed, on the client,
	 * whatever that client's level had for the player - and since a level pools only its own
	 * list, the client drew them out of its OWN car's resident slot:
	 *
	 *   [carhacks/net] peer 0 drives HAVANA model 4, but this machine draws RIO model 1 in
	 *   slot 0 - using that car's own colours (theirs is not loaded here)
	 *
	 * Six times in one ten-switch run, on every switch. The loop below is exactly the
	 * machinery that fixes it: it resolves the car to a canonical spare and hot-loads it
	 * (geometry, cosmetics, pages and rows), and records the slot so the release path -
	 * chkNetUnbindChangedPeers, which already covers player 0 - gives it back when the host
	 * switches again.
	 *
	 * The host folds from p = 1, because player 0 is ITSELF: folding your own car is not a
	 * thing. The pre-pass that only REMEMBERED a slot for the host's car went with this
	 * change - the loop computes the slot, so remembering it separately was the half of the
	 * job that could be done without importing anything, and it recorded a slot the release
	 * path could then free while the car was still being drawn. */
	for (p = jer_net_is_host() ? 1 : 0; p < CHK_NET_MAX_PLAYERS; p++)
	{
		int already = 0;

		if (!gChkNetPeerPickSet[p])
		{
			/* No identity for this player (they left, or nothing was reported yet): there
			 * is nothing to load, and a retry armed for them must not survive - it would
			 * keep the fold running every frame for a player who is gone. */
			chkNetLoadRetryClear(p);
			continue;
		}

		/* A car the level's OWN city ships needs no import WHEN THE LEVEL ALREADY
		 * HOLDS IT: its own list has that model (CHK_CITY_NATIVE names the level's
		 * city, so both spellings are the same question). That also keeps an
		 * identity that has not settled yet (a car reported before a level exists,
		 * i.e. model CHK_MODEL_NONE / a bare 0 the level does pool) from claiming a
		 * spare resident slot.
		 *
		 * WHAT IT MUST NOT DO is assume the level holds everything its city ships.
		 * A level pools ONE list, and a model outside it has to be brought in like
		 * a guest -- otherwise the peer's machine resolves a car the host cannot
		 * build and InitPlayer falls back to resident slot 0, i.e. the host draws
		 * the client in the HOST's own car: "I picked car 12 and spawned as car 1". */
		{
			int pcity = chkCarIdCity(gChkNetPeerPick[p]);
			int pmodel = chkCarIdModel(gChkNetPeerPick[p]);
			int holds;

			if (pmodel < 0)
			{
				chkNetLoadRetryClear(p);	/* nothing settled yet: not a car to import */
				continue;
			}

			if (pcity < 0)
				pcity = GameLevel;	/* CHK_CITY_NATIVE = the level's own city */

			/* 1 = the level's pool has it, -1 = cannot tell (this also runs when the
			 * host publishes its set, outside the hook that hands over the resident
			 * list): BOTH mean "do not import". Only a provable 0 -- an own-city car
			 * the level really does not hold -- goes to the import path. */
			holds = chkImportLevelHoldsModel(pmodel);

			if (pcity == GameLevel && holds != 0)
			{
				chkNetLoadRetryClear(p);	/* in the level's pool (or unknown): no import */
				continue;
			}
		}

		for (slot = 0; slot < CHK_IMPORT_MAX_SLOTS; slot++)
		{
			if (chkCarIdEqual(chkImportSlotId(slot), gChkNetPeerPick[p]))
			{
				already = 1;
				gChkNetPeerSlot[p] = slot;	/* two players can share one car: one slot */
				break;
			}
		}

		if (already)
		{
			/* The car is already in the set. If its geometry is really there the load is
			 * DONE; if it is NOT (a build that failed, or a level that had not loaded when
			 * this car was folded in), then THIS fold is the retry - try the build again and
			 * keep waiting (or give up) on the same schedule. */
			int slot0 = gChkNetPeerSlot[p];

			if (JerCarSlotUsable(slot0) || chkImportHotLoad(slot0) > 0)
			{
				chkNetLoadRetryClear(p);
			}
			else if (chkNetLoadRetryArm(p) == CHK_NET_RETRY_GAVEUP)
			{
				/* The wait ran out with the car named in the set but no geometry: from here
				 * the peer looks like this level's own car of the same number, which is the
				 * one symptom a player can see, so it is said once. */
				jer_console_log("player %d's car (%s model %d) could not be loaded here - they may look like this level's own car",
					p, chkNetCityName((int)gChkNetPeerPick[p].city), (int)gChkNetPeerPick[p].model);
			}

			continue;
		}

		/* The slot is CANONICAL (chkImportCanonicalSlot), derived from every pick in
		 * the session by owning player id -- never "the first free spare at the moment
		 * this peer's pick arrived". Arrival order is what used to give the same car
		 * different slots on different machines, taking the baked page indices and the
		 * palette rows with it. Ordering by player id is also append-only: a joiner's
		 * id is higher, so its car cannot displace one that is already in a slot. */
		{
			int need = 0;

			slot = chkImportCanonicalSlot(gChkNetPeerPick[p], &need);

			if (slot < 0 || !chkImportSlotFree(slot) || chkNetTestRefuseLoad(p))
			{
				int now = chkNetLoadRetryArm(p);

				/* A busy slot is usually a WAIT, not a refusal: the slot's last car is
				 * still on it, and the release pass frees it a frame or two later - so the
				 * retry (chkNetRetryPendingLoads) gets it. Say so once on screen. A retry
				 * is not news, so the notice stays quiet while the retries run; only when
				 * the wait runs out does the refusal on screen become final.
				 *
				 * The WAIT says "busy" and the FINAL says "no ... came free": the rigs
				 * forbid the terminal phrase (mp_tries T2: with slot reuse a switch must
				 * NOT exhaust the spares), so a transient that heals must not wear it. */
				if (now == CHK_NET_RETRY_FIRST)
					jer_console_log("[carhacks/net] player %d wants %s model %d, but every spare resident "
						"slot is busy right now - retrying",
						p, chkNetCityName((int)gChkNetPeerPick[p].city), (int)gChkNetPeerPick[p].model);
				else if (now == CHK_NET_RETRY_GAVEUP)
					jer_console_log("[carhacks/net] player %d wants %s model %d, but no spare resident "
						"slot came free - not importing it",
						p, chkNetCityName((int)gChkNetPeerPick[p].city), (int)gChkNetPeerPick[p].model);

				continue;
			}
		}

		if (chkImportSetSlot(slot, gChkNetPeerPick[p]))
		{
			folded++;

			gChkNetPeerSlot[p] = slot;	/* remember it: a peer leaving gives this slot back */

			printInfo("[carhacks/net] player %d's car (%s model %d) -> resident slot %d\n",
				p, chkNetCityName((int)gChkNetPeerPick[p].city),
				(int)gChkNetPeerPick[p].model, slot);

			/* A car folded in AFTER this machine loaded its level has no geometry:
			 * the level's own build ran against a heap that is live with its
			 * allocations, so without this the peer is drawn as the level's own car
			 * of the same number -- "it appears domestic to all other players and
			 * the host". chkImportHotLoad pushes the set into the engine's arrays,
			 * reads the city and builds that one slot into the engine's own pool;
			 * the resolvers then find it (including mp's own, on its next tick). */
			chkImportHotLoad(slot);

			/* Was it really built? "Yes" ends the wait for this car. "No" is ONE of
			 * two things, told apart by whether a level exists yet - and either way the
			 * ask is a WAIT, not a refusal, so it goes on the retry schedule rather than
			 * standing final (chkNetRetryPendingLoads is what makes it heal). */
			if (JerCarSlotUsable(slot))
			{
				chkNetLoadRetryClear(p);
			}
			else if (!chkImportEngineKnown())
			{
				/* No level yet: the pick arrived before this machine had a car list to
				 * fold it into, so the level's own build covers that slot when it runs -
				 * and the retry clears the moment it does. There is nothing to report,
				 * so this is said once, quietly. */
				if (chkNetLoadRetryArm(p) == CHK_NET_RETRY_FIRST)
					printInfo("[carhacks/net] player %d's car (%s model %d) folded into slot %d "
						"before this machine had a level - its own build will load it\n",
						p, chkNetCityName((int)gChkNetPeerPick[p].city),
						(int)gChkNetPeerPick[p].model, slot);
			}
			else
			{
				/* A level is up and this slot still has no geometry: the level's pool
				 * or the hot-load blocks are busy for a moment. Retry; only a wait that
				 * never succeeds leaves the peer drawn as this level's own car of the
				 * same number - the one symptom a player can see, so it is said on
				 * screen too, once, when the wait finally gives up. */
				int now = chkNetLoadRetryArm(p);

				if (now == CHK_NET_RETRY_FIRST)
				{
					printInfo("[carhacks/net] player %d's car (%s model %d) is in the set for slot %d, "
						"but this level could not build it yet - retrying\n",
						p, chkNetCityName((int)gChkNetPeerPick[p].city),
						(int)gChkNetPeerPick[p].model, slot);

					jer_console_log("player %d's car (%s model %d) cannot be loaded here yet - retrying",
						p, chkNetCityName((int)gChkNetPeerPick[p].city),
						(int)gChkNetPeerPick[p].model);
				}
				else if (now == CHK_NET_RETRY_GAVEUP)
				{
					printInfo("[carhacks/net] player %d's car (%s model %d) is in the set for slot %d, "
						"but this level never built it - carrying it to the next level\n",
						p, chkNetCityName((int)gChkNetPeerPick[p].city),
						(int)gChkNetPeerPick[p].model, slot);

					jer_console_log("player %d's car (%s model %d) could not be loaded here - they may look like this level's own car",
						p, chkNetCityName((int)gChkNetPeerPick[p].city),
						(int)gChkNetPeerPick[p].model);
				}
			}
		}
	}

	return folded;
}

static void chkNetBuildSetPayload(void)
{
	int slot, len = 0, count = 0, countAt, guest = chkImportGuestCity();

	gChkNetAgreed[len++] = (unsigned char)((guest < 0) ? CHK_CITY_NATIVE : guest);
	countAt = len++;				/* count, patched below */
	gChkNetAgreed[len++] = (unsigned char)chkImportSetVersion();

	for (slot = 0; slot < CHK_IMPORT_MAX_SLOTS && len + 3 <= CHK_NET_SET_MAX; slot++)
	{
		CHK_CAR_ID id = chkImportSlotId(slot);

		if (id.model == CHK_MODEL_NONE)
			continue;

		gChkNetAgreed[len++] = (unsigned char)slot;
		gChkNetAgreed[len++] = id.city;
		gChkNetAgreed[len++] = id.model;
		count++;
	}

	gChkNetAgreed[countAt] = (unsigned char)count;
	gChkNetAgreedLen = len;
	gChkNetAgreedGuest = guest;
}

void chkNetPublishSet(void)
{
	if (!chkNetAgreeEnabled() || !jer_net_is_active() || !jer_net_is_host())
		return;

	chkNetFoldPeerCars();
	chkNetBuildSetPayload();

	chkNetSendPacket(CHK_NET_SET, gChkNetAgreed, gChkNetAgreedLen);

	printInfo("[carhacks/net] published the agreed set: %d entr%s, %d guest cit%s\n",
		(int)gChkNetAgreed[1], (gChkNetAgreed[1] == 1) ? "y" : "ies",
		chkImportGuestCityCount(), (chkImportGuestCityCount() == 1) ? "y" : "ies");
}

int chkNetHasAgreedSet(void)
{
	/* True only where the session's set APPLIES: on a CLIENT that has received
	 * one. On the host this buffer is its own set, already applied - so the host
	 * keeps building from its own config (it is the authority). */
	return chkNetAgreeEnabled() && jer_net_is_active() && !jer_net_is_host() &&
		(gChkNetAgreedLen > 0);
}

int chkNetAgreedGuestCity(void)
{
	return gChkNetAgreedGuest;
}

int chkNetApplyAgreedSet(void)
{
	int i, n = 0, at, count;

	/* only a client adopts - see chkNetHasAgreedSet */
	if (!chkNetHasAgreedSet())
		return 0;

	count = (int)gChkNetAgreed[1];
	at = 3;						/* past guestCity, count, version */

	if (count > CHK_IMPORT_MAX_SLOTS)
		count = CHK_IMPORT_MAX_SLOTS;

	for (i = 0; i < count && at + 2 < gChkNetAgreedLen; i++, at += 3)
	{
		int slot = gChkNetAgreed[at];
		CHK_CAR_ID id = chkCarId(gChkNetAgreed[at + 1], gChkNetAgreed[at + 2]);

		if (chkCarIdEqual(chkImportSlotId(slot), id))
			continue;		/* already ours: nothing to adopt, and nothing to build */

		if (!chkImportSetSlot(slot, id))
			continue;

		n++;

		/* A joiner adopting the session's set needs the CARS, not just the table: its
		 * own level loaded before it knew about them - that is what joining a match in
		 * progress means - so this is where they get built. Same call the fold uses, so
		 * the geometry, the cosmetics, the texture pages and their rows land together,
		 * and the peer stops being drawn as the level's own car of that number. */
		if (chkImportHotLoad(slot) <= 0)
			printInfo("[carhacks/net] slot %d (%s model %d) is in the agreed set but this level "
				"could not build it - the peer driving it may look like the level's own car\n",
				slot, chkNetCityName((int)chkCarIdCity(id)), (int)chkCarIdModel(id));
	}

	printInfo("[carhacks/net] adopting the session's agreed set: %d entr%s applied "
		"(each carries its own source city)\n",
		n, (n == 1) ? "y" : "ies");

	return n;
}

/* ---------------------------------------------------------------------------
 * Who is driving what
 * ------------------------------------------------------------------------- */

/* What the LOCAL player is driving, read from the ENGINE rather than from the
 * pick (which only the car-select menu sets, and not at all inside a match): the
 * local car's resident slot, the city that slot's data came from, and the model
 * it holds. A slot the level imported from elsewhere names its source city; any
 * other slot is the level's own car, which needs no import on any machine, so it
 * reports CHK_CITY_NATIVE.
 *
 * EXCEPT WHEN THEY CHOSE ONE, and then the choice decides. The seat is not the
 * identity: mp seats a late joiner in a car (that is how the engine gets a player into
 * the world at all), and reporting THAT as what the player drives replaced their imported
 * car on every machine the moment the engine re-seated them:
 *
 *   [carhacks/net] player 1 drives VEGAS model 3        <- the pick, correctly advertised
 *   [mp] late joiner: player 1 -> slot 1 model 3 (city 0)
 *   [mp] player 1 changed car: slot 7 -> 2 (the session city model 3), mesh rebuilt
 *   [carhacks/net] player 1 drives level model 3        <- the SEAT, advertised from here on
 *
 * "It loaded in correctly but then got replaced by the chicago slot 3". The choice is what
 * the other machines must build, so it wins; the seat stays an engine detail. */
CHK_CAR_ID chkNetLocalCar(void)
{
	CAR_DATA* cp;
	int slot, city, model;

	/* the choice, when there is one (chkImportChosenCar survives the level consuming the
	 * pick - see carimport.c) */
	if (chkImportChosenIsSet())
	{
		CHK_CAR_ID chosen = chkImportChosenCar();

		if (chkCarIdIsSet(chosen))
			return chosen;
	}

	if (player[0].playerCarId < 0 || player[0].playerCarId >= MAX_CARS)
		return chkCarId(CHK_CITY_NATIVE, CHK_MODEL_NONE);

	cp = &car_data[player[0].playerCarId];
	slot = cp->ap.model;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return chkCarId(CHK_CITY_NATIVE, CHK_MODEL_NONE);

	model = residentCarModels[slot];

	if (model < 0)
		return chkCarId(CHK_CITY_NATIVE, CHK_MODEL_NONE);

	city = GetCarModelSourceCity(slot);

	return chkCarId((city >= 0) ? city : CHK_CITY_NATIVE, model);
}

int chkNetPeerCar(int id, CHK_CAR_ID* out)
{
	if (id < 0 || id >= CHK_NET_MAX_PLAYERS || !gChkNetPeerPickSet[id])
		return 0;

	if (out != NULL)
		*out = gChkNetPeerPick[id];

	return 1;
}

/* How many players we have a car identity for: one more than the highest player
 * id known (0 = the host). 0 while nothing is known yet. */
int chkNetPeerCount(void)
{
	int i, n = 0;

	for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
	{
		if (gChkNetPeerPickSet[i])
			n = i + 1;
	}

	return n;
}

static void chkNetLogCars(const char* who)
{
	char buf[192];
	int i, at = 0;

	buf[0] = '\0';

	for (i = 0; i < CHK_NET_MAX_PLAYERS && at < (int)sizeof(buf) - 32; i++)
	{
		if (!gChkNetPeerPickSet[i])
			continue;

		at += snprintf(buf + at, sizeof(buf) - (size_t)at, "%s%d=%s model %d",
			(at > 0) ? ", " : "", i,
			chkNetCityName((int)gChkNetPeerPick[i].city),
			(int)gChkNetPeerPick[i].model);
	}

	printInfo("[carhacks/net] cars (%s): %s\n", who, (buf[0] != '\0') ? buf : "none known");
}

/* The host's per-player table, broadcast so EVERY machine knows every player's
 * car. Without it a client's PICK stops at the host and a third machine never
 * learns what the other two are driving - which is what a peer's car has to be
 * identified against before it can be drawn (or rejected) as the right vehicle. */
static void chkNetBroadcastCars(void)
{
	unsigned char pay[1 + CHK_NET_MAX_PLAYERS * 3];
	int len = 1, i, count = 0;

	if (!jer_net_is_active() || !jer_net_is_host())
		return;

	for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
	{
		CHK_CAR_ID id;

		if (i == 0)
		{
			id = chkNetLocalCar();

			if (!chkCarIdIsSet(id))
				continue;

			gChkNetPeerPick[0] = id;
			gChkNetPeerPickSet[0] = 1;
		}
		else if (gChkNetPeerPickSet[i])
			id = gChkNetPeerPick[i];
		else
			continue;

		pay[len++] = (unsigned char)i;
		pay[len++] = id.city;
		pay[len++] = id.model;
		count++;
	}

	if (count == 0)
		return;

	pay[0] = (unsigned char)count;

	chkNetSendPacket(CHK_NET_CARS, pay, len);
}

/* ---------------------------------------------------------------------------
 * Inbound
 * ------------------------------------------------------------------------- */

static int chkNetOnRecv(void* ud, void* args)
{
	JER_ARGS_NET_RECV* a = (JER_ARGS_NET_RECV*)args;
	const unsigned char* p;
	int tag;

	(void)ud;

	if (a == NULL || a->channel == NULL || strcmp(a->channel, CHK_NET_CHANNEL) != 0)
		return JER_RESULT_CONTINUE;

	if (a->len < 2)
		return JER_RESULT_CONTINUE;

	p = (const unsigned char*)a->data;

	if (p[0] != CHK_NET_WIRE_VERSION)
	{
		printInfo("[carhacks/net] ignoring a message with wire version %d (mine is %d)\n",
			(int)p[0], CHK_NET_WIRE_VERSION);
		return JER_RESULT_CONTINUE;
	}

	tag = p[1];

	switch (tag)
	{
		case CHK_NET_REQ:
			/* a joiner asks: the host answers with the current set, and with the
			 * per-player table so it knows who is driving what */
			if (jer_net_is_host())
			{
				chkNetBroadcastCars();

				if (chkNetAgreeEnabled())
					chkNetPublishSet();
			}
			break;

		case CHK_NET_PICK:
			if (a->len < 4 || a->peer < 1 || a->peer >= CHK_NET_MAX_PLAYERS)
				break;

			gChkNetPeerPick[a->peer] = chkCarId(p[2], p[3]);
			gChkNetPeerPickSet[a->peer] = 1;

			printInfo("[carhacks/net] player %d drives %s model %d\n",
				a->peer, chkNetCityName((int)p[2]), (int)p[3]);

			/* a new car for this player may leave their old slot unused (the fold in
			 * chkNetPublishSet does this too, but only when the agreement is on) */
			chkNetUnbindChangedPeers();

			/* The host owns the SET, so it re-publishes with the claim in - but the
			 * identity table is not part of the import agreement: every machine needs
			 * to know which car each peer drives even when each keeps its own set
			 * (that is what decides whether a peer's car CAN be drawn here). */
			if (jer_net_is_host())
			{
				chkNetBroadcastCars();

				if (chkNetAgreeEnabled())
					chkNetPublishSet();
			}
			break;

		case CHK_NET_CARS:
			if (a->len < 4)
				break;

			{
				int count = (int)p[2];
				int at = 3, i;

				/* Who this table still names. A player the LAST table named and this one
				 * does not have left the session - see the release pass below. */
				int seen[CHK_NET_MAX_PLAYERS];

				for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
					seen[i] = 0;

				if (count > CHK_NET_MAX_PLAYERS)
					count = CHK_NET_MAX_PLAYERS;

				for (i = 0; i < count && at + 2 < a->len; i++, at += 3)
				{
					int who = (int)p[at];

					if (who < 0 || who >= CHK_NET_MAX_PLAYERS)
						continue;

					seen[who] = 1;

					/* OUR OWN row is ours to say once we have chosen a car, not the host's:
					 * the host's copy can be a table behind (sent before our last switch
					 * reached it), and taking it back would name our OLD car as still
					 * wanted here - which the release routine would then keep. */
					if (who == jer_net_local_player() && chkImportChosenIsSet() &&
						chkCarIdIsSet(chkImportChosenCar()))
					{
						gChkNetPeerPick[who] = chkImportChosenCar();
						gChkNetPeerPickSet[who] = 1;
						continue;
					}

					gChkNetPeerPick[who] = chkCarId(p[at + 1], p[at + 2]);
					gChkNetPeerPickSet[who] = 1;
				}

				chkNetLogCars("from the host");

				/* ...and BUILD what we just learned. This table is how a joiner finds
				 * out who drives what, and its own level loaded before any of those cars
				 * existed in it - that is what joining a match in progress means. Folding
				 * here is the catch-up: each new car enters the set and is hot-loaded
				 * (geometry, cosmetics, texture pages and their rows) instead of being
				 * drawn as the level's own car of the same number. Reported as "player 3
				 * didn't see player 2's imported car". */
				chkNetFoldPeerCars();

				/* The other half of the same contract: a player who is GONE unloads.
				 * Give their car's slot back - engine (pins, pool pages, baked index,
				 * geometry) and module (the set entry, residentCarModels, the source
				 * city) both - so it is free for whoever joins next. Only when nobody
				 * else drives that car: two players can pick the same one, and the
				 * canonical rule gives them a single slot. */
				chkNetReleaseDeparted(seen);
			}
			break;

		case CHK_NET_SET:
			if (a->len < 5)
				break;

			/* store the payload verbatim for the NEXT level load: the engine
			 * applies an import set once per level, so a set arriving mid-level
			 * cannot take effect until the next one */
			{
				int len = a->len - 2;

				if (len > CHK_NET_SET_MAX)
					len = CHK_NET_SET_MAX;

				memcpy(gChkNetAgreed, p + 2, (size_t)len);
				gChkNetAgreedLen = len;
				gChkNetAgreedGuest = (gChkNetAgreed[0] == CHK_CITY_NATIVE) ? -1 : (int)gChkNetAgreed[0];

				printInfo("[carhacks/net] the host's agreed set arrived: %d entr%s "
					"(applied at the next level)\n",
					(int)gChkNetAgreed[1],
					(gChkNetAgreed[1] == 1) ? "y" : "ies");

				if (chkImportGuestCityCount() > 0)
					printInfo("[carhacks/net] ... this machine's own set names %d guest cit%s; "
						"the host's set replaces it at the next level\n",
						chkImportGuestCityCount(),
						(chkImportGuestCityCount() == 1) ? "y" : "ies");
			}
			break;

		default:
			break;
	}

	return JER_RESULT_CONTINUE;
}

void chkNetAdvertisePick(int city, int model)
{
	unsigned char pay[2];

	/* NOT gated on the import agreement: this is "what I am driving", which every
	 * machine needs whether or not the sets are agreed. */
	if (!jer_net_is_active())
		return;

	/* CHK_DROP_ADVERT=<n>: throw away the first n adverts.
	 *
	 * A lost message is otherwise only reachable by dropping a real packet, and losing one
	 * is the exact failure the re-announce in chkNetOnFrame exists for - so the test has to
	 * be able to cause it. Logged, because otherwise the run looks like a car that simply
	 * took a few seconds to arrive. */
	{
		static int dropped;
		static int want = -1;

		if (want < 0)
		{
			const char* s = getenv("CHK_DROP_ADVERT");

			want = (s != NULL) ? atoi(s) : 0;
		}

		if (dropped < want)
		{
			dropped++;
			printInfo("[carhacks/net] CHK_DROP_ADVERT: dropping advert %d of %d (%s model %d)\n",
				dropped, want, chkNetCityName(city), model);
			return;
		}
	}

	pay[0] = (unsigned char)city;
	pay[1] = (unsigned char)model;

	if (chkNetSendPacket(CHK_NET_PICK, pay, 2))
		printInfo("[carhacks/net] told the session: %s model %d\n",
			chkNetCityName(city), model);
}

void chkNetRequestSet(void)
{
	if (!chkNetAgreeEnabled())
		return;

	if (chkNetSendPacket(CHK_NET_REQ, NULL, 0))
		printInfo("[carhacks/net] asked the host for the agreed set\n");
}

/* ---------------------------------------------------------------------------
 * Session lifecycle
 * ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------ */
/* Announcing what we drive                                            */
/* ------------------------------------------------------------------ */

/* How often an UNCHANGED car is announced again - see the dedupe in chkNetOnFrame. A few
 * seconds of redundancy on one small message, and it is what turns a lost pick into a
 * self-healing one.
 *
 * Counted in FRAMES, not milliseconds: carhacks takes no platform headers - mp keeps
 * windows.h out of its mod files for winsock ordering, and <time.h> is shadowed by the
 * game's own on this include path - and the engine's fixed step makes a frame count a good
 * enough heartbeat for something whose whole job is to be repeated. ~3 s at this rate. */
#define CHK_NET_ADVERT_FRAMES 90

static int chkNetOnFrame(void* ud, void* args)
{
	int active = jer_net_is_active();

	gChkNetFrame++;

	(void)ud;
	(void)args;

	if (active && !gChkNetSession)
	{
		int i;

		gChkNetSession = 1;

		for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
			gChkNetPeerPickSet[i] = 0;

		/* and re-announce our own car, for the same reason the agreed set is dropped: a
		 * second session must re-negotiate rather than inherit the last one */
		gChkNetLocalSet = 0;
		gChkNetAdvertFrame = 0;

		/* dropping the agreed set here is what makes a SECOND session
		 * re-negotiate instead of inheriting the last one's cars */
		gChkNetAgreedLen = 0;
		gChkNetAgreedGuest = -1;

		if (jer_net_is_host())
			chkNetPublishSet();
		else
			chkNetRequestSet();

		gChkNetLocalSet = 0;	/* re-advertise OUR car for this session */

		if (!chkNetAgreeEnabled())
			printInfo("[carhacks/net] import agreement OFF (mp_agree_imports = 0) - every machine keeps its own set\n");
	}
	else if (active)
	{
		/* Has anyone GONE? This must happen BEFORE the early returns below: they fire on
		 * every frame where the local car has not changed, which is the normal case - and
		 * putting the check after them meant it never ran. Only the roster knows a player
		 * left (the car table simply stops naming them), so this is the HOST's half of
		 * "a peer leaves -> release its slot". Idempotent. */
		chkNetReapDeparted();

		/* ...and the release decisions still waiting for a car to leave their slot. Also
		 * before the early returns, for the same reason. */
		chkNetRetryPendingReleases();

		/* ...and the peer cars whose fold could not get a slot, or whose build did not
		 * land yet. Same reason: this must run on the frames where nothing else about
		 * the session changed - which is exactly when a retry is due. */
		chkNetRetryPendingLoads();

		/* CHK_FORCE_PLAYER_PALETTE=<n>: a headless run has no colour picker, so the
		 * local car always comes out palette 0 - which means mp's owner-authoritative
		 * palette, and carhacks' correction of it, are never exercised from a script.
		 * This is the module's test lever for that, the way CHK_FORCE_CAR is for the
		 * menu. Idempotent, and re-applied until the car exists. */
		{
			const char* p = getenv("CHK_FORCE_PLAYER_PALETTE");

			if (p != NULL && *p >= '0' && *p <= '9' &&
				player[0].playerCarId >= 0 && player[0].playerCarId < MAX_CARS)
			{
				u_char want = (u_char)atoi(p);

				if (car_data[player[0].playerCarId].ap.palette != want)
				{
					car_data[player[0].playerCarId].ap.palette = want;
					printInfo("[carhacks/net] test lever: local car palette -> %d\n", (int)want);
				}
			}
		}

		/* Keep the session up to date with what we are driving. A change matters:
		 * mp spawns the local car and settles -mpcar AFTER the session is up, so a
		 * one-shot advert at join time would announce an unspawned car.
		 *
		 * A MISSED ADVERT MUST NOT STICK.
		 *
		 * This used to be "unchanged -> never advertise again", which is only right while
		 * every message arrives. It is the session's ONE one-shot direction: the host
		 * re-broadcasts its agreed set and its roster on a timer, so a client that misses
		 * those recovers by itself - but a client's own pick is announced once, and losing
		 * that advert, or the import it triggers on the host, leaves the host drawing that
		 * player's OLD car for the rest of the match with nothing left to retry it. That
		 * is a missed asset load that never heals.
		 *
		 * So an unchanged car is announced again every CHK_NET_ADVERT_MS. Everything below
		 * is a re-send (our row, the advert, the host's table) and the deliberate check is
		 * a query, so repeating it on an unchanged car changes no state - it just makes the
		 * announcement redundant enough to survive a loss. */
		CHK_CAR_ID mine = chkNetLocalCar();

		if (gChkNetLocalSet && chkCarIdEqual(mine, gChkNetLocal) &&
			gChkNetFrame - gChkNetAdvertFrame < CHK_NET_ADVERT_FRAMES)
			return JER_RESULT_CONTINUE;

		gChkNetLocal = mine;
		gChkNetLocalSet = 1;
		gChkNetAdvertFrame = gChkNetFrame;

		if (!chkCarIdIsSet(mine))
			return JER_RESULT_CONTINUE;

		/* OUR row, not player 0's: on a CLIENT player 0 is the host, and writing
		 * our own car there told this machine that the host drove our car - which
		 * is how a peer's car got reported as already-correct for a moment. */
		{
			int me = jer_net_local_player();
			int deliberate;

			/* Only a car that is a CHOICE belongs in the session's import set.
			 *
			 * A car of the level's own city is what mp hands a player who has not
			 * picked yet, and folding that in gave the ASSIGNED car a resident slot --
			 * after which the real pick (a guest city) found its canonical slot
			 * occupied by a car nobody drives. That is the mismatch the canonical rule
			 * exists to remove, so the assignment must not be advertised.
			 *
			 * A car from ANOTHER city is never an assignment: it came from a pick or
			 * from -mpcar, so it is advertised either way. A menu pick is deliberate
			 * whatever city it names, which covers the level's own city dropping a
			 * model its pool does not hold (the "I picked car 12" case). */
			deliberate = jer_net_local_car_chosen();

			if (!deliberate)
				return JER_RESULT_CONTINUE;

			if (me >= 0 && me < CHK_NET_MAX_PLAYERS)
			{
				gChkNetPeerPick[me] = mine;
				gChkNetPeerPickSet[me] = 1;
			}
		}

		chkNetAdvertisePick((int)mine.city, (int)mine.model);

		if (jer_net_is_host())
			chkNetBroadcastCars();
	}
	else if (!active && gChkNetSession)
	{
		gChkNetSession = 0;
		gChkNetAgreedLen = 0;
		gChkNetAgreedGuest = -1;

		printInfo("[carhacks/net] session ended - back to the local import set\n");

		/* The unload: this machine is going back to the frontend, so there is no map any
		 * more and nothing cross-city may stay held. Everything goes back - the slots
		 * (pins, lower-half pool pages, baked page indices, geometry) and the per-city
		 * state a level load would otherwise clear: the parsed page lists and the
		 * deferred palette lumps, which are pointers INTO the import buffers and are
		 * exactly what used to be read after a leave -> rejoin with a car from another
		 * city (the access violation reported doing that). */
		{
			int i;

			for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
			{
				gChkNetPeerPickSet[i] = 0;
				gChkNetPeerSlot[i] = -1;
				chkNetLoadRetryClear(i);	/* no session: nothing to wait for */
			}

			for (i = 0; i < CHK_IMPORT_MAX_SLOTS; i++)
				gChkNetRelPending[i] = 0;	/* everything goes back below anyway */
		}

		chkImportReleaseAll();

		/* The monitor: what each resident slot still holds cross-city, at the moment the
		 * session goes away. This is the line to read for "did anything leak?" - a slot
		 * that still shows pins or geometry after everyone has left is a slot whose
		 * resources were never given back. With the release above it should be silent.
		 * See the resource-lifecycle table in MP_ADAPTER.md for what each join/leave
		 * event is supposed to load and unload. */
		CarSlotResReport();
	}

	return JER_RESULT_CONTINUE;
}

/* Every frame, while a session is up: has anyone LEFT? A peer going away is the unload event
 * for its car's resident slot, and the session's own car table cannot report it - the table is
 * merged into a machine's state and a player who has gone is simply never named again. The
 * roster is what knows, so ask it (jer_net_player_present) and run the same release pass the
 * table handler runs. Idempotent: it only acts for players still flagged as present-car.
 *
 * Without this the HOST never released anything: a client leaving on its own (MP_TEST_LEAVE,
 * the pause menu's Exit, a dropped connection) left that client's car - and its lower-half pool
 * pages, baked page index and geometry - held for the rest of the session, so the next joiner
 * was refused the slot by chkImportSlotFree. */
static void chkNetReapDeparted(void)
{
	int seen[CHK_NET_MAX_PLAYERS];
	int i;

	for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
		seen[i] = jer_net_player_present(i);

	chkNetReleaseDeparted(seen);
}

static int chkNetOnLevelLaunch(void* ud, void* args)
{
	(void)ud;
	(void)args;

	/* NOTE: deliberately does NOT publish. This fires BEFORE the level's car data
	 * is set up, so the set does not exist yet and publishing here sent an empty
	 * one. chkNetNotifySetBuilt() is the real moment. Kept as the place a late
	 * joiner's REQ is answered from, which chkNetOnRecv already does. */
	return JER_RESULT_CONTINUE;
}

void chkNetNotifySetBuilt(void)
{
	/* The level's car data has just been set up, so the set is real now. A client
	 * that is still in the menus adopts it and loads the same cars. */
	if (jer_net_is_active() && jer_net_is_host())
		chkNetPublishSet();
}

void chkNetRegister(JERICHO_CONTEXT* ctx)
{
	if (!gChkNetRegistered)
	{
		gChkNetRegistered = 1;

		if (jer_net_register_channel(CHK_NET_CHANNEL, JER_NET_RELIABLE) < 0)
			ctx->jer_log(ctx, "[carhacks/net] channel '" CHK_NET_CHANNEL "' refused\n");
		else
			ctx->jer_log(ctx, "[carhacks/net] channel '" CHK_NET_CHANNEL "' registered (reliable)\n");
	}

	ctx->jer_register_hook(ctx, JER_EVENT_NET_RECV, chkNetOnRecv, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, chkNetOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_LEVEL_LAUNCH, chkNetOnLevelLaunch, NULL, 0);
}
