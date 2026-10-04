/* carhacks/carimport.c — the import set (see carimport.h).
 *
 * THE ONE RULE THIS ENFORCES: the engine reads a level's car data from ONE
 * foreign city. gCarModelSource[slot] says which city a resident slot's
 * CARMODEL_<n> comes from, but the level's geometry/palette import holds a
 * single source city (models.c), so asking for a second city does not work -
 * it silently looks the model up in the first city's table. This module keeps
 * that honest: the SECOND city is refused, with a log line, instead of being
 * quietly wrong.
 */

#include "driver2.h"

#include "jericho.h"		/* jer_error: a player-facing refusal notice */
#include "jer_config.h"

#include "system.h"		/* LevelNames[] for the log */
#include "mission.h"		/* residentCarModels[], JerSetCarModelSource */
#include "models.h"		/* InitCarImport, JerHotLoadCarModel */
#include "texture.h"		/* CarImportApplyPaletteForCity */
#include "cosmetic.h"		/* JerHotLoadCarCosmetics */
#include "jer_net.h"		/* jer_net_local_player: the canonical slot order */
#include "net.h"		/* chkNetPeerCar: the peers' cars, in id order */

#include "carid.h"
#include "carimport.h"

typedef struct CHK_IMPORT_ENTRY
{
	int used;		/* 0 = nothing for this slot */
	int model;		/* model to write, or -1 to leave the engine's */
	int city;		/* source city to write, or -1 to leave it (native) */
} CHK_IMPORT_ENTRY;

static CHK_IMPORT_ENTRY gChkSet[CHK_IMPORT_MAX_SLOTS];
static int gChkGuestCity = -1;		/* the FIRST guest city the set names, -1 = none
					 * (a set may name several: see chkImportSetSlot) */
static int gChkGuestCityCount;		/* how many DISTINCT guest cities it names */
static int gChkSetVersion;		/* bumps on every change */

static CHK_CAR_ID gChkPick;		/* the player's pick */
static int gChkPickSet;			/* 0 = nothing picked yet */

/* The CHOICE, which survives the consume (chkImportClearPick) that the level does when it
 * has read the pick. See chkImportSetLocalPick for why the two cannot be the same record. */
static CHK_CAR_ID gChkChosen;
static int gChkChosenSet;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* A new level (or a module reload): drop every entry and the guest city. The
 * player's pick SURVIVES: it is made in the frontend and consumed by the level
 * it starts, i.e. this runs first and the pick has to outlive it. */
void chkImportReset(void)
{
	int i;

	for (i = 0; i < CHK_IMPORT_MAX_SLOTS; i++)
	{
		gChkSet[i].used = 0;
		gChkSet[i].model = -1;
		gChkSet[i].city = -1;
	}

	gChkGuestCity = -1;
	gChkGuestCityCount = 0;
	gChkSetVersion++;
}

/* Consume the pick: the level that imported it has read it. */
void chkImportClearPick(void)
{
	gChkPick = chkCarId(CHK_CITY_NATIVE, CHK_MODEL_NONE);
	gChkPickSet = 0;
}

/* ---------------------------------------------------------------------------
 * The set
 * ------------------------------------------------------------------------- */

/* Give ONE resident slot back: everything the engine placed for it, plus the set entry that
 * claimed it, so the slot returns to the canonical assignment as if nothing had been imported
 * into it. Used when the car's last driver goes away (a peer leaving, or the local player
 * leaving/repicking) -- "the resources of a slot are the load of the event that created it,
 * given back by the unload of the event that removed it" (MP_ADAPTER.md).
 *
 * Both halves matter. The engine side (JerReleaseCarSlot) walks what was actually placed: the
 * pins and their lower-half pool pages, the baked 110..127 index each of the slot's sets holds,
 * its geometry block and its manifest entry. The module side clears the set entry AND the two
 * engine arrays the module writes through: residentCarModels[slot] and the slot's source city.
 * Leave either of those and the slot still looks taken -- chkImportSlotFree checks both -- so
 * the next joiner is refused a spare that is really free. That is the user's "in case we want to
 * use that slot again eventually".
 *
 * Returns 1 if the slot held anything, 0 if it was already free. */
