// profiles/registry.c — the vehicle-profile manifest + accessors.
//
// THIS FILE IS THE MANIFEST. The rows themselves live one-per-file in
// profiles/rows/ (see rows/rows.h); this is the single line-each list that
// brings them into an id-indexed table, exactly as gWdefs[] does for weapons
// and sFac[] does for factions. The list order MUST match the CD2_VEH_* enum in
// profile.h — the array index IS the profile id.
//
// To add a vehicle: copy a row file into rows/, add its extern to rows.h, and
// add one line below. Nothing else.

#include "driver2.h"
#include "profile.h"
#include "rows/rows.h"

// The manifest. Index == CD2_VEH_*.
static const CD2_VEH_PROFILE* const gVehRows[CD2_VEH_COUNT] =
{
	&cd2VehRowHornet,	/* CD2_VEH_HORNET */
	&cd2VehRowAvalanche,	/* CD2_VEH_AVALANCHE */
	&cd2VehRowCorvo,	/* CD2_VEH_CORVO */
	&cd2VehRowBruxa,	/* CD2_VEH_BRUXA */
	&cd2VehRowHighwayman,	/* CD2_VEH_HIGHWAYMAN */
	&cd2VehRowDeadstar,	/* CD2_VEH_DEADSTAR */
	&cd2VehRowObelisk,	/* CD2_VEH_OBELISK */
	&cd2VehRowBootlegger,	/* CD2_VEH_BOOTLEGGER */
	&cd2VehRowInvocada,	/* CD2_VEH_INVOCADA */
	&cd2VehRowFixer,		/* CD2_VEH_FIXER */
	&cd2VehRowWheelman	/* CD2_VEH_WHEELMAN */
};

const CD2_VEH_PROFILE* cd2VehDef(int profileId)
{
	if (profileId < 0 || profileId >= CD2_VEH_COUNT)
		return NULL;

	return gVehRows[profileId];
}

int cd2VehCount(void)
{
	return CD2_VEH_COUNT;
}

const char* cd2VehInternalName(int profileId)
{
	const CD2_VEH_PROFILE* p = cd2VehDef(profileId);

	return (p != NULL) ? p->internalName : "-";
}

const char* cd2VehDisplayName(int profileId)
{
	const CD2_VEH_PROFILE* p = cd2VehDef(profileId);

	return (p != NULL) ? p->displayName : "-";
}

// A vehicle is identified by (city, model number) — the same model number is a
// different car in each city, so both must match. A profile with modelSlot < 0
// has no fixed model and never matches.
int cd2VehFindByVehicle(int city, int model)
{
	int i;

	for (i = 0; i < CD2_VEH_COUNT; i++)
	{
		const CD2_VEH_PROFILE* p = gVehRows[i];

		if (p != NULL && p->modelSlot >= 0 && p->originCity == city && p->modelSlot == model)
			return i;
	}

	return CD2_VEH_NONE;
}

// The city a model number lives in, as a LevelNames[] name, for the dump.
static const char* cd2VehCityName(int city)
{
	switch (city)
	{
	case CD2_VEH_CITY_CHICAGO:	return "CHICAGO";
	case CD2_VEH_CITY_HAVANA:	return "HAVANA";
	case CD2_VEH_CITY_VEGAS:	return "VEGAS";
	case CD2_VEH_CITY_RIO:		return "RIO";
	default:			return "?";
	}
}

// One line per profile: the evidence a headless run is read for. Unconditional
// and cheap (six lines), so it can fire at boot before anyone flips a debug key.
void cd2VehDumpProfiles(void)
{
	int i;

	printInfo("[cainescrossfire] profiles: %d registered\n", CD2_VEH_COUNT);

	for (i = 0; i < CD2_VEH_COUNT; i++)
	{
		const CD2_VEH_PROFILE* p = gVehRows[i];
		const char* city;

		if (p == NULL)
		{
			printInfo("[cainescrossfire]   [%d] <null row>\n", i);
			continue;
		}

		city = cd2VehCityName(p->originCity);

		// the special's details come from the WEAPON, not the profile
		{
			const CD2_WEAPON_DEF* wd = cd2WpnDef(p->specialWeapon);

			printInfo("[cainescrossfire]   [%d] %s (%s) %s/%d stats A%d S%d H%d P%d special %s \"%s\" x%d rec%d palette %d\n",
				i, p->internalName, p->displayName, city, p->modelSlot,
				p->armor, p->speed, p->handling, p->specialPower,
				(wd != NULL) ? wd->name : "(none)",
				(wd != NULL) ? cd2WpnDisplayName(p->specialWeapon) : "-",
				(wd != NULL) ? wd->maxAmmo : 0,
				(wd != NULL) ? wd->refireCooldown : 0,
				p->palette);
		}

		// the crew mount deltas (a limo/truck body moves its own mount; all
		// zero = the module default). Printed for every profile so a run shows
		// which bodies carry an override.
		printInfo("[cainescrossfire]       crew offsets lat{%d,%d} fwd{%d,%d} up{%d,%d} yaw{%d,%d} sill %d arm %d%%\n",
			p->crew.lat[0], p->crew.lat[1], p->crew.fwd[0], p->crew.fwd[1],
			p->crew.up[0], p->crew.up[1], p->crew.yaw[0], p->crew.yaw[1],
			p->crew.sillRaise, p->crew.armScale ? p->crew.armScale : 100);
	}
}
