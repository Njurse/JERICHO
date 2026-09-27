// texprobe.c — TEMPORARY consumer of the jer_texture API.
//
//   *** DELETE THIS FILE when the pickup work lands. ***
//
// Phase-1 proved the raw mechanism (DR_PSYX_TEX + a projected quad). This file now
// exercises the REAL API end to end instead: it loads the mod's test icon through
// jer_texture_load (asset convention + registry + TGA decode + GPU upload) and draws
// it as a spinning, floating card through jer_texture_draw_card. Opt-in with
// JERICHO_TEXPROBE=1 so an ordinary run is untouched.

#include "driver2.h"
#include "cainescrossfire.h"
#include "cars.h"
#include "players.h"
#include "camera.h"
#include "system.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_texture.h"
#include "weapons/core/weapon_internal.h"	/* cd2WpnPlayerCar */

#include <stdlib.h>

#define CD2_SPIKE_HALF_W	150		/* card half-width, world units */
#define CD2_SPIKE_HALF_H	150
#define CD2_SPIKE_LIFT		200		/* raw world-frame lift (UP = +) */
#define CD2_SPIKE_SPIN		48		/* PSX angle units per frame */
#define CD2_SPIKE_NAME		"icons/test64"

static int gSpikeOn = 0;
static int gSpikeFrame = 0;
static JER_TEXTURE gSpikeTex = JER_TEX_NONE;

// Load at BOOT so the texture is resident BEFORE the level's GAME_START fires: that is
// what makes jer_texture's reload-on-GAME_START observable (and it is also the natural
// place a module would load its art).
static int cd2SpikeOnBoot(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (!gSpikeOn)
		return JER_RESULT_CONTINUE;

	gSpikeTex = jer_texture_load("cainescrossfire", CD2_SPIKE_NAME);

	if (gSpikeTex != JER_TEX_NONE)
	{
		int w = 0, h = 0;

		jer_texture_size(gSpikeTex, &w, &h);
		printInfo("[texprobe] loaded at BOOT: handle=%u size=%dx%d target=%d\n",
			(unsigned)gSpikeTex, w, h, jer_texture_target(gSpikeTex));
	}

	return JER_RESULT_CONTINUE;
}

static int cd2SpikeDrawWorld(void* ud, void* args)
{
	CAR_DATA* cp = NULL;

	(void)ud;
	(void)args;

	if (!gSpikeOn)
		return JER_RESULT_CONTINUE;

	// fallback for a run that never fired BOOT
	if (gSpikeTex == JER_TEX_NONE)
		gSpikeTex = jer_texture_load("cainescrossfire", CD2_SPIKE_NAME);

	if (gSpikeTex == JER_TEX_NONE || !cd2WpnPlayerCar(&cp))
		return JER_RESULT_CONTINUE;

	gSpikeFrame++;

	// Exercise the free/reload path once: freeing must invalidate the handle without
	// crashing, and re-loading must hand back a NEW generation (so a stale handle can
	// never alias the new texture).
	if (gSpikeFrame == 40)
	{
		printInfo("[texprobe] freeing handle=%u\n", (unsigned)gSpikeTex);

		jer_texture_free(gSpikeTex);
		printInfo("[texprobe] after free: valid=%d\n", jer_texture_valid(gSpikeTex));

		gSpikeTex = jer_texture_load("cainescrossfire", CD2_SPIKE_NAME);
		printInfo("[texprobe] re-loaded handle=%u valid=%d\n",
			(unsigned)gSpikeTex, jer_texture_valid(gSpikeTex));
	}

	// Draw it floating above the car, spinning. The card must be two-sided, and the
	// API mirrors the icon automatically when the card shows its back.
	jer_texture_draw_card(gSpikeTex,
		cp->hd.where.t[0],
		cp->hd.where.t[1] + CD2_SPIKE_LIFT,
		cp->hd.where.t[2],
		CD2_SPIKE_HALF_W, CD2_SPIKE_HALF_H,
		(gSpikeFrame * CD2_SPIKE_SPIN) & 4095,
		JER_TEX_DRAW_NONE);

	if (gSpikeFrame == 1 || (gSpikeFrame % 60) == 0)
		printInfo("[texprobe] f=%d drew card at car (%d,%d,%d)\n",
			gSpikeFrame, cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2]);

	return JER_RESULT_CONTINUE;
}

void cd2TexProbeRegister(JERICHO_CONTEXT* ctx)
{
	if (getenv("JERICHO_TEXPROBE") == NULL)
		return;

	gSpikeOn = 1;
	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, cd2SpikeOnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, cd2SpikeDrawWorld, NULL, 0);
	ctx->jer_log(ctx, "[texprobe] active: drawing %s via jer_texture\n", CD2_SPIKE_NAME);
}
