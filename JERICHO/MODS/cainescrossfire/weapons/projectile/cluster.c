// weapons/projectile/cluster.c — the CLUSTER MISSILE (projectile weapon).
//
// A primary weapon: a deep-orange missile that flies like the plain missile
// but, on impact, bursts into a short sequence of small explosions instead of
// one. When it hits a CAR the burst STICKS to it (the blasts keep riding the
// car at the point it was struck, so a moving target keeps getting chewed);
// when it hits the ground/scenery/its range the burst scatters at the hit
// point. The scheduling + car-sticking live in the fx library
// (weapons/fx/fx.c, cd2FxBarrage); the projectile pool calls it on impact
// because this def's barrageCount > 0.
//
// The parent impact still plays the def's impactFx (CD2_FX_CLUSTER, deep
// orange); the burst then uses CD2_FX_BOMBLET (small orange).

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/fx/fx.h"

#include <string.h>

static int gClusterChannel = -1;	// voice reserved for the launch sound

static void cd2ClusterFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR muzzle;
	VECTOR dir;
	VECTOR vel;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	cd2WpnShotMuzzle(&cd2WdefCluster, cp, 0, &muzzle);	// gunner's window (leanOut)
	cd2WpnForward(cp, &dir);

	vel.vx = (int)(((long long)dir.vx * cd2WdefCluster.speed) >> 12);
	vel.vy = (int)(((long long)dir.vy * cd2WdefCluster.speed) >> 12);
	vel.vz = (int)(((long long)dir.vz * cd2WdefCluster.speed) >> 12);

	cd2ProjectileSpawn(&cd2WdefCluster, cp, &muzzle, &vel, &dir);

	if (gClusterChannel < 0)
	{
		// GetFreeChannel(1), not GetFreeChannel(): sound.h declares the
		// parameter as 'int force = 1' (a C++ default argument), and this
		// module is C, so the default never applies and must be passed.
		gClusterChannel = GetFreeChannel(1);
		LockChannel(gClusterChannel);

		if (gCd2Cfg.debugLog)
			printInfo("[cainescrossfire] cluster sound: channel=%d locked\n", gClusterChannel);
	}

	// a low, thumpy launch, distinct from the missile's and the seeker's
	Start3DSoundVolPitch(gClusterChannel, SOUND_BANK_SFX, 6,
		muzzle.vx, muzzle.vy, muzzle.vz, -2200, 4096 - 512);
}

static CD2_WEAPON_DEF cd2MakeClusterDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_CLUSTER;
	d.name = "CLUSTER";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isBase = 0;
	d.hidden = 0;
	d.pickupEnabled = 1;
	d.pickupAmmo = 6;

	d.maxAmmo = 12;
	d.fireInterval = 45;
	d.refireCooldown = 140;	// 2.3s minimum between clusters

	d.damage = 260;		// light direct hit — the burst does the work
	d.speed = 1300;
	d.range = 11000;
	d.life = 0;

	d.splashRadius = 300;	// the parent warhead blast
	d.splashDamage = 200;
	d.explosionEffect = BIG_BANG;
	d.impactFx = CD2_FX_CLUSTER;	// deep orange parent burst

	d.homing = 0;
	d.collideScenery = 1;

	d.leanOut = 2;		// the passenger leans out to launch the cluster

	// fires straight-ish; the AI will still launch it through a small error
	d.fireCone = 600;

	// deep orange missile body
	d.colR = 255; d.colG = 90; d.colB = 0;

	// -------------------------------------------------------------------
	// Barrage: 5 small explosions, ~4 frames apart, scattered by +/-60
	// world units, sticking to a car it hits.
	// -------------------------------------------------------------------
	d.barrageCount = 5;
	d.barrageInterval = 4;
	d.barrageJitter = 60;
	d.barrageFx = CD2_FX_BOMBLET;
	d.barrageRadius = 220;
	d.barrageDamage = 140;
	d.barrageStick = 1;

	d.fire = cd2ClusterFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefCluster = cd2MakeClusterDef();
