// profiles/rows/invocada.c — INVOCADA (Rio, model 3).
//
// The storm. Heavy, slow to turn, and it does not need to catch you: its special
// drags you in. See weapons/special/invocada.c.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowInvocada =
{
	CD2_VEH_INVOCADA,
	"invocada",
	"Invocada",

	CD2_VEH_CITY_RIO,
	3,			// CARMODEL_3 in Rio

	// armor, speed, handling, specialPower (1..5)
	4, 3, 2, 5,

	// special weapon (name, ammo and recharge live in weapons/special/invocada.c)
	CD2_WID_SPECIAL_INVOCADA,

	// mass, powerRatio, traction, susCoeff, wheelSize, twistX, twistY, twistZ, cogY, topSpeedPct
	{ 5200, 3400, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT },

	0			// palette
};