int chkImportReleaseSlot(int slot)
{
	int held;

	if (slot < 0 || slot >= CHK_IMPORT_MAX_SLOTS)
		return 0;

	held = gChkSet[slot].used;

	JerReleaseCarSlot(slot);		/* pins, pool pages, baked index, geometry, manifest */

	if (slot < MAX_CAR_RESIDENT_MODELS)
	{
		/* the module writes these two, so the module clears them */
		JerSetCarModelSource(slot, -1);
		residentCarModels[slot] = 0;
	}

	memset(&gChkSet[slot], 0, sizeof(gChkSet[slot]));
	gChkSet[slot].model = -1;
	gChkSet[slot].city = -1;

	if (held)
	{
		gChkSetVersion++;
		printInfo("[carhacks] released resident slot %d - free for the next car\n", slot);
	}

	return held;
}

/* Leave the session: give everything back, cross-city. Called when the session ends, i.e. when
 * this machine is going back to the frontend and there is no map any more.
 *
 * The engine's JerReleaseAllCrossCity covers the placement state a level installs AND the two
 * per-city pointers that outlive a level (the parsed page lists and the deferred palette
 * lumps): reading those once the map is gone is the access violation reported when a player
 * left and rejoined with a car from a different city. This side clears the set, the guest-city
 * record and the pick, so the next session starts from nothing. */
void chkImportReleaseAll(void)
{
	int i;

	for (i = 0; i < CHK_IMPORT_MAX_SLOTS; i++)
		chkImportReleaseSlot(i);

	JerReleaseAllCrossCity();

	chkImportReset();		/* the set entries (already cleared), the guest city, a new version */
	chkImportClearPick();

	printInfo("[carhacks] session over: every cross-city resource given back\n");
}

/* Leave the session's PLACEMENT behind without forgetting the player's pick: everything the
 * slots hold (pins, lower-half pool pages, baked page indices, geometry), the parsed page
 * lists, the deferred palette lumps and the imported city buffers. Used on the way into the
 * frontend, where those paged-in car textures and palettes are not needed at all -- they are
 * re-read when the next level loads, and each level load cannot undo a leak that happened
 * while there was no level.
 *
 * The PICK survives: it is made in the frontend and consumed by the level it starts
 * (chkImportClearPick is deliberately not called here). */
void chkImportPurgePlacement(void)
{
	int i, held = 0;

	for (i = 0; i < CHK_IMPORT_MAX_SLOTS; i++)
	{
		if (gChkSet[i].used)
			held++;
	}

	/* no placement and nothing parsed: nothing to give back, and no log noise on the
	 * screens we pass through on the way in */
	if (held == 0 && gChkGuestCity < 0)
		return;

	for (i = 0; i < CHK_IMPORT_MAX_SLOTS; i++)
		chkImportReleaseSlot(i);

	JerReleaseAllCrossCity();

	chkImportReset();		/* the set entries and the guest city, KEEPING the pick */

	printInfo("[carhacks] frontend: released %d placed slot(s) and the cross-city state - the next level re-imports\n", held);
}

static CHK_IMPORT_ENTRY* chkSlot(int slot)
{
	if (slot < 0 || slot >= CHK_IMPORT_MAX_SLOTS)
		return NULL;

	return &gChkSet[slot];
}

/* The engine's live resident models for this level, handed over from
 * JER_EVENT_CAR_DATA_SOURCE (models.c) so chkImportSlotFree can see what another
 * module claimed. The POINTER is hook-only -- but the list is not: the peer fold
 * also runs when the host PUBLISHES its set (a peer's PICK, a session start),
 * which is outside the hook, and the resident list is fixed once a level has
 * loaded. So a COPY is kept and used whenever the pointer is gone. Without it a
 * publish-time fold could not see that mp owns slots 5/6 and took one of them. */
static int* gChkEngineModels;		/* live, only inside the hook */
static int  gChkEngineCount;		/* live count, or the cached count */
#define CHK_ENGINE_CACHE	16	/* room for MAX_CAR_RESIDENT_MODELS (12) */
static int  gChkEngineCache[CHK_ENGINE_CACHE];
static int  gChkEngineKnown;		/* a level has handed us its list at least once */

void chkImportSetEngineModels(int* models, int count)
{
	if (models == NULL)
	{
		gChkEngineModels = NULL;	/* keep gChkEngineCache/Known */
		return;
	}

	gChkEngineModels = models;
	gChkEngineCount = (count > 0) ? count : 0;

	if (gChkEngineCount > CHK_ENGINE_CACHE)
		gChkEngineCount = CHK_ENGINE_CACHE;

	memcpy(gChkEngineCache, models, (size_t)gChkEngineCount * sizeof(int));
	gChkEngineKnown = 1;
}

