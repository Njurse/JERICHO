// profiles/rows/bootlegger.c — BOOTLEGGER (Vegas, model 1).
//
// The runner. Light and quick, built to get out of trouble rather than to sit in
// it. Its special is a PLACEHOLDER: the gunner's window gun, held open for five
// seconds and wound up to a rate nothing can stand in front of for long.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowBootlegger =
{
	CD2_VEH_BOOTLEGGER,
	"bootlegger",
	"Bootlegger",

	CD2_VEH_CITY_VEGAS,
	1,			// CARMODEL_1 in Vegas

	// armor, speed, handling, specialPower (1..5)
	2, 5, 4, 3,

	// special weapon (name, ammo and recharge live in weapons/special/bootlegger.c)
	CD2_WID_SPECIAL_BOOTLEGGER,

	// mass, powerRatio, traction, susCoeff, wheelSize, twistX, twistY, twistZ, cogY, topSpeedPct
	{ 3000, 4600, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, 110 },

	1			// palette
};
