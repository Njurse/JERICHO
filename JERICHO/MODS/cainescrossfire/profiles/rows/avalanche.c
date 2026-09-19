// profiles/rows/avalanche.c — AVALANCHE: the Vegas monster truck.
//
//   identity   Vegas (model 11)
//   special    special_avalanche — a monster-truck crush: a short speed/grip
//              surge, and on ramming a car it climbs on top, pins it (zero
//              velocity), spins its tyres while still steering, then shoves it
//              off (see SPECIALS.md)
//   feel       very heavy: big mass and traction, bigger wheels, slower top
//
// The numbers below are the first-test placeholders — tune freely.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowAvalanche =
{
	CD2_VEH_AVALANCHE,	/* id */
	"avalanche",		/* internalName */
	"Avalanche",		/* displayName */

	CD2_VEH_CITY_VEGAS,	/* originCity */
	11,			/* modelSlot (CARMODEL_11) */

	/* armor, speed, handling, specialPower (1..5) */
	{ 4, 2, 3, 4 },

	/* special: id, internal name, display, capacity, recharge frames */
	{ CD2_WID_SPECIAL_AVALANCHE, "special_avalanche", "Monster Crush", 2, 900 },

	/* phys: mass, powerRatio, traction, susCoeff, wheelSize,
	 *       twistRateX, twistRateY, twistRateZ, cogY, topSpeedPct */
	{ 6800, 3800, 5400, CD2_VEH_INHERIT, 1400,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, 88 },

	-1			/* palette: special body, no extra palette */
};