/* What the ENGINE holds in resident `slot`, from the live list inside the hook or
 * from the copy outside it; -1 when we cannot tell. */
static int chkEngineModelAt(int slot)
{
	if (slot < 0)
		return -1;

	if (gChkEngineModels != NULL)
		return (slot < gChkEngineCount) ? gChkEngineModels[slot] : -1;

	if (gChkEngineKnown && slot < gChkEngineCount)
		return gChkEngineCache[slot];

	return -1;
}

/* Is `slot` still free to import into? A slot another module already gave a model
 * is NOT ours to take: mp writes ITS spare player slots (5 and 6) from the same
 * JER_EVENT_CAR_DATA_SOURCE, and both used to answer at the same priority, so one
 * silently overwrote the other. carhacks now answers LAST (priority 10) and skips
 * whatever it finds claimed -- mp's slots, or any other module's. */
int chkImportSlotFree(int slot)
{
	if (slot < 0 || slot >= CHK_IMPORT_MAX_SLOTS)
		return 0;

	if (gChkSet[slot].used)
		return 0;			/* already ours */

	if (chkEngineModelAt(slot) >= 0)
		return 0;			/* another module (or the level) claimed it */

	return 1;
}

/* Would this car need a spare resident slot at all?
 *
 * A car the level's own city ships needs no import WHEN THE LEVEL ALREADY HOLDS IT
 * (its own list names that model). chkImportLevelHoldsModel is tri-state: -1 means
 * "cannot tell" -- this also runs on the publish path, outside the hook that hands the
 * resident list over -- and BOTH 1 and -1 mean "do not import". Only a provable 0 goes
 * to the import path: a car from the level's own city that its pool really does not
 * hold (RIO's model 12, for one). This is the rule the pick and the peer fold used to
 * spell out separately, in one place so the two cannot drift apart. */
static int chkImportCarNeedsNoSlot(CHK_CAR_ID car)
{
	int city = chkCarIdCity(car);
	int model = chkCarIdModel(car);

	if (model < 0)
		return 1;			/* nothing settled yet: not a car to host */

	if (city < 0)
		city = GameLevel;		/* CHK_CITY_NATIVE = the level's own city */

	return (city == GameLevel && chkImportLevelHoldsModel(model) != 0);
}

/* The distinct cars this session needs, in the CANONICAL order: by the lowest owning
 * player id, ascending, with our own pick participating at OUR player id.
 *
 * Every machine derives the same order from the same picks -- which is the point. Both
 * callers used to take the "first free spare" at the moment their pick arrived (the
 * host folding peers as they come in, and each machine placing its OWN pick first,
 * locally), so the same car could sit in slot 5 on one machine and slot 7 on another,
 * and everything baked against a slot (the page index in the polys, the palette rows,
 * the hot-loaded geometry) went with it.
 *
 * Ordering by OWNING PLAYER ID also makes the mapping append-only: a player that joins
 * later has a higher id, so its car is appended and can never displace a car that is
 * already in a slot. Returns how many were written. */
static int chkImportCarOrder(CHK_CAR_ID* out, int max)
{
	int p, n = 0;
	int local = jer_net_local_player();

	for (p = 0; p < CHK_NET_MAX_PLAYERS && n < max; p++)
	{
		CHK_CAR_ID car;
		int i, dup = 0;

		if (p == local)
		{
			if (!gChkPickSet)
				continue;

			car = gChkPick;
		}
		else
		{
			if (!chkNetPeerCar(p, &car))
				continue;
		}

		if (chkImportCarNeedsNoSlot(car))
			continue;

		for (i = 0; i < n; i++)
		{
			if (chkCarIdEqual(out[i], car))
			{
				dup = 1;
				break;
			}
		}

		if (dup)
			continue;		/* two players in the same car share one slot */

		out[n++] = car;
	}

	return n;
}

