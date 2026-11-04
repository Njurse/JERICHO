/* Which pins and slots a world-VRAM reload may touch. The decision is pure
 * (carpinref.h) so it can be checked without the engine:
 *
 *   gcc -std=c99 -Wall -Wextra -o /tmp/test_world_reload src_rebuild/Game/C/JERICHO/test/test_world_reload.c && /tmp/test_world_reload
 *   g++ -x c++ -Wall -Wextra -o /tmp/test_world_reload src_rebuild/Game/C/JERICHO/test/test_world_reload.c && /tmp/test_world_reload
 *
 * Exit status 0 = every check passed.
 */
#include <stdio.h>

#include "../../carpinref.h"

static int gFails, gChecks;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_detach(void)
{
	/* lower-half pool pin: not on a world rectangle */
	CHECK(!jer_world_reload_detach_pin(0, 7));
	CHECK(!jer_world_reload_detach_pin(3, -1));

	/* placed on a tpagepos[] slot */
	CHECK(jer_world_reload_detach_pin(-1, 0));
	CHECK(jer_world_reload_detach_pin(-1, 18));

	/* not placed, or past the slot table */
	CHECK(!jer_world_reload_detach_pin(-1, -1));
	CHECK(!jer_world_reload_detach_pin(-1, 19));
	CHECK(JER_WORLD_SLOTS == 19);
}

static void test_slots(void)
{
	/* 12 resident pages: 0..11 resident, 12..18 streamed */
	CHECK(jer_world_reload_resident_slot(0, 12));
	CHECK(jer_world_reload_resident_slot(11, 12));
	CHECK(!jer_world_reload_resident_slot(12, 12));
	CHECK(!jer_world_reload_stream_slot(11, 12));
	CHECK(jer_world_reload_stream_slot(12, 12));
	CHECK(jer_world_reload_stream_slot(18, 12));
	CHECK(!jer_world_reload_stream_slot(19, 12));
	CHECK(!jer_world_reload_resident_slot(-1, 12));
	CHECK(!jer_world_reload_stream_slot(-1, 12));

	/* nothing resident: the whole table is the spool's */
	CHECK(!jer_world_reload_resident_slot(0, 0));
	CHECK(jer_world_reload_stream_slot(0, 0));
	CHECK(jer_world_reload_stream_slot(18, 0));
}

int main(void)
{
	test_detach();
	test_slots();

	if (gFails != 0)
	{
		printf("%d/%d failed\n", gFails, gChecks);
		return 1;
	}

	printf("%d checks passed\n", gChecks);
	return 0;
}
