// factions/factions.c — Combat D2 FACTIONS: the registry, the roster and the
// per-car assignment.
//
// (New concern, new sibling folder — see cainescrossfire_internal.h for the file map.)
//
// The five rows are the whole system. Two design notes worth having in code:
//
//   * CAINE competes = 0. He is the Calypso of this version of the story: he
//     put the field together and watches it, so he is a faction with a colour,
//     a name and a motive but never a car. cd2FacRosterFaction can never
//     return him, and cd2FacPlayerFaction refuses him.
//
//   * THE STANCE TABLE IS ALL-HOSTILE. Each faction wars with each other — a
//     free-for-all. ALLY and NEUTRAL exist so that a later alliance/betrayal
//     pass (Caine's deals, Jericho's turn) is a table edit and not a rewrite;
//     nothing reads the table yet either way.
//
// Vasquez has no battlecry on purpose: he is entirely silent in canon, so an
// empty line is the faithful value, not a missing one.

#include "driver2.h"
#include "cainescrossfire.h"
#include "cainescrossfire_internal.h"
#include "factions/factions.h"
#include "cars.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"
#include "ai/ai.h"		/* cd2AiIsOpponent */

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------
// Colours are the agreed palette: TANNER ice-white, MCKENZIE police blue,
// VASQUEZ toxic green, JERICHO chaos orange, CAINE mafia crimson.
//
// driverPedModel / gunnerPedModel are TANNER_MODEL for every faction today.
// They are two separate fields on purpose: ped model 1 renders through the
// sprite/bone path that motion_c.c gates on TANNER_MODEL, so giving a faction
// its own crew body is a two-field data edit once that path is opened up.
//
// carModelSlot / carPalette / specialWeapon / battlecry are the Twisted Metal
// per-faction flavour. Nothing reads them yet; carModelSlot and carPalette will
// be honoured by cd2AiSpawnOne, specialWeapon by the spawn-time arsenal.
static const CD2_FACTION sFac[CD2_FAC_COUNT] =
{
	// --- TANNER: the player. Off-book, off-leash, and the one who wants the
	// --- whole network exposed.
	{
		CD2_FAC_TANNER, "TANNER", "John Tanner", "Rogue Undercover",
		"Caine's criminal network exposed - enough evidence to bring them all down",
		0xE6, 0xEE, 0xF8,	/* ice white */
		0x8A, 0x9C, 0xB0,	/* accent: steel */
		TANNER_MODEL, TANNER_MODEL,
		-1, -1,
		CD2_WID_MISSILE, "You're coming down with me.",
		1, CD2_FAC_RANK_LEADER, 30, 60, 1
	},

	// --- MCKENZIE: The Law. Wants the rogue operation shut down and Tanner
	// --- brought in - the institution Tanner keeps rejecting.
	{
		CD2_FAC_MCKENZIE, "MCKENZIE", "Lt. McKenzie", "The Law",
		"Tanner brought in and the rogue operation shut down",
		0x2F, 0x6B, 0xFF,	/* police blue */
		0xC8, 0xA0, 0x30,	/* accent: badge gold */
		TANNER_MODEL, TANNER_MODEL,
		-1, -1,
		CD2_WID_SMG, "Tanner! Stand down!",
		1, CD2_FAC_RANK_LEADER, 70, 50, 1
	},

	// --- VASQUEZ: Caine's enforcer, silent in canon. Two cars on the field:
	// --- he is the muscle, not a personality.
	{
		CD2_FAC_VASQUEZ, "VASQUEZ", "Vasquez", "Enforcer",
		"Total control of Caine's US operations",
		0x35, 0xD0, 0x6A,	/* toxic green */
		0x2C, 0x5A, 0x38,	/* accent: olive */
		TANNER_MODEL, TANNER_MODEL,
		-1, -1,
		CD2_WID_SHOTGUN, "",
		1, CD2_FAC_RANK_MINION, 90, 70, 2
	},

	// --- JERICHO: Chaos / Free Agent. Starts loyal, ends up murdering Caine;
	// --- low loyalty is the honest number for him.
	{
		CD2_FAC_JERICHO, "JERICHO", "Charles Jericho", "Chaos / Free Agent",
		"Tanner dead, and Caine's empire for himself",
		0xFF, 0x8A, 0x1E,	/* chaos orange */
		0xC8, 0x28, 0x78,	/* accent: magenta */
		TANNER_MODEL, TANNER_MODEL,
		-1, -1,
		CD2_WID_SPECIAL_JERICHO, "Right in the face, boss!",
		1, CD2_FAC_RANK_LEADER, 20, 95, 1
	},

	// --- CAINE: the host. Runs the field, drives nothing.
	{
		CD2_FAC_CAINE, "CAINE", "Solomon Caine", "Organized Crime",
		"Four of them pointed at each other, and every one of them in debt to him",
		0xE0, 0x24, 0x24,	/* mafia crimson */
		0xD4, 0xAF, 0x37,	/* accent: gold */
		TANNER_MODEL, TANNER_MODEL,
		-1, -1,
		CD2_WID_MINE, "Everybody gets what they want. Eventually.",
		0, CD2_FAC_RANK_HOST, 50, 40, 0
	}
};

