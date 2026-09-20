// profiles/rows/bruxa.c — BRUXA: Rio's twin-barrel shotgun car.
//
//   identity   Rio (model 2)
//   special    special_bruxa — the repurposed double shotgun: twice the
//              pellets, a hard recoil shoved into the shooter, and a recoil on
//              the victim scaled by distance and how many pellets connected,
//              with a loud booming report (see SPECIALS.md)
//   feel       heavy and planted, built to soak up its own recoil
//
// The numbers below are the first-test placeholders — tune freely.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowBruxa =
{
	CD2_VEH_BRUXA,		/* id */
	"bruxa",		/* internalName */
	"Bruxa",		/* displayName */

	CD2_VEH_CITY_RIO,	/* originCity */
	2,			/* modelSlot (CARMODEL_2) */

	/* armor, speed, handling, specialPower (1..5) */
	4, 2, 3, 4,

	/* special weapon (its name, ammo and recharge live in its own file) */
	CD2_WID_SPECIAL_BRUXA,

	/* phys: mass, powerRatio, traction, susCoeff, wheelSize,
	 *       twistRateX, twistRateY, twistRateZ, cogY, topSpeedPct */
	{ 4800, 4800, 4600, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT },

	2			/* palette */
};
