// profiles/rows/obelisk.c — OBELISK (Rio, model 9).
//
// The barrage platform. Slow to turn and no bruiser, but its special floods the
// road with cheap homing missiles, so it wants to be pointed at whatever it is
// killing and left alone to do it. See weapons/special/obelisk.c for the salvo.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowObelisk =
{
	CD2_VEH_OBELISK,
	"obelisk",
	"Obelisk",

	CD2_VEH_CITY_RIO,
	9,			// CARMODEL_9 in Rio

	// armor, speed, handling, specialPower (1..5)
	3, 3, 2, 5,

	// special weapon (name, ammo and recharge live in weapons/special/obelisk.c)
	CD2_WID_SPECIAL_OBELISK,

	// mass, powerRatio, traction, susCoeff, wheelSize, twistX, twistY, twistZ, cogY, topSpeedPct
	{ 4400, 3600, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT },

	-1			// palette: inherit
};
