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
/* Aim camera: the over-the-shoulder aim view + the aim-point evaluation  */
/* ================================================================== */

int gAimLookBlend = 0;	/* 0 = settled (forward), 4096 = free-looking */

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

		/* --- aiming: "orbit looking OUTWARD" — the view tilts with the
		 * stick (camera-relative aim, crosshair centered on screen) and the
		 * player sits over the shoulder; conventional shooter behavior --- */
		camAngle->vx += gLookPitch;

		camPos->vx += (lp->pos[0] - camPos->vx) * AIM_PULL / 4096;
		camPos->vz += (lp->pos[2] - camPos->vz) * AIM_PULL / 4096;

		/* rebase into the module (negated) frame like the other branches —
		 * the engine hands the aim path RAW y (camera.c:570), and the
		 * shared smooth/cap/collision math below expects -rawY */
		if (a->basePos != NULL)
			camPos->vy = -((int*)a->basePos)[1] + AIM_HEIGHT;

		camPos->vx += FIXEDH(RCOS(heading) * AIM_LATERAL * gS.shoulder);
		camPos->vz += -FIXEDH(RSIN(heading) * AIM_LATERAL * gS.shoulder);

		if (a->basePos != NULL)
		{
			int clip = D2plSmoothCamera(camPos, (int*)a->basePos, a->inCar, NULL);

			D2plLogCamera(lp, a->baseDir, camPos, camAngle, clip);
		}

		SetGeomScreen(scr_z = gCameraDefaultScrZ + AIM_ZOOM);

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
 * the arms follow because they hang off the same root. The HEAD is locked
 * to the torso (local yaw 0) so the walk/aim cycle can't turn it away. */
static JER_BONE_ROT gSavedJoint1;	/* the shared motion buffer's original
					   rotations (snapshotted at aim start,
					   restored when aiming stops) */
static JER_BONE_ROT gSavedHead;
static JER_BONE_ROT* gSavedJoint1Slot;	/* the motion slot we last wrote */
static JER_BONE_ROT* gSavedHeadSlot;
static int gPoseSaved;

int D2plOnPedPose(void* userdata, void* args)
{
	JER_ARGS_PED_POSE* a = (JER_ARGS_PED_POSE*)args;
	JER_BONE_ROT* joint1;
	JER_BONE_ROT* head;

	(void)userdata;

	if (a->ped == NULL || a->skel == NULL)
		return JER_RESULT_CONTINUE;

	joint1 = jer_anim_bone_rotation(a->skel, JER_LIMB_JOINT_1);
	head = jer_anim_bone_rotation(a->skel, JER_LIMB_HEAD);

	if (gAiming)
	{
		/* the pvRotation bytes point into the SHARED per-type motion buffer,
		 * and the walk cycle ADVANCES the motion frame during the aim — so
		 * every slot we touch must be restored, or the twist persists in
		 * the other frames. Restore the PREVIOUS aim frame’s slot first,
		 * then snapshot + write the CURRENT one; the final slot is put
		 * back on the first non-aim frame. */
		if (gPoseSaved)
		{
			if (gSavedJoint1Slot != NULL)
				*gSavedJoint1Slot = gSavedJoint1;
			if (gSavedHeadSlot != NULL)
				*gSavedHeadSlot = gSavedHead;
			gPoseSaved = 0;
		}

		if (joint1 != NULL)
		{
			gSavedJoint1 = *joint1;
			gSavedJoint1Slot = joint1;
		}
		if (head != NULL)
		{
			gSavedHead = *head;
			gSavedHeadSlot = head;
		}
		gPoseSaved = 1;

		if (joint1 != NULL)
		{
			LPPEDESTRIAN pPed = (LPPEDESTRIAN)a->ped;
			int aimYaw = (-camera_angle.vy) & 0xfff;

			/* The torso faces the camera-forward (the back to the camera).
			 * Engine convention (pedest.c: the forward branch moves along
			 * dir.vy - 2048 and the model’s face is -z): a body that FACES
			 * the aim runs at dir = aimYaw - 2048, so the twist off the
			 * body is the shortest signed angle DIFF(body, aimYaw - 2048),
			 * clamped to the ~70 degree spine limit (796/4096). Beyond
			 * that the BODY turns (the movement is full 360). */
			joint1->vy = (short)jer_clamp_int(
				DIFF_ANGLES(pPed->dir.vy, aimYaw - 2048), -796, 796);
		}

		/* head lock: keep the head aligned with the (rotated) torso */
		if (head != NULL)
			head->vy = 0;
	}
	else if (gPoseSaved)
	{
		/* the aim stopped: restore the LAST touched slot */
		if (gSavedJoint1Slot != NULL)
			*gSavedJoint1Slot = gSavedJoint1;
		if (gSavedHeadSlot != NULL)
			*gSavedHeadSlot = gSavedHead;
		gPoseSaved = 0;
	}

	/* bone debug: log the RELATIVE transform chain while aiming — the
	 * ped's body yaw, the aim yaw, the diff written into the upper-body
	 * root, and the root/head local rotations as the engine reads them.
	 * Every transform here is LOCAL (parent-relative): the root matrix
	 * carries pPed->dir, and each bone's pvRotation is its own yaw
	 * relative to its parent. pdir is the engine's body heading
	 * (pPed->dir.vy); pdir - 2048 is the movement heading. */
	{
		static int gBoneLogTimer;

		if (--gBoneLogTimer <= 0 && gAiming)
		{
			LPPEDESTRIAN pPed = (LPPEDESTRIAN)a->ped;
			int aimYaw = (-camera_angle.vy) & 0xfff;

			gBoneLogTimer = 90;

			jer_log("[d2pl] bone: pedDir=%d aim=%d diff=%d | JOINT1 rot=(%d,%d,%d) | HEAD rot=(%d,%d,%d)\n",
				pPed->dir.vy, aimYaw, jer_anim_aim_diff(pPed->dir.vy, aimYaw),
				joint1 ? joint1->vx : 0, joint1 ? joint1->vy : 0, joint1 ? joint1->vz : 0,
				head ? head->vx : 0, head ? head->vy : 0, head ? head->vz : 0);
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
		/* NOTE: the TORSO rotation no longer happens here — a pvRotation
		 * write in this phase is DEAD (newRotateBones, the only reader,
		 * already ran). The torso faces the camera in D2plOnPedPose (the
		 * JER_EVENT_PED_POSE hook, which fires BEFORE newRotateBones). */

		/* force the arm into a holding pose: override the accumulated
		 * offsets of the upper arm / forearm / hand bones so the arm
		 * extends forward. Values are in the ped's local frame. */
		if (gAiming)
			w->poseArmAim(a->skel, ((LPPEDESTRIAN)a->ped)->dir.vz);
		else
			w->poseArm(a->skel, ((LPPEDESTRIAN)a->ped)->dir.vz);
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
		gAiming = 0;
		return;
	}

	/* L1 = aim (only with a weapon equipped) */
	gAiming = (gCurrentWeapon != NULL && (pad & MPAD_L1) != 0);

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
