#ifndef CARPINREF_H
#define CARPINREF_H

/* JERICHO: the pure bookkeeping behind the cross-city pin table (texture.c), split out so it
 * can be tested without the engine (no VRAM, no level, no globals). Header-only and static on
 * purpose: texture.c includes it, and a host-side harness can include it too, without the
 * build having to learn about a new translation unit.
 *
 * Three pieces:
 *
 *  1. OWNERS. A pinned page belongs to every resident slot whose model names its set, not to
 *     the first slot that asked for it. Two imported cars of the same city share pages (the
 *     loader dedups a set across slots), so "release slot 7" must not free a page slot 8 still
 *     draws with. A pin's owners are a bitmask of resident slots; the page goes back to the
 *     pool only when that mask reaches 0.
 *
 *  2. CLUT BANDS. Each placed pin's page CLUTs take a band of whole rows in the lower half
 *     pool's CLUT column. A released band goes onto a free list and is handed to the next pin
 *     that fits (first fit, split the rest off); adjacent free bands merge. Bands always start
 *     at the column's first x (960) and are counted in WHOLE rows, so a band never shares a
 *     row with the next taker (the old cursor carried a mid-row x into the next band and did
 *     not round, which could put the palette upload's first row on top of a band's last).
 *
 *  3. ARRAY REMOVAL. The pin table is parallel arrays; removing an entry must shift every one
 *     of them. One helper, so an array cannot be forgotten (the old loop skipped
 *     sPinPreferred, so a later pin inherited the removed pin's preferred rectangle).
 */

#define CAR_PIN_OWNERS_MAX	31		/* resident slots a mask can name (MAX_CAR_RESIDENT_MODELS is 12) */

static inline int CarPinOwnerBit(int slot)
{
	if (slot < 0 || slot >= CAR_PIN_OWNERS_MAX)
		return 0;

	return 1 << slot;
}

static inline int CarPinOwnersHas(int mask, int slot)
{
	return (mask & CarPinOwnerBit(slot)) != 0;
}

static inline int CarPinOwnersCount(int mask)
{
	int n = 0;

	while (mask)
	{
		n += mask & 1;
		mask = (int)((unsigned int)mask >> 1);
	}

	return n;
}

/* The lowest slot in a mask, or -1 when it is empty (for logs that want one name). */
static inline int CarPinOwnersFirst(int mask)
{
	int s;

	for (s = 0; s < CAR_PIN_OWNERS_MAX; s++)
	{
		if (mask & (1 << s))
			return s;
	}

	return -1;
}

/* Take resident `slot` out of every pin's owners (`owners[0..npins)`). Writes the pins that no
 * car owns any more into `freed`, ascending (at most `maxFreed`), and returns how many; `shared`
 * gets how many of the slot's pins another car still owns (kept). The caller gives the freed
 * pins back - from the LAST index down, so removing one does not move the next. */
static inline int CarPinDropOwner(int* owners, int npins, int slot, int* freed, int maxFreed, int* shared)
{
	int i, n = 0, keep = 0, bit = CarPinOwnerBit(slot);

	if (bit != 0)
	{
		for (i = 0; i < npins; i++)
		{
			if (!(owners[i] & bit))
				continue;

			owners[i] &= ~bit;

			if (owners[i] != 0)
				keep++;
			else if (n < maxFreed)
				freed[n++] = i;
		}
	}

	if (shared)
		*shared = keep;

	return n;
}

/* Remove entry `at` from an int array of `n` entries (shifting the tail down). Returns n-1,
 * or n when `at` is out of range. The caller applies it to every parallel array. */
static inline int CarPinArrayRemove(int* a, int n, int at)
{
	int k;

	if (at < 0 || at >= n)
		return n;

	for (k = at; k < n - 1; k++)
		a[k] = a[k + 1];

	return n - 1;
}

/* ---------------------------------------------------------------------------
 * CLUT bands
 * ------------------------------------------------------------------------- */

#define CAR_CLUT_FREE_MAX	32

typedef struct
{
	int y[CAR_CLUT_FREE_MAX];		/* first row of each free band, ascending */
	int rows[CAR_CLUT_FREE_MAX];		/* its height in rows */
	int count;
	int lost;				/* rows given back that the list had no room to keep */
} CAR_CLUT_FREE;

static inline void CarClutFreeReset(CAR_CLUT_FREE* fl)
{
	fl->count = 0;
	fl->lost = 0;
}

