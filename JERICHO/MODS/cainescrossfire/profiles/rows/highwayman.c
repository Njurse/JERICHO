// profiles/rows/highwayman.c — HIGHWAYMAN: Havana's flamethrower car.
//
//   identity   Havana (model 1)
//   special    special_highwayman — Breath of Fire: a 4 s narrow cone of huge
//              flames from the nose; tapping fire mid-burn launches homing
//              fireballs that explode for extra damage (see SPECIALS.md)
//   feel       light and aggressive, a bit of extra power over mass
//
// The numbers below are the first-test placeholders — tune freely.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowHighwayman =
{
	CD2_VEH_HIGHWAYMAN,	/* id */
	"highwayman",		/* internalName */
	"Highwayman",		/* displayName */

	CD2_VEH_CITY_HAVANA,	/* originCity */
	1,			/* modelSlot (CARMODEL_1) */

	/* armor, speed, handling, specialPower (1..5) */
	3, 3, 3, 5,

	/* special weapon (its name, ammo and recharge live in its own file) */
	CD2_WID_SPECIAL_HIGHWAYMAN,

	/* phys: mass, powerRatio, traction, susCoeff, wheelSize,
	 *       twistRateX, twistRateY, twistRateZ, cogY, topSpeedPct */
	{ 3600, 4600, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT },

	1			/* palette (placeholder) */
};
