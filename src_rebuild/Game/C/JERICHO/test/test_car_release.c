/* Host-side tests for the car-switch release logic (#14 / #12): the pure pieces the engine
 * (Game/C/carpinref.h) and carhacks (MODS/carhacks/slotrelease.h) use, with no engine, no VRAM
 * and no level. This directory is excluded from the game build (premake5.lua), so the file
 * only ever builds on its own:
 *
 *   gcc -std=c99 -Wall -Wextra -o /tmp/test_car_release src_rebuild/Game/C/JERICHO/test/test_car_release.c && /tmp/test_car_release
 *   g++ -x c++ -Wall -Wextra -o /tmp/test_car_release src_rebuild/Game/C/JERICHO/test/test_car_release.c && /tmp/test_car_release
 *   (MSVC: cl /TP src_rebuild\Game\C\JERICHO\test\test_car_release.c)
 *
 * Exit status 0 = every check passed. */
#include <stdio.h>
#include <string.h>

#include "../../carpinref.h"
#include "../../../../../JERICHO/MODS/carhacks/slotrelease.h"

static int gFails, gChecks;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ------------------------------------------------------------------ */
/* carhacks: the release decision                                      */
/* ------------------------------------------------------------------ */

static void test_verdict(void)
{
	/* held, wanted, cars */
	CHECK(chkReleaseVerdict(0, 0, 0) == CHK_REL_NOT_HELD);
	CHECK(chkReleaseVerdict(0, 1, 3) == CHK_REL_NOT_HELD);	/* nothing there: nothing to do */
	CHECK(chkReleaseVerdict(1, 0, 0) == CHK_REL_FREE);
	CHECK(chkReleaseVerdict(1, 1, 0) == CHK_REL_KEEP_WANTED);	/* named, empty: keep */
	CHECK(chkReleaseVerdict(1, 1, 2) == CHK_REL_KEEP_WANTED);	/* named wins over cars */
	CHECK(chkReleaseVerdict(1, 0, 1) == CHK_REL_DEFER_CARS);	/* #12: a car is still on it */
	CHECK(chkReleaseVerdict(1, 0, 5) == CHK_REL_DEFER_CARS);

	CHECK(strcmp(chkReleaseVerdictName(CHK_REL_FREE), "released") == 0);
	CHECK(strcmp(chkReleaseVerdictName(CHK_REL_KEEP_WANTED), "kept") == 0);
	CHECK(strcmp(chkReleaseVerdictName(CHK_REL_DEFER_CARS), "deferred") == 0);
	CHECK(strcmp(chkReleaseVerdictName(CHK_REL_NOT_HELD), "not held") == 0);
}

static void test_should_log(void)
{
	/* a first decision (or any change of verdict) is logged */
	CHECK(chkReleaseShouldLog(CHK_REL_NOT_HELD, 0, CHK_REL_DEFER_CARS, 1));
	CHECK(chkReleaseShouldLog(CHK_REL_DEFER_CARS, 1, CHK_REL_FREE, 0));
	CHECK(chkReleaseShouldLog(CHK_REL_DEFER_CARS, 1, CHK_REL_KEEP_WANTED, 1));

	/* a deferred slot re-checked every frame: silent until the car count changes */
	CHECK(!chkReleaseShouldLog(CHK_REL_DEFER_CARS, 1, CHK_REL_DEFER_CARS, 1));
	CHECK(chkReleaseShouldLog(CHK_REL_DEFER_CARS, 1, CHK_REL_DEFER_CARS, 2));
	CHECK(!chkReleaseShouldLog(CHK_REL_KEEP_WANTED, 0, CHK_REL_KEEP_WANTED, 0));
}

static void test_count_cars(void)
{
	int live[6] = { 1, 0, 1, 1, 1, 0 };
	int model[6] = { 7, 7, 3, 7, 0, 7 };	/* car 1 and 5 are not in the world */
	int first = 99;

	CHECK(chkReleaseCountCars(live, model, 6, 7, &first) == 2);
	CHECK(first == 0);
	CHECK(chkReleaseCountCars(live, model, 6, 3, &first) == 1);
	CHECK(first == 2);
	CHECK(chkReleaseCountCars(live, model, 6, 9, &first) == 0);
	CHECK(first == -1);
	CHECK(chkReleaseCountCars(live, model, 6, 0, NULL) == 1);
}