// sStance[a][b] — how `a` treats `b`. SELF on the diagonal; every other pair is
// HOSTILE (each faction wars with each other). The ALLY/NEUTRAL columns are here
// so the table above can be read as the shape of the story, not just its state.
//
//                TANNER  MCKENZIE  VASQUEZ  JERICHO  CAINE
static const unsigned char sStance[CD2_FAC_COUNT][CD2_FAC_COUNT] =
{
	/* TANNER   */ { CD2_FAC_STANCE_SELF, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE },
	/* MCKENZIE */ { CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_SELF, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE },
	/* VASQUEZ  */ { CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_SELF, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE },
	/* JERICHO  */ { CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_SELF, CD2_FAC_STANCE_HOSTILE },
	/* CAINE    */ { CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_HOSTILE, CD2_FAC_STANCE_SELF }
};

// ---------------------------------------------------------------------------
// The field: which faction fills each AI spawn slot, in cd2AiSpawnOne's spawn
// order. Caine can never appear here (competes = 0), which is exactly why the
// order is written out rather than derived from the rows.
// ---------------------------------------------------------------------------
static const int sRoster[] =
{
	CD2_FAC_MCKENZIE,
	CD2_FAC_VASQUEZ,
	CD2_FAC_VASQUEZ,
	CD2_FAC_JERICHO
};

#define CD2_FAC_ROSTER_COUNT	((int)(sizeof(sRoster) / sizeof(sRoster[0])))

// An opponent that reaches the sync without having been rostered (a spawn path
// that bypassed cd2AiSpawnOne, a second player, a hand-spawned test car) is
// named as the field's default muscle rather than left anonymous.
#define CD2_FAC_DEFAULT_OPPONENT	CD2_FAC_VASQUEZ

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// gCd2CarFaction[carId] — the faction every car the module drives belongs to.
// CD2_FAC_NONE means "not assigned yet".
static int gCd2CarFaction[MAX_CARS];

// ---------------------------------------------------------------------------
// Registry accessors
// ---------------------------------------------------------------------------
const CD2_FACTION* cd2FacDef(int factionId)
{
	if (factionId < 0 || factionId >= CD2_FAC_COUNT)
		return NULL;

	return &sFac[factionId];
}

const char* cd2FacTagOf(int factionId)
{
	const CD2_FACTION* f = cd2FacDef(factionId);

	return (f != NULL) ? f->tag : "-";
}

const char* cd2FacNameOf(int factionId)
{
	const CD2_FACTION* f = cd2FacDef(factionId);

	return (f != NULL) ? f->fullName : "-";
}

