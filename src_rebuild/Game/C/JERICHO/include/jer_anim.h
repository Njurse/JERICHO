#ifndef JER_ANIM_H
#define JER_ANIM_H

/* jer_anim — the JERICHO ped-animation API: read and write ANY of a ped's 23
 * bones.
 *
 * A drawn ped is a skeleton of NUM_BONES (23) joints, posed afresh every frame,
 * and BOTH ped-pose hooks hand a module the whole bone array as `void* skel`.
 * There are two channels and they are NOT interchangeable:
 *
 *   positions (vCurrPos)   - write from JER_EVENT_PED_SKELETON phase 0, before
 *                            the joint offsets are accumulated. The skeleton is
 *                            reset to its rest pose every frame, so re-apply on
 *                            every draw.
 *   rotations (pvRotation) - write from JER_EVENT_PED_POSE only: it fires
 *                            between SetupTannerSkeleton and newRotateBones, and
 *                            newRotateBones is the sole reader.
 *
 * Both are per-bone and parent-relative, so a pose is built by writing the
 * bones you care about. See
 * src_rebuild/Game/C/JERICHO/docs/ped-animation.md for the hierarchy, the full
 * pipeline map and the clobber table.
 *
 * GOTCHA — the position channel is WORLD-ORIENTED by the time the hook runs
 * (newRotateBones has already rotated the offsets through the chain), so a
 * body-relative offset has to be turned into that frame first:
 * jer_anim_rotate_offset() does exactly that.
 *
 * GOTCHA — a bone's rotation points INTO THE SHARED per-type motion buffer, so
 * a write leaks into every ped drawing that same motion frame. Snapshot what
 * you touch with jer_anim_save_rotations() and put it back with
 * jer_anim_restore_rotations() when you stop posing.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "jericho.h"

/* Every limb, mirroring motion_c.c's LIMBS enum (stable engine constants).
 *
 * The hierarchy, parent -> children:
 *   ROOT        -> LOWERBACK, HIPS
 *   LOWERBACK   -> JOINT_1, LHIP, RHIP
 *   JOINT_1     -> NECK, LSHOULDER, RSHOULDER     (the UPPER-BODY root)
 *   NECK        -> HEAD
 *   LSHOULDER   -> LELBOW -> LHAND -> LFINGERS    (and the R twins)
 *   LHIP        -> LKNEE  -> LFOOT -> LTOE        (and the R twins)
 *
 * So rotating JOINT_1 swings the torso, BOTH arms and the head together (the
 * legs do not follow), and moving a knee carries the whole lower leg. */
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
	JER_LIMB_LHIP,
	JER_LIMB_LKNEE,
	JER_LIMB_LFOOT,
	JER_LIMB_LTOE,
	JER_LIMB_RHIP,
	JER_LIMB_RKNEE,
	JER_LIMB_RFOOT,
	JER_LIMB_RTOE,
	JER_LIMB_JOINT,
	JER_LIMB_COUNT		/* 23 */
};

/* --- rotations (JER_EVENT_PED_POSE) --------------------------------- */

/* a bone's per-frame rotation, as a plain writable 3-short (mirrors SVECTOR,
 * so the module never needs the engine's bone structs). Angles are the engine's
 * 4096-per-turn scale and are relative to the parent bone. NULL when the bone
 * has no rotation data. */
typedef struct JER_BONE_ROT
{
	short vx, vy, vz;
} JER_BONE_ROT;

JER_BONE_ROT* jer_anim_bone_rotation(void* skel, int limb);

/* Snapshot / restore EVERY bone's rotation. Use them around a pose so the
 * shared motion buffer is left as the engine expects — otherwise the twist
 * persists into every frame of that motion (and every other ped using it).
 * `out`/`in` must hold JER_LIMB_COUNT entries. */
void jer_anim_save_rotations(void* skel, JER_BONE_ROT* out);
void jer_anim_restore_rotations(void* skel, const JER_BONE_ROT* in);

/* --- positions (JER_EVENT_PED_SKELETON phase 0) --------------------- */

/* a bone's accumulated joint offset, as a writable 3-int (mirrors VECTOR).
 * WORLD-ORIENTED by hook time — see jer_anim_rotate_offset(). */
typedef struct JER_BONE_POS
{
	int vx, vy, vz;
} JER_BONE_POS;

JER_BONE_POS* jer_anim_bone_pos(void* skel, int limb);

/* the same bone's REST parent->child delta (read-only). Pose AGAINST this so a
 * bone that is not being overridden keeps its stock placement. */
const JER_BONE_POS* jer_anim_bone_rest(void* skel, int limb);

/* --- skeleton queries ----------------------------------------------- */

/* a bone's parent limb, or -1 for the root (and for an out-of-range limb).
 * Pass the hook's `skel`, or NULL to use the engine's live skeleton table.
 * Walk the chain with it to pose a whole limb. */
int jer_anim_bone_parent(void* skel, int limb);

/* the model the engine draws at this joint (may be NULL). In PED_SKELETON
 * phase 1, hang an extra mesh off a bone — a weapon in the hand, say. */
void* jer_anim_bone_model(void* skel, int limb);

/* --- helpers -------------------------------------------------------- */

/* rotate a body-relative (x, z) offset into the world-oriented frame the
 * POSITION channel works in, by the ped's facing heading (pPed->dir.vy). The
 * offset's +Z maps to the direction (RSIN(heading), RCOS(heading)); pass the
 * heading of whichever facing should receive the offset. */
void jer_anim_rotate_offset(int* px, int* pz, int heading);

/* the wrap-safe signed diff (normalized to -2048..2048) between a body
 * heading and an aim yaw — the value to write for aim-mode torso/head
 * alignment. */
int jer_anim_aim_diff(int bodyHeading, int aimYaw);

#ifdef __cplusplus
}
#endif

#endif /* JER_ANIM_H */