static void test_resident_value(void)
{
	/* the -1 write: a spare the level left empty goes back to -1, never 0 */
	CHECK(chkReleaseResidentValue(-1) == -1);
	CHECK(chkReleaseResidentValue(-7) == -1);
	CHECK(chkReleaseResidentValue(0) == 0);		/* the level really had model 0 there */
	CHECK(chkReleaseResidentValue(4) == 4);
}

/* A deferred release, the way net.c drives it: decide, then re-decide every frame until the
 * car leaves the slot - one log line when deferred, none while nothing changes, one when it is
 * finally released. */
static void test_defer_path(void)
{
	int live[3] = { 1, 1, 0 };
	int model[3] = { 6, 2, 6 };
	CHK_REL_VERDICT last = CHK_REL_NOT_HELD, v;
	int lastCars = 0, cars, logged = 0, frame, freedAt = -1;

	for (frame = 0; frame < 10; frame++)
	{
		if (frame == 4)
			model[0] = 2;		/* the car is re-modelled onto the new slot */

		cars = chkReleaseCountCars(live, model, 3, 6, NULL);
		v = chkReleaseVerdict(1, 0, cars);

		if (frame == 0 || chkReleaseShouldLog(last, lastCars, v, cars))
			logged++;

		last = v;
		lastCars = cars;

		if (v == CHK_REL_FREE)
		{
			freedAt = frame;
			break;
		}
	}

	CHECK(freedAt == 4);
	CHECK(logged == 2);		/* "deferred" once, "released" once */
}

/* ------------------------------------------------------------------ */
/* engine: pin owners                                                  */
/* ------------------------------------------------------------------ */

static void test_owner_mask(void)
{
	int m = 0;

	CHECK(CarPinOwnerBit(-1) == 0);
	CHECK(CarPinOwnerBit(CAR_PIN_OWNERS_MAX) == 0);
	CHECK(CarPinOwnerBit(5) == 32);

	m |= CarPinOwnerBit(5);
	m |= CarPinOwnerBit(7);
	m |= CarPinOwnerBit(7);		/* idempotent */

	CHECK(CarPinOwnersHas(m, 5));
	CHECK(CarPinOwnersHas(m, 7));
	CHECK(!CarPinOwnersHas(m, 6));
	CHECK(CarPinOwnersCount(m) == 2);
	CHECK(CarPinOwnersFirst(m) == 5);
	CHECK(CarPinOwnersFirst(0) == -1);
	CHECK(CarPinOwnersCount(CarPinOwnerBit(11) | CarPinOwnerBit(0)) == 2);
}

static void test_drop_owner(void)
{
	/* pins 0..4: slot 5 alone, 5+7 shared, 7 alone, 5 alone, nobody's (unrelated) */
	int owners[5];
	int freed[8], shared = -1, n;

	owners[0] = CarPinOwnerBit(5);
	owners[1] = CarPinOwnerBit(5) | CarPinOwnerBit(7);
	owners[2] = CarPinOwnerBit(7);
	owners[3] = CarPinOwnerBit(5);
	owners[4] = CarPinOwnerBit(9);

	n = CarPinDropOwner(owners, 5, 5, freed, 8, &shared);

	CHECK(n == 2);
	CHECK(freed[0] == 0 && freed[1] == 3);	/* ascending */
	CHECK(shared == 1);			/* pin 1: slot 7 still draws with it */
	CHECK(owners[1] == CarPinOwnerBit(7));
	CHECK(owners[2] == CarPinOwnerBit(7));
	CHECK(owners[4] == CarPinOwnerBit(9));

	/* dropping a slot that owns nothing changes nothing */
	n = CarPinDropOwner(owners, 5, 6, freed, 8, &shared);
	CHECK(n == 0 && shared == 0);

	/* an out-of-range slot (no bit) cannot free anything */
	n = CarPinDropOwner(owners, 5, -1, freed, 8, NULL);
	CHECK(n == 0);
}

static void test_array_remove(void)
{
	int a[5] = { 10, 11, 12, 13, 14 };
	int n = 5;

	n = CarPinArrayRemove(a, n, 1);
	CHECK(n == 4 && a[0] == 10 && a[1] == 12 && a[2] == 13 && a[3] == 14);

	n = CarPinArrayRemove(a, n, 3);		/* the last */
	CHECK(n == 3 && a[2] == 13);

	n = CarPinArrayRemove(a, n, 7);		/* out of range: unchanged */
	CHECK(n == 3);
}

/* ------------------------------------------------------------------ */
/* engine: CLUT bands                                                  */
/* ------------------------------------------------------------------ */

