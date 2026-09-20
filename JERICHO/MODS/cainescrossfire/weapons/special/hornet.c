// weapons/special/hornet.c — HORNET's special: SPIKE STORM.
//
// The ring deploys while the special is ARMED: eight spikes spring up around
// the car (the first-cut visual is eight black lines in the eight compass
// directions) and any other car that comes inside the ring takes contact damage
// and a knockback every CD2_HORNET_CONTACT_INTERVAL frames.
//
// Pressing fire LAUNCHES the eight spikes outward in their eight facing
// directions — weakly homing with a narrow lock cone — and they explode on
// whatever they hit (world or car). The launch spends the charge and starts the
// special's recharge (the weapon framework's refire cooldown), so the ring
// blacks out briefly and then re-arms once the recharge is done.
//
// First cut: the ring is a damage/knock aura drawn with debug lines, not a
// modelled mesh; the launched spikes are the projectile pool with weak homing.

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

#define CD2_HORNET_RADIUS		520	// ring radius (world units)
#define CD2_HORNET_SPIKE_H		115	// how far a spike stands up out of the ring
#define CD2_HORNET_CONTACT_INTERVAL	15	// frames between ring contact hits
#define CD2_HORNET_HOP			140	// knockback strength on contact
#define CD2_HORNET_RELAUNCH_GAP		80	// frames the ring stays down after a launch

static int gHornetPost[MAX_CARS];	// frames until the ring may re-arm (post-launch)
static int gHornetAcc[MAX_CARS];	// contact-hit accumulator
static int gHornetChannel = -1;

static void cd2HornetLaunch(CAR_DATA* cp)
{
	VECTOR vel, dir, from;
	int i;

	from.vx = cp->hd.where.t[0];
	from.vy = cp->hd.where.t[1] + 40;
	from.vz = cp->hd.where.t[2];

	// fire the eight spikes outward, one per compass eighth
	for (i = 0; i < 8; i++)
	{
		cd2SpecCompass(cp, i, &dir);

		vel.vx = (int)(((long long)dir.vx * cd2WdefSpecialHornet.speed) >> 12);
		vel.vy = (int)(((long long)dir.vy * cd2WdefSpecialHornet.speed) >> 12);
		vel.vz = (int)(((long long)dir.vz * cd2WdefSpecialHornet.speed) >> 12);

		cd2ProjectileSpawn(&cd2WdefSpecialHornet, cp, &from, &vel, &dir);
	}

	if (gHornetChannel >= 0)
		Start3DSoundVolPitch(gHornetChannel, SOUND_BANK_SFX, 6, from.vx, from.vy, from.vz, -2500, 4096 + 900);
}

static void cd2HornetFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	cd2HornetLaunch(cp);

	// the ring blacks out for a beat, then re-arms
	gHornetPost[cp->id] = CD2_HORNET_RELAUNCH_GAP;

	if (gHornetChannel < 0)
	{
		gHornetChannel = GetFreeChannel(1);
		LockChannel(gHornetChannel);
	}
}

