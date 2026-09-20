// weapons/special/fixer.c — FIXER's special: the LASER. (PLACEHOLDER)
//
// While the special is SELECTED, Fixer paints a beam from its gunner to whatever
// it is holding in its forward cone with a clear line to it. The beam is a LOCK,
// not a hit: the longer the same car stays held, the hotter it goes -
// white, then yellow, then red - and the harder the shot that follows lands.
//
//   * the beam only exists while there is a target in the cone (fireCone) AND
//     lineClear has a line to it. Lose either and the beam, and the lock, go.
//   * a different target resets the lock: the charge belongs to one car.
//   * firing spends it: heavy damage scaled by the charge, plus a hard twist
//     AWAY from Fixer. Five seconds between shots, so the charge cannot simply
//     be held for ever.
//
// White -> yellow takes about two seconds and the turn is quick at the end of
// it (the fade is eased), yellow -> red another two, so a solid lock reaches
// full red - and full damage - at roughly 5.5s.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "objcoll.h"		/* lineClear - the line-of-sight test */
#include "jericho.h"
#include "jer_events.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/core/crew.h"		/* the gunner's side: CD2_CREW_GUNNER */
#include "weapons/special/special.h"

#include <string.h>

extern int ratan2(int y, int x);	/* the module's angle helper */

#define CD2_FIXER_RANGE		6200	// how far the lock reaches
#define CD2_FIXER_CONE		420	// ...and how far off the nose (heading units, ~37 deg)
#define CD2_FIXER_CHARGE	165	// frames of solid lock for a full charge (5.5s)
#define CD2_FIXER_YELLOW	60	// lock frames where white has turned yellow (2s)
#define CD2_FIXER_RED		120	// ...and where yellow has turned red (4s)
#define CD2_FIXER_DAMAGE_MIN	400	// a snap shot, barely a lock at all
#define CD2_FIXER_DAMAGE_MAX	2600	// a full red shot
#define CD2_FIXER_TWIST_MIN	200	// the twist away, at no charge...
#define CD2_FIXER_TWIST_MAX	1400	// ...and at full
#define CD2_FIXER_SIDE		1	// the gunner's muzzle (right)

static int gTarget[MAX_CARS];		// the car being held (-1)
static int gLock[MAX_CARS];		// frames of continuous lock on it
static int gBeamChannel = -1;

static void cd2FixerColor(int lock, int* r, int* g, int* b)
{
	int t = lock;

	if (t > CD2_FIXER_CHARGE)
		t = CD2_FIXER_CHARGE;

	*r = 255;

	if (t < CD2_FIXER_YELLOW)
	{
		// white, with the turn concentrated in the last part of the stretch
		int k = (t * 4096) / CD2_FIXER_YELLOW;
		int e = (k * k) >> 12;			// eased: mostly white, then quickly yellow

		*g = 255 - ((255 - 235) * e) / 4096;
		*b = 255 - (255 * e) / 4096;
	}
	else
	{
		// yellow -> red
		int k = ((t - CD2_FIXER_YELLOW) * 4096) / (CD2_FIXER_RED - CD2_FIXER_YELLOW);

		if (k > 4096)
			k = 4096;

		*g = 235 - (235 * k) / 4096;
		*b = 0;
	}
}

static int cd2FixerCharge(int carId)
{
	int t = gLock[carId];

	if (t > CD2_FIXER_CHARGE)
		t = CD2_FIXER_CHARGE;

	return t;
}

// The head of the beam: the gunner's side of the car.
static void cd2FixerMuzzle(const CAR_DATA* cp, VECTOR* out)
{
	cd2WpnMuzzle((CAR_DATA*)cp, CD2_FIXER_SIDE, out);
	out->vy += 40;
}

// Acquire: the best target in the cone, in range, with a clear line. `best` is
// kept if nothing better is found, so a lock does not flicker between two cars.
static int cd2FixerAcquire(CAR_DATA* cp, VECTOR* c, int keep)
{
	int j, best = -1, bestD = 0;
	long long range2 = (long long)CD2_FIXER_RANGE * CD2_FIXER_RANGE;

	for (j = 0; j < MAX_CARS; j++)
	{
		CAR_DATA* oc = &car_data[j];
		VECTOR him;
		long long dx, dz, d2;
		int toTarget, diff;

		if (j == cp->id || oc->controlType == CONTROL_TYPE_NONE || oc->ap.carCos == NULL)
			continue;

		dx = (long long)oc->hd.where.t[0] - c->vx;
		dz = (long long)oc->hd.where.t[2] - c->vz;
		d2 = dx * dx + dz * dz;

		if (d2 > range2)
			continue;

		// forward only: the beam is on the nose, not around the car
		toTarget = ratan2((int)dx, (int)dz);
		diff = (toTarget - cp->hd.direction) & 0xfff;

		if (diff > 2048)
			diff -= 4096;

		if (diff < 0)
			diff = -diff;

		if (diff > CD2_FIXER_CONE)
			continue;

		// and it has to be a LINE: scenery blocks a lock outright
		him.vx = oc->hd.where.t[0];
		him.vy = oc->hd.where.t[1] + 40;
		him.vz = oc->hd.where.t[2];

		if (lineClear((VECTOR*)c, &him) == 0)
			continue;

		// nearest wins; the one already held keeps it on a tie
		if (best < 0 || (int)d2 < bestD || (j == keep && (int)d2 <= bestD))
		{
			best = j;
			bestD = (int)d2;
		}
	}

	return best;
}