static void test_clut_rows(void)
{
	CHECK(CarClutRowsFor(0) == 0);
	CHECK(CarClutRowsFor(1) == 1);
	CHECK(CarClutRowsFor(4) == 1);
	CHECK(CarClutRowsFor(5) == 2);
	CHECK(CarClutRowsFor(32) == 8);

	/* a walk from (960,600): 5 CLUTs stop at (976,601) - two whole rows, not one */
	CHECK(CarClutRowsUsed(960, 600, 976, 601) == 2);
	/* 4 CLUTs wrap exactly to (960,601): one row */
	CHECK(CarClutRowsUsed(960, 600, 960, 601) == 1);
	CHECK(CarClutRowsUsed(960, 600, 960, 600) == 0);
}

static void test_clut_free_list(void)
{
	CAR_CLUT_FREE fl;
	int y;

	CarClutFreeReset(&fl);
	CHECK(CarClutFreeTake(&fl, 2) == -1);		/* empty */

	CHECK(CarClutFreeGive(&fl, 600, 2));
	CHECK(CarClutFreeGive(&fl, 610, 3));
	CHECK(fl.count == 2 && CarClutFreeRows(&fl) == 5);

	/* first fit, exact: the 2-row band goes whole */
	y = CarClutFreeTake(&fl, 2);
	CHECK(y == 600 && fl.count == 1);

	/* split: 1 of the 3 rows, the rest stays */
	y = CarClutFreeTake(&fl, 1);
	CHECK(y == 610 && fl.count == 1 && fl.y[0] == 611 && fl.rows[0] == 2);

	/* too big for anything */
	CHECK(CarClutFreeTake(&fl, 3) == -1);

	/* merge with the band after: 609+2 touches 611 */
	CHECK(CarClutFreeGive(&fl, 609, 2));
	CHECK(fl.count == 1 && fl.y[0] == 609 && fl.rows[0] == 4);

	/* merge with the band before */
	CHECK(CarClutFreeGive(&fl, 613, 1));
	CHECK(fl.count == 1 && fl.rows[0] == 5);

	/* a gap, then a band that closes it: before + after merge into one */
	CHECK(CarClutFreeGive(&fl, 620, 2));
	CHECK(fl.count == 2);
	CHECK(CarClutFreeGive(&fl, 614, 6));
	CHECK(fl.count == 1 && fl.y[0] == 609 && fl.rows[0] == 13);

	/* a double give-back (overlap) is refused, never kept twice */
	CHECK(!CarClutFreeGive(&fl, 610, 1));
	CHECK(!CarClutFreeGive(&fl, 605, 5));
	CHECK(CarClutFreeRows(&fl) == 13);

	/* full list: rows are counted lost, not handed out */
	{
		int i, ok = 1;

		CarClutFreeReset(&fl);

		for (i = 0; i < CAR_CLUT_FREE_MAX; i++)
			ok &= CarClutFreeGive(&fl, 600 + i * 3, 1);	/* gaps: no merging */

		CHECK(ok && fl.count == CAR_CLUT_FREE_MAX);
		CHECK(!CarClutFreeGive(&fl, 1000, 2));
		CHECK(fl.lost == 2);
	}
}

static void test_remap_releasable(void)
{
	int pins[3] = { 20, 35, 77 };

	CHECK(!CarRemapReleasable(35, pins, 3, 0));	/* a remaining pin still pages it in */
	CHECK(CarRemapReleasable(36, pins, 3, 0));
	CHECK(!CarRemapReleasable(36, pins, 3, 1));	/* another imported model names it */
	CHECK(CarRemapReleasable(36, pins, 0, 0));
}

/* ------------------------------------------------------------------ */
/* a small simulation of the engine's release, refcounts end to end    */
/* ------------------------------------------------------------------ */

#define SIM_PINS	8
#define SIM_POOL	30

typedef struct
{
	int set[SIM_PINS], owners[SIM_PINS], pool[SIM_PINS], clutY[SIM_PINS], clutRows[SIM_PINS];
	int n;
	unsigned char poolUsed[SIM_POOL];
	int watermark;			/* the lower half pool CLUT watermark */
	CAR_CLUT_FREE fl;
} SIM;

static int SimPoolAlloc(SIM* s)
{
	int i;

	for (i = 0; i < SIM_POOL; i++)
		if (!s->poolUsed[i]) { s->poolUsed[i] = 1; return i; }

	return -1;
}

