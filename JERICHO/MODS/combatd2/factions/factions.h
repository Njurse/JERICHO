// factions/factions.h — Combat D2 FACTIONS: the five teams of Caine's Crossfire.
//
// A faction is a car's TEAM IDENTITY. Twisted Metal (2012) is the reference:
// each team has a leader, its own colour, its own crew and its own special, and
// the tournament puts them all against each other. Caine is the Calypso of the
// piece — he assembled the field and watches, he does not compete.
//
// The five rows live beside this header in factions.c. Everything a faction
// carries is DATA, so cultivating a team means editing one row:
//
//   colour          the faction's hue — HUD names and the opponent map blips
//   crew models     the driver and gunner who lean out of the window
//   vehicle         a preferred resident car slot + palette (signature ride)
//   special         the CD2_WID_* it carries, and the battlecry line
//   temperament     loyalty / aggression offsets
//   rank            leader, minion, or the host
//   stance          how it treats every other faction (cd2FacStance)
//
// SCOPE OF THIS PASS: the registry, the per-car assignment and the coloured
// names in messages are live. The stance table, the vehicle preference, the
// crew models and the special weapon are DEFINED BUT UNREAD — nothing yet
// consults them (see FACTIONS.md for the list and for what would read them).
//
// THE FIELD: TANNER is the player's faction. The four AI cars are MCKENZIE x1,
// VASQUEZ x2 (Caine's enforcers) and JERICHO x1 — see sRoster in factions.c.

#ifndef CD2_FACTIONS_H
#define CD2_FACTIONS_H

#include "driver2.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"	/* CD2_WID_* — a faction's special weapon */

// ---------------------------------------------------------------------------
// Faction ids (index into the registry table in factions.c)
// ---------------------------------------------------------------------------
enum
{
	CD2_FAC_TANNER = 0,	// John Tanner — Rogue Undercover — the player
	CD2_FAC_MCKENZIE,	// Lt. McKenzie — The Law
	CD2_FAC_VASQUEZ,	// Vasquez — Caine's enforcer (silent in canon)
	CD2_FAC_JERICHO,	// Charles Jericho — Chaos / Free Agent
	CD2_FAC_CAINE,		// Solomon Caine — the host (competes = 0)
	CD2_FAC_COUNT,
	CD2_FAC_NONE = -1
};

// ---------------------------------------------------------------------------
// How two factions treat each other (cd2FacStance)
// ---------------------------------------------------------------------------
// EVERY pair of factions is HOSTILE: each one wars with each other, a free-for-
// all rather than the blocs the story brief first described. ALLY and NEUTRAL
// are kept in the enum (and in the table in factions.c) so a later alliance /
// betrayal pass is a table edit, not a rewrite.
enum
{
	CD2_FAC_STANCE_SELF = 0,
	CD2_FAC_STANCE_ALLY,
	CD2_FAC_STANCE_NEUTRAL,
	CD2_FAC_STANCE_HOSTILE,
	CD2_FAC_STANCE_COUNT
};

// ---------------------------------------------------------------------------
// A faction's place in the hierarchy (Twisted Metal's leader/minion split)
// ---------------------------------------------------------------------------
enum
{
	CD2_FAC_RANK_LEADER = 0,	// runs the faction, fields for it
	CD2_FAC_RANK_MINION,		// fields for it
	CD2_FAC_RANK_HOST,		// the Calypso: assembles the field, drives nothing
	CD2_FAC_RANK_COUNT
};

