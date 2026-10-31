/*
 * d2pl_menu.c -- the D2PL ON FOOT page under Pause > JERICHO Addons.
 *
 * Read-outs first (what the mod is doing right now), then the few runtime
 * switches, then a controls card. Labels stay short so nothing is cut off at
 * the pause screen's 48-character row. Rows with no action are read-only.
 */

#include "d2pl.h"
#include "jer_pause_menu.h"
#include "jer_math.h"

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
	snprintf(out, max, "Torso twist: < %d deg >", D2PL_DEG(h->torsoLimit));
}

/* left/right: +-5 degrees on the EQUIPPED weapon's holdtype, 0..135 */
static int ActTwist(void* ud, int dir)
{
	D2plHoldtype* h = D2plHoldtypeEdit(D2plActiveHoldtype());

	(void)ud;

	if (h != 0 && h->mask != 0)
		h->torsoLimit = jer_clamp_int(h->torsoLimit + (dir < 0 ? -57 : 57), 0, 1536);

	return JER_PAUSE_QUIT_NONE;
}

static void LblHeadPitch(void* ud, char* out, int max)
{
	const D2plHoldtype* h = D2plHoldtypeGet(D2plActiveHoldtype());

	(void)ud;
	snprintf(out, max, "Head %d, lean %d deg", D2PL_DEG(h->headLimit), D2PL_DEG(h->pitchLimit));
}

static void LblHits(void* ud, char* out, int max)
{
	(void)ud;

	if (gD2plLastHitHealth < 0)
		snprintf(out, max, "Hits: none yet");
	else
		snprintf(out, max, "Hits: %d, last ped %d%%", gD2plHitCount,
			gD2plLastHitHealth * 100 / D2PL_PED_HEALTH);
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
	snprintf(out, max, "Backpedal cone: < %d deg >", D2PL_DEG(gD2plBackpedalAngle));
}

/* left/right: +-5 degrees, 45..170 */
static int ActBackAngle(void* ud, int dir)
{
	(void)ud;
	gD2plBackpedalAngle = jer_clamp_int(gD2plBackpedalAngle + (dir < 0 ? -57 : 57), 512, 1934);
	return JER_PAUSE_QUIT_NONE;
}

static int ActReset(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	D2plSettingsReset();
	return JER_PAUSE_QUIT_NONE;
}

/* the built-in holdtypes are re-registered (a module-added holdtype would
 * need registering again; d2pl has none). */
void D2plSettingsReset(void)
{
	D2plHoldtypeInit();
	gD2plBackpedalOn = D2PL_BACKPEDAL;
	gD2plBackpedalAngle = D2PL_BACKPEDAL_ANGLE;
	gD2plSfxMute = 0;
	gD2plTestAim = -1;		/* re-read D2PL_TEST_AIM on the next frame */
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

/* --- test ped (debug) -------------------------------------------- */

static int ActTpSpawn(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	D2plTestPedSpawn();
	return JER_PAUSE_QUIT_NONE;
}

static int ActTpDespawn(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	D2plTestPedDespawn();
	return JER_PAUSE_QUIT_NONE;
}

static void LblTpBehaviour(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Behaviour: < %s >", D2plTestPedBehaviourName());
}

static int ActTpBehaviour(void* ud, int dir)
{
	(void)ud;
	D2plTestPedSetBehaviour(D2plTestPedBehaviour() + (dir < 0 ? -1 : 1));
	return JER_PAUSE_QUIT_NONE;
}

static void LblTpHealth(void* ud, char* out, int max)
{
	int hp = D2plTestPedHealth();

	(void)ud;

	if (hp < 0)
		snprintf(out, max, "Test ped: none");
	else
		snprintf(out, max, "Test ped HP: %d / %d", hp, D2PL_PED_HEALTH);
}

static const JER_PAUSE_MENU_ITEM gTestPedItems[] =
{
	{ "Spawn / respawn (Tanner)", NULL, ActTpSpawn,   NULL, NULL, 0 },
	{ NULL, LblTpBehaviour,       ActTpBehaviour,     NULL, NULL, 1 },
	{ NULL, LblTpHealth,          NULL,               NULL, NULL, 0 },
	{ "Despawn",                  NULL, ActTpDespawn, NULL, NULL, 0 },
	{ "Env: D2PL_TEST_PED=stand|follow|attack", NULL, NULL, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU gTestPedMenu =
{
	"Test ped (debug)",
	gTestPedItems,
	(int)(sizeof(gTestPedItems) / sizeof(gTestPedItems[0])),
	"d2pl.testped"
};

/* --- cards -------------------------------------------------------- */

static const JER_PAUSE_MENU_ITEM gControlsItems[] =
{
	{ "L1: aim",                  NULL, NULL, NULL, NULL, 0 },
	{ "R1: fire",                 NULL, NULL, NULL, NULL, 0 },
	{ "O key: reload",            NULL, NULL, NULL, NULL, 0 },
	{ "L2: next weapon",          NULL, NULL, NULL, NULL, 0 },
	{ "Stick / arrows back: backpedal", NULL, NULL, NULL, NULL, 0 },
	{ "On foot only; in a car the shoulders are stock", NULL, NULL, NULL, NULL, 0 },
	{ "Mouse aim/fire, wheel swap and rebinding: later", NULL, NULL, NULL, NULL, 0 },
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
	{ "WAVs: MODS/d2pl/sounds/",  NULL, NULL, NULL, NULL, 0 },
	{ "No WAV: fire = FE accept", NULL, NULL, NULL, NULL, 0 },
	{ "No WAV: reload = FE back", NULL, NULL, NULL, NULL, 0 },
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
	{ NULL, LblHits,      NULL,         NULL, NULL, 0 },
	{ NULL, LblTwist,     ActTwist,     NULL, NULL, 1 },
	{ NULL, LblHeadPitch, NULL,         NULL, NULL, 0 },
	{ NULL, LblBackpedal, ActBackpedal, NULL, NULL, 1 },
	{ NULL, LblBackAngle, ActBackAngle, NULL, NULL, 1 },
	{ NULL, LblTestAim,   ActTestAim,   NULL, NULL, 1 },
	{ "Test ped (debug)", NULL, NULL,   NULL, &gTestPedMenu, 0 },
	{ "Gun sounds",       NULL, NULL,   NULL, &gSfxMenu, 0 },
	{ "Controls",         NULL, NULL,   NULL, &gControlsMenu, 0 },
	{ "Reset to defaults", NULL, ActReset, NULL, NULL, 0 },
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
