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
// The live table. The colours start as the macros in teams.h and cd2TeamSet can
// change them at any time; nothing else in the module keeps its own copy.
// ---------------------------------------------------------------------------
static CD2_TEAM sTeam[CD2_FAC_COUNT] =
{
	// Tanner and McKenzie have to look like themselves: a light wash only.
	{ CD2_FAC_TANNER,	CD2_TEAM_TANNER_R,   CD2_TEAM_TANNER_G,   CD2_TEAM_TANNER_B,	  CD2_SUIT_CANONICAL,	"standard suit, lightly washed" },
	{ CD2_FAC_MCKENZIE,	CD2_TEAM_MCKENZIE_R, CD2_TEAM_MCKENZIE_G, CD2_TEAM_MCKENZIE_B,	  CD2_SUIT_CANONICAL,	"police uniform, lightly washed" },
	// These two are their colour.
	{ CD2_FAC_VASQUEZ,	CD2_TEAM_VASQUEZ_R,  CD2_TEAM_VASQUEZ_G,  CD2_TEAM_VASQUEZ_B,	  CD2_SUIT_FULL,	"suit is the team colour" },
	{ CD2_FAC_JERICHO,	CD2_TEAM_JERICHO_R,  CD2_TEAM_JERICHO_G,  CD2_TEAM_JERICHO_B,	  CD2_SUIT_FULL,	"suit is the team colour" },
	// The host: nobody asked for a colour, so he keeps the one he had.
	{ CD2_FAC_CAINE,	CD2_TEAM_CAINE_R,    CD2_TEAM_CAINE_G,    CD2_TEAM_CAINE_B,	  CD2_SUIT_CANONICAL,	"host - keeps his own colour" },
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
	const CD2_TEAM* t = cd2TeamOf(factionId);

	if (t == NULL)
		return 0;	/* unknown: leave the caller's values alone */

	if (r != NULL) *r = t->r;
	if (g != NULL) *g = t->g;
	if (b != NULL) *b = t->b;

	return 1;
}

// [D] [T]
short cd2TeamSuitTint(int factionId)
{
	const CD2_TEAM* t = cd2TeamOf(factionId);

	return (t != NULL) ? t->suitTint : 0;
}

// ---------------------------------------------------------------------------
// Change a team at runtime. This is what makes a mid-round colour change (or a
// future mid-game team switch) work with no further plumbing: every reader of a
// team colour reads this table, and the suit palette is keyed on the colour, so
// a new colour simply builds a new row set on the next draw.
// ---------------------------------------------------------------------------
void cd2TeamSet(int factionId, int r, int g, int b, int suitTint)
{
	CD2_TEAM* t = (CD2_TEAM*)cd2TeamOf(factionId);

	if (t == NULL)
	{
		jer_log("[cainescrossfire] cd2TeamSet: unknown faction %d, ignored\n", factionId);
		return;
	}

	if (r >= 0) t->r = (unsigned char)((r > 255) ? 255 : r);
	if (g >= 0) t->g = (unsigned char)((g > 255) ? 255 : g);
	if (b >= 0) t->b = (unsigned char)((b > 255) ? 255 : b);

	if (suitTint >= 0)
		t->suitTint = (short)((suitTint > 256) ? 256 : suitTint);

	jer_log("[cainescrossfire] team %s set to rgb=%02X%02X%02X suit=%d/256\n",
		cd2FacTagOf(t->factionId), t->r, t->g, t->b, (int)t->suitTint);
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
