/* Host-side tests for the jer_menu nesting (JER_MENU_MAX_DEPTH). The library is
 * deliberately free of the game headers, so it builds and runs on its own: no
 * engine, no level, no pad:
 *
 *   cl /nologo /EHsc /I JERICHO\sdk\include /Fe:jer_menu_test.exe ^
 *      src_rebuild\Game\C\JERICHO\src\jer_menu.c ^
 *      src_rebuild\Game\C\JERICHO\test\test_jer_menu.c
 *
 * Exit status 0 = every check passed.
 *
 * WHAT IT PINS, and why each one is worth a check rather than an eyeball:
 *
 *   - a FLAT menu still reports a select as 1, the way it always did, so nothing
 *     that used the library before this change can have noticed it;
 *   - an item that OPENS a submenu is a NEW outcome, not a select, and the menu
 *     actually descends (its labels change);
 *   - coming back UP lands on the parent with the cursor it had, which is the
 *     whole reason the levels are a stack rather than one saved pointer;
 *   - the ceiling is REFUSED rather than crossed: at the deepest allowed level a
 *     submenu reports JER_MENU_TOO_DEEP and the menu does not move, which is the
 *     one outcome that would silently corrupt the stack if it were wrong. */
#include <stdio.h>

#include "jer_menu.h"

static int gFails, gChecks;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* the pad bits the caller would pass in -- the library never learns their names */
#define B_UP	0x1000
#define B_DOWN	0x2000
#define B_SEL	0x4000

/* ------------------------------------------------------------------ */
/* Two trees: one deep enough to hit the ceiling                       */
/* ------------------------------------------------------------------ */

/* a plain flat menu, for the "unchanged" checks */
static const char* const kFlat[] = { "Alpha", "Beta", "Gamma" };

/* the deep chain: lv0 -> lv1 -> lv2 -> lv3 -> lv4, and lv4's first item wants a
 * child of its own (lv5), which is one level BELOW the ceiling.
 *
 * Tables are indexed by ITEM, so each one has an entry per item of its level and a
 * zeroed entry where the item is a leaf. */
static const char* const lv5[] = { "L5a" };

static const char* const lv4[] = { "L4a", "L4b" };
static const JerMenuSub lv4subs[] = { { lv5, 1, NULL }, { NULL, 0, NULL } };

static const char* const lv3[] = { "L3a" };
static const JerMenuSub lv3subs[] = { { lv4, 2, lv4subs } };

static const char* const lv2[] = { "L2a" };
static const JerMenuSub lv2subs[] = { { lv3, 1, lv3subs } };

static const char* const lv1[] = { "L1a" };
static const JerMenuSub lv1subs[] = { { lv2, 1, lv2subs } };

static const char* const lv0[] = { "L0a", "L0b" };
static const JerMenuSub lv0subs[] = { { lv1, 1, lv1subs }, { NULL, 0, NULL } };

/* press SELECT once */
static int Select(JerMenu* m)
{
	return jer_menu_update(m, B_UP, B_DOWN, B_SEL, 0, B_SEL);
}

/* move the cursor down n times (a fresh press each time) */
static void Down(JerMenu* m, int n)
{
	int i;

	for (i = 0; i < n; i++)
		jer_menu_update(m, B_UP, B_DOWN, B_SEL, 0, B_DOWN);
}

