// profiles/rows/wheelman.c — WHEELMAN (Chicago, model 12).
//
// The bomb. Its special is a PLACEHOLDER: a bang at its own position and
// everything around it pays. See weapons/special/wheelman.c.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowWheelman =
{
	CD2_VEH_WHEELMAN,
	"wheelman",
	"Wheelman",

	CD2_VEH_CITY_CHICAGO,
	12,			// CARMODEL_12 in Chicago

	// armor, speed, handling, specialPower (1..5)
	4, 3, 3, 4,

	// special weapon (name, ammo and recharge live in weapons/special/wheelman.c)
	CD2_WID_SPECIAL_WHEELMAN,

	// mass, powerRatio, traction, susCoeff, wheelSize, twistX, twistY, twistZ, cogY, topSpeedPct
	{ 4600, 3600, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT },

	-1			// palette: inherit
};