// ---------------------------------------------------------------------------
// The row. Field-by-field notes are in factions.c next to the actual values.
// ---------------------------------------------------------------------------
typedef struct CD2_FACTION
{
	int id;			// CD2_FAC_* (mirrors the row's index; for logs)
	const char* tag;	// short uppercase name for messages ("TANNER")
	const char* fullName;	// the character ("John Tanner")
	const char* title;	// the faction's flavour label ("Rogue Undercover")
	const char* motive;	// what they want out of Caine's tournament

	unsigned char r, g, b;	// the faction colour (HUD names, map blips)
	unsigned char ar, ag, ab;	// accent colour, for badges/details later

	int driverPedModel;	// PED_MODEL_TYPES the crew DRIVER is spawned with
	int gunnerPedModel;	// ... and the GUNNER. Both TANNER_MODEL for now:
				// model 1 has no bone-draw path yet (motion_c.c
				// gates it on TANNER_MODEL), so the two fields are
				// kept separate precisely so that day is a data edit.

	int carModelSlot;	// preferred resident car slot (-1 = any this level
				// loaded) — the faction's signature vehicle
	int carPalette;		// preferred palette (-1 = any)

	int specialWeapon;	// CD2_WID_* this faction's cars carry as a special
	const char* battlecry;	// the line that plays off its special

	int competes;		// 0 = the host: assembled the field, drives nothing
	int rank;		// CD2_FAC_RANK_*
	int loyalty;		// 0..100 — how much they honour a deal ("no real
				// loyalty" is a LOW number, not an alliance)
	int aggression;		// 0..100 — temperament offset for the AI later
	int carsPerLevel;	// cars this faction puts on the field (summary of
				// sRoster; cd2FacDump cross-checks the two)
} CD2_FACTION;

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------
// The row for a faction id, or NULL when the id is not CD2_FAC_*.
const CD2_FACTION* cd2FacDef(int factionId);

// "TANNER" / "John Tanner" — the tag is the message name, the full name the
// credits line. Both fall back to "-" for CD2_FAC_NONE.
const char* cd2FacTagOf(int factionId);
const char* cd2FacNameOf(int factionId);

// ---------------------------------------------------------------------------
// Stances
// ---------------------------------------------------------------------------
// How `a` treats `b` (CD2_FAC_STANCE_*). SELF on the diagonal, and for anything
// that is not a faction (CD2_FAC_NONE) the answer is HOSTILE — an unrostered car
// is nobody's friend. cd2FacAtWar is the shorthand the consumer code wants.
int cd2FacStance(int a, int b);
int cd2FacAtWar(int a, int b);

// The faction colour. Returns 1 and fills r/g/b, or 0 for CD2_FAC_NONE (the
// caller then keeps whatever it was going to draw anyway).
int cd2FacColourOf(int factionId, unsigned char* r, unsigned char* g, unsigned char* b);

// ---------------------------------------------------------------------------
// Per-car assignment
// ---------------------------------------------------------------------------
// Every car the module drives carries a faction; gCd2CarFaction (factions.c) is
// the storage. NONE means "not assigned yet" — the sync and the AI spawner fill
// it in, and cd2FacTagOfCar is what the message code asks.
int cd2FacOfCarId(int carId);
int cd2FacOfCar(const CAR_DATA* cp);
void cd2FacSetCar(int carId, int factionId);
void cd2FacForgetCar(int carId);

// The tag / colour of the car's faction. The tag is NULL (and the colour 0)
// when the car has no faction, so a caller can fall back to its old naming.
const char* cd2FacTagOfCar(const CAR_DATA* cp);
int cd2FacColourOfCar(const CAR_DATA* cp, unsigned char* r, unsigned char* g, unsigned char* b);

// Which faction the AI's spawn slot `index` fields (the roster table). Always a
// competing faction — the host is not on the field.
int cd2FacRosterFaction(int spawnIndex);

// Hand a freshly spawned opponent its faction (call once per car, after the car
// exists). Safe on NULL / an already-assigned car id.
void cd2FacAssignAiCar(void* car, int spawnIndex);

// The player's faction (config `player_faction`, TANNER by default). A host
// faction is refused here: Caine does not drive.
int cd2FacPlayerFaction(void);

// ---------------------------------------------------------------------------
// Hooks + observability
// ---------------------------------------------------------------------------
// Register the GAME_START / CAR_STEP / RESET_CAR handlers. Called once from the
// module entry (combatd2.c), BEFORE cd2AiRegister so the field is reset before
// the AI's own level-start state.
void cd2FacRegister(JERICHO_CONTEXT* ctx);

// Log the field plan (one line per faction, plus a carsPerLevel/roster
// consistency check). Called at GAME_START; cheap and unconditional, because it
// is the evidence a headless run is read for.
void cd2FacDumpField(void);

#endif /* CD2_FACTIONS_H */
