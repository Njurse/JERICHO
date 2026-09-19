// profiles/profiles_map.c — resolve the roster into the engine, and apply it.
//
// The engine knows cars only as "resident slot N" (a row in residentCarModels[]
// / car_cosmetics[]). This unit is the bridge from a profile's (city, model) to
// that slot, and from the slot to the two things a profile dictates:
//
//   * the car's native physics — the CD2_VEH_PHYS overrides are written into
//     car_cosmetics[slot] at level start, before any car is built, so the whole
//     sim (and cd2GetStats, which derives from these fields) sees them; and
//   * the car's paint — cp->ap.palette is set from the profile the first time a
//     car of that slot is stepped.
//
// Timing (verified in main.c): SetupResidentModels fires
// JER_EVENT_CAR_DATA_SOURCE, then LoadGameLevel->LoadCosmetics fills
// car_cosmetics[], THEN JER_EVENT_GAME_START fires, and only after that are the
// cars created. So CAR_DATA_SOURCE writes the resident models and GAME_START
// writes the cosmetics — both land before anything reads them.
//
// A profile that cannot be placed (every slot claimed) is left unresolved and
// logged; nothing else breaks.

#include "driver2.h"
#include "profile.h"
#include "cainescrossfire_internal.h"	/* cd2OwnsCar */
#include "rows/rows.h"
#include "cars.h"
#include "cosmetic.h"
#include "system.h"		/* LevelNames[] */
#include "mission.h"		/* GameLevel */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// State (reset each level from CAR_DATA_SOURCE)
// ---------------------------------------------------------------------------
static int gCd2VehSlot[CD2_VEH_COUNT];			// profile -> resident slot (-1)
static int gCd2SlotProfile[MAX_CAR_RESIDENT_MODELS];	// resident slot -> profile (-1)
static int gCd2VehFielded[CD2_VEH_COUNT];		// in this match's field?
static int gCd2VehPalDone[MAX_CARS];			// paint set for this car already
static int gCd2CarProfile[MAX_CARS];			// car -> profile (CD2_VEH_NONE)
static int gCd2VehPlayer = CD2_VEH_NONE;		// the player's chosen profile
static int gCd2GuestCity = -1;				// the one importable guest city

// ---------------------------------------------------------------------------
// Fielded set
// ---------------------------------------------------------------------------
static int cd2VehTokMatch(const char* tok, int len, const char* name)
{
	int i;

	for (i = 0; i < len; i++)
	{
		char a = tok[i];
		char b = name[i];

		if (b == 0)
			return 0;
		if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
		if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
		if (a != b)
			return 0;
	}

	return name[len] == 0;
}

// A comma/space separated list of internal names, case-insensitive.
static int cd2VehListHas(const char* list, const char* name)
{
	const char* p = list;
	int n = (int)strlen(name);

	while (*p != 0)
	{
		const char* q;

		while (*p == ' ' || *p == ',' || *p == '\t' || *p == ';')
			p++;

		if (*p == 0)
			break;

		q = p;
		while (*p != 0 && *p != ',' && *p != ' ' && *p != '\t' && *p != ';')
			p++;

		if (cd2VehTokMatch(q, (int)(p - q), name))
			return 1;
	}

	return 0;
}

// Which profiles this match fields: the config list, or — when empty — the
// profiles whose home city is the level's own. CC_PROFILES (a run-only env
// override, never persisted) wins, so a harness can field any set.
static void cd2VehBuildFielded(int level)
{
	const char* list = getenv("CC_PROFILES");
	int i;

	if (list == NULL || *list == 0)
		list = jer_config_get_str("cainescrossfire", "profiles", "");

	for (i = 0; i < CD2_VEH_COUNT; i++)
	{
		const CD2_VEH_PROFILE* p = cd2VehDef(i);

		if (p == NULL)
		{
			gCd2VehFielded[i] = 0;
			continue;
		}

		if (list != NULL && *list != 0)
			gCd2VehFielded[i] = cd2VehListHas(list, p->internalName);
		else
			gCd2VehFielded[i] = (p->originCity == level);
	}

	// The player must always have a car: if the field is empty (a level with no
	// home profile and no list), fall back to a run of the roster.
	{
		int any = 0;

		for (i = 0; i < CD2_VEH_COUNT; i++)
			any |= gCd2VehFielded[i];

		if (!any && CD2_VEH_COUNT > 0)
			gCd2VehFielded[0] = 1;
	}

	// the player's chosen profile is always on the field, so its model gets a slot
	if (gCd2VehPlayer != CD2_VEH_NONE)
		gCd2VehFielded[gCd2VehPlayer] = 1;
}

