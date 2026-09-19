// profiles/rows/deadstar.c — DEADSTAR: Rio's turbo-ram car.
//
//   identity   Rio (model 12)
//   special    special_deadstar — Death Dash: instant acceleration to turbo top
//              speed with the horn blaring twice (0.75 s, then a second full
//              blast); 2.5 s of dash that deals 4x collision damage and +15%
//              more on a near-90-degree hit into a car's side (see SPECIALS.md)
//   feel       light, high-power, built to ram
//
// The numbers below are the first-test placeholders — tune freely.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowDeadstar =
{
	CD2_VEH_DEADSTAR,	/* id */
	"deadstar",		/* internalName */
	"Deadstar",		/* displayName */

	CD2_VEH_CITY_RIO,	/* originCity */
	12,			/* modelSlot (CARMODEL_12) */

	/* armor, speed, handling, specialPower (1..5) */
	{ 3, 5, 3, 4 },

	/* special: id, internal name, display, capacity, recharge frames */
	{ CD2_WID_NONE, "special_deadstar", "Death Dash", 2, 750 },

	/* phys: mass, powerRatio, traction, susCoeff, wheelSize,
	 *       twistRateX, twistRateY, twistRateZ, cogY, topSpeedPct */
	{ 3400, 5200, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, 118 },

	-1			/* palette: special body, no extra palette */
};