static inline int CarClutFreeRows(const CAR_CLUT_FREE* fl)
{
	int i, n = 0;

	for (i = 0; i < fl->count; i++)
		n += fl->rows[i];

	return n;
}

/* Take `rows` rows from the free list: the first band that fits (ascending y), splitting the
 * rest of it off. Returns the first row, or -1 when no band is big enough. */
static inline int CarClutFreeTake(CAR_CLUT_FREE* fl, int rows)
{
	int i, k, y;

	if (rows <= 0)
		return -1;

	for (i = 0; i < fl->count; i++)
	{
		if (fl->rows[i] < rows)
			continue;

		y = fl->y[i];

		if (fl->rows[i] == rows)
		{
			for (k = i; k < fl->count - 1; k++)
			{
				fl->y[k] = fl->y[k + 1];
				fl->rows[k] = fl->rows[k + 1];
			}

			fl->count--;
		}
		else
		{
			fl->y[i] += rows;
			fl->rows[i] -= rows;
		}

		return y;
	}

	return -1;
}

/* Give a band back, keeping the list sorted and merging it with a neighbour it touches.
 * Returns 1 if kept, 0 if the list was full (the rows are then counted in `lost`, and stay
 * unusable until the level ends - never handed out twice). An overlap with a band already
 * free is refused the same way: it would mean a double give-back. */
static inline int CarClutFreeGive(CAR_CLUT_FREE* fl, int y, int rows)
{
	int i, k;

	if (rows <= 0 || y < 0)
		return 0;

	/* where it goes: before the first band that starts after it */
	for (i = 0; i < fl->count; i++)
	{
		if (fl->y[i] > y)
			break;
	}

	/* refuse an overlap with either neighbour */
	if (i > 0 && fl->y[i - 1] + fl->rows[i - 1] > y)
		return 0;

	if (i < fl->count && y + rows > fl->y[i])
		return 0;

	/* merge with the band before it */
	if (i > 0 && fl->y[i - 1] + fl->rows[i - 1] == y)
	{
		fl->rows[i - 1] += rows;

		/* ...and with the one after, if that now touches too */
		if (i < fl->count && fl->y[i - 1] + fl->rows[i - 1] == fl->y[i])
		{
			fl->rows[i - 1] += fl->rows[i];

			for (k = i; k < fl->count - 1; k++)
			{
				fl->y[k] = fl->y[k + 1];
				fl->rows[k] = fl->rows[k + 1];
			}

			fl->count--;
		}

		return 1;
	}

	/* merge with the band after it */
	if (i < fl->count && y + rows == fl->y[i])
	{
		fl->y[i] = y;
		fl->rows[i] += rows;
		return 1;
	}

	if (fl->count >= CAR_CLUT_FREE_MAX)
	{
		fl->lost += rows;
		return 0;
	}

	for (k = fl->count; k > i; k--)
	{
		fl->y[k] = fl->y[k - 1];
		fl->rows[k] = fl->rows[k - 1];
	}

	fl->y[i] = y;
	fl->rows[i] = rows;
	fl->count++;

	return 1;
}

/* The VRAM rows a page's CLUTs take: four 16-wide CLUTs to a row of the 64-wide column,
 * starting at the column's first x. */
static inline int CarClutRowsFor(int npalettes)
{
	if (npalettes <= 0)
		return 0;

	return (npalettes + 3) / 4;
}

/* How many WHOLE rows a walk used that started at (firstX, startY) and stopped at
 * (endX, endY): a walk that stopped mid-row still owns that row. */
static inline int CarClutRowsUsed(int firstX, int startY, int endX, int endY)
{
	int rows = endY - startY;

	if (endX != firstX)
		rows++;

	return (rows > 0) ? rows : 0;
}

/* ---------------------------------------------------------------------------
 * The baked index (remap / reservation) of a set
 * ------------------------------------------------------------------------- */

/* May the remap/reservation for source set `set` be given back? Only when no pin left in the
 * table still pages that set in (the remap is keyed by set number alone, so ANY city's pin
 * counts) and no other imported slot's model still names it (its polys baked that index). */
static inline int CarRemapReleasable(int set, const int* pinSet, int npins, int namedByOtherSlot)
{
	int i;

	if (namedByOtherSlot)
		return 0;

	for (i = 0; i < npins; i++)
	{
		if (pinSet[i] == set)
			return 0;
	}

	return 1;
}

#endif /* CARPINREF_H */
