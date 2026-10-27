/* d2pl_pose.c — the holdtype pose and the aim point.
 *
 * WHY THE ARM USED TO CRINKLE
 * ---------------------------
 * The aim arm was written in PED_SKELETON phase 0 as bone POSITIONS. By then
 * newRotateBones has already baked every mesh with the walk-cycle rotation,
 * so the forearm and hand were moved onto the aim ray while still drawn in
 * their animated orientation, and the segments folded over each other.
 *
 * NOW
 * ---
 * Everything is a ROTATION written in PED_POSE, the one channel
 * newRotateBones reads (see ped-animation.md section 3):
 *   1. torso/neck yaw twist toward the aim, clamped per holdtype;
 *   2. right-arm IK: rebuild the engine's bone matrices up to the shoulder
 *      (mStore[b] = mStore[parent] * Rz*Ry*Rx(-x, y, z) of the parent's
 *      rotation), then rotate the shoulder so the upper arm points at the
 *      target, then the elbow so the forearm continues on it. The minimal
 *      rotation is applied ON TOP of the animated one, so the clip's twist
 *      survives;
 *   3. the holdtype's own pose hook (keyframe override / custom clip).
 * PED_SKELETON is read-only now: phase 0 caches the shoulder and measures
 * the root yaw, phase 1 caches the muzzle for the laser.
 */

#include "d2pl.h"
#include "jer_anim.h"
#include "jer_math.h"
#include "jer_npc.h"		/* jer_npc_owned: tell the player from an owned NPC */

#include <math.h>
#include <string.h>

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
/* Engine-matching rotation math (float; 4096 = one turn)              */
/* ------------------------------------------------------------------ */

typedef float D2plM3[3][3];

#define D2PL_ANG2RAD(a)	((float)(a) * (6.28318530718f / 4096.0f))
#define D2PL_RAD2ANG(r)	((int)floorf((r) * (4096.0f / 6.28318530718f) + 0.5f))

static void M3Mul(const D2plM3 a, const D2plM3 b, D2plM3 out)
{
	D2plM3 t;
	int i, j;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];

	memcpy(out, t, sizeof(t));
}

static void M3Apply(const D2plM3 m, const float v[3], float out[3])
{
	float t[3];
	int i;

	for (i = 0; i < 3; i++)
		t[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];

	out[0] = t[0]; out[1] = t[1]; out[2] = t[2];
}

static void M3ApplyT(const D2plM3 m, const float v[3], float out[3])
{
	float t[3];
	int i;

	for (i = 0; i < 3; i++)
		t[i] = m[0][i] * v[0] + m[1][i] * v[1] + m[2][i] * v[2];

	out[0] = t[0]; out[1] = t[1]; out[2] = t[2];
}

/* RotMatrixZYX_gte (PsyCross): Rz * Ry * Rx, PSX sign conventions */
static void M3FromZYX(float x, float y, float z, D2plM3 m)
{
	float cx = cosf(x), sx = sinf(x);
	float cy = cosf(y), sy = sinf(y);
	float cz = cosf(z), sz = sinf(z);

	m[0][0] = cz * cy;	m[0][1] = cz * sy * sx - sz * cx;	m[0][2] = cz * sy * cx + sz * sx;
	m[1][0] = sz * cy;	m[1][1] = sz * sy * sx + cz * cx;	m[1][2] = sz * sy * cx - cz * sx;
	m[2][0] = -sy;		m[2][1] = cy * sx;					m[2][2] = cy * cx;
}

/* the inverse of M3FromZYX */
static void M3ToZYX(const D2plM3 m, float* x, float* y, float* z)
{
	float s = -m[2][0];

	if (s > 1.0f) s = 1.0f;
	if (s < -1.0f) s = -1.0f;

	*y = asinf(s);
	*x = atan2f(m[2][1], m[2][2]);
	*z = atan2f(m[1][0], m[0][0]);
}

/* the matrix newRotateBones builds from a parent's raw rotation (x negated) */
static void M3FromBone(const JER_BONE_ROT* r, D2plM3 m)
{
	M3FromZYX(D2PL_ANG2RAD(-r->vx), D2PL_ANG2RAD(r->vy), D2PL_ANG2RAD(r->vz), m);
}