int cd2VehIsFielded(int profileId)
{
	if (profileId < 0 || profileId >= CD2_VEH_COUNT)
		return 0;

	return gCd2VehFielded[profileId];
}

int cd2VehPlayerProfile(void)
{
	return gCd2VehPlayer;
}

void cd2VehSetPlayerProfile(int profileId)
{
	gCd2VehPlayer = (profileId >= 0 && profileId < CD2_VEH_COUNT) ? profileId : CD2_VEH_NONE;
}

int cd2VehFieldedCount(void)
{
	int i, n = 0;

	for (i = 0; i < CD2_VEH_COUNT; i++)
		n += gCd2VehFielded[i] ? 1 : 0;

	return n;
}

int cd2VehSlotOf(int profileId)
{
	if (profileId < 0 || profileId >= CD2_VEH_COUNT)
		return -1;

	return gCd2VehSlot[profileId];
}

int cd2VehProfileOfSlot(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return CD2_VEH_NONE;

	return gCd2SlotProfile[slot];
}

int cd2VehOfCar(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL)
		return CD2_VEH_NONE;

	// The per-car assignment wins when set; otherwise fall back to the slot.
	if (cd2VehOfCarId(cp->id) != CD2_VEH_NONE)
		return cd2VehOfCarId(cp->id);

	return cd2VehProfileOfSlot(cp->ap.model);
}

int cd2VehOfCarId(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return CD2_VEH_NONE;

	return gCd2CarProfile[carId];
}

void cd2VehSetCarProfile(int carId, int profileId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	if (profileId < 0 || profileId >= CD2_VEH_COUNT)
		profileId = CD2_VEH_NONE;

	gCd2CarProfile[carId] = profileId;

	if (gCd2Cfg.debugLog)
		printInfo("[cainescrossfire] profile: car=%d -> %s (%s)\n",
			carId, cd2VehInternalName(profileId), cd2VehDisplayName(profileId));
}

void cd2VehForgetCar(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	gCd2CarProfile[carId] = CD2_VEH_NONE;
}

// ---------------------------------------------------------------------------
// Slot resolution (JER_EVENT_CAR_DATA_SOURCE)
// ---------------------------------------------------------------------------
static void cd2VehClaimSlot(int profileId, int slot)
{
	gCd2VehSlot[profileId] = slot;

	if (slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS)
		gCd2SlotProfile[slot] = profileId;
}

