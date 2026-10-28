/* d2pl_weapon.c — the test weapons, their laser and the HUD.
 *
 * Three weapons, each a D2plWeaponDef row naming a holdtype and a fire mode:
 *   pistol      semi-auto: one shot per R1 press
 *   machine gun automatic: 6 rounds a second while R1 is held
 *   debug gun   every frame while R1 is held, endless ammo (for testing)
 *
 *   L1          aim
 *   R1          fire (while aiming)
 *   O / Circle reload (was Cross, which is also run/forward on foot)
 *   L2          next weapon (press)
 *   (on foot d2pl strips L1/R1/L2 from stock Tanner; mouse is deferred)
 *
 * There is still no gun mesh: a shot is the hit-scan (damage stub), the
 * aim-point marker, jer_fx_impact sparks + black puff where it lands on a ped
 * or the ground, a short recoil pulse for the pose, and the fire sound. Sounds are placeholders: the frontend menu
 * samples, loaded in game through jer_sound_request_menu_bank (accept = fire,
 * back = reload). See HOLDTYPES.md.
 */

#include "d2pl.h"
#include "jer_math.h"
#include "jer_sound.h"
#include "jer_npc.h"
#include <math.h>

#include <stdio.h>
#include <stdlib.h>

#define D2PL_LASER_ENABLED	1

#define D2PL_TICK_HZ		30		/* JER_EVENT_FRAME rate (game logic) */

enum
{
	D2PL_FIRE_SEMI = 0,		/* one shot per trigger press */
	D2PL_FIRE_AUTO,			/* roundsPerSec while held */
	D2PL_FIRE_EVERY_FRAME	/* one shot every tick while held */
};

typedef struct D2plWeaponDef
{
	const char* name;
	int holdtype;			/* D2PL_HOLD_* or a registered id */
	int fireMode;			/* D2PL_FIRE_* */
	int roundsPerSec;		/* AUTO only */
	int clipSize;			/* 0 = endless */
	int reserve;
	int reloadFrames;
	int fireSfx;			/* JER_MENU_SFX_* fallback, -1 = silent */
	int reloadSfx;
	const char* fireWav;	/* MODS/d2pl/sounds/<name>.wav when present */
	const char* reloadWav;
	int damage;				/* per hit, of D2PL_PED_HEALTH */
} D2plWeaponDef;

typedef struct D2plWeaponState
{
	int clip;
	int reserve;
	int credit;				/* AUTO: += roundsPerSec per tick, fire at D2PL_TICK_HZ */
	int reloading;
} D2plWeaponState;

static const D2plWeaponDef gWeaponDefs[] = {
	{ "PISTOL",	D2PL_HOLD_PISTOL, D2PL_FIRE_SEMI,		0, 12, 60,  45, JER_MENU_SFX_ACCEPT, JER_MENU_SFX_BACK,
		"pistol_fire", "pistol_reload", 25 },
	{ "MG",		D2PL_HOLD_RIFLE,  D2PL_FIRE_AUTO,		6, 30, 120, 60, JER_MENU_SFX_ACCEPT, JER_MENU_SFX_BACK,
		"mg_fire", "mg_reload", 10 },
	{ "DEBUG",	D2PL_HOLD_PISTOL, D2PL_FIRE_EVERY_FRAME, 0, 0,  0,   0,  JER_MENU_SFX_ACCEPT, -1,
		"debug_fire", 0, 2 },
};

#define D2PL_WEAPON_COUNT ((int)(sizeof(gWeaponDefs) / sizeof(gWeaponDefs[0])))

static D2plWeaponState gWeapons[D2PL_WEAPON_COUNT];
static int gWeapon = 0;

int D2plActiveHoldtype(void)
{
	return gWeaponDefs[gWeapon].holdtype;
}

static int gMarkerTimer = 0;
static int gMarker[3] = { 0, 0, 0 };

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Debug/test lever: hold aim without a pad, for an unattended -frames run.
 * Seeded once from D2PL_TEST_AIM=1; the pause menu flips it at runtime. */
int gD2plTestAim = -1;
int gD2plSfxMute = 0;

static int D2plTestAim(void)
{
	if (gD2plTestAim < 0)
	{
		const char* env = getenv("D2PL_TEST_AIM");

		gD2plTestAim = (env != 0 && env[0] != 0 && env[0] != '0') ? 1 : 0;
	}

	return gD2plTestAim;
}

/* a mod-folder WAV when there is one, else the FE placeholder */
static void D2plSfx(const char* wav, int sfx)
{
	if (gD2plSfxMute)
		return;

	if (wav != 0 && jer_sound_mod_wav_play(D2PL_MOD_ID, wav))
		return;

	if (sfx >= 0)
		jer_sound_menu_play(sfx);
}

