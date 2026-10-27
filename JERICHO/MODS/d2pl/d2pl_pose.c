/* d2pl_pose.c — the aim pose and the aim point.
 *
 * THE ARM POSE FIX
 * ----------------
 * The old code built the arm from body-relative offsets and rotated them by
 * a heading. Two things went wrong there:
 *   1. it passed the wrong field as the facing (dir.vz, the roll, not the
 *      yaw), so the arm pointed at a fixed world direction;
 *   2. it worked in the position channel, which is WORLD-ORIENTED by the
 *      time PED_SKELETON phase 0 runs, so a local offset has to be rotated
 *      first — and even then it only pointed "forward", not at the aim.
 *
 * This version skips the local frame entirely: it computes where the right
 * shoulder is (by summing the documented ROOT->LOWERBACK->JOINT_1->RSHOULDER
 * chain), takes the world direction from there to the camera aim point, and
 * lays the forearm and hand out along that direction. The arm therefore
 * always points at what the laser points at, at any body/aim angle.
 *
 * The only rotation write is the torso/neck twist in PED_POSE, which is the
 * one channel newRotateBones reads.
 */

#include "d2pl.h"
#include "jer_anim.h"
#include "jer_math.h"
#include "jer_npc.h"		/* jer_npc_owned: tell the player from an owned NPC */

#include <math.h>

/* The shoulder chain (the hierarchy is part of the stable engine contract;
 * jer_anim_bone_parent is not in the exported addon surface). */
static const int gShoulderChain[] = {
	JER_LIMB_ROOT,
	JER_LIMB_LOWERBACK,
	JER_LIMB_JOINT_1,
	JER_LIMB_RSHOULDER
};

/* Rotations we overwrite while aiming, so the shared motion buffer can be
 * handed back untouched when the aim is released. */
static JER_BONE_ROT gSavedRots[JER_LIMB_COUNT];
static int gSavedValid = 0;

/* one-shot diagnostic so an unattended run can prove the aim pose ran */
static int gAimPoseLogged = 0;

/* ------------------------------------------------------------------ */
/* Aim point — the camera-centre ray                                   */
/* ------------------------------------------------------------------ */

int D2plAimPoint(int out[3])
{
	int yaw, pitch, cp, sp, dx, dz;

	if (!gD2plCamValid)
		return 0;

	yaw = (-gD2plCamAngle[1]) & 0xfff;
	pitch = gD2plCamAngle[0] & 0xfff;

	cp = D2plCos(pitch);
	sp = D2plSin(pitch);
	dx = (D2plSin(yaw) * cp) >> 12;
	dz = (D2plCos(yaw) * cp) >> 12;

	out[0] = gD2plCamPos[0] + ((dx * D2PL_AIM_RANGE) >> 12);
	out[1] = gD2plCamPos[1] + ((sp * D2PL_AIM_RANGE) >> 12);
	out[2] = gD2plCamPos[2] + ((dz * D2PL_AIM_RANGE) >> 12);

	return 1;
}

