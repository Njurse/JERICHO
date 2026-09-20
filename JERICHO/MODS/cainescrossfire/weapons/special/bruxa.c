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
#include "motion/shove.h"		/* cd2ShovePush - the one shove that survives the sim */
#include "knock/knock.h"		/* the nose-up kick (and its ceiling) */

#include <string.h>

// THE RECOIL, in the only terms that work. This used to write
// cp->st.n.linearVelocity directly, which the module's OWN sim overwrites from
// its velX/velZ before the car moves - so the shove was gone before the car did
// anything with it, and no constant was ever large enough to see: 1500 read as
// 0.37 speed units per frame, and cranking it to 15000 (as it stands in the
// working tree) only reached 3.7, against a car whose top speed is in the
// hundreds. A percentage of the car's own top speed, handed to the sim through
// motion/shove.c, is the same path the turbo's engagement shove uses.
#define CD2_BRUXA_RECOIL_PCT	32	// shooter's backward shove (% of its own top speed)
// The nose-up kick is a KNOCK, not an angular velocity: the sim ZEROES
// angularVelocity[0] and [2] every frame (they are the render-only knock's
// channel), so writing a pitch there wrote into a field that was cleared before
// it was ever read.
#define CD2_BRUXA_RECOIL_PITCH	CD2_KNOCK_IMPULSE_TO(CD2_KNOCK_MAX_PITCH)
#define CD2_BRUXA_RECOIL_LIFT	1

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

	// strong recoil into the SHOOTER: shove it backward - through the sim, which
	// is the only place a shove survives - and kick its nose up with a knock
	cd2SpecFwdRight(cp, &fwd, &right);

	cd2ShovePush(cp->id, -fwd.vx, -fwd.vz, CD2_BRUXA_RECOIL_PCT);

	cd2KnockAdd(cp->id, CD2_BRUXA_RECOIL_PITCH, 0, 0, CD2_BRUXA_RECOIL_LIFT, 0);

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
