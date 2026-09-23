// weapons/special/wheelman.c — WHEELMAN's special: the SHOCKWAVE. (PLACEHOLDER)
//
// PLACEHOLDER. A big bang goes off where Wheelman is standing, and everything
// within CD2_WHEELMAN_RADIUS takes falling damage from it - worst at the centre,
// nothing at the rim. It is a bomb he is inside, so it is short range, awkward
// to use well, and very good when it is not.
//
// The bang itself has NO COLLISION: its FX profile is collide = 0, so the
// engine's own explosion push/damage branch never runs for it and the blast is
// pure presentation. The damage below is the whole of it, once - the two cannot
// double-dip, which is what "no collision" is buying.
//
// Wheelman is skipped as the blast's victim. He paid the charge to be at the
// centre of it; a special that kills its own driver is a bug, not a weapon.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "jericho.h"
#include "jer_events.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"	/* cd2AoeBlast */
#include "weapons/fx/fx.h"
#include "weapons/special/special.h"

#include <string.h>

#define CD2_WHEELMAN_RADIUS	2200	// how far the blast reaches (world units)
#define CD2_WHEELMAN_DAMAGE	1800	// at the centre; falls off to nothing at the rim
#define CD2_WHEELMAN_CHANNEL_LOW	700	// the sound is pitched down for weight
#define CD2_WHEELMAN_SELF_HOP	180	// Wheelman is thrown up by his own blast

static int gShockChannel = -1;

static void cd2WheelmanFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	VECTOR at;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	at.vx = cp->hd.where.t[0];
	at.vy = cp->hd.where.t[1] + 30;
	at.vz = cp->hd.where.t[2];

	// knock the car up a little on its own blast, so it reads as a detonation
	// rather than a decal. The blast does no damage to it (skip = cp).
	cp->st.n.linearVelocity[1] += CD2_WHEELMAN_SELF_HOP;

	cd2AoeBlast(&at, CD2_WHEELMAN_RADIUS, CD2_WHEELMAN_DAMAGE, CD2_FX_SHOCK, cp, NULL, cp);

	// how many cars the blast actually caught, so a run can show the special
	// doing something rather than only going off
	{
		int i, caught = 0;

		for (i = 0; i < MAX_CARS; i++)
		{
			CAR_DATA* oc = &car_data[i];
			int dx, dz, dist;

			if (oc == cp || oc->controlType == CONTROL_TYPE_NONE || oc->ap.carCos == NULL)
				continue;

			dx = at.vx - oc->hd.where.t[0];
			dz = at.vz - oc->hd.where.t[2];
			dist = (ABS(dx) + ABS(dz)) / 2;

			if (dist <= CD2_WHEELMAN_RADIUS)
				caught++;
		}

		printInfo("[cainescrossfire] wheelman: shockwave at (%d,%d) r=%d dmg=%d, caught %d car(s)\n",
			at.vx, at.vz, CD2_WHEELMAN_RADIUS, CD2_WHEELMAN_DAMAGE, caught);
	}

	if (gShockChannel < 0)
	{
		gShockChannel = cd2TakeVoice();
	}

	if (gShockChannel >= 0)
		Start3DSoundVolPitch(gShockChannel, SOUND_BANK_SFX, 6,
			at.vx, at.vy, at.vz, -1200, 4096 - CD2_WHEELMAN_CHANNEL_LOW);
}

void cd2SpecialWheelmanRegister(JERICHO_CONTEXT* ctx)
{
	(void)ctx;
}

static CD2_WEAPON_DEF cd2MakeWheelmanDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_WHEELMAN;
	d.name = "special_wheelman";
	d.displayName = "Shockwave";	// placeholder
	d.cls = CD2_WCLS_AOE;

	d.isSpecial = 1;

	d.leanOut = 0;			// nothing to lean out of: the bomb is the car

	d.maxAmmo = 2;			// placeholder
	d.fireInterval = 30;
	d.refireCooldown = 700;		// placeholder: 23s

	d.damage = CD2_WHEELMAN_DAMAGE;
	d.speed = 0;
	d.range = CD2_WHEELMAN_RADIUS;
	d.life = 0;

	d.explosionEffect = CD2_FX_SHOCK;

	d.colR = 210; d.colG = 230; d.colB = 255;	// pale, like the bang

	d.fire = cd2WheelmanFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialWheelman = cd2MakeWheelmanDef();
