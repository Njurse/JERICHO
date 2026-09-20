// profiles/rows/fixer.c — FIXER (Chicago, model 2).
//
// The sniper. Its special is a PLACEHOLDER: a laser it has to hold on you, and
// the longer it holds, the worse the shot that follows is. See
// weapons/special/fixer.c.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowFixer =
{
	CD2_VEH_FIXER,
	"fixer",
	"Fixer",

	CD2_VEH_CITY_CHICAGO,
	2,			// CARMODEL_2 in Chicago

	// armor, speed, handling, specialPower (1..5)
	3, 3, 4, 5,

	// special weapon (name, ammo and recharge live in weapons/special/fixer.c)
	CD2_WID_SPECIAL_FIXER,

	// mass, powerRatio, traction, susCoeff, wheelSize, twistX, twistY, twistZ, cogY, topSpeedPct
	{ 3800, 3800, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT },

	3			// palette
};
