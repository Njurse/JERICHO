// profiles/rows/corvo.c — CORVO: Rio's siren car.
//
//   identity   Rio (model 0)
//   special    special_corvo — the siren runs for 4 s and throws a revolving
//              lightning bolt that zaps in-range cars: shaky twist velocity,
//              a small vertical jump on the first hit, moderate damage
//              (see SPECIALS.md)
//   feel       middleweight, engine-biased (a touch of extra power)
//
// The numbers below are the first-test placeholders — tune freely.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowCorvo =
{
	CD2_VEH_CORVO,		/* id */
	"corvo",		/* internalName */
	"Corvo",		/* displayName */

	CD2_VEH_CITY_RIO,	/* originCity */
	0,			/* modelSlot (CARMODEL_0) */

	/* armor, speed, handling, specialPower (1..5) */
	{ 3, 3, 3, 4 },

	/* special: id, internal name, display, capacity, recharge frames */
	{ CD2_WID_NONE, "special_corvo", "Siren's Wrath", 3, 540 },

	/* phys: mass, powerRatio, traction, susCoeff, wheelSize,
	 *       twistRateX, twistRateY, twistRateZ, cogY, topSpeedPct */
	{ CD2_VEH_INHERIT, 4300, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT },

	-1			/* palette: model 0 has no extra palette */
};
