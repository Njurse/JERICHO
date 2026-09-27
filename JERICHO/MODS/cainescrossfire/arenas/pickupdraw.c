// arenas/pickupdraw.c — the pickup ICONS, and the world draw that puts them on
// spinning, floating planes.
//
// Two halves:
//
//   1. THE ICON TABLE. A pickup names a weapon (CD2_WID_*) or is health, and that maps
//      to an asset in the mod's textures/icons/ folder (see jer_texture.h and
//      tools/icons.py). The mapping is explicit and greppable rather than derived from
//      a display name, because a display name is prose and may change.
//
//   2. THE PRESENTATION. An active pickup is drawn as an upright card that spins about
//      the world vertical and bobs up and down — the Twisted-Metal-style pickup. The
//      STEP (proximity + collection) stays in arena.c's frame loop; only the DRAW moves
//      here, because a draw has to happen inside the render pass (JER_EVENT_DRAW_WORLD)
//      and not during the sim.
//
// A missing icon is not fatal: the pickup falls back to arena.c's line marker, so a mod
// with no art still plays.

#include "driver2.h"
#include "cainescrossfire.h"
#include "main.h"			/* FrameCnt - the frame clock the animation runs on */
#include "camera.h"
#include "cars.h"
#include "profile.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_texture.h"
#include "weapons/core/weapon.h"

#include <stdlib.h>

#define CD2_MOD_ID	"cainescrossfire"

// Where a card's CENTRE floats above the ground plane the marker stands on. arena.c's
// bar is CD2_PICKUP_HEIGHT tall, so this sits the icon in the upper half of where that
// bar was - the pickup reads at the same height it always did.
#define CD2_PICKUP_CARD_Y	110

// The bob's rate, PSX angle units per frame (the spin rate is per-arena). 24 gives a
// slow float; a bigger number bobs faster.
#define CD2_PICKUP_BOB_RATE	24

// ---------------------------------------------------------------------------
// The icon table
// ---------------------------------------------------------------------------

typedef struct CD2_PICKUP_ICON
{
	int wid;		// CD2_WID_*
	const char* name;	// asset under textures/icons/, without the extension
} CD2_PICKUP_ICON;

// CD2_WID_* -> icon name. One line per weapon; the asset is icons/<name>.tga and the
// generator in tools/icons.py writes the matching filename (wid_<name>.tga).
static const CD2_PICKUP_ICON cd2PickupIcons[] =
{
	{ CD2_WID_MG,			"wid_mg" },
	{ CD2_WID_MISSILE,		"wid_missile" },
	{ CD2_WID_MINE,			"wid_mine" },
	{ CD2_WID_HOMING,		"wid_homing" },
	{ CD2_WID_CLUSTER,		"wid_cluster" },
	{ CD2_WID_ZOOMY,		"wid_zoomy" },
	{ CD2_WID_FREEZE,		"wid_freeze" },
	{ CD2_WID_SHOTGUN,		"wid_shotgun" },
	{ CD2_WID_SMG,			"wid_smg" },
	{ CD2_WID_SPECIAL_HORNET,	"wid_special_hornet" },
	{ CD2_WID_SPECIAL_AVALANCHE,	"wid_special_avalanche" },
	{ CD2_WID_SPECIAL_CORVO,	"wid_special_corvo" },
	{ CD2_WID_SPECIAL_BRUXA,	"wid_special_bruxa" },
	{ CD2_WID_SPECIAL_HIGHWAYMAN,	"wid_special_highwayman" },
	{ CD2_WID_SPECIAL_DEADSTAR,	"wid_special_deadstar" },
	{ CD2_WID_SPECIAL_OBELISK,	"wid_special_obelisk" },
	{ CD2_WID_SPECIAL_BOOTLEGGER,	"wid_special_bootlegger" },
	{ CD2_WID_SPECIAL_INVOCADA,	"wid_special_invocada" },
	{ CD2_WID_SPECIAL_FIXER,	"wid_special_fixer" },
	{ CD2_WID_SPECIAL_WHEELMAN,	"wid_special_wheelman" },
};

#define CD2_PICKUP_ICON_COUNT	(int)(sizeof(cd2PickupIcons) / sizeof(cd2PickupIcons[0]))

// The asset name for a weapon, or NULL when it has none (a weapon added to CD2_WID_*
// without an icon here simply falls back to the line marker).
const char* cd2PickupIconName(int wid)
{
	int i;

	for (i = 0; i < CD2_PICKUP_ICON_COUNT; i++)
	{
		if (cd2PickupIcons[i].wid == wid)
			return cd2PickupIcons[i].name;
	}

	return NULL;
}

