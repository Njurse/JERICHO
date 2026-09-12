// weapons/drops/mine.c — the MINE (drop weapon).
//
// A HIDDEN drop weapon: it never spawns as a map pickup and is never part of
// a starting arsenal (only reachable via the debug "all weapons" grant). It
// is dropped behind the caster, falls to the ground, arms, and detonates when
// a NON-caster car drives within def->radius.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

#include <string.h>

#define CD2_MINE_THROW	30	// backward launch speed (world units/frame)
#define CD2_MINE_POP	18	// initial upward pop (world units/frame)

static void cd2MineDrop(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	const MATRIX* w;
	const SVECTOR* cb;
	VECTOR p, vel;
	int back;

	if (cp == NULL || cp->ap.carCos == NULL)
		return;

	w = &cp->hd.where;
	cb = &cp->ap.carCos->colBox;
	back = cb->vz;		// full length -> release behind the tail

	p.vx = w->t[0] - (int)(((long long)w->m[0][2] * back) >> 12);
	p.vy = w->t[1] + (cb->vy / 2);
	p.vz = w->t[2] - (int)(((long long)w->m[2][2] * back) >> 12);

	vel.vx = -(int)(((long long)w->m[0][2] * CD2_MINE_THROW) >> 12);
	vel.vy = CD2_MINE_POP;
	vel.vz = -(int)(((long long)w->m[2][2] * CD2_MINE_THROW) >> 12);

	cd2DropSpawn(&cd2WdefMine, cp, &p, &vel);

	Start3DSoundVolPitch(-1, SOUND_BANK_SFX, 5,
		p.vx, p.vy, p.vz, -2000, 4096 + 2048);
}

static CD2_WEAPON_DEF cd2MakeMineDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_MINE;
	d.name = "MINE";
	d.cls = CD2_WCLS_DROP;

	d.isBase = 0;
	d.hidden = 1;		// never in pickups / starting arsenal
	d.pickupEnabled = 0;
	d.pickupAmmo = 0;

	d.maxAmmo = 8;
	d.fireInterval = 40;
	d.refireCooldown = 120;	// 2s minimum between drops

	d.damage = 700;		// damage on the triggering car
	d.speed = 0;		// dropped, not flown
	d.range = 0;
	d.life = 0;

	d.radius = 400;		// proximity trigger radius
	d.splashRadius = 450;	// explosion radius
	d.splashDamage = 700;	// explosion damage at centre
	d.explosionEffect = BIG_BANG;

	d.homing = 0;		// RESERVED (no logic this turn)
	d.collideScenery = 1;	// mine lands on walls/roofs instead of falling through

	d.colR = 255; d.colG = 60; d.colB = 60;

	d.fire = cd2MineDrop;
	return d;
}

const CD2_WEAPON_DEF cd2WdefMine = cd2MakeMineDef();