// Find a resident slot for a fielded profile's model.
//
// The engine can import from only ONE foreign city per level (InitCarImport
// holds a single city, models.c), and a native model needs no import at all -
// the level's own lump carries every model 0..12. So:
//
//   * a NATIVE profile (originCity == the level) reuses a resident slot that
//     already holds its model; failing that it takes an empty spare slot, the
//     level's own lump supplying the geometry (modelSource stays -1);
//   * a FOREIGN profile is placed only when its city is the level's one guest
//     city, into an empty spare slot with modelSource set for the import.
//
// Civilian slots (0..4) are never repurposed: they carry the level's own models
// and the ambient traffic, and stealing one is what made the level's cars look
// wrong. SPECIAL_CAR_SLOT is never taken.
static void cd2VehPlaceProfile(int profileId, JER_ARGS_CAR_DATA_SOURCE* a)
{
	const CD2_VEH_PROFILE* p = cd2VehDef(profileId);
	int native = (p != NULL) && (p->originCity == a->level);
	int slot;

	if (p == NULL || p->modelSlot < 0 || a->models == NULL)
		return;

	// a native model that is already resident: reuse its slot untouched
	if (native)
	{
		for (slot = 0; slot < a->count && slot < MAX_CAR_RESIDENT_MODELS; slot++)
		{
			if (a->models[slot] == p->modelSlot)
			{
				cd2VehClaimSlot(profileId, slot);

				printInfo("[cainescrossfire] profile %s -> resident slot %d (%s's own model %d)\n",
					p->internalName, slot, LevelNames[p->originCity], p->modelSlot);
				return;
			}
		}
	}
	else
	{
		// only one guest city can be imported per level
		if (gCd2GuestCity == -1)
			gCd2GuestCity = p->originCity;
		else if (gCd2GuestCity != p->originCity)
		{
			printInfo("[cainescrossfire] profile %s (%s): only one guest city per level (%s already), not fielded\n",
				p->internalName, LevelNames[p->originCity], LevelNames[gCd2GuestCity]);
			return;
		}
	}

	// an empty spare slot (5..SPECIAL_CAR_SLOT-1)
	for (slot = SPECIAL_CAR_SLOT - 1; slot >= 5; slot--)
	{
		if (slot >= a->count || slot >= MAX_CAR_RESIDENT_MODELS)
			continue;
		if (gCd2SlotProfile[slot] != -1)
			continue;
		if (a->models[slot] != -1)
			continue;		// don't clobber what carhacks (or a level) put here

		a->models[slot] = p->modelSlot;

		if (a->modelSource != NULL)
			a->modelSource[slot] = native ? -1 : p->originCity;

		cd2VehClaimSlot(profileId, slot);

		printInfo("[cainescrossfire] profile %s -> resident slot %d (%s %s model %d)\n",
			p->internalName, slot, native ? "own" : "imported", LevelNames[p->originCity], p->modelSlot);
		return;
	}

	printInfo("[cainescrossfire] profile %s: no free spare slot for %s model %d, not fielded\n",
		p->internalName, LevelNames[p->originCity], p->modelSlot);
}

static void cd2VehResetState(void)
{
	int i;

	for (i = 0; i < CD2_VEH_COUNT; i++)
	{
		gCd2VehSlot[i] = -1;
		gCd2VehFielded[i] = 0;
	}

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		gCd2SlotProfile[i] = -1;

	for (i = 0; i < MAX_CARS; i++)
	{
		gCd2VehPalDone[i] = 0;
		gCd2CarProfile[i] = CD2_VEH_NONE;
	}

	gCd2GuestCity = -1;
}

