/* jer_anim.c — the JERICHO ped-animation API implementation.
 * See jer_anim.h + JERICHO/docs/ped-animation.md.
 *
 * jer_anim_bone_rotation() lives in the GAME (motion_c.c), where the real
 * BONE struct is visible — the core deliberately stays free of the game
 * headers, and a hand-rolled layout mirror would be fragile against the
 * engine's packing rules. This file keeps only the pure helpers. */

#include "jer_anim.h"
#include "jer_internal.h"

int jer_anim_aim_diff(int bodyHeading, int aimYaw)
{
	int d = (bodyHeading - aimYaw) & 0xfff;

	if (d >= 2048)
		d -= 4096;

	return d;
}
