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

int jer_anim_torso_yaw(void* skel, int bodyYaw)
{
	JER_BONE_ROT* root = jer_anim_bone_rotation(skel, JER_LIMB_ROOT);
	JER_BONE_ROT* lowerback = jer_anim_bone_rotation(skel, JER_LIMB_LOWERBACK);
	int y = bodyYaw;

	if (root != NULL)
		y += root->vy;
	if (lowerback != NULL)
		y += lowerback->vy;

	return y & 0xfff;
}

/* matches the validated baseline when the animation yaws are zero:
 * DIFF_ANGLES(dir, aimYaw - 2048) = wrap((aimYaw - 2048) - dir) */
int jer_anim_twist_to_aim(void* skel, int bodyYaw, int aimYaw)
{
	int torso = jer_anim_torso_yaw(skel, bodyYaw);
	int d = ((aimYaw - 2048) - torso) & 0xfff;

	if (d >= 2048)
		d -= 4096;

	return d;
}