int cd2PickupIconCount(void)
{
	return CD2_PICKUP_ICON_COUNT;
}

// ---------------------------------------------------------------------------
// The loaded icons
// ---------------------------------------------------------------------------

static JER_TEXTURE gIconHealth = JER_TEX_NONE;
static JER_TEXTURE gIconWeapon[CD2_WID_COUNT];
static int gIconsLoaded;

// The handle for a pickup's icon, or JER_TEX_NONE when nothing is authored (the
// caller draws its line marker then). Kept as one entry point so the draw code never
// indexes the table itself - a weapon with no icon simply falls back.
JER_TEXTURE cd2PickupIconFor(int type, int weapon)
{
	if (type == CD2_PICKUP_HEALTH)
		return gIconHealth;

	if (weapon >= 0 && weapon < CD2_WID_COUNT)
		return gIconWeapon[weapon];

	return JER_TEX_NONE;
}

int cd2PickupIconsLoaded(void)
{
	return gIconsLoaded;
}

// Load every icon once. Registered on JER_EVENT_BOOT: art is resident well before the
// first level, and jer_texture re-reads it from disk on GAME_START, so an icon edited
// between runs is picked up without a rebuild. A missing file is not an error - that
// pickup just draws the line marker.
static int cd2PickupIconsLoad(void* ud, void* args)
{
	int i, want = 1 + CD2_PICKUP_ICON_COUNT, got = 0;

	(void)ud;
	(void)args;

	for (i = 0; i < CD2_WID_COUNT; i++)
		gIconWeapon[i] = JER_TEX_NONE;

	gIconHealth = jer_texture_load(CD2_MOD_ID, "icons/health");

	if (gIconHealth != JER_TEX_NONE)
		got++;

	for (i = 0; i < CD2_PICKUP_ICON_COUNT; i++)
	{
		char path[64];
		int wid = cd2PickupIcons[i].wid;
		JER_TEXTURE tex;

		snprintf(path, sizeof(path), "icons/%s", cd2PickupIcons[i].name);
		tex = jer_texture_load(CD2_MOD_ID, path);

		if (wid >= 0 && wid < CD2_WID_COUNT)
			gIconWeapon[wid] = tex;

		if (tex != JER_TEX_NONE)
			got++;
	}

	gIconsLoaded = got;

	printInfo("[pickups] icons: %d/%d loaded (a missing one falls back to the line marker)\n",
		got, want);

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// The presentation, drawn mid-render with the camera matrices live. One card per live
// pickup: upright, spinning about the world vertical, bobbing on a sine, its icon from
// the mod's textures/icons/. A pickup with no icon keeps arena.c's line marker, so a
// mod with no art still plays.
static int cd2PickupDrawWorld(void* ud, void* args)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	int i;

	(void)ud;
	(void)args;

	if (a == NULL)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < a->pickupCount && i < CD2_ARENA_MAX_PICKUPS; i++)
	{
		const CD2_ARENA_PICKUP* p = &a->pickups[i];
		JER_TEXTURE icon;
		int spin, bob, y;

		if (!cd2ArenaPickupActive(i))
			continue;

		icon = cd2PickupIconFor(p->type, p->weapon);

		if (icon == JER_TEX_NONE)
		{
			cd2ArenaDrawPickupMarker(p);
			continue;
		}

		// A per-pickup phase offset, so a row of pickups does not turn and bob in
		// lockstep (i * 683 is an arbitrary spread around the 4096-unit circle).
		spin = (FrameCnt * a->pickupSpin + i * 683) & 4095;
		bob = (a->pickupBob > 0) ? (a->pickupBob * RSIN(FrameCnt * CD2_PICKUP_BOB_RATE + i * 512)) / ONE : 0;

		// The card floats with its centre above the ground plane the marker stands on
		// (CD2_PICKUP_HEIGHT is that bar's top), and the raw world frame is Y-UP here -
		// jer_texture applies the engine's flip internally, so a bigger y is higher.
		y = CD2_PICKUP_CARD_Y + bob;

		// NO_OCCLUDE, deliberately: a pickup is a thing the player must be able to see,
		// and its card sits at the same depth as the ground it stands on, so a normal
		// depth sort lets the road draw over it. The line marker these replace was drawn
		// through the overlay path, i.e. always on top too, so this keeps the behaviour
		// the arenas already had.
		jer_texture_draw_card(icon, p->x, y, p->z,
			a->pickupSize, a->pickupSize, spin, JER_TEX_DRAW_NO_OCCLUDE);
	}

	return JER_RESULT_CONTINUE;
}

void cd2PickupDrawRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, cd2PickupIconsLoad, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, cd2PickupDrawWorld, NULL, 0);
}