static void M3ToBone(const D2plM3 m, JER_BONE_ROT* r)
{
	float x, y, z;

	M3ToZYX(m, &x, &y, &z);

	r->vx = (short)(D2PL_RAD2ANG(-x) & 0xfff);
	r->vy = (short)(D2PL_RAD2ANG(y) & 0xfff);
	r->vz = (short)(D2PL_RAD2ANG(z) & 0xfff);
}

static float V3Norm(float v[3])
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (l > 1e-6f)
	{
		v[0] /= l; v[1] /= l; v[2] /= l;
	}

	return l;
}

/* the smallest rotation taking unit u onto unit v (Rodrigues) */
static void M3Align(const float u[3], const float v[3], D2plM3 m)
{
	float ax = u[1] * v[2] - u[2] * v[1];
	float ay = u[2] * v[0] - u[0] * v[2];
	float az = u[0] * v[1] - u[1] * v[0];
	float c = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
	float k;

	if (c < -0.9999f)
	{
		/* opposite: leave it, a half-turn arm is never a useful pose */
		memset(m, 0, sizeof(D2plM3));
		m[0][0] = m[1][1] = m[2][2] = 1.0f;
		return;
	}

	k = 1.0f / (1.0f + c);

	m[0][0] = ax * ax * k + c;	m[0][1] = ax * ay * k - az;	m[0][2] = ax * az * k + ay;
	m[1][0] = ay * ax * k + az;	m[1][1] = ay * ay * k + c;	m[1][2] = ay * az * k - ax;
	m[2][0] = az * ax * k - ay;	m[2][1] = az * ay * k + ax;	m[2][2] = az * az * k + c;
}

static void M3RotY(float a, D2plM3 m)
{
	M3FromZYX(0.0f, a, 0.0f, m);
}

static int RestDir(void* skel, int limb, float out[3])
{
	const JER_BONE_POS* r = jer_anim_bone_rest(skel, limb);

	if (r == 0)
		return 0;

	out[0] = (float)r->vx;
	out[1] = (float)r->vy;
	out[2] = (float)r->vz;

	return V3Norm(out) > 0.5f;
}

/* ------------------------------------------------------------------ */
/* Root-yaw calibration                                                */
/* ------------------------------------------------------------------ */

/* The root matrix is RotYXZ(pPed->dir). The addon sees the player's dir
 * through the host (the visual facing), so the offset between the two is
 * measured from what newRotateBones actually produced: PED_POSE predicts
 * the upper-arm direction without the root yaw, PED_SKELETON phase 0 reads
 * the engine's result, and the yaw between them is the root yaw. */
static int gRootYawOffset = 2048;	/* documented: player dir = ped dir - 2048 */
static int gRootYawMeasured = 0;
static float gPredictLocal[3];		/* RELBOW offset in the root-yaw-free frame */
static int gPredictValid = 0;
static int gPredictPsDir = 0;

static int gShoulderWorld[3];
static int gShoulderValid = 0;

/* ------------------------------------------------------------------ */
/* Two-bone arm (shoulder -> elbow -> hand) as rotations                */
/* ------------------------------------------------------------------ */

/* Rotate `bone` so the child segment `child` (whose rest offset is rotated
 * by M[parent] * S(bone)) points along world direction `d`, blended by
 * weight w (0..1). On return `frame` is the child's matrix M[child]. */
static void AimSegment(void* skel, int bone, int child, D2plM3 frame,
	const float d[3], float w)
{
	JER_BONE_ROT* rot = jer_anim_bone_rotation(skel, bone);
	D2plM3 s, q, m;
	float off[3], u[3], t[3];

	if (rot == 0)
		return;

	M3FromBone(rot, s);

	if (RestDir(skel, child, off))
	{
		M3Apply(s, off, u);			/* the segment in the parent frame now */
		M3ApplyT(frame, d, t);		/* the target in the parent frame */
		V3Norm(t);

		if (w < 0.999f)
		{
			t[0] = u[0] + (t[0] - u[0]) * w;
			t[1] = u[1] + (t[1] - u[1]) * w;
			t[2] = u[2] + (t[2] - u[2]) * w;
			V3Norm(t);
		}

		M3Align(u, t, q);
		M3Mul(q, s, s);
		M3ToBone(s, rot);
		M3FromBone(rot, s);			/* use the quantised value downstream */
	}

	M3Mul(frame, s, m);
	memcpy(frame, m, sizeof(D2plM3));
}