static int SimPoolUsed(const SIM* s)
{
	int i, n = 0;

	for (i = 0; i < SIM_POOL; i++)
		n += s->poolUsed[i];

	return n;
}

/* CarPinRecord + CarImportPin's band choice, condensed */
static void SimRecordAndPlace(SIM* s, int set, int slot, int npal)
{
	int p, rows = CarClutRowsFor(npal), y;

	for (p = 0; p < s->n; p++)
	{
		if (s->set[p] == set)
		{
			s->owners[p] |= CarPinOwnerBit(slot);
			return;
		}
	}

	p = s->n++;
	s->set[p] = set;
	s->owners[p] = CarPinOwnerBit(slot);
	s->pool[p] = SimPoolAlloc(s);

	y = CarClutFreeTake(&s->fl, rows);

	if (y < 0)
	{
		y = s->watermark;
		s->watermark += rows;
	}

	s->clutY[p] = y;
	s->clutRows[p] = rows;
}

/* JerReleaseCarSlot's pin half, condensed */
static int SimRelease(SIM* s, int slot, int* shared)
{
	int freed[SIM_PINS], n, f;

	n = CarPinDropOwner(s->owners, s->n, slot, freed, SIM_PINS, shared);

	for (f = n - 1; f >= 0; f--)
	{
		int i = freed[f], m = s->n;

		s->poolUsed[s->pool[i]] = 0;
		CarClutFreeGive(&s->fl, s->clutY[i], s->clutRows[i]);

		CarPinArrayRemove(s->set, m, i);
		CarPinArrayRemove(s->owners, m, i);
		CarPinArrayRemove(s->pool, m, i);
		CarPinArrayRemove(s->clutY, m, i);
		CarPinArrayRemove(s->clutRows, m, i);
		s->n = m - 1;
	}

	return n;
}

static void test_simulation(void)
{
	SIM s;
	int shared, freedN, k, wm;

	memset(&s, 0, sizeof(s));
	s.watermark = 546;		/* past the guest palette tables, say */
	CarClutFreeReset(&s.fl);

	/* two VEGAS cars: slot 5 names sets 20,35; slot 6 names 35,36 (35 is shared) */
	SimRecordAndPlace(&s, 20, 5, 6);	/* 2 rows */
	SimRecordAndPlace(&s, 35, 5, 4);	/* 1 row  */
	SimRecordAndPlace(&s, 35, 6, 4);	/* shared: no second copy */
	SimRecordAndPlace(&s, 36, 6, 8);	/* 2 rows */

	CHECK(s.n == 3);
	CHECK(SimPoolUsed(&s) == 3);
	CHECK(s.watermark == 546 + 5);

	/* #12: releasing slot 5 must NOT free set 35 (slot 6 still draws with it) */
	freedN = SimRelease(&s, 5, &shared);
	CHECK(freedN == 1 && shared == 1);
	CHECK(s.n == 2 && SimPoolUsed(&s) == 2);
	CHECK(s.set[0] == 35 && s.owners[0] == CarPinOwnerBit(6));
	CHECK(CarClutFreeRows(&s.fl) == 2);

	/* the next car reuses the freed rows instead of moving the watermark (#14) */
	wm = s.watermark;
	SimRecordAndPlace(&s, 51, 7, 5);	/* 2 rows: fits the freed band exactly */
	CHECK(s.watermark == wm);
	CHECK(CarClutFreeRows(&s.fl) == 0);

	/* switching back and forth many times does not walk the column or the pool */
	for (k = 0; k < 50; k++)
	{
		SimRelease(&s, 7, NULL);
		SimRecordAndPlace(&s, 51 + (k & 1), 7, 5);
	}

	CHECK(s.watermark == wm);
	CHECK(SimPoolUsed(&s) == 3);

	/* everyone gone: everything back */
	SimRelease(&s, 6, NULL);
	SimRelease(&s, 7, NULL);
	CHECK(s.n == 0 && SimPoolUsed(&s) == 0);
	CHECK(CarClutFreeRows(&s.fl) == s.watermark - 546);	/* every band row is free again */
	CHECK(s.fl.count == 1);				/* and merged back into one band */
}

int main(void)
{
	test_verdict();
	test_should_log();
	test_count_cars();
	test_resident_value();
	test_defer_path();
	test_owner_mask();
	test_drop_owner();
	test_array_remove();
	test_clut_rows();
	test_clut_free_list();
	test_remap_releasable();
	test_simulation();

	printf("%d check(s), %d failed\n", gChecks, gFails);

	return gFails ? 1 : 0;
}