/* The canonical resident slot for `car`, or -1 when the session needs more spares than
 * this level has free. `outCount` (optional) receives how many cars the session needs.
 *
 * The i-th car in the canonical order takes the i-th slot this LEVEL leaves free, in
 * ascending order -- both halves matter. The index alone is not enough: a level may
 * hold cars of its own in the upper residents (this one keeps models 9 and 10 in slots
 * 5 and 6 for players 5 and 6), so the free list is part of the mapping. It is the same
 * list on every machine, because it excludes only what the level itself put there
 * (a model in the engine's resident table that is not one of ours), never what our own
 * set happens to have applied so far. */
int chkImportCanonicalSlot(CHK_CAR_ID car, int* outCount)
{
	CHK_CAR_ID order[CHK_NET_MAX_PLAYERS];
	int spare[CHK_IMPORT_MAX_SLOTS];
	int n = chkImportCarOrder(order, CHK_NET_MAX_PLAYERS);
	int k = 0, i, slot;

	/* Already placed: keep it where it is. A slot never MOVES -- its geometry and the
	 * page indices baked against it would have to be rebuilt, and with player-id
	 * ordering a car does not have to move in the first place. */
	for (slot = 0; slot < CHK_IMPORT_MAX_SLOTS; slot++)
	{
		if (chkCarIdEqual(chkImportSlotId(slot), car))
		{
			if (outCount != NULL)
				*outCount = n;

			return slot;
		}
	}

	/* the spare slots this level leaves for us, ascending */
	for (slot = CHK_IMPORT_SPARE_FIRST; slot < CHK_IMPORT_MAX_SLOTS; slot++)
	{
		if (gChkSet[slot].used)
			continue;		/* ours, but another car's */

		if (chkEngineModelAt(slot) >= 0)
			continue;		/* the level's own car lives here */

		spare[k++] = slot;
	}

	for (i = 0; i < n; i++)
	{
		if (!chkCarIdEqual(order[i], car))
			continue;

		if (outCount != NULL)
			*outCount = n;

		return (i < k) ? spare[i] : -1;
	}

	/* not part of this session's set (a caller asking about a car nobody picked) */
	if (outCount != NULL)
		*outCount = n + 1;

	return (n < k) ? spare[n] : -1;
}

/* Does the LEVEL already hold this model in its resident pool?
 *
 * "This level's own city" is NOT the same as "this level has that car". A level
 * reads only the resident models ITS OWN list names, so a model its city ships can
 * still be missing -- RIO's model 12 (the special) is in RIO's files and in no RIO
 * take-a-ride level. Every caller that used to skip an own-city car on the old
 * assumption now asks this instead.
 *
 * THREE ANSWERS, and the third one matters: 1 = the pool holds it, 0 = it provably
 * does not, -1 = CANNOT TELL, because there is no live resident list (this is
 * hook-only, like chkImportSlotFree: the pointer is handed over by
 * JER_EVENT_CAR_DATA_SOURCE and dropped at the end of it). A caller that would
 * IMPORT on a "no" must treat -1 as "leave it alone": the peer fold also runs when
 * the host PUBLISHES its set (a peer's PICK, a session start), which is not inside
 * that hook, and guessing "not held" there folded an own-city car the level already
 * had. An own-city "guest" writes the level's own city's car palettes into a guest
 * civ_clut block, so the whole session's colours go wrong. */
int chkImportLevelHoldsModel(int model)
{
	int i;

	if (gChkEngineModels == NULL && !gChkEngineKnown)
		return -1;			/* no level has told us its list yet */

	if (model < 0)
		return 0;

	for (i = 0; i < gChkEngineCount; i++)
	{
		if (chkEngineModelAt(i) == model)
			return 1;
	}

	return 0;
}

const char* chkCityName(int city)
{
	if (city < 0)
		return "level";

	return (city < CHK_CITY_COUNT_LIMIT) ? LevelNames[city] : "?";
}

/* The set may name AS MANY source cities as it has slots.
 *
 * This used to be a gate - "at most ONE foreign city" - on the belief that the
 * engine holds one source city per level. It does not: mission.c keeps
 * gCarModelSource[] per resident SLOT and every consumer (models.c, cars.c,
 * texture.c, players.c) reads it per slot, so a set drawn from three cities
 * builds and spawns exactly like a set drawn from one. Measured 2026-10-03 on
 * CHICAGO with HAVANA, VEGAS and RIO at once: 6/6 slots built and spawned, each
 * guest city in its own civ_clut block (8..15 / 16..23 / 24..31), base CLUT
 * column 180 rows used / 30 free - the same numbers as the two-city run.
 *
 * The thing that refused the third city was this function, not the engine. */