/* M[RSHOULDER] without the root yaw: S(ROOT) S(LOWERBACK) S(JOINT_1) */
static int ShoulderFrameLocal(void* skel, D2plM3 out)
{
	int i;
	D2plM3 s;

	memset(out, 0, sizeof(D2plM3));
	out[0][0] = out[1][1] = out[2][2] = 1.0f;

	for (i = 0; i < 3; i++)	/* ROOT, LOWERBACK, JOINT_1: the parents of the chain */
	{
		JER_BONE_ROT* r = jer_anim_bone_rotation(skel, gShoulderChain[i]);

		if (r == 0)
			return 0;

		M3FromBone(r, s);
		M3Mul(out, s, out);
	}

	return 1;
}

/* d: unit world direction for the arm (render frame, y down) */
static void PoseArm(void* skel, int rootYaw, const float d[3], float w)
{
	D2plM3 frame, ry;

	if (!ShoulderFrameLocal(skel, frame))
		return;

	M3RotY(D2PL_ANG2RAD(rootYaw), ry);
	M3Mul(ry, frame, frame);

	AimSegment(skel, JER_LIMB_RSHOULDER, JER_LIMB_RELBOW, frame, d, w);
	AimSegment(skel, JER_LIMB_RELBOW, JER_LIMB_RHAND, frame, d, w);
}