static int D2plIsPlayerPed(void* ped)
{
	if (ped == 0)
		return 0;

	if (gD2plPlayerPed0 == 0)
	{
		/* PED_MOVE does not fire for an idle ped, so learn the player's ped
		 * from the draw hooks. PED_POSE/PED_SKELETON fire for the player and
		 * for module-owned NPCs; the latter answer jer_npc_owned() true. */
		if (jer_npc_owned(ped))
			return 0;

		gD2plPlayerPed0 = ped;
	}

	return ped == gD2plPlayerPed0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int D2plBoneLength(void* skel, int limb)
{
	const JER_BONE_POS* rest = jer_anim_bone_rest(skel, limb);
	float len;

	if (rest == 0)
		return 0;

	len = sqrtf((float)(rest->vx * rest->vx + rest->vy * rest->vy + rest->vz * rest->vz));

	return (int)(len + 0.5f);
}

static int D2plShoulderWorld(void* skel, const int* playerPos, int out[3])
{
	int i;

	out[0] = out[1] = out[2] = 0;

	for (i = 0; i < (int)(sizeof(gShoulderChain) / sizeof(gShoulderChain[0])); i++)
	{
		const JER_BONE_POS* bone = jer_anim_bone_pos(skel, gShoulderChain[i]);

		if (bone == 0)
			return 0;

		out[0] += bone->vx;
		out[1] += bone->vy;
		out[2] += bone->vz;
	}

	out[0] += playerPos[0];
	out[1] += playerPos[1];
	out[2] += playerPos[2];

	return 1;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_GAME_START — forget the previous level's ped             */
/* ------------------------------------------------------------------ */

int D2plOnGameStart(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	gD2plPlayerPed0 = 0;
	gD2plCamValid = 0;
	gD2plAiming = 0;
	gSavedValid = 0;
	gAimPoseLogged = 0;

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_PED_POSE — torso and head follow the aim                  */
/* ------------------------------------------------------------------ */

int D2plOnPedPose(void* userdata, void* args)
{
	JER_ARGS_PED_POSE* a = (JER_ARGS_PED_POSE*)args;
	const JER_HOST_API* api = jerHostApiGet();
	JER_PLAYER_STATE ps;
	JER_BONE_ROT* joint1;
	JER_BONE_ROT* head;
	int bodyYaw, aimYaw, diff, torso, neck;

	(void)userdata;

	if (a == 0 || a->ped == 0 || a->skel == 0)
		return JER_RESULT_CONTINUE;

	/* local player only: PED_POSE also fires for owned NPCs */
	if (!D2plIsPlayerPed(a->ped))
		return JER_RESULT_CONTINUE;

	joint1 = jer_anim_bone_rotation(a->skel, JER_LIMB_JOINT_1);
	head = jer_anim_bone_rotation(a->skel, JER_LIMB_HEAD);

	if (gD2plAiming)
	{
		/* the motion bytes point into the SHARED per-type buffer: hand the
		 * previous frame's values back before taking a fresh snapshot */
		if (gSavedValid)
		{
			jer_anim_restore_rotations(a->skel, gSavedRots);
			gSavedValid = 0;
		}

		jer_anim_save_rotations(a->skel, gSavedRots);
		gSavedValid = 1;

		bodyYaw = 0;

		if (api != 0 && api->player != 0 && api->player->get_state != 0 &&
			api->player->get_state(api->player->main(), &ps))
		{
			/* the player dir on foot is the VISUAL facing (ped dir - 2048) */
			bodyYaw = ps.dir & 0xfff;
		}

		aimYaw = gD2plCamValid ? ((-gD2plCamAngle[1]) & 0xfff) : gD2plViewHeading;

		/* d2pl's convention: compare the body dir with aimYaw - 2048; the
		 * result is the shortest signed twist, clamped to the spine. */
		diff = jer_anim_aim_diff(bodyYaw, (aimYaw - 2048) & 0xfff);
		torso = jer_clamp_int(diff, -D2PL_TORSO_LIMIT, D2PL_TORSO_LIMIT);
		neck = jer_clamp_int(diff - torso, -D2PL_HEAD_LIMIT, D2PL_HEAD_LIMIT);

		if (joint1 != 0)
			joint1->vy = (short)torso;

		/* lock the head to the aim too, not just to the torso */
		if (head != 0)
			head->vy = (short)neck;
	}
	else if (gSavedValid)
	{
		jer_anim_restore_rotations(a->skel, gSavedRots);
		gSavedValid = 0;
	}

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_PED_SKELETON — the arm reaches at the aim point            */
/* ------------------------------------------------------------------ */

int D2plOnPedSkeleton(void* userdata, void* args)
{
	JER_ARGS_PED_SKELETON* a = (JER_ARGS_PED_SKELETON*)args;

	(void)userdata;

	if (a == 0 || a->ped == 0 || a->skel == 0)
		return JER_RESULT_CONTINUE;

	if (!D2plIsPlayerPed(a->ped))
		return JER_RESULT_CONTINUE;

	/* the shadow pass draws the same skeleton; never pose twice */
	if (a->shadow)
		return JER_RESULT_CONTINUE;

	if (a->phase == 0)
	{
		JER_BONE_POS* elbow;
		JER_BONE_POS* hand;
		int aim[3];
		int shoulder[3];
		int dx, dy, dz, len, elbowLen, handLen;
		static int seen;

		if (!seen)
		{
			seen = 1;
			jer_log("[d2pl] skeleton hook: aiming=%d cam=%d\n",
				gD2plAiming, gD2plCamValid);
		}

		if (!gD2plAiming || a->playerPos == 0)
			return JER_RESULT_CONTINUE;

		if (!D2plAimPoint(aim))
			return JER_RESULT_CONTINUE;

		if (!D2plShoulderWorld(a->skel, (const int*)a->playerPos, shoulder))
			return JER_RESULT_CONTINUE;

		dx = aim[0] - shoulder[0];
		dy = aim[1] - shoulder[1];
		dz = aim[2] - shoulder[2];

		len = (int)sqrtf((float)(dx * dx + dy * dy + dz * dz));

		if (len < 16)
			return JER_RESULT_CONTINUE;

		elbow = jer_anim_bone_pos(a->skel, JER_LIMB_RELBOW);
		hand = jer_anim_bone_pos(a->skel, JER_LIMB_RHAND);

		if (elbow == 0 || hand == 0)
			return JER_RESULT_CONTINUE;

		/* the bone's own rest length keeps the reach in the same units as
		 * the stock skeleton (the model is ~single-digit offsets) */
		elbowLen = D2plBoneLength(a->skel, JER_LIMB_RELBOW);
		handLen = D2plBoneLength(a->skel, JER_LIMB_RHAND);

		if (elbowLen < 1)
			elbowLen = 6;
		if (handLen < 1)
			handLen = 6;

		if (!gAimPoseLogged)
		{
			gAimPoseLogged = 1;
			jer_log("[d2pl] aim pose: shoulder=(%d,%d,%d) aim=(%d,%d,%d) reach=%d arm=%d/%d\n",
				shoulder[0], shoulder[1], shoulder[2],
				aim[0], aim[1], aim[2], len, elbowLen, handLen);
		}

		/* world-oriented deltas: shoulder -> elbow -> hand, all on the ray
		 * to the aim point. This is the fix: no body-relative rotation and
		 * no "forward only" assumption. */
		elbow->vx = (int)(((long long)dx * elbowLen) / len);
		elbow->vy = (int)(((long long)dy * elbowLen) / len);
		elbow->vz = (int)(((long long)dz * elbowLen) / len);

		hand->vx = (int)(((long long)dx * handLen) / len);
		hand->vy = (int)(((long long)dy * handLen) / len);
		hand->vz = (int)(((long long)dz * handLen) / len);
	}
	else if (a->phase == 1 && a->jointPos != 0 && a->playerPos != 0)
	{
		/* accumulated joint positions are valid now: cache the muzzle
		 * origin for the laser. SVECTOR is 4 shorts (x, y, z, pad). */
		const short* jp = (const short*)a->jointPos;
		const int* pp = (const int*)a->playerPos;
		int base = JER_LIMB_RHAND * 4;

		gD2plHand[0] = jp[base + 0] + pp[0];
		gD2plHand[1] = jp[base + 1] + pp[1];
		gD2plHand[2] = jp[base + 2] + pp[2];
		gD2plHandValid = 1;
	}

	return JER_RESULT_CONTINUE;
}