/* --- hit scan. Health lives in jer_npc (jer_npc_health / jer_npc_damage)
 * and deaths in d2pl_health.c (D2plDamage). */
#define D2PL_HIT_SLOTS	16
#define D2PL_HIT_RADIUS	120
#define D2PL_HIT_RANGE	8000

/* fire feedback, per weapon class: the kick a shot gives gD2plRecoil and
 * how fast it bleeds off per tick (/8). Pistol: a sharp one-shot pulse;
 * MG: a softer kick that each round refreshes while the trigger is held. */
#define D2PL_RECOIL_PISTOL	4096
#define D2PL_RECOIL_RIFLE	2600
#define D2PL_RECOIL_DECAY	5		/* keep 5/8 per tick: ~4 ticks visible */

int gD2plRecoil = 0;

/* impact FX: the car-collision sparks/debris plus a black smoke puff
 * (jer_fx_impact), pushed back along the shot */
static void D2plImpactFx(const int p[3], const int d[3], int len, int kind)
{
	int vx = 0, vz = 0;

	if (len > 0)
	{
		vx = -d[0] * 24 / len;
		vz = -d[2] * 24 / len;
	}

	jer_fx_impact(p[0], p[1], p[2], vx, vz, kind);
}

int gD2plHitCount = 0;
int gD2plLastHitHealth = -1;
int gD2plLastHitPos[3] = { 0, 0, 0 };

/* walk the shot from the hand toward the aim point; the first ped inside
 * D2PL_HIT_RADIUS takes the hit and the marker moves onto it */
static void D2plHitScan(const D2plWeaponDef* def, const int aim[3])
{
	const JER_HOST_API* api = jerHostApiGet();
	int from[3], d[3], len, full, steps, i;

	if (!gD2plHandValid)
		return;

	from[0] = gD2plHand[0];
	from[1] = gD2plHand[1];
	from[2] = gD2plHand[2];

	d[0] = aim[0] - from[0];
	d[1] = aim[1] - from[1];
	d[2] = aim[2] - from[2];
	len = (int)sqrtf((float)d[0] * d[0] + (float)d[1] * d[1] + (float)d[2] * d[2]);

	if (len < 1)
		return;

	full = len;

	if (len > D2PL_HIT_RANGE)
		len = D2PL_HIT_RANGE;

	steps = len / D2PL_HIT_RADIUS;

	for (i = 1; i <= steps; i++)
	{
		float t = (float)(i * D2PL_HIT_RADIUS) / (float)full;
		int p[3];
		JerNpc* n;

		p[0] = from[0] + (int)(d[0] * t);
		p[1] = from[1] + (int)(d[1] * t);
		p[2] = from[2] + (int)(d[2] * t);

		n = jer_npc_nearest(p[0], p[1], p[2], D2PL_HIT_RADIUS);

		if (n != 0)
		{
			int hp;

			gD2plAimedPed = (void*)n;
			hp = D2plDamage((void*)n, def->damage);

			gD2plHitCount++;
			gD2plLastHitHealth = hp;
			gD2plLastHitPos[0] = p[0];
			gD2plLastHitPos[1] = p[1];
			gD2plLastHitPos[2] = p[2];
			gMarker[0] = p[0];
			gMarker[1] = p[1];
			gMarker[2] = p[2];

			/* the target readout (d2pl_health.c) shows the health now;
			 * a death goes to the jer console */
			D2plImpactFx(p, d, full, JER_FX_IMPACT_PED);
			gMarkerTimer = 20;

			return;
		}

		/* the ground stops the shot: impact FX + the marker there.
		 * map_height is up-positive; the shot is in the render frame. */
		if (api != 0 && api->car != 0 && api->car->map_height != 0)
		{
			JER_VEC q;
			int ground;

			q.x = p[0];
			q.y = -p[1];
			q.z = p[2];
			q.pad = 0;

			ground = -api->car->map_height(&q);

			if (p[1] >= ground)
			{
				p[1] = ground - 8;
				gMarker[0] = p[0];
				gMarker[1] = p[1];
				gMarker[2] = p[2];
				gMarkerTimer = 20;

				D2plImpactFx(p, d, full, JER_FX_IMPACT_WORLD);
				return;
			}
		}
	}
}

static int D2plMainOnFoot(const JER_HOST_API* api, JER_PLAYER_STATE* ps)
{
	if (api == 0 || api->player == 0 || api->player->get_state == 0 ||
		api->player->main == 0)
		return 0;

	if (!api->player->get_state(api->player->main(), ps))
		return 0;

	return ps->playerCarId < 0;
}

