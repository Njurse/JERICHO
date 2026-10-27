/*
 * d2pl_menu.c -- the D2PL ON FOOT page under Pause > JERICHO Addons.
 *
 * Read-outs first (what the mod is doing right now), then the few runtime
 * switches, then a controls card. Labels stay short so nothing is cut off at
 * the pause screen's 48-character row. Rows with no action are read-only.
 */

#include "d2pl.h"
#include "jer_pause_menu.h"

#include <stdio.h>

#define D2PL_DEG(a)	(((a) * 360 + 2048) / 4096)

static const char* OnOff(int v)
{
	return v ? "ON" : "OFF";
}

/* --- status ------------------------------------------------------- */

static void LblWeapon(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Weapon: < %s >", D2plWeaponName());
}

static int ActWeapon(void* ud, int dir)
{
	(void)ud;
	D2plWeaponCycle(dir < 0 ? -1 : 1);
	return JER_PAUSE_QUIT_NONE;
}

static void LblAmmo(void* ud, char* out, int max)
{
	char ammo[24];

	(void)ud;
	D2plWeaponAmmoText(ammo, sizeof(ammo));
	snprintf(out, max, "Ammo: %s", ammo);
}

static void LblHold(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Holdtype: %s", D2plHoldtypeGet(D2plActiveHoldtype())->name);
}

static void LblAiming(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Aiming: %s", OnOff(gD2plAiming));
}

static void LblTwist(void* ud, char* out, int max)
{
	const D2plHoldtype* h = D2plHoldtypeGet(D2plActiveHoldtype());

	(void)ud;
	snprintf(out, max, "Twist limit: torso %d, head %d deg",
		D2PL_DEG(h->torsoLimit), D2PL_DEG(h->headLimit));
}

/* --- settings ----------------------------------------------------- */

static void LblBackpedal(void* ud, char* out, int max)
{
	const char* state;

	(void)ud;

	if (!gD2plBackpedalOn)
		state = "OFF";
	else if (!D2plHoldtypeGet(D2plActiveHoldtype())->allowBackpedal)
		state = "ON (not this hold)";
	else if (gD2plBackpedalActive)
		state = "ON (backing up)";
	else
		state = "ON";

	snprintf(out, max, "Backpedal: %s", state);
}

static int ActBackpedal(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gD2plBackpedalOn = !gD2plBackpedalOn;
	return JER_PAUSE_QUIT_NONE;
}

static void LblBackAngle(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Backpedal past: %d deg off view", D2PL_DEG(D2PL_BACKPEDAL_ANGLE));
}

static void LblSfx(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Gun SFX: %s", gD2plSfxMute ? "MUTED" : "ON");
}

static int ActSfx(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gD2plSfxMute = !gD2plSfxMute;
	return JER_PAUSE_QUIT_NONE;
}

static void LblTestAim(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Test aim (D2PL_TEST_AIM): %s", OnOff(gD2plTestAim > 0));
}

static int ActTestAim(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gD2plTestAim = gD2plTestAim > 0 ? 0 : 1;
	return JER_PAUSE_QUIT_NONE;
}

/* --- cards -------------------------------------------------------- */

static const JER_PAUSE_MENU_ITEM gControlsItems[] =
{
	{ "L1: aim",                  NULL, NULL, NULL, NULL, 0 },
	{ "R1: fire",                 NULL, NULL, NULL, NULL, 0 },
	{ "X (tap): reload",          NULL, NULL, NULL, NULL, 0 },
	{ "R2: next weapon",          NULL, NULL, NULL, NULL, 0 },
	{ "Aim + stick back: backpedal", NULL, NULL, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU gControlsMenu =
{
	"Controls",
	gControlsItems,
	(int)(sizeof(gControlsItems) / sizeof(gControlsItems[0])),
	"d2pl.controls"
};

static const JER_PAUSE_MENU_ITEM gSfxItems[] =
{
	{ "Placeholder sounds until gun SFX", NULL, NULL, NULL, NULL, 0 },
	{ "Fire: FE accept",          NULL, NULL, NULL, NULL, 0 },
	{ "Reload: FE back",          NULL, NULL, NULL, NULL, 0 },
	{ NULL, LblSfx, ActSfx, NULL, NULL, 1 },
};

static const JER_PAUSE_MENU gSfxMenu =
{
	"Gun sounds",
	gSfxItems,
	(int)(sizeof(gSfxItems) / sizeof(gSfxItems[0])),
	"d2pl.sfx"
};

static const JER_PAUSE_MENU_ITEM gMainItems[] =
{
	{ NULL, LblWeapon,    ActWeapon,    NULL, NULL, 1 },
	{ NULL, LblAmmo,      NULL,         NULL, NULL, 0 },
	{ NULL, LblHold,      NULL,         NULL, NULL, 0 },
	{ NULL, LblAiming,    NULL,         NULL, NULL, 0 },
	{ NULL, LblTwist,     NULL,         NULL, NULL, 0 },
	{ NULL, LblBackpedal, ActBackpedal, NULL, NULL, 1 },
	{ NULL, LblBackAngle, NULL,         NULL, NULL, 0 },
	{ NULL, LblTestAim,   ActTestAim,   NULL, NULL, 1 },
	{ "Gun sounds",       NULL, NULL,   NULL, &gSfxMenu, 0 },
	{ "Controls",         NULL, NULL,   NULL, &gControlsMenu, 0 },
};

static const JER_PAUSE_MENU gMainMenu =
{
	"D2PL ON FOOT " D2PL_VERSION,
	gMainItems,
	(int)(sizeof(gMainItems) / sizeof(gMainItems[0])),
	"d2pl.main"
};

void D2plMenuRegister(void)
{
	const JER_HOST_API* api = jerHostApiGet();

	if (api != NULL && api->ui != NULL && api->ui->pause_menu_register != NULL)
		api->ui->pause_menu_register(&gMainMenu);
}
