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
	return (city >= 0 && city < 4) ? LevelNames[city] : "level";
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
/* Is ANYBODY still driving this car? The table (the players still here) and our own pick. Two
 * players can pick the same car, and the canonical rule puts them in ONE slot - so a release
 * must ask this before giving a slot back, or the survivor's car disappears with the leaver. */
static int chkNetCarStillWanted(CHK_CAR_ID car)
{
	int i;

	if (chkImportLocalPickCity() >= 0 &&
		chkCarIdEqual(chkCarId(chkImportLocalPickCity(), chkImportLocalPickModel()), car))
		return 1;

	for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
	{
		if (gChkNetPeerPickSet[i] && chkCarIdEqual(gChkNetPeerPick[i], car))
			return 1;
	}

	return 0;
}

/* The unload half of the car table: for every player the previous table named and the current one
 * does not, forget them and give their car's slot back - unless somebody else still drives that
 * car. Runs after the table has been applied and folded, so the surviving picks are current.
 *
 * This is "a peer leaves -> release its slot" from the resource-lifetime table in MP_ADAPTER.md.
 * Before it, a departed player's car stayed in its resident slot (and its lower-half pool pages,
 * baked page index and geometry stayed consumed) for the rest of the session. */
void chkNetReleaseDeparted(const int* seen)
{
	int i;

	for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
	{
		int slot;

		if (seen[i] || !gChkNetPeerPickSet[i])
			continue;			/* still here, or never was */

		printInfo("[carhacks/net] player %d left the session\n", i);

		gChkNetPeerPickSet[i] = 0;
		gChkNetPeerPick[i] = chkCarId(CHK_CITY_NATIVE, CHK_MODEL_NONE);

		slot = gChkNetPeerSlot[i];
		gChkNetPeerSlot[i] = -1;

		if (slot < 0 || slot >= CHK_IMPORT_MAX_SLOTS)
			continue;

		if (chkNetCarStillWanted(chkImportSlotId(slot)))
			continue;			/* somebody else drives it: the slot stays */

		{
			/* forget every record pointing at this slot, then give it back */
			int k;

			for (k = 0; k < CHK_NET_MAX_PLAYERS; k++)
			{
				if (gChkNetPeerSlot[k] == slot)
					gChkNetPeerSlot[k] = -1;
			}
		}

		chkImportReleaseSlot(slot);
	}
}

int chkNetFoldPeerCars(void)
{
	int folded = 0, p, slot;
	for (p = 1; p < CHK_NET_MAX_PLAYERS; p++)
	{
		int already = 0;

		if (!gChkNetPeerPickSet[p])
			continue;

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
				continue;		/* nothing settled yet: not a car to import */

			if (pcity < 0)
				pcity = GameLevel;	/* CHK_CITY_NATIVE = the level's own city */

			/* 1 = the level's pool has it, -1 = cannot tell (this also runs when the
			 * host publishes its set, outside the hook that hands over the resident
			 * list): BOTH mean "do not import". Only a provable 0 -- an own-city car
			 * the level really does not hold -- goes to the import path. */
			holds = chkImportLevelHoldsModel(pmodel);

			if (pcity == GameLevel && holds != 0)
				continue;		/* in the level's pool (or unknown): no import */
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
			continue;

		/* The slot is CANONICAL (chkImportCanonicalSlot), derived from every pick in
		 * the session by owning player id -- never "the first free spare at the moment
		 * this peer's pick arrived". Arrival order is what used to give the same car
		 * different slots on different machines, taking the baked page indices and the
		 * palette rows with it. Ordering by player id is also append-only: a joiner's
		 * id is higher, so its car cannot displace one that is already in a slot. */
		{
			int need = 0;

			slot = chkImportCanonicalSlot(gChkNetPeerPick[p], &need);

			if (slot < 0 || !chkImportSlotFree(slot))
			{
				/* Player-facing: a peer's car could not be brought in, so the host
				 * is told on screen as well as in the log. */
				jer_error("[carhacks/net] player %d wants %s model %d, but no spare resident "
					"slot is free - not importing it",
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
			if (chkImportHotLoad(slot) <= 0)
			{
				printInfo("[carhacks/net] player %d's car (%s model %d) is in the set for slot %d, "
					"but this level could not build it yet - carrying it to the next level\n",
					p, chkNetCityName((int)gChkNetPeerPick[p].city),
					(int)gChkNetPeerPick[p].model, slot);

				/* Say it on screen as well: from here the peer looks like this
				 * level's own car of the same number, which is the one symptom a
				 * player can see and report. */
				jer_error("player %d's car (%s model %d) cannot be loaded here yet - they may look like this level's own car",
					p, chkNetCityName((int)gChkNetPeerPick[p].city),
					(int)gChkNetPeerPick[p].model);
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
 * reports CHK_CITY_NATIVE. */
CHK_CAR_ID chkNetLocalCar(void)
{
	CAR_DATA* cp;
	int slot, city, model;

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

static int chkNetOnFrame(void* ud, void* args)
{
	int active = jer_net_is_active();

	(void)ud;
	(void)args;

	if (active && !gChkNetSession)
	{
		int i;

		gChkNetSession = 1;

		for (i = 0; i < CHK_NET_MAX_PLAYERS; i++)
			gChkNetPeerPickSet[i] = 0;

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
		 * one-shot advert at join time would announce an unspawned car. */
		CHK_CAR_ID mine = chkNetLocalCar();

		if (gChkNetLocalSet && chkCarIdEqual(mine, gChkNetLocal))
			return JER_RESULT_CONTINUE;

		gChkNetLocal = mine;
		gChkNetLocalSet = 1;

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
			}
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