/* Recompute the set's guest cities: the FIRST one (still reported in the
 * session's agreed-set header, which predates per-slot cities) and how many
 * distinct ones it names. */
static void chkImportRefreshGuestCity(void)
{
	int i, j;

	gChkGuestCity = -1;
	gChkGuestCityCount = 0;

	for (i = 0; i < CHK_IMPORT_MAX_SLOTS; i++)
	{
		int city = gChkSet[i].used ? gChkSet[i].city : -1;

		if (city < 0)
			continue;

		if (gChkGuestCity < 0)
			gChkGuestCity = city;

		for (j = 0; j < i; j++)
		{
			if (gChkSet[j].used && gChkSet[j].city == city)
				break;		/* named earlier: not another distinct city */
		}

		if (j == i)
			gChkGuestCityCount++;
	}
}

int chkImportSetSlot(int slot, CHK_CAR_ID id)
{
	CHK_IMPORT_ENTRY* e = chkSlot(slot);
	int city;

	if (e == NULL)
		return 0;

	if (!chkCarIdIsSet(id))
		return 0;

	city = chkCarIdCity(id);

	e->used = 1;
	e->model = chkCarIdModel(id);
	e->city = city;
	gChkSetVersion++;

	chkImportRefreshGuestCity();

	return 1;
}

int chkImportSetSlotModel(int slot, int model)
{
	CHK_IMPORT_ENTRY* e = chkSlot(slot);

	if (e == NULL || model < 0 || model >= CHK_MODEL_LIMIT)
		return 0;

	e->used = 1;
	e->model = model;
	/* leave the source alone: this is the traffic knock, which only wants a
	 * different body in a civilian slot */
	gChkSetVersion++;

	chkImportRefreshGuestCity();

	return 1;
}

CHK_CAR_ID chkImportSlotId(int slot)
{
	if (slot < 0 || slot >= CHK_IMPORT_MAX_SLOTS || !gChkSet[slot].used)
		return chkCarId(CHK_CITY_NATIVE, CHK_MODEL_NONE);

	return chkCarId((gChkSet[slot].city >= 0) ? gChkSet[slot].city : CHK_CITY_NATIVE,
		(gChkSet[slot].model >= 0) ? gChkSet[slot].model : CHK_MODEL_NONE);
}

int chkImportGuestCity(void)
{
	return gChkGuestCity;
}

/* How many distinct source cities the set names (0 = none, the level's own). */
int chkImportGuestCityCount(void)
{
	return gChkGuestCityCount;
}

int chkImportSetVersion(void)
{
	return gChkSetVersion;
}

/* ---------------------------------------------------------------------------
 * The player's pick
 * ------------------------------------------------------------------------- */

void chkImportSetLocalPick(int city, int model)
{
	if (city < 0 || city >= CHK_CITY_COUNT_LIMIT || model < 0 || model >= CHK_MODEL_LIMIT)
		return;

	gChkPick = chkCarId(city, model);
	gChkPickSet = 1;

	/* And keep it as the CHOSEN car, which the consume below does NOT clear. The
	 * difference matters the moment anyone asks "what is this player driving?": that
	 * question is about the CHOICE, not about the seat. Measured on a real session, on
	 * the host watching a client who had picked VEGAS model 3:
	 *
	 *   [carhacks/net] player 1 drives VEGAS model 3
	 *   [mp] late joiner: player 1 -> slot 1 model 3 (city 0)      <- the engine seats them
	 *   [mp] player 1 changed car: slot 7 -> 2 (the session city model 3)
	 *   [carhacks/net] player 1 drives level model 3               <- identity follows the SEAT
	 *
	 * From there every machine "knows" the player drives CHICAGO 3, so the imported car is
	 * replaced by the level's own - "it loaded in correctly but then got replaced by the
	 * chicago slot 3". The pick was right the whole time; only the reporting was wrong. */
	gChkChosen = gChkPick;
	gChkChosenSet = 1;

	printInfo("[carhacks] import: pick set to %s model %d\n", chkCityName(city), model);
}

/* The car the local player CHOSE, whether or not the level has consumed the pick yet.
 * -1/unset until they choose one. This is what the session should be told a player is
 * driving - see chkNetLocalCar, and gChkChosen for the measurement that made the
 * difference. */