static void PredictElbow(void* skel)
{
	D2plM3 frame, s;
	JER_BONE_ROT* r = jer_anim_bone_rotation(skel, JER_LIMB_RSHOULDER);
	const JER_BONE_POS* off = jer_anim_bone_rest(skel, JER_LIMB_RELBOW);
	float o[3];

	gPredictValid = 0;

	if (r == 0 || off == 0 || !ShoulderFrameLocal(skel, frame))
		return;

	M3FromBone(r, s);
	M3Mul(frame, s, frame);

	o[0] = (float)off->vx;
	o[1] = (float)off->vy;
	o[2] = (float)off->vz;

	M3Apply(frame, o, gPredictLocal);
	gPredictValid = 1;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_PED_POSE — twist, IK and the holdtype hook                 */
/* ------------------------------------------------------------------ */

int D2plOnPedPose(void* userdata, void* args)
{
	JER_ARGS_PED_POSE* a = (JER_ARGS_PED_POSE*)args;
	const JER_HOST_API* api = jerHostApiGet();
	const D2plHoldtype* h;
	JER_PLAYER_STATE ps;
	JER_BONE_ROT* joint1;
	JER_BONE_ROT* head;
	int bodyYaw, aimYaw, diff, torso, neck, psDir, rootYaw;
	int active;

	(void)userdata;

	if (a == 0 || a->ped == 0 || a->skel == 0)
		return JER_RESULT_CONTINUE;

	/* local player only: PED_POSE also fires for owned NPCs */
	if (!D2plIsPlayerPed(a->ped))
		return JER_RESULT_CONTINUE;

	h = D2plHoldtypeGet(D2plActiveHoldtype());
	active = (gD2plAiming && h->aimIk) || (!gD2plAiming && h->idleIk) ||
		(gD2plAiming && h->torsoLimit > 0) || h->poseHook != 0;

	/* the motion bytes point into the SHARED per-type buffer: hand the
	 * previous frame's values back before anything else reads them */
	if (gSavedValid)
	{
		jer_anim_restore_rotations(a->skel, gSavedRots);
		gSavedValid = 0;
	}

	psDir = 0;

	if (api != 0 && api->player != 0 && api->player->get_state != 0 &&
		api->player->get_state(api->player->main(), &ps))
	{
		/* the player dir on foot is the VISUAL facing (ped dir - 2048) */
		psDir = ps.dir & 0xfff;
	}

	if (!active)
	{
		gPredictValid = 0;
		return JER_RESULT_CONTINUE;
	}

	jer_anim_save_rotations(a->skel, gSavedRots);
	gSavedValid = 1;

	rootYaw = (psDir + gRootYawOffset) & 0xfff;

	joint1 = jer_anim_bone_rotation(a->skel, JER_LIMB_JOINT_1);
	head = jer_anim_bone_rotation(a->skel, JER_LIMB_HEAD);

	if (gD2plAiming)
	{
		bodyYaw = psDir;
		aimYaw = gD2plCamValid ? ((-gD2plCamAngle[1]) & 0xfff) : gD2plViewHeading;

		/* d2pl's convention: compare the body dir with aimYaw - 2048; the
		 * result is the shortest signed twist, split spine first, then neck */
		diff = jer_anim_aim_diff(bodyYaw, (aimYaw - 2048) & 0xfff);
		torso = jer_clamp_int(diff, -h->torsoLimit, h->torsoLimit);
		neck = jer_clamp_int(diff - torso, -h->headLimit, h->headLimit);

		if (joint1 != 0 && (h->mask & (1u << JER_LIMB_JOINT_1)))
			joint1->vy = (short)((joint1->vy + torso) & 0xfff);

		if (head != 0 && (h->mask & (1u << JER_LIMB_HEAD)))
			head->vy = (short)((head->vy + neck) & 0xfff);
	}

	if (gD2plAiming && h->aimIk && gShoulderValid)
	{
		int aim[3];

		if (D2plAimPoint(aim))
		{
			float d[3];

			d[0] = (float)(aim[0] - gShoulderWorld[0]);
			d[1] = (float)(aim[1] - gShoulderWorld[1]);
			d[2] = (float)(aim[2] - gShoulderWorld[2]);

			if (V3Norm(d) > 16.0f)
				PoseArm(a->skel, rootYaw, d, 1.0f);
		}
	}
	else if (!gD2plAiming && h->idleIk && h->idleWeight > 0)
	{
		/* weapon out, not aimed: forward along the body, pitched down.
		 * The engine faces along ped dir - 2048 = the visual facing. */
		float d[3];
		float yaw = D2PL_ANG2RAD(psDir);
		float pitch = D2PL_ANG2RAD(h->idlePitch);

		d[0] = sinf(yaw) * cosf(pitch);
		d[1] = sinf(pitch);			/* render y is down */
		d[2] = cosf(yaw) * cosf(pitch);

		PoseArm(a->skel, rootYaw, d, (float)h->idleWeight / 4096.0f);
	}

	if (h->poseHook != 0)
		h->poseHook(a->skel, gD2plAiming, h->poseUser);

	/* predict the upper arm from the FINAL rotations; PED_SKELETON compares
	 * it with the engine's result to measure the root yaw */
	PredictElbow(a->skel);
	gPredictPsDir = psDir;

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_PED_SKELETON — read-only: shoulder, root yaw, muzzle       */
/* ------------------------------------------------------------------ */

int D2plOnPedSkeleton(void* userdata, void* args)
{
	JER_ARGS_PED_SKELETON* a = (JER_ARGS_PED_SKELETON*)args;

	(void)userdata;

	if (a == 0 || a->ped == 0 || a->skel == 0)
		return JER_RESULT_CONTINUE;

	if (!D2plIsPlayerPed(a->ped))
		return JER_RESULT_CONTINUE;

	/* the shadow pass draws the same skeleton */
	if (a->shadow)
		return JER_RESULT_CONTINUE;

	if (a->phase == 0)
	{
		const JER_BONE_POS* elbow;

		if (a->playerPos == 0)
			return JER_RESULT_CONTINUE;

		gShoulderValid = D2plShoulderWorld(a->skel, (const int*)a->playerPos, gShoulderWorld);

		/* vCurrPos is world-oriented here: compare it with the yaw-free
		 * prediction and learn the root-yaw offset */
		elbow = jer_anim_bone_pos(a->skel, JER_LIMB_RELBOW);

		if (gPredictValid && elbow != 0)
		{
			float pl = gPredictLocal[0] * gPredictLocal[0] + gPredictLocal[2] * gPredictLocal[2];
			float el = (float)elbow->vx * elbow->vx + (float)elbow->vz * elbow->vz;

			if (pl > 4.0f && el > 4.0f)
			{
				int yaw = D2PL_RAD2ANG(atan2f((float)elbow->vx, (float)elbow->vz) -
					atan2f(gPredictLocal[0], gPredictLocal[2]));
				int off = (yaw - gPredictPsDir) & 0xfff;

				if (!gRootYawMeasured)
				{
					gRootYawMeasured = 1;
					jer_log("[d2pl] root yaw offset measured %d (assumed %d)\n",
						off, gRootYawOffset);
				}

				gRootYawOffset = off;
			}
		}

		if (!gAimPoseLogged && gD2plAiming)
		{
			gAimPoseLogged = 1;
			jer_log("[d2pl] aim pose: holdtype=%s shoulder=(%d,%d,%d)\n",
				D2plHoldtypeGet(D2plActiveHoldtype())->name,
				gShoulderWorld[0], gShoulderWorld[1], gShoulderWorld[2]);
		}
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