static int cd2VehOnCarDataSource(void* ud, void* args)
{
	JER_ARGS_CAR_DATA_SOURCE* a = (JER_ARGS_CAR_DATA_SOURCE*)args;
	int i;

	(void)ud;

	cd2VehResetState();
	cd2VehBuildFielded(a->level);

	for (i = 0; i < CD2_VEH_COUNT; i++)
	{
		if (gCd2VehFielded[i])
			cd2VehPlaceProfile(i, a);
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Cosmetic overrides (GAME_START — after LoadCosmetics, before the cars)
// ---------------------------------------------------------------------------
static void cd2VehApplyCosmetics(void)
{
	int i;

	for (i = 0; i < CD2_VEH_COUNT; i++)
	{
		const CD2_VEH_PROFILE* p = cd2VehDef(i);
		const CD2_VEH_PHYS* ph;
		int slot = gCd2VehSlot[i];
		CAR_COSMETICS* cc;

		if (p == NULL || slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
			continue;

		ph = &p->phys;
		cc = &car_cosmetics[slot];

		// CD2_VEH_INHERIT (0) leaves the model's own value in place; a set field
		// is written over it. carCos fields are short, and every value here is
		// well inside that range.
		if (ph->mass)       cc->mass       = (short)ph->mass;
		if (ph->powerRatio) cc->powerRatio = (short)ph->powerRatio;
		if (ph->traction)   cc->traction   = (short)ph->traction;
		if (ph->susCoeff)   cc->susCoeff   = (short)ph->susCoeff;
		if (ph->wheelSize)  cc->wheelSize  = (short)ph->wheelSize;
		if (ph->twistRateX) cc->twistRateX = (short)ph->twistRateX;
		if (ph->twistRateY) cc->twistRateY = (short)ph->twistRateY;
		if (ph->twistRateZ) cc->twistRateZ = (short)ph->twistRateZ;
		if (ph->cogY)       cc->cog.vy     = (short)ph->cogY;

		printInfo("[cainescrossfire] profile %s: slot %d cosmetics mass=%d power=%d traction=%d wheelSize=%d topSpeedPct=%d\n",
			p->internalName, slot, cc->mass, cc->powerRatio, cc->traction,
			cc->wheelSize, ph->topSpeedPct);
	}
}

// ---------------------------------------------------------------------------
// Paint (per car, first time it is stepped)
// ---------------------------------------------------------------------------
static int cd2VehOnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	int profileId;

	(void)ud;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	// Only the module's own cars (the player and the AI opponents) carry a
	// profile — ambient traffic must not be mis-identified by a repurposed slot.
	if (!cd2OwnsCar(cp))
		return JER_RESULT_CONTINUE;

	// Record the car's profile once, from its resident slot.
	profileId = cd2VehOfCarId(cp->id);

	if (profileId == CD2_VEH_NONE)
	{
		profileId = cd2VehProfileOfSlot(cp->ap.model);

		if (profileId != CD2_VEH_NONE)
		{
			const CD2_VEH_PROFILE* p = cd2VehDef(profileId);

			cd2VehSetCarProfile(cp->id, profileId);

			// hand the car its special weapon. The profile only NAMES the
			// weapon; the weapon's own def carries the ammo (maxAmmo) and the
			// recharge. The player also STARTS on it.
			if (p != NULL && p->specialWeapon != CD2_WID_NONE)
			{
				const CD2_WEAPON_DEF* wd = cd2WpnDef(p->specialWeapon);

				if (wd != NULL)
				{
					cd2WpnCarGrant(cp, p->specialWeapon, wd->maxAmmo);
					cd2WpnCarSelect(cp, p->specialWeapon);
				}
			}
		}
	}

	if (gCd2VehPalDone[cp->id])
		return JER_RESULT_CONTINUE;

	if (profileId != CD2_VEH_NONE)
	{
		const CD2_VEH_PROFILE* p = cd2VehDef(profileId);

		if (p != NULL && p->palette >= 0)
		{
			cp->ap.palette = (char)p->palette;

			if (gCd2Cfg.debugLog)
				printInfo("[cainescrossfire] profile %s: car %d paint -> palette %d\n",
					p->internalName, cp->id, p->palette);
		}

		gCd2VehPalDone[cp->id] = 1;
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static int cd2VehOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gCd2VehPalDone[i] = 0;
		gCd2CarProfile[i] = CD2_VEH_NONE;
	}

	cd2VehApplyCosmetics();

	return JER_RESULT_CONTINUE;
}

// JER_EVENT_FRONTEND_ENTERED — back in the menus: drop the module's vehicle
// override, so the next start begins from the level's own cars and not the last
// match's pick. (The engine clears wantedCar itself in ReInitFrontend; the
// player's profile is the module's to clear.)
static int cd2VehOnFrontendEntered(void* ud, void* args)
{
	(void)ud;
	(void)args;

	cd2VehSetPlayerProfile(CD2_VEH_NONE);
	cd2VehResetState();

	printInfo("[cainescrossfire] frontend: cleared the vehicle profile override\n");
	return JER_RESULT_CONTINUE;
}

static int cd2VehOnResetCar(void* ud, void* args)
{
	JER_ARGS_RESET_CAR* a = (JER_ARGS_RESET_CAR*)args;

	(void)ud;

	if (a != NULL && a->carId >= 0 && a->carId < MAX_CARS)
	{
		gCd2VehPalDone[a->carId] = 0;
		gCd2CarProfile[a->carId] = CD2_VEH_NONE;
	}

	return JER_RESULT_CONTINUE;
}

void cd2VehDumpResolution(void)
{
	int i;

	printInfo("[cainescrossfire] profile field: %d fielded this level\n", cd2VehFieldedCount());

	for (i = 0; i < CD2_VEH_COUNT; i++)
	{
		if (!gCd2VehFielded[i])
			continue;

		printInfo("[cainescrossfire]   %s -> slot %d\n", cd2VehInternalName(i), gCd2VehSlot[i]);
	}
}

void cd2VehRegister(JERICHO_CONTEXT* ctx)
{
	cd2VehResetState();

	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DATA_SOURCE, cd2VehOnCarDataSource, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2VehOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2VehOnFrontendEntered, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2VehOnCarStep, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, cd2VehOnResetCar, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] %d vehicle profile(s) registered\n", cd2VehCount());
}