CHK_CAR_ID chkImportChosenCar(void)
{
	return gChkChosenSet ? gChkChosen : chkCarId(CHK_CITY_NATIVE, CHK_MODEL_NONE);
}

int chkImportChosenIsSet(void)
{
	return gChkChosenSet;
}

CHK_CAR_ID chkImportLocalPick(void)
{
	return gChkPick;
}

int chkImportLocalPickCity(void)
{
	return gChkPickSet ? chkCarIdCity(gChkPick) : -1;
}

int chkImportLocalPickModel(void)
{
	return gChkPickSet ? chkCarIdModel(gChkPick) : -1;
}

/* ---------------------------------------------------------------------------
/* The config fallback: "import = slot:city:model, ..." and the traffic knock
 * -------------------------------------------------------------------------
 *
 * `skipGuestEntries` drops the "import" entries but STILL applies the traffic
 * knock. It is a caller's choice ("ignore the config's car entries"), not a
 * conflict guard: a set may name as many source cities as it has slots, and the
 * player's pick is applied AFTERWARDS and claims only its own slot, so a pick
 * from one city and config entries from another coexist. The caller passes 0. The
 * knock picks a native model into a civilian slot (chkImportSetSlotModel) and
 * claims no city, so it never conflicts. */

int chkImportLoadConfig(const char* section, int count, int skipGuestEntries)
{
	const char* list = jer_config_get_str(section, "import", "");
	int changes = 0, n = 0;
	const char* p;

	if (list != NULL && *list != '\0')
	{
		for (p = list; *p != '\0' && n < CHK_IMPORT_MAX_SLOTS; )
		{
			int vals[3];
			int v;

			/* Skip separators and any spacing between entries. Without this a
			 * "slot:city:model, slot:city:model" list parsed only its first
			 * entry: the next one began with a space, so the number scan found
			 * nothing and the entry was rejected as malformed. */
			while (*p == ',' || *p == ' ' || *p == '\t')
				p++;

			if (*p == '\0')
				break;

			for (v = 0; v < 3; v++)
			{
				int got = 0;

				vals[v] = 0;

				while (*p >= '0' && *p <= '9')
				{
					vals[v] = vals[v] * 10 + (*p - '0');
					p++;
					got = 1;
				}

				if (!got)
				{
					vals[v] = -1;
					break;
				}

				if (v < 2)
				{
					if (*p == ':')
						p++;
					else
					{
						vals[0] = -1;
						break;
					}
				}
			}

			while (*p != '\0' && *p != ',')		/* next entry */
				p++;

			if (*p == ',')
				p++;

			n++;

			if (vals[0] < 0 || vals[0] >= count || vals[1] < 0 || vals[1] >= CHK_CITY_COUNT_LIMIT ||
				vals[2] < 0 || vals[2] >= CHK_MODEL_LIMIT)
			{
				printInfo("[carhacks] import entry %d ignored (want slot:city:model)\n", n);
				continue;
			}

			if (skipGuestEntries)
			{
				printInfo("[carhacks] import entry %d (%s) skipped: the player picked a car from another city\n",
					n, chkCityName(vals[1]));
				continue;
			}

			if (chkImportSetSlot(vals[0], chkCarId(vals[1], vals[2])))
				changes++;
		}
	}

	/* the traffic knock: a different BODY in a civilian slot (ambient traffic
	 * picks its model from slots 0..4). Applied after the imports above so an
	 * explicit import wins the slot. */
	{
		int tmodel = jer_config_get_int(section, "traffic_model", -1);
		int tslot = jer_config_get_int(section, "traffic_slot", 2);

		if (tmodel >= 0 && tmodel < CHK_MODEL_LIMIT && tslot >= 0 && tslot < count)
		{
			if (chkImportSetSlotModel(tslot, tmodel))
			{
				changes++;

				printInfo("[carhacks] cross-city: resident slot %d -> model %d (traffic)\n",
					tslot, tmodel);
			}
		}
	}

	return changes;
}

/* ---------------------------------------------------------------------------
 * The pick, as an import
 * ------------------------------------------------------------------------- */

