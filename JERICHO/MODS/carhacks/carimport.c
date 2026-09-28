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

#include "jer_config.h"

#include "system.h"		/* LevelNames[] for the log */

#include "carid.h"
#include "carimport.h"

typedef struct CHK_IMPORT_ENTRY
{
	int used;		/* 0 = nothing for this slot */
	int model;		/* model to write, or -1 to leave the engine's */
	int city;		/* source city to write, or -1 to leave it (native) */
} CHK_IMPORT_ENTRY;

static CHK_IMPORT_ENTRY gChkSet[CHK_IMPORT_MAX_SLOTS];
static int gChkGuestCity = -1;		/* the set's single foreign city, -1 = none */
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

static const char* chkCityName(int city)
{
	if (city < 0)
		return "level";

	return (city < CHK_CITY_COUNT_LIMIT) ? LevelNames[city] : "?";
}

/* The guest-city gate: at most ONE foreign city in the set. */
static int chkClaimGuestCity(int city, int slot)
{
	if (city < 0)
		return 1;

	if (gChkGuestCity < 0)
	{
		gChkGuestCity = city;
		return 1;
	}

	if (gChkGuestCity == city)
		return 1;

	/* MEASUREMENT LEVER (two_guest_cities = 1): let a second foreign city through
	 * so the ENGINE's behaviour with one can be observed, instead of only this
	 * gate's refusal. This is not a supported mode - the level is read from one
	 * city (models.c), the palette upload keys off GetCarImportCity()
	 * (cars.c:1623) and the CLUT band is nearly full (VRAM.md) - it exists to say
	 * what a second city actually costs. See MP_ADAPTER.md's hotload hand-off. */
	if (jer_config_get_int("carhacks", "two_guest_cities", 0))
	{
		printInfo("[carhacks] import: slot %d also from %s (two_guest_cities lever - "
			"measuring a SECOND source city, not supported)\n",
			slot, chkCityName(city));

		return 1;
	}

	printInfo("[carhacks] import: slot %d wants %s but this level already reads cars from %s - "
		"the engine holds ONE source city per level, so the entry is dropped\n",
		slot, chkCityName(city), chkCityName(gChkGuestCity));

	return 0;
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

	if (!chkClaimGuestCity(city, slot))
		return 0;

	e->used = 1;
	e->model = chkCarIdModel(id);
	e->city = city;
	gChkSetVersion++;

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
 * The config fallback: "import = slot:city:model, ..." and the traffic knock
 * ------------------------------------------------------------------------- */

int chkImportLoadConfig(const char* section, int count)
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

	if (city == level)
	{
		/* the level's own city: its list already has this car, so there is
		 * nothing to import - wantedCar alone is enough. */
		printInfo("[carhacks] import: the pick (%s model %d) is this level's own car - no import\n",
			chkCityName(city), model);
		return 0;
	}

	/* A FOREIGN pick goes into a spare resident slot. InitPlayer prefers a slot
	 * the model was IMPORTED into over a native one with the same number
	 * (players.c), so the spare slot wins even when the level also lists that
	 * number as one of its own civilians. */
	for (slot = CHK_IMPORT_SPARE_FIRST; slot < count && slot < CHK_IMPORT_MAX_SLOTS; slot++)
	{
		if (!gChkSet[slot].used)
			break;
	}

	if (slot >= count || slot >= CHK_IMPORT_MAX_SLOTS)
	{
		printInfo("[carhacks] import: the pick (%s model %d) needs a spare resident slot and "
			"none is free - riding the level's own car of that number\n",
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

void chkImportApplyToCarData(int count, int* models, int* modelSource)
{
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

	printInfo("[carhacks] import set: level %s, guest city %s, %d entr%s, version %d\n",
		chkCityName(level), chkCityName(gChkGuestCity),
		entries, (entries == 1) ? "y" : "ies", gChkSetVersion);
}
