/*
 * d2pl_weapon.c — Driver 2 Parallel Lines (weapon system: registry, aim camera, arm pose, HUD, laser).
 */
#include "d2pl.h"
#include "jer_anim.h"	/* JER_LIMB_*, jer_anim_bone_rotation, jer_anim_aim_diff */

#include "players.h"
#include "cars.h"
#include "camera.h"
#include "pad.h"
#include "dr2math.h"
#include "main.h"
#include "draw.h"
#include "models.h"
#include "pres.h"
#include "pedest.h"
#include "civ_ai.h"
#include "dr2roads.h"
#include "PsyX/PsyX_public.h"

#include <math.h>

/* the debug line API used by the engine's collision overlays (PC only) */
#ifndef PSX
extern void Debug_AddLine(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
/* JERICHO-HOOK (DebugOverlay.cpp): depth-sorted variant — the line joins
 * the world's ordering table at its Z bucket, so walls occlude it */
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
#endif

/* --- weapon-related shared state (defined here; extern in d2pl.h) --- */

WeaponBase* gWeapons[MAX_WEAPONS];
int gWeaponCount = 0;
WeaponBase* gCurrentWeapon = NULL;
int gAiming = 0;
int gHandPos[3];		/* world RHAND position (muzzle origin) */
int gAimPoint[3];	/* world aim point at weapon range */

/* aim-line state (extern in d2pl.h): while aiming the module owns the aim
 * heading/pitch — see the header comment */
int gAimYaw = 0;
int gAimPitch = 0;
int gAimPitchTarget = 0;

/* impact marker state */
int gMarkerTimer;	/* frames left until the marker fades */
int gMarkerPos[3];
int gPedScaleLogged;	/* one-shot per-level ped-scale diagnostic */

/* mouse integration: relative motion consumed by the look handler (yaw/pitch
 * nudges on top of the stick); buttons map to aim (right), primary fire
 * (left), secondary fire (middle). Per-pixel nudge at sens 64:
 * (dx * sens * MOUSE_YAW) / (64*128). */
#include <SDL.h>

#define MOUSE_YAW	1152
#define MOUSE_PITCH	1152

/* input-device authority: the mouse only drives the view while it is the
 * recent input device (motion/click grants ~0.75 s; live controller
 * analog revokes it immediately) — a connected controller + an idle mouse
 * must never fight over the camera */
#define MOUSE_AUTHORITY_FRAMES 45


/* ================================================================== */
/* Registry                                                              */
/* ================================================================== */

void WeaponSystem_Register(WeaponBase* weapon)
{
	if (weapon == NULL || gWeaponCount >= MAX_WEAPONS)
		return;

	gWeapons[gWeaponCount++] = weapon;
}

void WeaponSystem_Equip(WeaponBase* weapon)
{
	if (gCurrentWeapon == weapon)
		return;

	if (gCurrentWeapon != NULL)
		gCurrentWeapon->onHolster();

	gCurrentWeapon = weapon;

	if (weapon != NULL)
		weapon->onEquip();
}

void WeaponSystem_Cycle(int direction)
{
	int i;

	if (gWeaponCount == 0)
		return;

	if (gCurrentWeapon == NULL)
	{
		WeaponSystem_Equip(gWeapons[0]);
		return;
	}

	for (i = 0; i < gWeaponCount; i++)
	{
		if (gWeapons[i] == gCurrentWeapon)
			break;
	}

	if (i >= gWeaponCount)
		i = 0;

	i = (i + direction + gWeaponCount) % gWeaponCount;
	WeaponSystem_Equip(gWeapons[i]);
}

WeaponBase* WeaponSystem_Current(void)
{
	return gCurrentWeapon;
}

int* WeaponSystem_HandPos(void)
{
	return gHandPos;
}

/* ================================================================== */
/* JER_EVENT_CAMERA_LOOK — right-stick orbit + pitch free-look         */
/* ================================================================== */

/* the natural heading the settled camera tracks behind: for a car it's
 * the speed-weighted blend of the body heading and the ground-velocity
 * heading; on foot it's the ped's facing. Computed FRESH every call from
 * live state — never a snapshot — so resuming automatic tracking after a
 * pause (manual look, spin-out) never corrects against a stale target.
 *
 * The blend weights the direction VECTORS (RSIN/RCOS components), not
 * the angles directly, so a heading crossing 0/360 cannot snap. */

/* ================================================================== */
/* D2plLaserColor - laser-sight colour from the settings                  */
/* ================================================================== */

void D2plLaserColor(CVECTOR* out)
{
	switch (gS.laserColor)
	{
	case D2PL_LASER_GREEN:
		out->r = 0; out->g = 250; out->b = 0; break;
	case D2PL_LASER_BLUE:
		out->r = 0; out->g = 90; out->b = 250; break;
	case D2PL_LASER_WHITE:
		out->r = 250; out->g = 250; out->b = 250; break;
	case D2PL_LASER_RED:
	default:
		out->r = 250; out->g = 0; out->b = 0; break;
	}
}

/* ================================================================== */
/* Weapon registry                                                     */
/* ================================================================== */


/* ================================================================== */
/* Aim camera: the GTA4 over-the-shoulder aim view + the aim-line state */
/* ================================================================== */

int gAimLookBlend = 0;	/* 0 = settled (forward), 4096 = free-looking */

/* aim pressed: snapshot the aim line from the CURRENT camera so entering
 * aim never swings the view — the camera then eases (via D2plSmoothCamera)
 * from the chase view into the shoulder rig while keeping the same look
 * heading. Pitch is NORMALIZED (no free-look drift carryover). */
void D2plAimEnter(PLAYER* lp)
{
	(void)lp;

	gAimYaw = (-camera_angle.vy) & 0xfff;
	gAimPitch = 0;
	gAimPitchTarget = 0;
	gYawSpeed = 0;
}

/* aim released: park the engine orbit at the last aim heading so the
 * chase camera resumes FROM the aim view (orbit angle A puts the camera at
 * A around the player and it LOOKS back, so look heading = A + 2048). */
void D2plAimExit(PLAYER* lp)
{
	lp->cameraAngle = (gAimYaw - 2048) & 0xfff;
}

void D2plAimAtPlayer(SVECTOR* camAngle, VECTOR* camPos, int* base,
	int inCar, int baseDir, int camYaw, int gripping)
{
	VECTOR target;
	int baseY = -base[1];	/* camera-frame anchor y (base y is RAW) */

	target.vx = base[0];
	target.vy = baseY;
	target.vz = base[2];

	if (inCar)
	{
		VECTOR carPoint;
		VECTOR aheadPoint;
		int diff;
		int blendTarget;

		/* orbit/focal point: above the car's base in the CAMERA frame
		 * (base y is RAW; up = -y). The old raw-frame focal sat 160
		 * units below the car in the camera frame, so the view pitched
		 * down at a point under the car and the car drifted out of the
		 * frame — now it aims at roof height. */
		carPoint.vx = base[0];
		carPoint.vy = baseY + CAR_ORBIT_UP;
		carPoint.vz = base[2];

		/* settled focal point: was looking ahead of where you're driving but trying sole focus on car after reviewing GTA4 footage. */
		// Update - this fixed the shitty driving camera problem that made it too much like a mobile game, I like this a lot more
		aheadPoint.vx = base[0] + FIXEDH(RSIN(baseDir));
		aheadPoint.vy = baseY + CAR_ORBIT_UP;
		aheadPoint.vz = base[2] + FIXEDH(RCOS(baseDir));

		/* gravitate toward looking AHEAD of the car only very close to the
		 * settled angle, and onto the car itself much earlier while
		 * view-panning: the ahead zone spans ~5.6 degrees and falls off
		 * fully to the car by ~22.5 degrees, so plenty of angles that
		 * should look at the car do. While the stick is actually being
		 * held (gripping) the focal snaps STRAIGHT to the car — a pure
		 * spherical pan, no stretching toward the ahead point. The settled
		 * orbit angle is baseDir + gCameraAngle (the camera parked BEHIND
		 * the car, facing forward — camera.c:560); measure against THAT. */
		if (gripping)
		{
			blendTarget = 4096;	/* panning: look at the car, always */
		}
		else
		{
			diff = ABS(jer_angle_diff(camYaw, (baseDir + gCameraAngle) & 0xfff));

			if (diff <= 64)
				blendTarget = 0;	/* settled: look ahead of the car */
			else if (diff >= 256)
				blendTarget = 4096;	/* panning: look at the car */
			else
				blendTarget = ((diff - 64) * 4096) / 192;
		}

		gAimLookBlend = jer_lerp_int(gAimLookBlend, blendTarget, 4);

		target.vx = jer_lerp(aheadPoint.vx, carPoint.vx, gAimLookBlend);
		target.vy = jer_lerp(aheadPoint.vy, carPoint.vy, gAimLookBlend);
		target.vz = jer_lerp(aheadPoint.vz, carPoint.vz, gAimLookBlend);
	}
	else
	{
		gAimLookBlend = 0;

		/* aim at the torso: the chest sits FOOT_AIM_HEIGHT above the
		 * waist anchor in the camera frame (negated, so up = -y) — the
		 * camera sits a little low and looks up at his chest */
		target.vy = baseY - FOOT_AIM_HEIGHT;
	}

	PointAtTarget(camPos, &target, camAngle);
}

/* ================================================================== */
/* JER_EVENT_CAMERA — framing + FOV override + aim camera              */
/* ================================================================== */


/* ================================================================== */
/* D2plAimCamera - the aiming branch of the camera placement              */
/* ================================================================== */

int D2plAimCamera(void* args, int heading)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	PLAYER* lp = (PLAYER*)a->player;
	VECTOR* camPos = (VECTOR*)a->cameraPosition;
	SVECTOR* camAngle = (SVECTOR*)a->cameraAngle;
	int* base = (int*)a->basePos;
	VECTOR anchor;
	VECTOR lookAt;
	int baseY;
	int ox, oy, oz;
	int cosP, sinP;
	int distH;

	(void)heading;

	/* the rig needs the chase anchor; without it let the stock camera stand */
	if (base == NULL)
		return JER_RESULT_CONTINUE;

	/* --- GTA4 over-the-shoulder rig: an ABSOLUTE camera behind the
	 * module's aim line (gAimYaw/gAimPitch), not a pull of the chase
	 * position. The anchor is the character's torso; the camera sits
	 * standoff behind, shoulder-lateral and slightly above, and looks at a
	 * frame point AHEAD of the anchor so the character reads ~1/3 from the
	 * screen edge with the aim line running camera -> frame point -> aim
	 * point at weapon range. The pitch elevates the camera offset around
	 * the anchor; the rendered view pitch is then FORCED to the module's
	 * gAimPitch (see below) so the stick pitch and the rendered angle stay
	 * one. --- */
	baseY = -base[1];	/* camera-frame anchor y (base y is RAW) */

	anchor.vx = base[0];
	anchor.vy = baseY - FOOT_AIM_HEIGHT;	/* the torso */
	anchor.vz = base[2];

	/* offset in the aim frame: behind (-forward) + lateral (shoulder
	 * side) + height above the anchor (render y-down: negative = up) */
	ox = -FIXEDH(RSIN(gAimYaw) * AIM_STANDOFF)
		+ FIXEDH(RCOS(gAimYaw) * AIM_SHOULDER_LAT * gS.shoulder);
	oz = -FIXEDH(RCOS(gAimYaw) * AIM_STANDOFF)
		- FIXEDH(RSIN(gAimYaw) * AIM_SHOULDER_LAT * gS.shoulder);
	oy = AIM_HEIGHT;

	/* pitch-elevate the offset around the anchor: the camera rises/falls
	 * in the vertical plane while staying on the behind-shoulder line
	 * (distH*sinP vertical swing; deliberately NO horizontal spill — the
	 * shoulder camera must not drift sideways off the aim axis) */
	cosP = RCOS(gAimPitch);
	sinP = RSIN(gAimPitch);
	distH = SquareRoot0(ox * ox + oz * oz);

	camPos->vx = anchor.vx + FIXEDH(ox * cosP);
	camPos->vz = anchor.vz + FIXEDH(oz * cosP);
	camPos->vy = anchor.vy + FIXEDH(oy * cosP) - FIXEDH(distH * sinP);

	/* look at a frame point ahead of the anchor along the aim heading
	 * (slightly above the anchor, so the character sits low/off-centre in
	 * the frame like GTA4) */
	lookAt.vx = anchor.vx + FIXEDH(RSIN(gAimYaw) * AIM_FRAME_DIST);
	lookAt.vz = anchor.vz + FIXEDH(RCOS(gAimYaw) * AIM_FRAME_DIST);
	lookAt.vy = anchor.vy - AIM_FRAME_UP;

	/* smooth + collision-push + terrain-cap the position FIRST (the shared
	 * rig), then aim at the frame point from the smoothed position — the
	 * same order as the normal path, so the enter/exit transitions blend
	 * through D2plSmoothCamera's bounded-rate position. PointAtTarget
	 * derives the yaw from the frame point; its derived PITCH is
	 * deliberately overridden with the module's aim pitch (the lookAt
	 * geometry pitch would be damped by the camera's own elevation — the
	 * reticle line and the rendered view must be one). */
	{
		int clip = D2plSmoothCamera(camPos, base, a->inCar, NULL);

		PointAtTarget(camPos, &lookAt, camAngle);
		camAngle->vx = (short)(-gAimPitch & 0xfff);

		D2plLogCamera(lp, a->baseDir, camPos, camAngle, clip);
	}

	/* aim FOV: the base-FOV override is RESPECTED (the old code used
	 * gCameraDefaultScrZ, silently discarding the user's setting) plus the
	 * aim zoom for the focus effect */
	{
		int fovScrZ = gS.fovEnabled ? D2plFovScrZ(gS.fovDeg) : gCameraDefaultScrZ;

		SetGeomScreen(scr_z = fovScrZ + AIM_ZOOM);
	}

	gCurrentWeapon->aimPoint(gAimPoint);

	a->override = 1;

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* JER_EVENT_PED_SKELETON - arm pose + weapon mesh at RHAND               */
/* ================================================================== */

/* JER_EVENT_PED_POSE — the torso faces the camera while aiming. This is
 * the ONLY effective rotation channel: the hook fires between
 * SetupTannerSkeleton and newRotateBones, so mutating the JOINT_1 (upper-
 * body root) rotation here propagates through the neck to the head, and
 * the arms follow because they hang off the same root.
 *
 * HEAD CHANNEL WARNING: HEAD.pvRotation is DEAD — newRotateBones builds
 * the head matrix from NECK.pvRotation.vy - pPed->head_rot (motion_c.c:
 * 1419-1420) and never reads the leaf's own slot. The head lock therefore
 * writes the NECK slot (aligned with the twisted torso) and zeroes
 * pPed->head_rot (the stock L2/R2 look channel, TurnHead). Both are
 * per-frame slots in the SHARED per-type motion buffer, so every touched
 * slot is restored (see JERICHO/docs/ped-animation.md §7). */
/* the rotation slots a pose touches, with their original values — they
 * point into the SHARED per-type motion buffer (ped-animation.md §7), so
 * every dirty slot must be restored or the walk cycle keeps the pose when
 * the animation loops back to that frame. Restored every frame before the
 * current slots are written, and again on the first non-posing frame. */
#define D2PL_POSE_SLOT_MAX 4	/* JOINT_1 + NECK + weapon arm x2 */
static struct
{
	JER_BONE_ROT* slot;
	JER_BONE_ROT  saved;
} gPoseSlots[D2PL_POSE_SLOT_MAX];
static int gPoseSlotCount;

static void D2plPoseRestoreAll(void)
{
	int i;

	for (i = 0; i < gPoseSlotCount; i++)
	{
		if (gPoseSlots[i].slot != NULL)
			*gPoseSlots[i].slot = gPoseSlots[i].saved;
		gPoseSlots[i].slot = NULL;
	}

	gPoseSlotCount = 0;
}

static void D2plPoseTrack(JER_BONE_ROT* slot)
{
	if (slot == NULL)
		return;

	if (gPoseSlotCount >= D2PL_POSE_SLOT_MAX)
	{
		/* a dropped slot is never restored -> permanent corruption of the
		 * shared motion buffer; make it loud instead of silent */
		jer_log("[d2pl] pose: %d rotation slots touched (max %d) — pose will not restore cleanly\n",
			gPoseSlotCount + 1, D2PL_POSE_SLOT_MAX);
		return;
	}

	gPoseSlots[gPoseSlotCount].slot = slot;
	gPoseSlots[gPoseSlotCount].saved = *slot;
	gPoseSlotCount++;
}

int D2plOnPedPose(void* userdata, void* args)
{
	JER_ARGS_PED_POSE* a = (JER_ARGS_PED_POSE*)args;
	WeaponBase* w = gCurrentWeapon;
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)a->ped;
	JER_BONE_ROT* joint1 = NULL;
	JER_BONE_ROT* neck = NULL;
	JER_BONE_ROT* shoulder = NULL;
	JER_BONE_ROT* elbow = NULL;
	int left;

	(void)userdata;

	if (a->ped == NULL || a->skel == NULL)
		return JER_RESULT_CONTINUE;

	/* weapons are on-foot only: nothing to pose — restore any dirty slots */
	if (w == NULL || player[0].playerCarId >= 0)
	{
		D2plPoseRestoreAll();
		return JER_RESULT_CONTINUE;
	}

	/* the weapon hangs off whichever arm matches the camera shoulder
	 * (the same convention poseArm/poseArmAim use) */
	left = (w->shoulderSide > 0);

	joint1 = jer_anim_bone_rotation(a->skel, JER_LIMB_JOINT_1);
	neck = jer_anim_bone_rotation(a->skel, JER_LIMB_NECK);
	shoulder = jer_anim_bone_rotation(a->skel, left ? JER_LIMB_LSHOULDER : JER_LIMB_RSHOULDER);
	elbow = jer_anim_bone_rotation(a->skel, left ? JER_LIMB_LELBOW : JER_LIMB_RELBOW);

	/* restore the slots dirtied LAST frame first, then snapshot + write
	 * the CURRENT frame's slots: the pvRotation bytes point into the
	 * SHARED per-type motion buffer and the animation advances the frame
	 * every draw, so an un-restored write reappears when the cycle loops
	 * back to that frame (and is visible to every ped sharing the type) */
	D2plPoseRestoreAll();

	/* arm pose: parent-relative vOffset written pre-newRotateBones, so
	 * the engine composes it through the body + torso-twist chain — the
	 * arm tracks the aim axis automatically while aiming */
	if (gAiming)
		w->poseArmAim(a->skel, pPed->dir.vy);
	else
		w->poseArm(a->skel, pPed->dir.vy);

	/* zero the weapon-side arm ROTATIONS (slot-tracked + restored) so the
	 * walk cycle's arm swing can't rotate the posed hand */
	D2plPoseTrack(shoulder);
	D2plPoseTrack(elbow);

	if (shoulder != NULL)
		shoulder->vx = shoulder->vy = shoulder->vz = 0;
	if (elbow != NULL)
		elbow->vx = elbow->vy = elbow->vz = 0;

	if (gAiming)
	{
		int aimYaw = gAimYaw;

		/* The torso faces the camera-forward (the back to the camera).
		 * Engine convention (pedest.c: the forward branch moves along
		 * dir.vy - 2048 and the model's face is -z): a body that FACES
		 * the aim runs at dir = aimYaw - 2048, so the twist off the
		 * body is the shortest signed angle DIFF(body, aimYaw - 2048),
		 * clamped to the ~70 degree spine limit (796/4096). Beyond
		 * that the BODY turns (the movement is full 360). The twist is
		 * measured against the COMPOSED torso yaw (jer_anim_torso_yaw:
		 * dir + ROOT + LOWERBACK animation yaws), so the walk cycle's
		 * own torso sway can't push the aim axis off the camera line
		 * (ped-animation.md §7). */
		D2plPoseTrack(joint1);
		D2plPoseTrack(neck);

		if (joint1 != NULL)
			joint1->vy = (short)jer_clamp_int(
				jer_anim_twist_to_aim(a->skel, pPed->dir.vy, aimYaw), -796, 796);

		/* head lock: keep the head aligned with the (rotated) torso.
		 * The NECK slot is the effective head-yaw channel (HEAD's own
		 * slot is never read); zeroing it aims the head straight along
		 * the twisted torso. head_rot is the stock L2/R2 look channel
		 * (TurnHead, camera.c:440-465) — neutralize it so a held look
		 * button can't turn the head while aiming. */
		pPed->head_rot = 0;

		if (neck != NULL)
			neck->vy = 0;
	}

	/* bone debug: log the RELATIVE transform chain while aiming — the
	 * ped's body yaw, the COMPOSED torso yaw (dir + ROOT + LOWERBACK
	 * animation yaws), the aim yaw, the head channels (NECK slot +
	 * head_rot), and the written rotations (JOINT1 = the twist, which
	 * should satisfy torso + twist ~= aimYaw - 2048 mod 4096; NECK and
	 * the weapon-arm slots should read 0 while posing). Every transform
	 * here is LOCAL (parent-relative): the root matrix carries
	 * pPed->dir, and each bone's pvRotation is the yaw the engine
	 * composes for that bone's CHILDREN. pdir is the engine's body
	 * heading (pPed->dir.vy); pdir - 2048 is the movement heading. */
	{
		static int gBoneLogTimer;

		if (--gBoneLogTimer <= 0 && gAiming)
		{
			int aimYaw = gAimYaw;
			int torso = jer_anim_torso_yaw(a->skel, pPed->dir.vy);

			gBoneLogTimer = 90;

			jer_log("[d2pl] bone: pedDir=%d torso=%d aim=%d head_rot=%d | JOINT1 rot=(%d,%d,%d) | NECK rot=(%d,%d,%d) | SHOULDER rot=(%d,%d,%d) ELBOW rot=(%d,%d,%d)\n",
				pPed->dir.vy, torso, aimYaw, pPed->head_rot,
				joint1 ? joint1->vx : 0, joint1 ? joint1->vy : 0, joint1 ? joint1->vz : 0,
				neck ? neck->vx : 0, neck ? neck->vy : 0, neck ? neck->vz : 0,
				shoulder ? shoulder->vx : 0, shoulder ? shoulder->vy : 0, shoulder ? shoulder->vz : 0,
				elbow ? elbow->vx : 0, elbow ? elbow->vy : 0, elbow ? elbow->vz : 0);
		}
	}

	return JER_RESULT_CONTINUE;
}