int chkImportApplyPick(int level, int count)
{
	int slot, model, city;

	if (!gChkPickSet)
		return 0;

	city = chkCarIdCity(gChkPick);
	model = chkCarIdModel(gChkPick);

	if (city < 0)
		return 0;

	if (city == level && chkImportLevelHoldsModel(model) != 0)
	{
		/* the level's own city, and this model is in its resident pool (or we
		 * cannot see the pool from here): its own list already has this car, so
		 * there is nothing to import - wantedCar alone is enough. */
		printInfo("[carhacks] import: the pick (%s model %d) is this level's own car - no import\n",
			chkCityName(city), model);
		return 0;
	}

	if (city == level)
	{
		/* THE LEVEL'S OWN CITY IS NOT THE SAME AS THE LEVEL'S OWN POOL. A level
		 * reads only the resident models its own list names, so a model its city
		 * ships can still be absent (RIO's model 12 - the special - is in RIO's
		 * files and in no RIO take-a-ride level). Nothing imports an own-city car
		 * on the old assumption, and the engine then has nothing to build it from:
		 * InitPlayer falls back to resident slot 0, i.e. the level's FIRST car,
		 * which is exactly "I picked car 12 and spawned as car 1". So an own-city
		 * model the level does not hold is imported like a guest, from its own
		 * city's files (LoadCarImport reads LevelFiles[city] regardless of whether
		 * that city is the level's). */
		printInfo("[carhacks] import: the pick (%s model %d) is this level's own city but not in "
			"its resident pool - importing it from its own files\n",
			chkCityName(city), model);
	}

	/* A pick goes into its CANONICAL spare resident slot, not "the first free one":
	 * the slot is derived from the whole set of picks by player id
	 * (chkImportCanonicalSlot), so every machine puts the same car in the same slot,
	 * and the page indices and palette rows baked against that slot agree too.
	 * InitPlayer prefers a slot the model was IMPORTED into over a native one with
	 * the same number (players.c), so this slot wins even when the level also lists
	 * that number as one of its own civilians. */
	{
		int need = 0;

		slot = chkImportCanonicalSlot(chkCarId(city, model), &need);

		if (slot >= 0 && slot >= count && need <= count)
			slot = -1;		/* the caller's pool is smaller than the session needs */
	}

	if (slot < 0 || slot >= CHK_IMPORT_MAX_SLOTS || !chkImportSlotFree(slot))
	{
		/* Player-facing: their pick could not be imported, so tell them. */
		jer_error("[carhacks] import: the pick (%s model %d) needs a spare resident slot and "
			"none is free - riding the level's own car of that number",
			chkCityName(city), model);
		return 0;
	}

	printInfo("[carhacks] import: the pick (%s model %d) -> resident slot %d (level %s)\n",
		chkCityName(city), model, slot, chkCityName(level));

	/* Note for the next reader: do NOT write wantedCar[] from here.
	 *
	 * This was tried as a fix for a guest pick spawning in the level's own car of the same
	 * number, on the reading that wantedCar[] holds a RESIDENT SLOT. It does not: it holds a
	 * MODEL. mission.c compares it against the model each resident slot carries
	 * (`residentCarModels[j] != wantedCar[i]`), and players.c derives the slot from that
	 * model - so writing the slot index made the guest resolve to no model at all and fall
	 * back to the first resident car. The model write was already right; the car that ended
	 * up wrong is chosen in mp's own rebuild path (see mp_session.c, the "changed car"
	 * lines), not here. */
	return chkImportSetSlot(slot, chkCarId(city, model));
}

/* ---------------------------------------------------------------------------
 * The engine write
 * ------------------------------------------------------------------------- */

void chkImportApplyToCarData(int count, int* models, int* modelSource){
	int slot;

	if (models == NULL || modelSource == NULL)
		return;

	for (slot = 0; slot < count && slot < CHK_IMPORT_MAX_SLOTS; slot++)
	{
		if (!gChkSet[slot].used)
			continue;

		if (gChkSet[slot].model >= 0)
			models[slot] = gChkSet[slot].model;

		if (gChkSet[slot].city >= 0)
			modelSource[slot] = gChkSet[slot].city;

		printInfo("[carhacks] import: slot %d <- model %d from %s\n",
			slot,
			(gChkSet[slot].model >= 0) ? gChkSet[slot].model : models[slot],
			chkCityName(gChkSet[slot].city));
	}
}

void chkImportDump(int level)
{
	int slot, entries = 0;

	for (slot = 0; slot < CHK_IMPORT_MAX_SLOTS; slot++)
	{
		if (gChkSet[slot].used)
			entries++;
	}

	printInfo("[carhacks] import set: level %s, guest cities %d, %d entr%s, version %d\n",
		chkCityName(level), gChkGuestCityCount,
		entries, (entries == 1) ? "y" : "ies", gChkSetVersion);
}

