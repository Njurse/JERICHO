// combatd2menu.c — Combat D2: the pause menu.
//
// (Split out of combatd2.c; see combatd2_internal.h for the file map.)
//
// The one-page JERICHO pause menu: the handling tuners (top speed / accel /
// brake / handling / grip, presets, Tight Turn), the weapons + AI tuning, the
// debug views, and "Total Car". Writes go straight into gCd2Cfg and are saved
// through cd2SaveConfig.

#include "driver2.h"
#include "combatd2.h"
#include "combatd2_internal.h"
#include "cars.h"
#include "players.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_hud.h"
#include "jer_pause_menu.h"
#include "weapons/core/weapon.h"
#include "ai/ai.h"
#include "jer_math.h"
#include <stdio.h>

// Option-name tables for the label callbacks (only the menu needs these).
static const char* const kPresetNames[] = { "Default", "Turbo", "Drifty", "Custom" };
static const char* const kTightInputNames[] = { "Handbrake", "Wheelspin", "Off" };

// ---------------------------------------------------------------------------
// Pause menu (single page)
// ---------------------------------------------------------------------------

static void cd2LabelEnabled(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Enabled: %s", gCd2Cfg.enabled ? "ON" : "OFF"); }
static int  cd2ToggleEnabled(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.enabled = !gCd2Cfg.enabled; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTopSpeed(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Top Speed: %d", gCd2Cfg.topSpeed); }
static int  cd2AdjTopSpeed(void* ud, int dir) { (void)ud; gCd2Cfg.topSpeed = jer_clamp_int(gCd2Cfg.topSpeed + dir * 10, 60, 400); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelAccel(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Acceleration: %d", gCd2Cfg.accel); }
static int  cd2AdjAccel(void* ud, int dir) { (void)ud; gCd2Cfg.accel = jer_clamp_int(gCd2Cfg.accel + dir, 1, 24); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelBrake(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Braking: %d", gCd2Cfg.brake); }
static int  cd2AdjBrake(void* ud, int dir) { (void)ud; gCd2Cfg.brake = jer_clamp_int(gCd2Cfg.brake + dir, 2, 30); cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelHandling(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Handling: %d", gCd2Cfg.handling); }
static int  cd2AdjHandling(void* ud, int dir) { (void)ud; gCd2Cfg.handling = jer_clamp_int(gCd2Cfg.handling + dir * 5, 10, 120); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelGrip(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Grip: %d", gCd2Cfg.grip); }
static int  cd2AdjGrip(void* ud, int dir) { (void)ud; gCd2Cfg.grip = jer_clamp_int(gCd2Cfg.grip + dir * 64, 128, 4096); gCd2Cfg.preset = CD2_PRESET_CUSTOM; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTightToggle(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Tight Turn: %s", gCd2Cfg.tightTurn ? "ON" : "OFF"); }
static int  cd2ToggleTight(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.tightTurn = !gCd2Cfg.tightTurn; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTmbButtons(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "TMB Buttons: %s", gCd2Cfg.tmbButtons ? "ON" : "OFF"); }
static int  cd2ToggleTmbButtons(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.tmbButtons = !gCd2Cfg.tmbButtons; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTightStrength(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Tight Pivot: %d%%", gCd2Cfg.tightStrength); }
static int  cd2AdjTightStrength(void* ud, int dir) { (void)ud; gCd2Cfg.tightStrength = jer_clamp_int(gCd2Cfg.tightStrength + dir * 5, 0, 100); cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelTightInput(void* ud, char* out, int max)
{
	(void)ud;
	if (gCd2Cfg.tmbButtons)
	{
		// PS-position reference: X/Cross (bottom) is the default Tight Turn,
		// Square (left) is Gas. Flip for pads that label the left button "X".
		snprintf(out, max, "Tight Turn Button: %s",
			gCd2Cfg.tmbTight ? "Square (Gas on X/Cross)" : "X / Cross (Gas on Square)");
	}
	else
		snprintf(out, max, "Tight Input: %s", kTightInputNames[gCd2Cfg.tightInput]);
}
static int  cd2CycleTightInput(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	if (gCd2Cfg.tmbButtons)
		gCd2Cfg.tmbTight = !gCd2Cfg.tmbTight;
	else
		gCd2Cfg.tightInput = (gCd2Cfg.tightInput + 1) % 3;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelDebug(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Telemetry Log: %s", gCd2Cfg.debugLog ? "ON" : "OFF"); }
static int  cd2ToggleDebug(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.debugLog = !gCd2Cfg.debugLog; cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cd2LabelPreset(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Preset: %s", kPresetNames[gCd2Cfg.preset]); }
static int  cd2CyclePreset(void* ud, int dir) { (void)ud; (void)dir; gCd2Cfg.preset = (gCd2Cfg.preset + 1) % 3; cd2ApplyPreset(); cd2SaveConfig(); return JER_PAUSE_QUIT_NONE; }

static int cd2ResetDefaults(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.preset = CD2_PRESET_DEFAULT;
	cd2ApplyPreset();
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

// ---- submenu: Tight Turn -------------------------------------------------
static const JER_PAUSE_MENU_ITEM cd2TightItems[] =
{
	{ NULL, cd2LabelTightToggle,   cd2ToggleTight,      NULL, NULL, 0 },
	{ NULL, cd2LabelTightStrength, cd2AdjTightStrength, NULL, NULL, 1 },
	{ NULL, cd2LabelTightInput,    cd2CycleTightInput,  NULL, NULL, 1 },
};

static const JER_PAUSE_MENU cd2TightMenu =
{ "Tight Turn", cd2TightItems, 3 };

// ---- submenu: Debug ------------------------------------------------------
static int cd2TotalCar(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	// defer: combatd2sim.c applies it on the next physics frame (after
	// unpausing) so the wreck + explosion don't trigger while the menu is up.
	cd2CarQueueTotal();

	return JER_PAUSE_QUIT_NONE;
}

// ---- submenu: Weapons ----------------------------------------------------
// Test/dev helpers: grant every weapon at once, plus a per-weapon grant/clear
// toggle. Base weapons (the machine gun) are not listed - always carried.
static void cd2LabelAllWeapons(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "All Weapons (Test): %s", gCd2Cfg.allWeapons ? "ON" : "OFF");
}

static int cd2ToggleAllWeapons(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.allWeapons = !gCd2Cfg.allWeapons;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static int cd2GrantAllNow(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	cd2WpnGrantAllMax();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelWeapon(void* ud, char* out, int max)
{
	int id = (int)(size_t)ud;
	const CD2_WEAPON_DEF* d = cd2WpnDef(id);

	if (d == NULL)
	{
		snprintf(out, max, "(invalid)");
		return;
	}

	if (d->isBase || cd2WpnAmmo(id) < 0)
		snprintf(out, max, "%s: infinite", d->name);
	else
		snprintf(out, max, "%s: %d", d->name, cd2WpnAmmo(id));
}

static int cd2ToggleWeapon(void* ud, int dir)
{
	int id = (int)(size_t)ud;
	const CD2_WEAPON_DEF* d = cd2WpnDef(id);
	(void)dir;

	if (d == NULL)
		return JER_PAUSE_QUIT_NONE;

	if (cd2WpnOwns(id))
		cd2WpnClear(id);
	else
		cd2WpnGrant(id, (d->maxAmmo > 0) ? d->maxAmmo : 10);

	return JER_PAUSE_QUIT_NONE;
}

// ---- AI + balance test helpers -------------------------------------------
static void cd2LabelAi(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Opponent AI: %s", gCd2Cfg.aiOpponent ? "ON" : "OFF");
}

static int cd2ToggleAi(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.aiOpponent = !gCd2Cfg.aiOpponent;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelAiState(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "AI State: %s", cd2AiStateName());
}

static int cd2CycleAiState(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.aiForceState = (gCd2Cfg.aiForceState + 1) % CD2_AI_STATE_COUNT;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelAiDebug(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "AI Readout: %s", gCd2Cfg.aiDebug ? "ON" : "OFF");
}

static int cd2ToggleAiDebug(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.aiDebug = !gCd2Cfg.aiDebug;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelRespawn(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Car Respawn: %s", gCd2Cfg.respawn ? "ON" : "OFF");
}

static int cd2ToggleRespawn(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.respawn = !gCd2Cfg.respawn;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelAiRole(void* ud, char* out, int max)
{
	static const char* names[] = { "Auto", "Chaser", "Flanker", "Ambusher", "Harvester" };
	int r = gCd2Cfg.aiRole;
	(void)ud;

	if (r < -1 || r >= CD2_AI_ROLE_COUNT)
		r = -1;

	snprintf(out, max, "Opponent Role: %s", names[r + 1]);
}

static int cd2CycleAiRole(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gCd2Cfg.aiRole++;

	if (gCd2Cfg.aiRole >= CD2_AI_ROLE_COUNT)
		gCd2Cfg.aiRole = -1;

	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelNavDebug(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Nav Debug: %s", gCd2Cfg.navDebug ? "ON" : "OFF");
}

static int cd2ToggleNavDebug(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.navDebug = !gCd2Cfg.navDebug;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelCarCarNerf(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Car-Car Damage: %d%%", gCd2Cfg.carCarDamage);
}

static int cd2CycleCarCarNerf(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.carCarDamage = 10 + ((gCd2Cfg.carCarDamage - 10 + 5) % 91);
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static void cd2LabelScenery(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Scenery Damage: %d%%", gCd2Cfg.sceneryDamage);
}

static int cd2CycleScenery(void* ud, int dir)
{
	(void)ud;
	(void)dir;
	gCd2Cfg.sceneryDamage = (gCd2Cfg.sceneryDamage + 5) % 105;
	cd2SaveConfig();
	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM cd2WeaponItems[] =
{
	{ NULL, cd2LabelAllWeapons, cd2ToggleAllWeapons, NULL, NULL, 0 },
	{ "Grant All Now", NULL, cd2GrantAllNow, NULL, NULL, 0 },
	{ NULL, cd2LabelWeapon, cd2ToggleWeapon, (void*)(size_t)CD2_WID_MISSILE, NULL, 0 },
	{ NULL, cd2LabelWeapon, cd2ToggleWeapon, (void*)(size_t)CD2_WID_MINE, NULL, 0 },
	{ NULL, cd2LabelAi, cd2ToggleAi, NULL, NULL, 0 },
	{ NULL, cd2LabelAiState, cd2CycleAiState, NULL, NULL, 0 },
	{ NULL, cd2LabelAiRole, cd2CycleAiRole, NULL, NULL, 0 },
	{ NULL, cd2LabelAiDebug, cd2ToggleAiDebug, NULL, NULL, 0 },
	{ NULL, cd2LabelRespawn, cd2ToggleRespawn, NULL, NULL, 0 },
	{ NULL, cd2LabelNavDebug, cd2ToggleNavDebug, NULL, NULL, 0 },
	{ NULL, cd2LabelScenery, cd2CycleScenery, NULL, NULL, 0 },
	{ NULL, cd2LabelCarCarNerf, cd2CycleCarCarNerf, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU cd2WeaponMenu =
{ "Weapons", cd2WeaponItems, 12 };

static const JER_PAUSE_MENU_ITEM cd2DebugItems[] =
{
	{ NULL, cd2LabelDebug, cd2ToggleDebug, NULL, NULL, 0 },
	{ "Total Car", NULL, cd2TotalCar, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU cd2DebugMenu =
{ "Debug", cd2DebugItems, 2 };

// ---- main menu -----------------------------------------------------------
static const JER_PAUSE_MENU_ITEM cd2MenuItems[] =
{
	{ NULL, cd2LabelEnabled,  cd2ToggleEnabled, NULL, NULL, 0 },
	{ NULL, cd2LabelTopSpeed, cd2AdjTopSpeed,   NULL, NULL, 1 },
	{ NULL, cd2LabelAccel,    cd2AdjAccel,      NULL, NULL, 1 },
	{ NULL, cd2LabelBrake,    cd2AdjBrake,      NULL, NULL, 1 },
	{ NULL, cd2LabelHandling, cd2AdjHandling,   NULL, NULL, 1 },
	{ NULL, cd2LabelGrip,     cd2AdjGrip,       NULL, NULL, 1 },
	{ NULL, cd2LabelTmbButtons, cd2ToggleTmbButtons, NULL, NULL, 0 },
	{ "Tight Turn...", NULL, NULL, NULL, &cd2TightMenu, 0 },
	{ NULL, cd2LabelPreset,   cd2CyclePreset,   NULL, NULL, 1 },
	{ "Weapons...", NULL, NULL, NULL, &cd2WeaponMenu, 0 },
	{ "Debug...", NULL, NULL, NULL, &cd2DebugMenu, 0 },
	{ "Reset to Defaults", NULL, cd2ResetDefaults, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU cd2Menu =
{ "Combat D2", cd2MenuItems, 12 };

// ---------------------------------------------------------------------------
// Registration: the module entry wires the menu in here (the menu owns its own
// registration so combatd2.c does not need to see cd2Menu).
// ---------------------------------------------------------------------------
void cd2MenuRegister(JERICHO_CONTEXT* ctx)
{
	jer_pause_menu_register(&cd2Menu);
}
