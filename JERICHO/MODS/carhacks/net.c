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
 * Whether the engine can actually hold the car is NOT this function's call: a car
 * from a second foreign city is refused by the set's one-guest-city rule, loudly
 * (chkImportSetSlot in carimport.c). That refusal is the honest limit - the
 * engine reads one foreign city per level - and the peer keeps the clean fallback
 * instead (ChkOnCarPeerDraw). */
int chkNetFoldPeerCars(void)
{
	int folded = 0, p, slot;

	for (p = 1; p < CHK_NET_MAX_PLAYERS; p++)
	{
		int already = 0;

		if (!gChkNetPeerPickSet[p])
			continue;

		/* A car from the level's OWN city needs no import: the level already lists
		 * its own city's vehicles. Skipping those also stops an identity that has
		 * not settled yet (mp reports a car before a level exists) from claiming a
		 * spare resident slot with the level's own model 0. Said either way - as
		 * CHK_CITY_NATIVE or as the level's own index - it is the same car. */
		if (chkCarIdCity(gChkNetPeerPick[p]) < 0 || chkCarIdCity(gChkNetPeerPick[p]) == GameLevel)
			continue;

		for (slot = 0; slot < CHK_IMPORT_MAX_SLOTS; slot++)
		{
			if (chkCarIdEqual(chkImportSlotId(slot), gChkNetPeerPick[p]))
			{
				already = 1;
				break;
			}
		}

		if (already)
			continue;

		for (slot = CHK_IMPORT_SPARE_FIRST; slot < CHK_IMPORT_MAX_SLOTS; slot++)
		{
			if (chkImportSlotFree(slot))
				break;
		}

		if (slot >= CHK_IMPORT_MAX_SLOTS)
		{
			/* Player-facing: a peer's car could not be brought in, so the host
			 * is told on screen as well as in the log. */
			jer_error("[carhacks/net] player %d wants %s model %d, but no spare resident "
				"slot is free - not importing it",
				p, chkNetCityName((int)gChkNetPeerPick[p].city), (int)gChkNetPeerPick[p].model);
			continue;
		}

		if (chkImportSetSlot(slot, gChkNetPeerPick[p]))
		{
			folded++;

			printInfo("[carhacks/net] player %d's car (%s model %d) -> resident slot %d\n",
				p, chkNetCityName((int)gChkNetPeerPick[p].city),
				(int)gChkNetPeerPick[p].model, slot);
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

	printInfo("[carhacks/net] published the agreed set: guest city %s, %d entr%s\n",
		chkNetCityName(gChkNetAgreedGuest), (int)gChkNetAgreed[1],
		(gChkNetAgreed[1] == 1) ? "y" : "ies");
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
	int i, n = 0, at, count, guest;

	/* only a client adopts - see chkNetHasAgreedSet */
	if (!chkNetHasAgreedSet())
		return 0;

	guest = (gChkNetAgreed[0] == CHK_CITY_NATIVE) ? -1 : (int)gChkNetAgreed[0];
	count = (int)gChkNetAgreed[1];
	at = 3;						/* past guestCity, count, version */

	if (count > CHK_IMPORT_MAX_SLOTS)
		count = CHK_IMPORT_MAX_SLOTS;

	for (i = 0; i < count && at + 2 < gChkNetAgreedLen; i++, at += 3)
	{
		int slot = gChkNetAgreed[at];
		CHK_CAR_ID id = chkCarId(gChkNetAgreed[at + 1], gChkNetAgreed[at + 2]);

		if (chkImportSetSlot(slot, id))
			n++;
	}

	printInfo("[carhacks/net] adopting the session's agreed set: guest city %s, %d entr%s applied\n",
		chkNetCityName(guest), n, (n == 1) ? "y" : "ies");

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

				if (count > CHK_NET_MAX_PLAYERS)
					count = CHK_NET_MAX_PLAYERS;

				for (i = 0; i < count && at + 2 < a->len; i++, at += 3)
				{
					int who = (int)p[at];

					if (who < 0 || who >= CHK_NET_MAX_PLAYERS)
						continue;

					gChkNetPeerPick[who] = chkCarId(p[at + 1], p[at + 2]);
					gChkNetPeerPickSet[who] = 1;
				}

				chkNetLogCars("from the host");
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

				printInfo("[carhacks/net] the host's agreed set arrived: guest city %s, %d entr%s "
					"(applied at the next level)\n",
					chkNetCityName(gChkNetAgreedGuest), (int)gChkNetAgreed[1],
					(gChkNetAgreed[1] == 1) ? "y" : "ies");

				if (chkImportGuestCity() >= 0 && gChkNetAgreedGuest >= 0 &&
					chkImportGuestCity() != gChkNetAgreedGuest)
					printInfo("[carhacks/net] ... this machine reads from %s, the host from %s: "
						"the host's wins at the next level\n",
						chkNetCityName(chkImportGuestCity()), chkNetCityName(gChkNetAgreedGuest));
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
