// weapons/special/bruxa.c — BRUXA's special: DOUBLE BOOM.
//
// The double shotgun: fires TWICE the pellets of the ordinary SHOTGUN (from
// both windows at once), shoves a hard recoil back into the
// shooter, and its pellets carry a heavy per-hit knock so a victim takes a
// shove scaled by how many pellets connected (and how close the blast was — a
// closer shot lands more pellets). A big, low, loud report sells it.
//
// First cut: the target recoil rides on the raycast pool's per-pellet knock
// (strength = per-pellet damage), so more contacts = harder shove. A per-hit
// distance term is a later refinement.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "jericho.h"
#include "jer_events.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/special/special.h"

#include <string.h>

#define CD2_BRUXA_RECOIL		1500	// shooter's backward shove (world units/frame, *4096)
#define CD2_BRUXA_RECOIL_PITCH		0x14000	// nose-up pitch added to the shooter (raw avel)

static int gBruxaChannel = -1;

static void cd2BruxaFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR fwd, right;
	VECTOR muzzle;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	// the doubled-double blast, alternating the two windows
	cd2RaycastScatter(&cd2WdefSpecialBruxa, cp,
		cd2WdefSpecialBruxa.pelletCount,
		cd2WdefSpecialBruxa.pelletSpread,
		cd2WdefSpecialBruxa.pelletFanout);

	// strong recoil into the SHOOTER: shove it backward and kick its nose up
	cd2SpecFwdRight(cp, &fwd, &right);

	cp->st.n.linearVelocity[0] -= (int)(((long long)fwd.vx * CD2_BRUXA_RECOIL) >> 12);
	cp->st.n.linearVelocity[1] -= (int)(((long long)fwd.vy * CD2_BRUXA_RECOIL) >> 12);
	cp->st.n.linearVelocity[2] -= (int)(((long long)fwd.vz * CD2_BRUXA_RECOIL) >> 12);

	cp->st.n.angularVelocity[0] += CD2_BRUXA_RECOIL_PITCH;

	if (gBruxaChannel < 0)
	{
		gBruxaChannel = GetFreeChannel(1);
		LockChannel(gBruxaChannel);
	}

	cd2WpnShotMuzzle(&cd2WdefSpecialBruxa, cp, 0, &muzzle);

	// a real loud BOOM: the same low sample, pitched down and boosted
	if (gBruxaChannel >= 0)
		Start3DSoundVolPitch(gBruxaChannel, SOUND_BANK_SFX, 6,
			muzzle.vx, muzzle.vy, muzzle.vz, -1200, 4096 - 2200);
}

static CD2_WEAPON_DEF cd2MakeBruxaDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_BRUXA;
	d.name = "special_bruxa";
	d.displayName = "Double Boom";
	d.cls = CD2_WCLS_SHOTGUN;

	d.isSpecial = 1;

	d.maxAmmo = 4;			// profile capacity
	d.fireInterval = 27;
	d.refireCooldown = 450;		// profile recharge: 15s

	d.damage = 760;			// PER PELLET — double the ordinary shotgun's 380
	d.speed = 2200;
	d.range = 2600;			// still a shotgun: short
	d.life = 0;

	d.splashRadius = 0;
	d.splashDamage = 0;
	d.explosionEffect = BIG_BANG;

	d.collideScenery = 1;
	d.fireCone = 500;

	d.leanOut = 3;			// both crew fire it

	d.colR = 255; d.colG = 90; d.colB = 60;	// hot orange-red

	d.pelletCount = 40;		// 2x the double shotgun's 20
	d.pelletSpread = 820;
	d.pelletFanout = 280;

	d.fire = cd2BruxaFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialBruxa = cd2MakeBruxaDef();

void cd2SpecialBruxaRegister(JERICHO_CONTEXT* ctx)
{
	(void)ctx;
	// Bruxa is pure fire-time (pellets + shooter recoil), so it needs no status
	// hooks of its own.
}