// Ring contact + the eight-line visual, once per car per frame, while armed.
static int cd2HornetOnFrame(void* ud, void* args)
{
	int i, j;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		VECTOR c;
		int armed;

		if (gHornetPost[i] > 0)
			gHornetPost[i]--;

		armed = (cp->ap.carCos != NULL) && (cd2WpnCarArmed(cp) == CD2_WID_SPECIAL_HORNET);

		if (!armed || gHornetPost[i] > 0)
		{
			gHornetAcc[i] = 0;
			continue;
		}

		c.vx = cp->hd.where.t[0];
		c.vy = cp->hd.where.t[1] + 40;
		c.vz = cp->hd.where.t[2];

		// The eight spikes, one per compass direction: a short vertical line
		// standing up out of the ring, plus a faint spoke back to the car. They
		// are here only while the special is the ARMED weapon, so selecting it
		// visibly arms the car and any other weapon retracts them.
		for (j = 0; j < 8; j++)
		{
			VECTOR dir, base, tip;

			cd2SpecCompass(cp, j, &dir);

			base.vx = c.vx + (dir.vx * CD2_HORNET_RADIUS >> 12);
			base.vy = cp->hd.where.t[1];
			base.vz = c.vz + (dir.vz * CD2_HORNET_RADIUS >> 12);

			tip = base;
			tip.vy = base.vy + CD2_HORNET_SPIKE_H;

			cd2WpnLine(&base, &tip, 224, 224, 236);		// bright steel spike
			cd2WpnLine(&base, &c, 96, 96, 116);		// faint spoke to the car
		}

		// contact damage + knockback to other cars inside the ring
		if (++gHornetAcc[i] >= CD2_HORNET_CONTACT_INTERVAL)
		{
			gHornetAcc[i] = 0;

			for (j = 0; j < MAX_CARS; j++)
			{
				CAR_DATA* oc = &car_data[j];
				long long dx, dz, d2, base2, reach2;
				int reach;

				if (j == i || oc->controlType == CONTROL_TYPE_NONE || oc->ap.carCos == NULL)
					continue;

				dx = (long long)oc->hd.where.t[0] - c.vx;
				dz = (long long)oc->hd.where.t[2] - c.vz;
				d2 = dx * dx + dz * dz;

				// the ring is measured centre-to-centre, so a big car can have its
				// flank in the spikes with its centre still outside them. Add what
				// the victim's own body reaches, so touching the spikes counts.
				//
				// All of this is 64-bit on purpose: a squared world distance
				// overflows 32 bits on a big map, and a wrapped NEGATIVE read as
				// "inside the ring" - the ring was damaging and knocking cars on
				// the far side of the level.
				reach = CD2_HORNET_RADIUS +
					(oc->ap.carCos->colBox.vx + oc->ap.carCos->colBox.vz) / 2;
				reach2 = (long long)reach * reach;
				base2 = (long long)CD2_HORNET_RADIUS * CD2_HORNET_RADIUS;

				if (d2 > reach2)
					continue;

				// only worth saying when it is the flank case the reach was added for
				if (d2 > base2)
					printInfo("[cainescrossfire] hornet: ring hit car=%d, flank in the spikes\n", j);

				cd2WpnDamageCar(oc, &c, cd2WdefSpecialHornet.damage, cp);

				{
					VECTOR dir;
					long long adx = (dx < 0 ? -dx : dx);
					long long adz = (dz < 0 ? -dz : dz);
					long long ad = adx + adz;

					if (ad < 1) ad = 1;
					dir.vx = (int)(-(dx * 4096) / ad);
					dir.vz = (int)(-(dz * 4096) / ad);
					dir.vy = 0;
					cd2WpnKnock(oc, &c, &dir, CD2_HORNET_HOP);
				}
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

static int cd2HornetOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gHornetPost[i] = 0;
		gHornetAcc[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

void cd2SpecialHornetRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2HornetOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2HornetOnGameStart, NULL, 0);
	// the same reset when the game returns to the frontend menus
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2HornetOnGameStart, NULL, 0);
}

static CD2_WEAPON_DEF cd2MakeHornetDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_HORNET;
	d.name = "special_hornet";
	d.displayName = "Spike Storm";
	d.cls = CD2_WCLS_PROJECTILE;

	d.isSpecial = 1;
	d.hidden = 0;
	d.pickupEnabled = 0;

	d.maxAmmo = 3;			// profile capacity
	d.fireInterval = 30;
	d.refireCooldown = 600;		// profile recharge: 20s at 30fps

	d.damage = 260;			// ring contact AND a launched spike's direct hit
	d.speed = 2600;
	d.range = 7000;
	d.life = 0;

	d.splashRadius = 260;
	d.splashDamage = 220;		// explodes on impact with anything
	d.explosionEffect = BIG_BANG;

	d.homing = 1;
	d.homingRate = 40;		// weak homing
	d.collideScenery = 1;

	d.fireCone = 300;		// narrow lock-on cone

	d.colR = 30; d.colG = 30; d.colB = 30;	// the spikes read black

	d.fire = cd2HornetFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialHornet = cd2MakeHornetDef();
