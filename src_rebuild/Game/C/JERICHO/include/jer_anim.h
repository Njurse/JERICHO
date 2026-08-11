#ifndef JER_ANIM_H
#define JER_ANIM_H

/* jer_anim — the JERICHO ped-animation API.
 *
 * Opens the missing window in the ped draw pipeline: JER_EVENT_PED_POSE
 * fires in DrawTanner() between SetupTannerSkeleton and newRotateBones —
 * the ONLY place a per-bone ROTATION write (mutating the raw motion bytes
 * a bone's pvRotation points at) actually takes effect (newRotateBones is
 * the sole reader and runs right after the hook). PED_SKELETON phase-0
 * remains the channel for per-bone POSITION (vCurrPos) writes. See
 * JERICHO/docs/ped-animation.md for the full pipeline map and clobber
 * table.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "jericho.h"

/* limb indices mirror motion_c.c's LIMBS enum (stable engine constants) */
enum
{
	JER_LIMB_ROOT = 0,
	JER_LIMB_LOWERBACK,
	JER_LIMB_JOINT_1,	/* the upper-body root: torso + arms + head */
	JER_LIMB_NECK,
	JER_LIMB_HEAD,
	JER_LIMB_LSHOULDER,
	JER_LIMB_LELBOW,
	JER_LIMB_LHAND,
	JER_LIMB_LFINGERS,
	JER_LIMB_RSHOULDER,
	JER_LIMB_RELBOW,
	JER_LIMB_RHAND,
	JER_LIMB_RFINGERS,
	JER_LIMB_HIPS,
	JER_LIMB_COUNT
};

/* --- helpers ------------------------------------------------------ */

/* the per-frame rotation a bone's pvRotation points at, as a plain
 * writable 3-short (mirrors SVECTOR; the module needn't see the engine's
 * bone structs). Write ->vy (and vx/vz) here from a PED_POSE handler. */
typedef struct JER_BONE_ROT
{
	short vx, vy, vz;
} JER_BONE_ROT;

/* resolve a limb to its writable per-frame rotation (NULL when the bone
 * has no rotation data). Defined in the GAME (motion_c.c), where the real
 * BONE layout lives; safe to call from a PED_POSE handler. */
JER_BONE_ROT* jer_anim_bone_rotation(void* skel, int limb);

/* the wrap-safe signed diff (normalized to -2048..2048) between a body
 * heading and an aim yaw — the value to write for aim-mode torso/head
 * alignment. */
int jer_anim_aim_diff(int bodyHeading, int aimYaw);

/* the COMPOSED upper-body yaw — the direction the torso actually faces:
 * bodyYaw + ROOT.pvRotation.vy + LOWERBACK.pvRotation.vy. The walk/run
 * cycle's own torso sway composes into the upper-body frame (newRotateBones
 * builds matrix(LOWERBACK) from ROOT.pvRotation and matrix(JOINT_1) from
 * LOWERBACK.pvRotation; bReverseYRotation is 0 in normal play so the ROOT
 * yaw is not negated). Measuring the aim twist against this instead of the
 * raw body heading keeps the aim axis on the camera line even mid-stride. */
int jer_anim_torso_yaw(void* skel, int bodyYaw);

/* the twist to write into JOINT_1.pvRotation.vy so the upper body faces
 * aimYaw (camera-forward), in the engine's model-face convention: a body
 * that FACES the aim runs at dir = aimYaw - 2048 (pedest.c AnimatePed
 * moves along dir.vy - 2048 and the model face is -z). Computed against
 * the composed torso yaw, returned as the signed shortest path
 * (-2048..2047); the caller clamps to the spine limit. */
int jer_anim_twist_to_aim(void* skel, int bodyYaw, int aimYaw);

#ifdef __cplusplus
}
#endif

#endif /* JER_ANIM_H */