// ---------------------------------------------------------------------------
// Stances
// ---------------------------------------------------------------------------
int cd2FacStance(int a, int b)
{
	if (a < 0 || a >= CD2_FAC_COUNT || b < 0 || b >= CD2_FAC_COUNT)
		return CD2_FAC_STANCE_HOSTILE;	// not a faction: nobody's friend

	return sStance[a][b];
}

int cd2FacAtWar(int a, int b)
{
	return cd2FacStance(a, b) == CD2_FAC_STANCE_HOSTILE;
}

int cd2FacColourOf(int factionId, unsigned char* r, unsigned char* g, unsigned char* b)
{
	const CD2_FACTION* f = cd2FacDef(factionId);

	if (f == NULL)
		return 0;

	if (r != NULL) *r = f->r;
	if (g != NULL) *g = f->g;
	if (b != NULL) *b = f->b;

	return 1;
}

// ---------------------------------------------------------------------------
// Per-car assignment
// ---------------------------------------------------------------------------
int cd2FacOfCarId(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return CD2_FAC_NONE;

	return gCd2CarFaction[carId];
}

int cd2FacOfCar(const CAR_DATA* cp)
{
	if (cp == NULL)
		return CD2_FAC_NONE;

	return cd2FacOfCarId(cp->id);
}

void cd2FacSetCar(int carId, int factionId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	if (factionId < 0 || factionId >= CD2_FAC_COUNT)
		factionId = CD2_FAC_NONE;

	gCd2CarFaction[carId] = factionId;

	if (gCd2Cfg.debugLog)
		printInfo("[cainescrossfire] faction: car=%d -> %s (%s)\n",
			carId, cd2FacTagOf(factionId), cd2FacNameOf(factionId));
}

void cd2FacForgetCar(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	gCd2CarFaction[carId] = CD2_FAC_NONE;
}

const char* cd2FacTagOfCar(const CAR_DATA* cp)
{
	int id = cd2FacOfCar(cp);

	return (id == CD2_FAC_NONE) ? NULL : cd2FacTagOf(id);
}

int cd2FacColourOfCar(const CAR_DATA* cp, unsigned char* r, unsigned char* g, unsigned char* b)
{
	return cd2FacColourOf(cd2FacOfCar(cp), r, g, b);
}

int cd2FacRosterFaction(int spawnIndex)
{
	if (spawnIndex < 0)
		return CD2_FAC_NONE;

	// past the end of the roster the field wraps rather than going anonymous
	return sRoster[spawnIndex % CD2_FAC_ROSTER_COUNT];
}

void cd2FacAssignAiCar(void* vcar, int spawnIndex)
{
	CAR_DATA* cp = (CAR_DATA*)vcar;
	int faction;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	faction = cd2FacRosterFaction(spawnIndex);

	// a host faction cannot be on the field, however the roster is edited
	if (faction == CD2_FAC_NONE || !sFac[faction].competes)
		faction = CD2_FAC_DEFAULT_OPPONENT;

	cd2FacSetCar(cp->id, faction);
}

int cd2FacPlayerFaction(void)
{
	int faction = gCd2Cfg.playerFaction;

	// refuse anything that is not a faction, and anything that does not
	// compete: the player drives a car, so the host is not an option.
	if (faction < 0 || faction >= CD2_FAC_COUNT || !sFac[faction].competes)
	{
		if (gCd2Cfg.debugLog)
			printInfo("[cainescrossfire] faction: player_faction %d is not playable, using TANNER\n", faction);

		faction = CD2_FAC_TANNER;
	}

	return faction;
}

