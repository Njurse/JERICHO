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

#include "carid.h"
#include "carimport.h"
#include "net.h"

#include <string.h>

/* Player ids are the bridge's (0 = host, 1.. = clients). */
#define CHK_NET_MAX_PLAYERS	8

/* Payload cap: [guestCity][count][version] + 8 x [slot][city][model]. */
#define CHK_NET_SET_MAX		(3 + CHK_IMPORT_MAX_SLOTS * 3)

static int gChkNetRegistered;
static int gChkNetSession;		/* a session was live last frame */

/* What each peer says it wants to drive. */
static CHK_CAR_ID gChkNetPeerPick[CHK_NET_MAX_PLAYERS];
static int gChkNetPeerPickSet[CHK_NET_MAX_PLAYERS];

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

static const char* chkNetCityName(int city)
{
	return (city >= 0 && city < 4) ? LevelNames[city] : "level";
}

/* Fold every peer's claim into this machine's set, so the host LOADS what its
 * clients asked to drive. A claim already in the set is left alone; a new one
 * takes a spare resident slot. Returns how many were folded. */
static int chkNetFoldPeerPicks(void)
{
	int folded = 0, p, slot;

	for (p = 1; p < CHK_NET_MAX_PLAYERS; p++)
	{
		int already = 0;

		if (!gChkNetPeerPickSet[p])
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
			if (chkImportSlotId(slot).model == CHK_MODEL_NONE)
				break;
		}

		if (slot >= CHK_IMPORT_MAX_SLOTS)
		{
			printInfo("[carhacks/net] player %d wants %s model %d, but no spare resident "
				"slot is free - not importing it\n",
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

	chkNetFoldPeerPicks();
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
			/* a joiner asks: the host answers with the current set */
			if (jer_net_is_host() && chkNetAgreeEnabled())
				chkNetPublishSet();
			break;

		case CHK_NET_PICK:
			if (a->len < 4 || a->peer < 1 || a->peer >= CHK_NET_MAX_PLAYERS)
				break;

			gChkNetPeerPick[a->peer] = chkCarId(p[2], p[3]);
			gChkNetPeerPickSet[a->peer] = 1;

			printInfo("[carhacks/net] player %d wants %s model %d\n",
				a->peer, chkNetCityName((int)p[2]), (int)p[3]);

			/* the host owns the set, so it re-publishes with the claim in */
			if (jer_net_is_host() && chkNetAgreeEnabled())
				chkNetPublishSet();
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

	if (!chkNetAgreeEnabled() || !jer_net_is_active())
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

		if (!chkNetAgreeEnabled())
			printInfo("[carhacks/net] import agreement OFF (mp_agree_imports = 0) - every machine keeps its own set\n");
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
