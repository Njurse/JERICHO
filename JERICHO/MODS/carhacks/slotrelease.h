#ifndef CARHACKS_SLOTRELEASE_H
#define CARHACKS_SLOTRELEASE_H

/* carhacks: the release DECISION for a resident slot, as pure logic (no engine, no globals),
 * so it can be tested off-engine. Header-only and static on purpose: net.c includes it, a
 * host-side test includes it, and the module build does not have to learn a new file.
 *
 * One rule, used by every path that can leave a slot unused - a peer leaving, a peer
 * changing car, the local player changing car, a load that failed (carhacks/net.c,
 * chkNetReleaseSlotIfUnused):
 *
 *   the slot is freed only when NO player's car identity still names the car on it,
 *   AND NO car in the world is still on the slot.
 *
 * Named-but-empty keeps it (someone will draw it again - a player who left their car and
 * got back in, a peer whose CARS row still says it). Not-named-but-occupied DEFERS it: the
 * geometry and pages are what that car is drawn with this frame, so freeing them is the
 * #12 crash/corruption. A deferred slot is retried every frame until the car is gone. */

typedef enum
{
	CHK_REL_NOT_HELD = 0,		/* nothing imported there: nothing to do */
	CHK_REL_FREE,			/* give it back now */
	CHK_REL_KEEP_WANTED,		/* a player still names this car */
	CHK_REL_DEFER_CARS		/* nobody names it, but a car is still on the slot */
} CHK_REL_VERDICT;

static inline CHK_REL_VERDICT chkReleaseVerdict(int held, int wanted, int carsOnSlot)
{
	if (!held)
		return CHK_REL_NOT_HELD;

	if (wanted)
		return CHK_REL_KEEP_WANTED;

	if (carsOnSlot > 0)
		return CHK_REL_DEFER_CARS;

	return CHK_REL_FREE;
}

static inline const char* chkReleaseVerdictName(CHK_REL_VERDICT v)
{
	switch (v)
	{
		case CHK_REL_NOT_HELD:		return "not held";
		case CHK_REL_FREE:		return "released";
		case CHK_REL_KEEP_WANTED:	return "kept";
		case CHK_REL_DEFER_CARS:	return "deferred";
	}

	return "?";
}

/* How many cars are on resident slot `slot`, given each car's "in the world" flag and model
 * (the engine's car_data[i].controlType != CONTROL_TYPE_NONE and car_data[i].ap.model).
 * `first` gets the first such car's index, or -1. */
static inline int chkReleaseCountCars(const int* live, const int* model, int ncars, int slot, int* first)
{
	int i, n = 0;

	if (first)
		*first = -1;

	for (i = 0; i < ncars; i++)
	{
		if (!live[i] || model[i] != slot)
			continue;

		if (n == 0 && first)
			*first = i;

		n++;
	}

	return n;
}

/* A deferred slot is re-checked every frame; say so only when something changed (the verdict,
 * or how many cars are on it), so the log has one line per decision and not one per frame. */
static inline int chkReleaseShouldLog(CHK_REL_VERDICT lastVerdict, int lastCars,
	CHK_REL_VERDICT verdict, int cars)
{
	if (verdict != lastVerdict)
		return 1;

	return (verdict == CHK_REL_DEFER_CARS && cars != lastCars);
}

/* What residentCarModels[slot] goes back to on release: the value the LEVEL had there before
 * any import (-1 for a spare the level left empty). Writing 0 - the old behaviour - made a
 * freed spare look like it held model 0, so the engine (and chkImportSlotFree's "does the level
 * hold something here" reading of the list) saw a car that is not there. `levelModel` is what
 * the level's own list said, or anything negative when it is not known. */
static inline int chkReleaseResidentValue(int levelModel)
{
	return (levelModel >= 0) ? levelModel : -1;
}

#endif /* CARHACKS_SLOTRELEASE_H */
