// profiles/rows/hornet.c — HORNET: Chicago's light, spiky skirmisher.
//
//   identity   Chicago (model 3)
//   special    special_hornet — deploys a ring of spikes that punish contact,
//              or launches them forward as weak-homing darts (see SPECIALS.md)
//   feel       light and quick: low mass, a little extra top speed; traction
//              stays stock (it is quick, not grippy)
//
// The numbers below are the first-test placeholders — tune freely.

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowHornet =
{
	CD2_VEH_HORNET,		/* id */
	"hornet",		/* internalName */
	"Hornet",		/* displayName */

	CD2_VEH_CITY_CHICAGO,	/* originCity (LevelNames index) */
	3,			/* modelSlot (CARMODEL_3) */

	/* armor, speed, handling, specialPower (1..5) */
	{ 2, 4, 4, 3 },

	/* special weapon: id (until registered), internal name, display, capacity, recharge frames (30 Hz) */
	{ CD2_WID_NONE, "special_hornet", "Spike Storm", 3, 600 },

	/* phys: mass, powerRatio, traction, susCoeff, wheelSize,
	 *       twistRateX, twistRateY, twistRateZ, cogY, topSpeedPct
	 * CD2_VEH_INHERIT (0) = keep the model's own value. */
	{ 3000, 4400, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT,
	  CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, CD2_VEH_INHERIT, 115 },

	3			/* palette (placeholder) */
};
