/*
 * teams/teams.c -- the team table and the boot dump.
 *
 * The colours themselves are macros in teams.h; this file adds what is derived
 * from them (which suit rule a character follows) and the one place that turns a
 * faction id into a colour, so a caller never reaches into a faction row.
 */
#include "driver2.h"		/* engine base types first, as the module's other files do */
#include "cainescrossfire.h"
#include "jericho.h"		/* jer_log, JERICHO_CONTEXT (factions.h needs it) */
#include "factions/factions.h"	/* CD2_FAC_* ids, cd2FacTagOf */

#include "teams/teams.h"

// ---------------------------------------------------------------------------
// The table. One row per faction; the colour is NOT repeated here, it comes from
// the macros in teams.h so there is still exactly one place to edit a colour.
// ---------------------------------------------------------------------------
static const CD2_TEAM sTeam[CD2_FAC_COUNT] =
{
	// Tanner and McKenzie have to look like themselves: a light wash only.
	{ CD2_FAC_TANNER,	CD2_SUIT_CANONICAL,	"standard suit, lightly washed" },
	{ CD2_FAC_MCKENZIE,	CD2_SUIT_CANONICAL,	"police uniform, lightly washed" },
	// These two are their colour.
	{ CD2_FAC_VASQUEZ,	CD2_SUIT_FULL,		"suit is the team colour" },
	{ CD2_FAC_JERICHO,	CD2_SUIT_FULL,		"suit is the team colour" },
	// The host: nobody asked for a colour, so he keeps the one he had.
	{ CD2_FAC_CAINE,	CD2_SUIT_CANONICAL,	"host - keeps his own colour" },
};

// [D] [T]
const CD2_TEAM* cd2TeamOf(int factionId)
{
	int i;

	for (i = 0; i < CD2_FAC_COUNT; i++)
	{
		if (sTeam[i].factionId == factionId)
			return &sTeam[i];
	}

	return NULL;
}

// [D] [T]
int cd2TeamColour(int factionId, unsigned char* r, unsigned char* g, unsigned char* b)
{
	unsigned char cr = 0, cg = 0, cb = 0;

	switch (factionId)
	{
		case CD2_FAC_TANNER:	cr = CD2_TEAM_TANNER_R;   cg = CD2_TEAM_TANNER_G;   cb = CD2_TEAM_TANNER_B;   break;
		case CD2_FAC_MCKENZIE:	cr = CD2_TEAM_MCKENZIE_R; cg = CD2_TEAM_MCKENZIE_G; cb = CD2_TEAM_MCKENZIE_B; break;
		case CD2_FAC_VASQUEZ:	cr = CD2_TEAM_VASQUEZ_R;  cg = CD2_TEAM_VASQUEZ_G;  cb = CD2_TEAM_VASQUEZ_B;  break;
		case CD2_FAC_JERICHO:	cr = CD2_TEAM_JERICHO_R;  cg = CD2_TEAM_JERICHO_G;  cb = CD2_TEAM_JERICHO_B;  break;
		case CD2_FAC_CAINE:	cr = CD2_TEAM_CAINE_R;    cg = CD2_TEAM_CAINE_G;    cb = CD2_TEAM_CAINE_B;    break;

		default:
			return 0;	/* unknown: leave the caller's values alone */
	}

	if (r != NULL) *r = cr;
	if (g != NULL) *g = cg;
	if (b != NULL) *b = cb;

	return 1;
}

// [D] [T]
short cd2TeamSuitTint(int factionId)
{
	const CD2_TEAM* t = cd2TeamOf(factionId);

	return (t != NULL) ? t->suitTint : 0;
}

// ---------------------------------------------------------------------------
// The boot dump: one line per team, so a headless run shows the table it is
// actually using rather than the one someone remembers editing.
// ---------------------------------------------------------------------------
void cd2TeamDump(void)
{
	int i;

	jer_log("[cainescrossfire] teams: %d rows (team colour -> suit tint)\n", CD2_FAC_COUNT);

	for (i = 0; i < CD2_FAC_COUNT; i++)
	{
		unsigned char r = 0, g = 0, b = 0;

		cd2TeamColour(sTeam[i].factionId, &r, &g, &b);

		jer_log("[cainescrossfire]   %-9s rgb=%02X%02X%02X suit=%3d/256  %s\n",
			cd2FacTagOf(sTeam[i].factionId), r, g, b,
			(int)sTeam[i].suitTint,
			(sTeam[i].note != NULL) ? sTeam[i].note : "");
	}
}