/* Can the ENGINE build resident `slot` NOW?
 *
 * A set normally reaches the engine from JER_EVENT_CAR_DATA_SOURCE, before the
 * level's models are built from the level heap. A car folded in AFTER that (a
 * joiner's pick, which is the whole point of the mp channel) has no geometry, so
 * the machine draws the level's own car of the same number instead - "I picked
 * Havana's car and it looks domestic to everyone else". The resident list and the
 * per-slot source are module-visible, and the engine can build one slot into its
 * own pool (JerHotLoadCarModel), so the sequence is:
 *
 *   1. say where each of OUR slots comes from (the arrays the hook would have set)
 *   2. read in the cities the set now names (InitCarImport covers all of them)
 *   3. build this slot's geometry (refused, and left alone, if it does not fit)
 *   4. apply that city's cosmetics for the slot, in the SAME call: car_cosmetics
 *      carries the wheels, the shadow corners, the collision box and the COG, and
 *      mp's swap path reads it when it rebuilds a peer's car
 *      (cp->ap.carCos + CreateDentableCar). Doing both here means the rebuild mp is
 *      about to do is the right one, and a machine that could not build the geometry
 *      leaves the substitute car alone.
 *
 * A no-op before any level has loaded (that level builds everything itself) and
 * for a slot we do not own. Returns a positive number when anything was applied
 * (0 = not built, and then the slot keeps the car it has).
 */
int chkImportHotLoad(int slot)
{
	int i, built, cos;

	if (slot < 0 || slot >= CHK_IMPORT_MAX_SLOTS || !gChkSet[slot].used)
		return 0;

	if (!gChkEngineKnown)
		return 0;			/* no level yet: its own build will cover this */

	for (i = CHK_IMPORT_SPARE_FIRST; i < CHK_IMPORT_MAX_SLOTS; i++)
	{
		int city, model;

		if (!gChkSet[i].used || gChkSet[i].model < 0)
			continue;

		model = gChkSet[i].model;
		city = gChkSet[i].city;

		residentCarModels[i] = model;
		JerSetCarModelSource(i, city);
	}

	InitCarImportMidLevel();

	/* A city read in mid-level needs its palette lump deferred too: the rows are
	 * uploaded on the first draw of a car that names them (CarImportPin ->
	 * ProcessImportedPaletteRows), and without this the hot-loaded car would be
	 * coloured by whichever city the level already had. */
	CarImportApplyPaletteForCity(GetCarModelSourceCity(slot));

	built = JerHotLoadCarModel(slot);
	cos = JerHotLoadCarCosmetics(slot);

	/* And its TEXTURE PAGES, which the level-load walk recorded for the slots it knew
	 * about: without this the hot-loaded model's polys read whatever their baked index
	 * holds -- the local city's materials, while its cosmetics (and so its handling)
	 * are already its own. The walk is idempotent now, so the cars already pinned are
	 * left alone and only this slot's pages are added; CarImportPin places them, and
	 * their palette rows with them, before the next draw. */
	/* Only a NEW build brings new sets: the sets a model needs are what the build walks
	 * and records (CarModelSet), so a slot whose geometry was already there was covered
	 * by the level-load walk. Keying this on the build also keeps a repeated hot load
	 * from re-walking and re-logging every slot's sets. */
	if (built > 0)
		JerHotLoadCarTpages();

	if (built > 0 || cos > 0)
		printInfo("[carhacks/net] slot %d is now the imported car itself: geometry %s, cosmetics %s\n",
			slot, (built > 0) ? "built" : "already there", (cos > 0) ? "applied" : "already there");

	return (built > 0) ? built : (cos > 0 ? 1 : 0);
}

/* The set's own slot for a (city, model) car, or -1. Used to hot-load a peer's
 * pick on a machine whose running level does not have it. */
int chkImportSlotForCar(int city, int model)
{
	int i;

	if (model < 0)
		return -1;

	for (i = 0; i < CHK_IMPORT_MAX_SLOTS; i++)
	{
		if (!gChkSet[i].used || gChkSet[i].model != model)
			continue;

		if (city < 0 || gChkSet[i].city == city)
			return i;
	}

	return -1;
}
