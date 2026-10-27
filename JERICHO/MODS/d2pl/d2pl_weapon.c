/* d2pl_weapon.c — the baked-in debug pistol, its laser and the HUD.
 *
 * This is deliberately a *debug* weapon: 12 rounds, semi-auto, no damage
 * model and no hand mesh (the shipped data has no pistol MODEL, and the
 * addon cannot call the game's model renderer through the host API). It
 * exists to exercise the aim pose and the laser sight:
 *
 *   L1          aim
 *   R1          fire (semi-auto, while aiming)
 *   X (Cross)   reload
 *
 * The laser is the d2pl-style depth-sorted line from the right hand to the
 * camera-centre aim point; the debug X marks the last shot.
 */

#include "d2pl.h"
#include "jer_math.h"

#include <stdio.h>
#include <stdlib.h>

#define D2PL_LASER_ENABLED	1

typedef struct D2plPistol
{
	int clip;
	int reserve;
	int clipSize;
	int fireRate;		/* frames between shots */
	int reloadTime;		/* frames a reload takes */
	int cooldown;
	int reloading;
} D2plPistol;

static D2plPistol gPistol = { 12, 60, 12, 6, 45, 0, 0 };

static int gMarkerTimer = 0;
static int gMarker[3] = { 0, 0, 0 };

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Debug/test lever: hold aim without a pad, for an unattended -frames run.
 * Set D2PL_TEST_AIM=1. */
static int D2plTestAim(void)
{
	static int forced = -1;

	if (forced < 0)
	{
		const char* env = getenv("D2PL_TEST_AIM");

		forced = (env != 0 && env[0] != 0 && env[0] != '0') ? 1 : 0;
	}

	return forced;
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

static void D2plFinishReload(void)
{
	int need = gPistol.clipSize - gPistol.clip;

	if (gPistol.reserve >= need)
	{
		gPistol.reserve -= need;
		gPistol.clip = gPistol.clipSize;
	}
	else
	{
		gPistol.clip += gPistol.reserve;
		gPistol.reserve = 0;
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

	if (!D2plMainOnFoot(api, &ps))
	{
		gD2plAiming = 0;
		return;
	}

	if (api->player->pad != 0 && ps.padid >= 0 && ps.padid <= 1)
	{
		int n = api->player->pad(ps.padid, pads, 2);

		if (n > 0)
			held = pads[0];
		if (n > 1)
			pressed = pads[1];
	}

	gD2plAiming = D2plTestAim() || ((held & D2PL_MPAD_L1) != 0);

	if (gPistol.cooldown > 0)
		gPistol.cooldown--;

	if (gPistol.reloading > 0)
	{
		gPistol.reloading--;

		if (gPistol.reloading == 0)
			D2plFinishReload();
	}

	/* reload: X is the stock run button on foot, so use only its press
	 * edge and leave the held bit to the engine's movement */
	if ((pressed & D2PL_MPAD_CROSS) != 0 && gPistol.reloading == 0 &&
		gPistol.clip < gPistol.clipSize && gPistol.reserve > 0)
	{
		gPistol.reloading = gPistol.reloadTime;
	}

	if (gD2plAiming && (pressed & D2PL_MPAD_R1) != 0 &&
		gPistol.cooldown == 0 && gPistol.reloading == 0 && gPistol.clip > 0)
	{
		gPistol.clip--;
		gPistol.cooldown = gPistol.fireRate;

		if (D2plAimPoint(aim))
		{
			gMarker[0] = aim[0];
			gMarker[1] = aim[1];
			gMarker[2] = aim[2];
			gMarkerTimer = 20;
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

	/* impact marker: a small black X for a few frames */
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

/* ------------------------------------------------------------------ */
/* JER_EVENT_DRAW_OVERLAY — ammo readout                               */
/* ------------------------------------------------------------------ */

int D2plOnOverlay(void* userdata, void* args)
{
	const JER_HOST_API* api = jerHostApiGet();
	JER_PLAYER_STATE ps;
	char text[32];

	(void)userdata;
	(void)args;

	if (!D2plMainOnFoot(api, &ps))
		return JER_RESULT_CONTINUE;

	if (api->ui == 0 || api->ui->text_draw_at == 0)
		return JER_RESULT_CONTINUE;

	sprintf(text, "[%d/%d]", gPistol.clip, gPistol.reserve);
	api->ui->text_draw_at(20, 210, text, 0xFFFFFF, 1);

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void D2plWeaponInit(void)
{
	gPistol.clip = gPistol.clipSize;
	gPistol.reserve = 60;
	gPistol.cooldown = 0;
	gPistol.reloading = 0;

	gMarkerTimer = 0;
	gMarker[0] = gMarker[1] = gMarker[2] = 0;
}