static void cd2FixerFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	{
		int t = gTarget[cp->id];

		if (t < 0)
		{
			printInfo("[cainescrossfire] fixer: fired with no lock (car=%d)\n", cp->id);
			return;
		}

		{
			CAR_DATA* vic = &car_data[t];
			int charge = cd2FixerCharge(cp->id);
			int damage = CD2_FIXER_DAMAGE_MIN +
				((CD2_FIXER_DAMAGE_MAX - CD2_FIXER_DAMAGE_MIN) * charge) / CD2_FIXER_CHARGE;
			int twist = CD2_FIXER_TWIST_MIN +
				((CD2_FIXER_TWIST_MAX - CD2_FIXER_TWIST_MIN) * charge) / CD2_FIXER_CHARGE;
			VECTOR at, dir;
			long long dx, dz;
			long long ad;

			cd2FixerMuzzle(cp, &at);

			dx = (long long)vic->hd.where.t[0] - at.vx;
			dz = (long long)vic->hd.where.t[2] - at.vz;
			ad = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

			if (ad < 1)
				ad = 1;

			dir.vx = (int)((dx * 4096) / ad);	// away from Fixer
			dir.vz = (int)((dz * 4096) / ad);
			dir.vy = 0;

			cd2WpnDamageCar(vic, &at, damage, cp);
			cd2WpnKnock(vic, &at, &dir, twist);

			printInfo("[cainescrossfire] fixer: shot on car=%d -> car=%d lock=%d damage=%d twist=%d\n",
				cp->id, t, charge, damage, twist);

			if (gBeamChannel < 0)
			{
				gBeamChannel = GetFreeChannel(1);
				LockChannel(gBeamChannel);
			}

			if (gBeamChannel >= 0)
				Start3DSoundVolPitch(gBeamChannel, SOUND_BANK_SFX, 6,
					at.vx, at.vy, at.vz, -1600, 4096 - 700);
		}

		// spent: the lock starts again from nothing
		gTarget[cp->id] = -1;
		gLock[cp->id] = 0;
	}
}

static int cd2FixerOnFrame(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		VECTOR c, at, him;
		int t;

		if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
		{
			gTarget[i] = -1;
			gLock[i] = 0;
			continue;
		}

		// the beam is on only while the special is the ARMED weapon - the same
		// "always on while selected" the Hornet ring uses
		if (cd2WpnCarArmed(cp) != CD2_WID_SPECIAL_FIXER)
		{
			gTarget[i] = -1;
			gLock[i] = 0;
			continue;
		}

		c.vx = cp->hd.where.t[0];
		c.vy = cp->hd.where.t[1] + 40;
		c.vz = cp->hd.where.t[2];

		t = cd2FixerAcquire(cp, &c, gTarget[i]);

		if (t < 0)
		{
			// nothing to hold: the beam goes, and so does the charge
			if (gTarget[i] >= 0)
				printInfo("[cainescrossfire] fixer: lock lost (car=%d)\n", i);

			gTarget[i] = -1;
			gLock[i] = 0;
			continue;
		}

		if (t != gTarget[i])
		{
			// a different car: the charge belongs to one target
			gTarget[i] = t;
			gLock[i] = 0;
		}
		else if (gLock[i] < CD2_FIXER_CHARGE)
		{
			gLock[i]++;
		}

		{
			int r, g, b;

			cd2FixerColor(gLock[i], &r, &g, &b);

			cd2FixerMuzzle(cp, &at);
			him.vx = car_data[t].hd.where.t[0];
			him.vy = car_data[t].hd.where.t[1] + 40;
			him.vz = car_data[t].hd.where.t[2];

			cd2WpnLine(&at, &him, r, g, b);

			// the dot on the target, so the lock point is legible
			cd2WpnLine(&him, &him, r, g, b);
		}
	}

	return JER_RESULT_CONTINUE;
}

static int cd2FixerOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gTarget[i] = -1;
		gLock[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

void cd2SpecialFixerRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2FixerOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2FixerOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2FixerOnGameStart, NULL, 0);
}

static CD2_WEAPON_DEF cd2MakeFixerDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_FIXER;
	d.name = "special_fixer";
	d.displayName = "Laser Lock";	// placeholder
	d.cls = CD2_WCLS_AOE;

	d.isSpecial = 1;

	d.leanOut = CD2_CREW_GUNNER;	// the gunner holds the beam

	d.maxAmmo = 2;			// placeholder
	d.fireInterval = 30;
	d.refireCooldown = 150;		// 5s between special shots

	d.damage = CD2_FIXER_DAMAGE_MAX;	// the real figure is scaled by the lock
	d.speed = 0;
	d.range = CD2_FIXER_RANGE;
	d.life = 0;

	d.fireCone = CD2_FIXER_CONE;
	d.explosionEffect = LITTLE_BANG;

	d.colR = 255; d.colG = 235; d.colB = 255;	// white -> yellow -> red at the beam

	d.fire = cd2FixerFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialFixer = cd2MakeFixerDef();