int D2plOnSkeleton(void* userdata, void* args)
{
	JER_ARGS_PED_SKELETON* a = (JER_ARGS_PED_SKELETON*)args;
	WeaponBase* w = gCurrentWeapon;
	SVECTOR* vJPos = (SVECTOR*)a->jointPos;
	VECTOR* playerPos = (VECTOR*)a->playerPos;
	VECTOR* cameraPos = (VECTOR*)a->cameraPos;
	int handLimb = WL_RHAND;

	(void)userdata;

	if (w == NULL)
		return JER_RESULT_CONTINUE;

	/* weapons are on-foot only */
	if (player[0].playerCarId >= 0)
		return JER_RESULT_CONTINUE;

	/* the arm that holds the weapon follows the camera shoulder */
	handLimb = (w->shoulderSide > 0) ? WL_LHAND : WL_RHAND;

	if (a->phase == 0)
	{
		/* NOTE: the arm pose moved to D2plOnPedPose (JER_EVENT_PED_POSE,
		 * BEFORE newRotateBones): it is written as parent-relative vOffset
		 * + zeroed shoulder/elbow pvRotation slots, so the engine composes
		 * it through the body + torso-twist chain (ped-animation.md §6).
		 * Nothing happens here — a pvRotation write in this phase is DEAD
		 * (newRotateBones, the only reader, already ran) and a vCurrPos
		 * write would be a model-frame poke. */
	}
	else if (a->phase == 1 && !a->shadow)
	{
		/* one-shot per-level diagnostic: the ped's real world scale vs the
		 * camera framing, so the on-foot camera units stay in sync with
		 * the ped (Driver's inverted y makes vy extent the height) */
		if (!gPedScaleLogged)
		{
			int i;
			int minX = 32767, maxX = -32767, minY = 32767, maxY = -32767;
			int minZ = 32767, maxZ = -32767;

			for (i = 0; i < 23; i++)
			{
				if (vJPos[i].vx < minX) minX = vJPos[i].vx;
				if (vJPos[i].vx > maxX) maxX = vJPos[i].vx;
				if (vJPos[i].vy < minY) minY = vJPos[i].vy;
				if (vJPos[i].vy > maxY) maxY = vJPos[i].vy;
				if (vJPos[i].vz < minZ) minZ = vJPos[i].vz;
				if (vJPos[i].vz > maxZ) maxZ = vJPos[i].vz;
			}

			jer_log("[d2pl] ped: head_pos=%d skel X[%d..%d] Y[%d..%d] Z[%d..%d] H=%d W=%d camDelta=(%d,%d) player=(%d,%d,%d)\n",
				((LPPEDESTRIAN)a->ped)->head_pos,
				minX, maxX, minY, maxY, minZ, maxZ,
				maxY - minY, maxX - minX,
				cameraPos->vx - playerPos->vx, cameraPos->vz - playerPos->vz,
				playerPos->vx, playerPos->vy, playerPos->vz);

			gPedScaleLogged = 1;
		}

		/* world-space hand position (muzzle origin) */
		gHandPos[0] = vJPos[handLimb].vx + playerPos->vx;
		gHandPos[1] = vJPos[handLimb].vy + playerPos->vy;
		gHandPos[2] = vJPos[handLimb].vz + playerPos->vz;

		/* draw the weapon mesh at the hand, camera-relative, barrel along
		 * the view axis (identity matrix -> camera-aligned) */
		if (w->meshModel != NULL)
		{
			VECTOR pos;
			MATRIX ident;

			pos.vx = vJPos[handLimb].vx + playerPos->vx - cameraPos->vx;
			pos.vy = vJPos[handLimb].vy + playerPos->vy - cameraPos->vy;
			pos.vz = vJPos[handLimb].vz + playerPos->vz - cameraPos->vz;

			/* InitMatrix(ident) sets the identity ROTATION (the macro only
			 * touches the 3x3; the translation comes from the pos arg) — the
			 * old code never initialized the matrix, so SetRotMatrix loaded
			 * stack garbage into the GTE rotation; every later draw (the
			 * engine's bone models) then projected through it and the OT
			 * writes went out of bounds (the weapon access violation) */
			InitMatrix(ident);

			SetRotMatrix(&ident);

			RenderModel(w->meshModel, &ident, &pos, 1, PLOT_NO_SHADE, 0, 0);

			/* restore the GTE state the engine's own bone draws rely on —
			 * the camera rotation it set at the top of newShowTanner plus
			 * the zero translation — otherwise Tanner's body would be drawn
			 * with the weapon's identity rotation and the hand's offset */
			{
				VECTOR zero = { 0, 0, 0 };

				gte_SetRotMatrix(&inv_camera_matrix);
				gte_SetTransVector(&zero);
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* JER_EVENT_FRAME — weapon update + laser line + impact marker        */
/* ================================================================== */


/* ================================================================== */
/* D2plWeaponFrame - per-frame weapon state: aim toggle, laser sight,     */
/* impact marker, fire/reload update                                      */
/* ================================================================== */

void D2plWeaponFrame(PLAYER* lp, int pad, int padNew)
{
	/* weapons only work on foot */
	if (lp->playerCarId >= 0)
	{
		if (gAiming)
			D2plAimExit(lp);	/* got in a car while aiming: release the aim line */

		gAiming = 0;
		return;
	}

	/* L1 = aim (only with a weapon equipped); edge-detect the enter/exit
	 * so the aim line snapshots at entry (no view swing) and the orbit
	 * parks at the last aim heading on exit (no snap back) */
	{
		int newAiming = (gCurrentWeapon != NULL && (pad & MPAD_L1) != 0);

		if (newAiming && !gAiming)
			D2plAimEnter(lp);
		else if (!newAiming && gAiming)
			D2plAimExit(lp);

		gAiming = newAiming;
	}

#ifndef PSX
		/* laser-sight line from Tanner's hand to the evaluated aim point
		 * while aiming. Depth-sorted: it respects Z order (walls occlude
		 * it). The hand/aim positions are in the render (negated) frame
		 * while Debug_AddLine expects y-up world coords — negate y here. */
	if (gAiming && gS.laserColor != D2PL_LASER_OFF && gCurrentWeapon != NULL &&
		(gHandPos[0] != 0 || gHandPos[1] != 0 || gHandPos[2] != 0))
	{
		VECTOR from;
		VECTOR to;
		CVECTOR col;

		D2plLaserColor(&col);

		from.vx = gHandPos[0];
		from.vy = -gHandPos[1];
		from.vz = gHandPos[2];

		to.vx = gAimPoint[0];
		to.vy = -gAimPoint[1];
		to.vz = gAimPoint[2];

		Debug_AddLineDepth(from, to, col);
	}

	/* impact marker: a big black X at the last shot's aim point, visible
	 * for MARKER_LIFETIME frames so the player can trace where the weapon
	 * is hitting */
	if (gMarkerTimer > 0)
	{
		VECTOR c;
		CVECTOR black = { 0, 0, 0 };
		int dx, dz;
		int size;
		int dist;

		c.vx = gMarkerPos[0];
		c.vy = -gMarkerPos[1];	/* render frame -> y-up for Debug_AddLine */
		c.vz = gMarkerPos[2];

		/* size scales with the distance from the camera */
		dist = ABS(gMarkerPos[0] - camera_position.vx)
			+ ABS(gMarkerPos[1] - camera_position.vy)
			+ ABS(gMarkerPos[2] - camera_position.vz);

		size = jer_clamp_int(dist / 8, MARKER_MIN_SIZE, MARKER_MAX_SIZE);

		dx = size;
		dz = size;

		/* an asterisk: both diagonals + horizontal + vertical */
		{
			VECTOR p;

			p.vx = c.vx - dx; p.vy = c.vy - dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx + dx; p.vy = c.vy - dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx - dx; p.vy = c.vy + dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx + dx; p.vy = c.vy + dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx - dx; p.vy = c.vy; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx + dx; p.vy = c.vy; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx; p.vy = c.vy - dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx; p.vy = c.vy + dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);
		}

		gMarkerTimer--;
	}
#endif

	if (gCurrentWeapon != NULL)
	{
		/* lazily resolve the weapon mesh once models are loaded */
		if (gCurrentWeapon->meshModel == NULL)
			gCurrentWeapon->init();

		gCurrentWeapon->aiming = gAiming;

		/* clear the fired flag the weapon may have set last frame */
		gCurrentWeapon->fired = 0;

		gCurrentWeapon->update(pad, padNew);

		/* a round was fired this frame: stamp the impact marker */
		if (gCurrentWeapon->fired)
		{
			gMarkerTimer = MARKER_LIFETIME;
			gMarkerPos[0] = gAimPoint[0];
			gMarkerPos[1] = gAimPoint[1];
			gMarkerPos[2] = gAimPoint[2];
		}
	}

}

/* ================================================================== */
/* JER_EVENT_DRAW_OVERLAY - weapon HUD                                    */
/* ================================================================== */

int D2plOnOverlay(void* userdata, void* args)
{
	WeaponBase* w = gCurrentWeapon;
	char text[64];

	(void)userdata;
	(void)args;

	if (w == NULL)
		return JER_RESULT_CONTINUE;

	/* HUD only while on foot */
	if (player[0].playerCarId >= 0)
		return JER_RESULT_CONTINUE;

	/* ammo bottom-left, will be creating a stylized D2 appropriate version soon. */
	sprintf(text, "[%d/%d]", w->clip, w->reserve);
	SetTextColour(255, 255, 255);
	PrintString(text, 20, 220);

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* JER_EVENT_PAUSE_MENU — D2PL Settings bridge                         */
/* ================================================================== */


/* ================================================================== */
/* The demo weapon                                                       */
/* ================================================================== */

class DemoPistol : public WeaponBase
{
public:
	DemoPistol()
	{
		id = "pistol";
		name = "9mm Pistol";
		damage = 100;
		fireMode = 0;		/* semi-auto */
		clipSize = 12;
		maxReserve = 60;
		fireRate = 6;
		reloadTime = 45;
		range = 30000;
		spread = 0;
		meshName = "BOMB";	/* placeholder mesh */
		shoulderSide = -1;
		aimZoom = 0;
	}
};

static DemoPistol gDemoPistol;


/* ================================================================== */
/* D2plWeaponInit - register the demo pistol (called from the entry)      */
/* ================================================================== */

void D2plWeaponInit(void)
{
	gDemoPistol.init();
	WeaponSystem_Register(&gDemoPistol);
	WeaponSystem_Equip(&gDemoPistol);
}
