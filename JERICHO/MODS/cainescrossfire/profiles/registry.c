// profiles/registry.c — the vehicle-profile registry + accessors.
//
// The roster is DATA: profiles/*.json is generated into
// profiles/generated_profiles.c by tools/gen_profiles.py, and this file turns
// that table into the runtime API. There is no hand-maintained manifest and no
// compile-time per-car enum any more.
//
// Two resolutions happen here, and they are deliberately separate:
//
//   homeCity         the contestant's stock arena (CHICAGO..RIO). This is what
//                    "field the home city's roster" reads.
//   vehicleCity      where the car's geometry comes from - a stock city OR a
//                    carhacks car-data bank named in the JSON (e.g. CROSSFIRE).
//                    A bank is resolved through JerCityName(), and registered
//                    on demand from the profile's optional bankLev/bankLcf.
//
// A special weapon is named ("special_hornet") and resolved here through
// cd2WpnFindByName; everything about the weapon stays in its own def.

#include "driver2.h"
#include "system.h"			/* JerCityName / JerCityRegister */
#include "profile.h"

#include <string.h>

extern const CD2_VEH_PROFILE cd2VehBuiltinProfiles[];
extern const int cd2VehBuiltinCount;

static int gVehSpecial[CD2_VEH_MAX_PROFILES];
static int gVehVehicleCity[CD2_VEH_MAX_PROFILES];
static char gVehCityLogged[CD2_VEH_MAX_PROFILES];
static char gVehSpecialLogged[CD2_VEH_MAX_PROFILES];
static int gVehLoaded;

static int cd2VehCaseCmp(const char* a, const char* b)
{
	while (*a != 0 && *b != 0)
	{
		int ca = *a, cb = *b;

		if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';

		if (ca != cb)
			return ca - cb;

		a++;
		b++;
	}

	return (unsigned char)*a - (unsigned char)*b;
}

// A stock city name or a registered bank name -> city index, or -1.
static int cd2VehCityFromName(const char* name)
{
	int i;

	if (name == NULL || name[0] == 0)
		return -1;

	if (cd2VehCaseCmp(name, "chicago") == 0) return CD2_VEH_CITY_CHICAGO;
	if (cd2VehCaseCmp(name, "havana") == 0)  return CD2_VEH_CITY_HAVANA;
	if (cd2VehCaseCmp(name, "vegas") == 0)   return CD2_VEH_CITY_VEGAS;
	if (cd2VehCaseCmp(name, "rio") == 0)     return CD2_VEH_CITY_RIO;

	for (i = 0; i < JerCityCount(); i++)
	{
		if (JerCityRegistered(i) && cd2VehCaseCmp(JerCityName(i), name) == 0)
			return i;
	}

	return -1;
}

static const char* cd2VehCityLabel(int city)
{
	const char* name = (city >= 0) ? JerCityName(city) : "";

	return (name != NULL && name[0] != 0) ? name : "?";
}

const CD2_VEH_PROFILE* cd2VehDef(int profileId)
{
	if (profileId < 0 || profileId >= cd2VehBuiltinCount ||
	    profileId >= CD2_VEH_MAX_PROFILES)
		return NULL;

	return &cd2VehBuiltinProfiles[profileId];
}