int main(void)
{
	JerMenu m;

	/* ---------------------------------------------------------------- */
	/* 1. A FLAT MENU is unchanged: select is 1, no levels, back does nothing */
	/* ---------------------------------------------------------------- */
	jer_menu_list(&m, "Flat", kFlat, 3);

	CHECK(jer_menu_depth(&m) == 0);
	CHECK(jer_menu_has_sub(&m) == 0);
	CHECK(jer_menu_back(&m) == 0);			/* nothing to go back to */

	Down(&m, 1);
	CHECK(jer_menu_label(&m) != NULL && jer_menu_label(&m)[0] == 'B');	/* "Beta" */

	CHECK(Select(&m) == JER_MENU_SELECT);		/* 1, exactly as before */
	CHECK(jer_menu_depth(&m) == 0);			/* and it did not move */

	/* ---------------------------------------------------------------- */
	/* 2. AN ITEM WITH A SUBMENU opens it, and that is not a select */
	/* ---------------------------------------------------------------- */
	jer_menu_list(&m, "Top", lv0, 2);
	jer_menu_submenus(&m, lv0subs, 2);

	CHECK(jer_menu_depth(&m) == 0);
	CHECK(jer_menu_has_sub(&m) == 1);		/* item 0 (L0a) opens lv1 */
	CHECK(Select(&m) == JER_MENU_OPENED);
	CHECK(jer_menu_depth(&m) == 1);
	CHECK(jer_menu_label(&m)[1] == '1');		/* "L1a" - we are in the child */

	/* ---------------------------------------------------------------- */
	/* 3. COMING BACK lands on the parent WITH ITS CURSOR */
	/* ---------------------------------------------------------------- */
	/* put the top cursor on item 1 first, so a restore that reset it to 0 shows */
	jer_menu_list(&m, "Top", lv0, 2);
	jer_menu_submenus(&m, lv0subs, 2);
	Down(&m, 1);
	CHECK(jer_menu_label(&m)[1] == '0');		/* "L0b" */

	/* item 1 is a leaf, so this selects rather than descends */
	CHECK(Select(&m) == JER_MENU_SELECT);
	CHECK(jer_menu_depth(&m) == 0);

	/* now go down and back up from item 0 */
	jer_menu_update(&m, B_UP, B_DOWN, B_SEL, 0, B_UP);	/* cursor back to 0 */
	CHECK(Select(&m) == JER_MENU_OPENED);
	CHECK(jer_menu_depth(&m) == 1);
	CHECK(jer_menu_back(&m) == 1);
	CHECK(jer_menu_depth(&m) == 0);
	CHECK(jer_menu_label(&m)[1] == '0');		/* back on "L0a", not reset */

	/* ---------------------------------------------------------------- */
	/* 4. THE CEILING IS REFUSED, and nothing moves */
	/* ---------------------------------------------------------------- */
	jer_menu_list(&m, "Deep", lv0, 2);
	jer_menu_submenus(&m, lv0subs, 2);

	CHECK(Select(&m) == JER_MENU_OPENED);		/* depth 1 */
	CHECK(Select(&m) == JER_MENU_OPENED);		/* depth 2 */
	CHECK(Select(&m) == JER_MENU_OPENED);		/* depth 3 */
	CHECK(Select(&m) == JER_MENU_OPENED);		/* depth 4 = the deepest allowed */
	CHECK(jer_menu_depth(&m) == JER_MENU_MAX_DEPTH - 1);

	/* lv4's item 0 declares lv5, which would be depth 5 -- one past the stack */
	CHECK(jer_menu_has_sub(&m) == 1);
	CHECK(Select(&m) == JER_MENU_TOO_DEEP);
	CHECK(jer_menu_depth(&m) == JER_MENU_MAX_DEPTH - 1);	/* refused, did not move */
	CHECK(jer_menu_label(&m)[1] == '4');			/* still "L4a" */

	/* and backing all the way out cannot underflow */
	while (jer_menu_back(&m))
		;
	CHECK(jer_menu_depth(&m) == 0);
	CHECK(jer_menu_back(&m) == 0);

	/* ---------------------------------------------------------------- */
	/* 5. A LEAF UNDER A SUBMENU still selects (the child is a plain list) */
	/* ---------------------------------------------------------------- */
	jer_menu_list(&m, "Deep", lv0, 2);
	jer_menu_submenus(&m, lv0subs, 2);

	CHECK(Select(&m) == JER_MENU_OPENED);		/* into lv1 */
	CHECK(jer_menu_has_sub(&m) == 1);		/* lv1's item 0 opens lv2 */
	CHECK(Select(&m) == JER_MENU_OPENED);

	jer_menu_end(&m);				/* a closed menu ignores everything */
	CHECK(Select(&m) == JER_MENU_NOTHING);

	printf("test_jer_menu: %d check(s), %d failed\n", gChecks, gFails);

	return gFails != 0;
}
