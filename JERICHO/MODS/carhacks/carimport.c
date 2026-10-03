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

	printInfo("[carhacks] import: pick set to %s model %d\n", chkCityName(city), model);
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

	/* A pick goes into a spare resident slot. InitPlayer prefers a slot the model
	 * was IMPORTED into over a native one with the same number (players.c), so the
	 * spare slot wins even when the level also lists that number as one of its own
	 * civilians. */
	for (slot = CHK_IMPORT_SPARE_FIRST; slot < count && slot < CHK_IMPORT_MAX_SLOTS; slot++)
	{
		if (chkImportSlotFree(slot))
			break;
	}

	if (slot >= count || slot >= CHK_IMPORT_MAX_SLOTS)
	{
		/* Player-facing: their pick could not be imported, so tell them. */
		jer_error("[carhacks] import: the pick (%s model %d) needs a spare resident slot and "
			"none is free - riding the level's own car of that number",
			chkCityName(city), model);
		return 0;
	}

	printInfo("[carhacks] import: the pick (%s model %d) -> resident slot %d (level %s)\n",
		chkCityName(city), model, slot, chkCityName(level));

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
 *
 * A no-op before any level has loaded (that level builds everything itself) and
 * for a slot we do not own. Returns the bytes the engine built (0 = not built).
 */
int chkImportHotLoad(int slot)
{
	int i;

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

	return JerHotLoadCarModel(slot);
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
