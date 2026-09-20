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
	/* It is a TRUCK. It was 5 speed / 3 handling, which made a heavy car the
	 * quickest and most agile thing on the field - the dash is supposed to be
	 * what makes it fast, not its chassis. */
	4, 3, 2, 4,

	/* special weapon (its name, ammo and recharge live in its own file) */
	CD2_WID_SPECIAL_DEADSTAR,

	/* phys: mass, powerRatio, traction, susCoeff, wheelSize,
	 *       twistRateX, twistRateY, twistRateZ, cogY, topSpeedPct */
	/* heavy and underpowered: weight is what a truck is for. It was 3400 mass
	 * against 5200 power with an 18% top-speed bonus, i.e. a featherweight. */
	{ 6800, 3400, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, 100 },

	-1			/* palette: special body, no extra palette */
};