int cd2VehCount(void)
{
	return cd2VehBuiltinCount;
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

int cd2VehHomeCityOf(int profileId)
{
	const CD2_VEH_PROFILE* p = cd2VehDef(profileId);

	return (p != NULL) ? p->homeCity : -1;
}

// Resolve the vehicle's source. A stock city is immediate; a bank is looked up
// by name and, when the profile carries bankLev, registered on demand. The
// resolution is cached only on success, so a bank installed later in the same
// session is picked up on the next level.
int cd2VehVehicleCityOf(int profileId)
{
	const CD2_VEH_PROFILE* p = cd2VehDef(profileId);
	int city;

	if (p == NULL)
		return -1;

	if (gVehVehicleCity[profileId] >= 0)
		return gVehVehicleCity[profileId];

	city = cd2VehCityFromName(p->vehicleCityName);

	if (city < 0 && p->bankLev != NULL && p->bankLev[0] != 0)
	{
		city = JerCityRegister(p->vehicleCityName, p->bankLev,
			(p->bankLcf != NULL && p->bankLcf[0] != 0) ? p->bankLcf : NULL);

		if (city >= 0 && !gVehCityLogged[profileId])
		{
			gVehCityLogged[profileId] = 1;
			printInfo("[cainescrossfire] profile %s: bank %s registered as city %d (%s)\n",
				p->internalName, p->vehicleCityName, city, p->bankLev);
		}
	}

	if (city >= 0)
	{
		gVehVehicleCity[profileId] = city;
		return city;
	}

	if (!gVehCityLogged[profileId])
	{
		gVehCityLogged[profileId] = 1;
		printInfo("[cainescrossfire] profile %s: vehicle source %s is not registered%s\n",
			p->internalName, p->vehicleCityName,
			(p->bankLev != NULL && p->bankLev[0] != 0) ? " and its .LEV is not on disk" : "");
	}

	return -1;
}

int cd2VehSpecialOf(int profileId)
{
	const CD2_VEH_PROFILE* p = cd2VehDef(profileId);

	if (p == NULL)
		return CD2_WID_NONE;

	if (!gVehLoaded)
		cd2VehLoad();

	return gVehSpecial[profileId];
}

void cd2VehLoad(void)
{
	int i;

	gVehLoaded = 1;

	for (i = 0; i < cd2VehBuiltinCount && i < CD2_VEH_MAX_PROFILES; i++)
	{
		const CD2_VEH_PROFILE* p = &cd2VehBuiltinProfiles[i];

		gVehVehicleCity[i] = -1;
		gVehCityLogged[i] = 0;
		gVehSpecialLogged[i] = 0;

		if (p->specialName == NULL || p->specialName[0] == 0)
		{
			gVehSpecial[i] = CD2_WID_NONE;
			continue;
		}

		gVehSpecial[i] = cd2WpnFindByName(p->specialName);

		if (gVehSpecial[i] == CD2_WID_NONE)
		{
			gVehSpecialLogged[i] = 1;
			printInfo("[cainescrossfire] profile %s: unknown special '%s'\n",
				p->internalName, p->specialName);
		}
	}
}

int cd2VehFindByVehicle(int city, int model)
{
	int i;

	for (i = 0; i < cd2VehCount(); i++)
	{
		const CD2_VEH_PROFILE* p = cd2VehDef(i);

		if (p != NULL && p->modelSlot >= 0 &&
		    cd2VehVehicleCityOf(i) == city && p->modelSlot == model)
			return i;
	}

	return CD2_VEH_NONE;
}

int cd2VehFindByName(const char* name)
{
	int i;

	if (name == NULL || name[0] == 0)
		return CD2_VEH_NONE;

	for (i = 0; i < cd2VehCount(); i++)
	{
		const CD2_VEH_PROFILE* p = cd2VehDef(i);

		if (p != NULL && strcmp(p->internalName, name) == 0)
			return i;
	}

	return CD2_VEH_NONE;
}

// One line per profile: the evidence a headless run is read for. Unconditional
// and cheap, so it can fire at boot before anyone flips a debug key.
void cd2VehDumpProfiles(void)
{
	int i;

	printInfo("[cainescrossfire] profiles: %d registered\n", cd2VehCount());

	for (i = 0; i < cd2VehCount(); i++)
	{
		const CD2_VEH_PROFILE* p = cd2VehDef(i);
		int vehicleCity;
		int special;

		if (p == NULL)
		{
			printInfo("[cainescrossfire]   [%d] <null row>\n", i);
			continue;
		}

		vehicleCity = cd2VehVehicleCityOf(i);
		special = cd2VehSpecialOf(i);

		{
			const CD2_WEAPON_DEF* wd = cd2WpnDef(special);

			printInfo("[cainescrossfire]   [%d] %s (%s) home %s vehicle %s/%d stats A%d S%d H%d P%d special %s \"%s\" x%d rec%d palette %d\n",
				i, p->internalName, p->displayName,
				cd2VehCityLabel(p->homeCity),
				(vehicleCity >= 0) ? cd2VehCityLabel(vehicleCity) : p->vehicleCityName,
				p->modelSlot,
				p->armor, p->speed, p->handling, p->specialPower,
				(wd != NULL) ? wd->name : "(none)",
				(wd != NULL) ? cd2WpnDisplayName(special) : "-",
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