static void D2plFinishReload(const D2plWeaponDef* def, D2plWeaponState* w)
{
	int need = def->clipSize - w->clip;

	if (w->reserve >= need)
	{
		w->reserve -= need;
		w->clip = def->clipSize;
	}
	else
	{
		w->clip += w->reserve;
		w->reserve = 0;
	}
}

static void D2plFireShot(const D2plWeaponDef* def, D2plWeaponState* w)
{
	int aim[3];

	if (def->clipSize > 0)
		w->clip--;

	D2plSfx(def->fireWav, def->fireSfx);

	/* fire feedback for the pose: pistol pulses to full, the rifle hold
	 * refreshes a softer kick each round */
	{
		int kick = def->holdtype == D2PL_HOLD_RIFLE ? D2PL_RECOIL_RIFLE : D2PL_RECOIL_PISTOL;

		if (gD2plRecoil < kick)
			gD2plRecoil = kick;
	}

	if (D2plAimPoint(aim))
	{
		gMarker[0] = aim[0];
		gMarker[1] = aim[1];
		gMarker[2] = aim[2];
		gMarkerTimer = 20;
		D2plHitScan(def, aim);
	}
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_FRAME — aim/fire/reload, laser, impact marker             */
/* ------------------------------------------------------------------ */

void D2plWeaponUpdate(void)
{
	const JER_HOST_API* api = jerHostApiGet();
	JER_PLAYER_STATE ps;
	int pads[2] = { 0, 0 };
	int held = 0;
	int pressed = 0;
	int aim[3];

	/* recoil bleeds off at the tick rate, firing or not */
	gD2plRecoil = gD2plRecoil * D2PL_RECOIL_DECAY / 8;

	if (gD2plRecoil < 64)
		gD2plRecoil = 0;

	if (!D2plMainOnFoot(api, &ps))
	{
		gD2plAiming = 0;
		D2plHealthTick(0);
		return;
	}

	D2plDrawPedTick();

	if (api->player->pad != 0 && ps.padid >= 0 && ps.padid <= 1)
	{
		int n = api->player->pad(ps.padid, pads, 2);

		if (n > 0)
			held = pads[0];
		if (n > 1)
			pressed = pads[1];
	}

	gD2plAiming = !D2plPlayerDead() && (D2plTestAim() || ((held & D2PL_MPAD_L1) != 0));

	{
		const D2plWeaponDef* def = &gWeaponDefs[gWeapon];
		D2plWeaponState* w = &gWeapons[gWeapon];
		int canFire;

		/* L2 press: next weapon */
		if ((pressed & D2PL_MPAD_CYCLE) != 0)
		{
			D2plWeaponCycle(1);
			def = &gWeaponDefs[gWeapon];
			w = &gWeapons[gWeapon];

			if (api->ui != 0 && api->ui->hud_message != 0)
				api->ui->hud_message(def->name, 45);
		}

		if (w->reloading > 0)
		{
			w->reloading--;

			if (w->reloading == 0)
				D2plFinishReload(def, w);
		}

		/* reload: keyboard O (press edge). Cross used to reload, but it is
		 * also the stock run/forward button on foot. */
		if ((D2plReloadKeyPressed() || (pressed & D2PL_MPAD_CIRCLE) != 0) && def->clipSize > 0 &&
			w->reloading == 0 && w->clip < def->clipSize && w->reserve > 0)
		{
			w->reloading = def->reloadFrames;

			D2plSfx(def->reloadWav, def->reloadSfx);
		}

		canFire = gD2plAiming && w->reloading == 0 &&
			(def->clipSize == 0 || w->clip > 0);

		switch (def->fireMode)
		{
			case D2PL_FIRE_SEMI:
				if (canFire && (pressed & D2PL_MPAD_R1) != 0)
					D2plFireShot(def, w);
				break;

			case D2PL_FIRE_AUTO:
				/* the first shot leaves on the press, then one every
				 * D2PL_TICK_HZ / roundsPerSec ticks, exactly on average */
				if (canFire && (held & D2PL_MPAD_R1) != 0)
				{
					if ((pressed & D2PL_MPAD_R1) != 0)
						w->credit = D2PL_TICK_HZ;
					else
						w->credit += def->roundsPerSec;

					if (w->credit >= D2PL_TICK_HZ)
					{
						w->credit -= D2PL_TICK_HZ;
						D2plFireShot(def, w);
					}
				}
				else
					w->credit = 0;
				break;

			case D2PL_FIRE_EVERY_FRAME:
				if (canFire && (held & D2PL_MPAD_R1) != 0)
					D2plFireShot(def, w);
				break;
		}
	}

	/* laser: hand -> aim point. The positions are in the render (Y-down)
	 * frame; the debug line API wants world Y-up. */
	if (D2PL_LASER_ENABLED && gD2plAiming && gD2plHandValid &&
		api->debug != 0 && api->debug->line_depth != 0 && D2plAimPoint(aim))
	{
		JER_VEC from;
		JER_VEC to;
		JER_CVEC col;

		col.r = 255;
		col.g = 0;
		col.b = 0;
		col.code = 0;

		from.x = gD2plHand[0];
		from.y = -gD2plHand[1];
		from.z = gD2plHand[2];
		from.pad = 0;

		to.x = aim[0];
		to.y = -aim[1];
		to.z = aim[2];
		to.pad = 0;

		api->debug->line_depth(&from, &to, &col);
	}

	D2plHealthTick(1);

	/* impact marker: a small black X for a few frames (alongside the
	 * jer_fx_impact sparks + black puff on a ped or ground hit) */
	if (gMarkerTimer > 0)
	{
		JER_VEC c;
		JER_VEC p;
		int size = 40;

		if (api->debug != 0 && api->debug->line != 0)
		{
			c.x = gMarker[0];
			c.y = -gMarker[1];
			c.z = gMarker[2];
			c.pad = 0;

			p = c;
			p.x -= size;
			p.y -= size;
			api->debug->line(&p, &c, 0x000000);

			p = c;
			p.x += size;
			p.y -= size;
			api->debug->line(&p, &c, 0x000000);

			p = c;
			p.x -= size;
			p.y += size;
			api->debug->line(&p, &c, 0x000000);

			p = c;
			p.x += size;
			p.y += size;
			api->debug->line(&p, &c, 0x000000);
		}

		gMarkerTimer--;
	}
}

void D2plSfxFire(void)
{
	D2plSfx(gWeaponDefs[0].fireWav, gWeaponDefs[0].fireSfx);
}

/* Pause-menu readouts and the L2 cycle (a reload in progress is dropped). */
const char* D2plWeaponName(void)
{
	return gWeaponDefs[gWeapon].name;
}

void D2plWeaponAmmoText(char* out, int max)
{
	const D2plWeaponState* w = &gWeapons[gWeapon];

	if (gWeaponDefs[gWeapon].clipSize == 0)
		snprintf(out, max, "endless");
	else if (w->reloading > 0)
		snprintf(out, max, "reloading");
	else
		snprintf(out, max, "%d / %d", w->clip, w->reserve);
}

void D2plWeaponCycle(int dir)
{
	gWeapons[gWeapon].reloading = 0;
	gWeapon = (gWeapon + (dir < 0 ? D2PL_WEAPON_COUNT - 1 : 1)) % D2PL_WEAPON_COUNT;
	gWeapons[gWeapon].credit = 0;
}

/* ------------------------------------------------------------------ */
/* JER_EVENT_DRAW_OVERLAY — ammo readout                               */
/* ------------------------------------------------------------------ */

int D2plOnOverlay(void* userdata, void* args)
{
	const JER_HOST_API* api = jerHostApiGet();
	JER_PLAYER_STATE ps;
	char text[48];

	(void)userdata;
	(void)args;

	if (!D2plMainOnFoot(api, &ps))
		return JER_RESULT_CONTINUE;

	if (api->ui == 0 || api->ui->text_draw_at == 0)
		return JER_RESULT_CONTINUE;

	/* lower-left, small: weapon, ammo, last hit (world, y up) */
	{
		char ammo[24];

		D2plWeaponAmmoText(ammo, sizeof(ammo));
		snprintf(text, sizeof(text), "%s  %s", gWeaponDefs[gWeapon].name, ammo);
		api->ui->text_draw_at(8, 196, text, 0xFFFFFF, 1);

		if (gD2plLastHitHealth < 0)
			snprintf(text, sizeof(text), "HIT  none");
		else
			snprintf(text, sizeof(text), "HIT  %d %d %d  %d%%",
				gD2plLastHitPos[0], -gD2plLastHitPos[1], gD2plLastHitPos[2],
				gD2plLastHitHealth * 100 / D2PL_PED_HEALTH);
		api->ui->text_draw_at(8, 208, text, 0xC0C0C0, 1);
	}

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void D2plWeaponInit(void)
{
	int i;

	for (i = 0; i < D2PL_WEAPON_COUNT; i++)
	{
		gWeapons[i].clip = gWeaponDefs[i].clipSize;
		gWeapons[i].reserve = gWeaponDefs[i].reserve;
		gWeapons[i].credit = 0;
		gWeapons[i].reloading = 0;
	}

	gWeapon = 0;
	gMarkerTimer = 0;
	gD2plRecoil = 0;
	gMarker[0] = gMarker[1] = gMarker[2] = 0;

	/* placeholder SFX: keep the FE menu samples resident in game */
	jer_sound_request_menu_bank();
}