// ---------------------------------------------------------------------------
// The assignment rules: the player car and any opponent the spawner missed
// ---------------------------------------------------------------------------
// A CAR_STEP handler, so it sees one car at a time and costs nothing.
static int cd2FacOnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!gCd2Cfg.enabled || !gCd2Cfg.factions || cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	// The player car does not exist yet at GAME_START (the AI's own level-start
	// note says the same), so the player's faction is derived here, the first
	// time the car is stepped. A car already carrying one keeps it.
	if (cd2FacOfCarId(cp->id) == CD2_FAC_NONE)
	{
		if (cp->controlType == CONTROL_TYPE_PLAYER)
			cd2FacSetCar(cp->id, cd2FacPlayerFaction());
		else if (cd2AiIsOpponent(cp))
			cd2FacSetCar(cp->id, CD2_FAC_DEFAULT_OPPONENT);
	}

	return JER_RESULT_CONTINUE;
}

// GAME_START: a fresh level. Forget every car and log the field it expects.
static int cd2FacOnGameStart(void* ud, void* args)
{
	int i;
	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
		gCd2CarFaction[i] = CD2_FAC_NONE;

	cd2FacDumpField();

	return JER_RESULT_CONTINUE;
}

// RESET_CAR: a car's damage state is cleared (level load, respawn). Identity is
// not damage state, so a factioned car KEEPS its faction; only a player car is
// re-derived, so a stale assignment to a recycled slot cannot survive.
static int cd2FacOnResetCar(void* ud, void* args)
{
	JER_ARGS_RESET_CAR* a = (JER_ARGS_RESET_CAR*)args;
	(void)ud;

	if (a->carId < 0 || a->carId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (car_data[a->carId].controlType == CONTROL_TYPE_PLAYER)
		cd2FacSetCar(a->carId, cd2FacPlayerFaction());

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Observability
// ---------------------------------------------------------------------------
// One line per faction + a check that the two descriptions of the field agree
// (carsPerLevel is the readable summary; sRoster is what actually spawns).
void cd2FacDumpField(void)
{
	int counted[CD2_FAC_COUNT];
	int playerFac = cd2FacPlayerFaction();
	int i;

	for (i = 0; i < CD2_FAC_COUNT; i++)
		counted[i] = 0;

	for (i = 0; i < CD2_FAC_ROSTER_COUNT; i++)
		counted[sRoster[i]]++;

	printInfo("[cainescrossfire] factions: %d rows, player=%s, %d AI cars rostered\n",
		CD2_FAC_COUNT, cd2FacTagOf(playerFac), CD2_FAC_ROSTER_COUNT);

	for (i = 0; i < CD2_FAC_COUNT; i++)
	{
		const CD2_FACTION* f = &sFac[i];
		const char* cry = f->battlecry;

		// The player's faction fields one car that is not in the AI roster, so
		// its expected total is the roster count plus the player's own car.
		int expected = counted[i] + ((i == playerFac && f->competes) ? 1 : 0);

		if (cry == NULL)
			cry = "";

		printInfo("[cainescrossfire] faction %d %-8s %-15s %-17s rgb=%02X%02X%02X competes=%d rank=%d cars=%d/%d special=%s\n",
			f->id, f->tag, f->fullName, f->title,
			f->r, f->g, f->b, f->competes, f->rank, expected, f->carsPerLevel,
			cd2WpnName(f->specialWeapon));

		// an empty battlecry is a value, not a missing one (Vasquez is silent)
		printInfo("[cainescrossfire] faction %s wants: %s | cry: %s\n",
			f->tag, (f->motive != NULL) ? f->motive : "-", cry);

		if (expected != f->carsPerLevel)
			printInfo("[cainescrossfire] factions: WARNING %s says carsPerLevel=%d but the roster fields %d\n",
				f->tag, f->carsPerLevel, expected);
	}
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_cainescrossfire_entry in cainescrossfire.c)
// ---------------------------------------------------------------------------
void cd2FacRegister(JERICHO_CONTEXT* ctx)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
		gCd2CarFaction[i] = CD2_FAC_NONE;

	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2FacOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2FacOnCarStep, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, cd2FacOnResetCar, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] factions registered (SDK v%d)\n", ctx->sdkVersion);
}
