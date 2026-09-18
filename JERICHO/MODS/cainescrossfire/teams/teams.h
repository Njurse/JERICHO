#ifndef TEAMS_H
#define TEAMS_H

/*
 * teams/teams.h -- the team colours, and how far each character's SUIT takes
 * them. This is the ONE place to change either.
 *
 * Why a header: the colours are shared between things that must not drift apart.
 * factions/ reads its r/g/b from these macros (factions still owns everything
 * else about a faction -- stance, weapons, rank, roster), and the pedestrian
 * palette uses suitTint to give a character's suit the team colour.
 *
 * Colour order: plain bytes, exactly as factions.c has always written them. This
 * is NOT the B<<16|G<<8|R packing the polygon colour words use.
 */

// ---------------------------------------------------------------------------
// The team colours
// ---------------------------------------------------------------------------
// Tanner and McKenzie look like themselves -- Tanner's standard suit, McKenzie's
// police uniform -- so their suits only take a light wash of the team colour.
// Jericho and Vasquez are their colour.
#define CD2_TEAM_TANNER_R	0xff
#define CD2_TEAM_TANNER_G	0xe8
#define CD2_TEAM_TANNER_B	0x82

#define CD2_TEAM_MCKENZIE_R	0x29
#define CD2_TEAM_MCKENZIE_G	0x61
#define CD2_TEAM_MCKENZIE_B	0xba

#define CD2_TEAM_VASQUEZ_R	0xd1
#define CD2_TEAM_VASQUEZ_G	0x32
#define CD2_TEAM_VASQUEZ_B	0x2a

#define CD2_TEAM_JERICHO_R	0x2b
#define CD2_TEAM_JERICHO_G	0x4d
#define CD2_TEAM_JERICHO_B	0x22

// Caine is the host, not one of the four characters; he keeps the crimson he
// already had rather than being given a colour nobody asked for.
#define CD2_TEAM_CAINE_R	0xe0
#define CD2_TEAM_CAINE_G	0x24
#define CD2_TEAM_CAINE_B	0x24

// ---------------------------------------------------------------------------
// The suit rule
// ---------------------------------------------------------------------------
// 0..256: how far a character's OUTFIT moves toward the team colour.
//
// IMPORTANT -- this is a DYED tint, not a fill. The suit must keep the shading
// AND the contrast the original texture had: folds, seams, the lit side and the
// dark side all survive, and only the hue moves.
//
// What that means for the palette maths (JerichoMakeClutRow): a per-entry remap
// that replaces each colour with "team hue at this entry's brightness" is NOT
// enough. It keeps brightness but throws away the ratio between entries, and the
// result reads as a solid colour -- which is exactly what was reported on the
// first pass. The tint has to be multiplicative -- the original entry scaled
// toward the team hue -- so the relative differences between entries survive
// along with the absolute brightness.
#define CD2_SUIT_CANONICAL	60	// keep the look, wash it lightly
#define CD2_SUIT_FULL		256	// the suit is the team colour

// Used when a faction has no team row, and as the materialised default of the
// team_palette_strength config key. CD2_SUIT_FULL matches what the feature did
// before the per-character rule existed.
#define CD2_SUIT_TINT_DEFAULT	CD2_SUIT_FULL

// The dark end of the outfit is lifted by this much (of 31) before the tint is
// mixed in, so a suit whose fabric is nearly black still shows its team colour
// instead of staying black. 0 keeps whatever the texture had.
#define CD2_SUIT_FLOOR_DEFAULT	10

/* One row per faction: the colour lives in the macros above, this carries what
 * is derived from it. */
typedef struct CD2_TEAM
{
	int factionId;			// CD2_FAC_* this row belongs to
	short suitTint;			// CD2_SUIT_CANONICAL / CD2_SUIT_FULL (256 needs more than a byte)
	const char* note;		// why this character's suit behaves as it does
} CD2_TEAM;

/* The row for a faction, or NULL when the id is not one of ours. */
const CD2_TEAM* cd2TeamOf(int factionId);

/* The team colour as plain bytes. Returns 0 (and leaves the outputs alone) for
 * an unknown id, so a caller can fall back to stock. */
int cd2TeamColour(int factionId, unsigned char* r, unsigned char* g, unsigned char* b);

/* How far that faction's suit takes its team colour; 0 for an unknown id. */
short cd2TeamSuitTint(int factionId);

/* Log the table (one line per team) at boot -- the evidence a headless run is
 * read for. */
void cd2TeamDump(void);

#endif /* TEAMS_H */
