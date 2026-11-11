/* WHLX trailer on LUMP_CAR_WHEELS. The pure reader lives in wheel_ext.h so this
 * builds without the engine:
 *
 *   gcc -std=c99 -Wall -Wextra -o /tmp/test_wheel_ext src_rebuild/Game/C/JERICHO/test/test_wheel_ext.c && /tmp/test_wheel_ext
 *   g++ -x c++ -Wall -Wextra -o /tmp/test_wheel_ext src_rebuild/Game/C/JERICHO/test/test_wheel_ext.c && /tmp/test_wheel_ext
 *
 * Exit status 0 = every check passed. */
#include <stdio.h>
#include <string.h>

#include "../../wheel_ext.h"

static int gFails, gChecks;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void put_i32(unsigned char* p, int v)
{
	unsigned int u = (unsigned int)v;

	p[0] = (unsigned char)u;
	p[1] = (unsigned char)(u >> 8);
	p[2] = (unsigned char)(u >> 16);
	p[3] = (unsigned char)(u >> 24);
}

/* The test's stand-in for a wheel MODEL: the byte it was asked to end at. */
static int test_model_end(const unsigned char* body, int size, int off, void* user)
{
	int* ends = (int*)user;
	int i;

	(void)body;
	(void)size;

	for (i = 0; ends != NULL && ends[i] != -1; i += 2)
	{
		if (ends[i] == off)
			return ends[i + 1];
	}

	return off;
}

static int write_v1(unsigned char* body, int count, const int* offs)
{
	int i;

	put_i32(body, count);

	for (i = 0; i < count; i++)
		put_i32(body + 4 + i * 4, offs[i]);

	return 4 + count * 4;
}

static void write_trailer(unsigned char* body, int at, int version, int slots, const int* offs)
{
	int i, n;

	body[at] = 0x57;
	body[at + 1] = 0x48;
	body[at + 2] = 0x4C;
	body[at + 3] = 0x58;
	put_i32(body + at + 4, version);
	put_i32(body + at + 8, slots);

	n = (slots > 0 && slots <= WHEEL_EXT_SLOTS) ? slots : 0;

	for (i = 0; i < n * WHEEL_EXT_KINDS; i++)
		put_i32(body + at + 12 + i * 4, offs[i]);
}

static void test_v1_only(void)
{
	unsigned char body[64];
	int offs[3];
	int end;
	const char* why = "stale";

	memset(body, 0, sizeof(body));
	offs[0] = 16;
	offs[1] = -1;
	offs[2] = -1;
	write_v1(body, 3, offs);

	{
		int ends[4];

		ends[0] = 16;
		ends[1] = 18;	/* not a multiple of 4: the reader pads */
		ends[2] = -1;
		ends[3] = 0;
		end = WheelExtV1End(body, 32, test_model_end, ends);
	}

	CHECK(end == 20);
	CHECK(WheelExtRead(body, 32, end, NULL, &why) == 0);
	CHECK(why == NULL);
}

static void test_absent_slots_and_one_owned(void)
{
	unsigned char body[512];
	int v1[3];
	int table[WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS];
	int got[WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS];
	int ends[6];
	int end, i;
	const char* why = NULL;

	memset(body, 0xA5, sizeof(body));
	v1[0] = 16;
	v1[1] = -1;
	v1[2] = 40;
	write_v1(body, 3, v1);

	ends[0] = 16;
	ends[1] = 30;
	ends[2] = 40;
	ends[3] = 47;	/* 47 -> pad to 48 */
	ends[4] = -1;
	ends[5] = 0;

	end = WheelExtV1End(body, (int)sizeof(body), test_model_end, ends);
	CHECK(end == 48);

	for (i = 0; i < WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS; i++)
		table[i] = -1;

	/* Slot 0 is the default car: it points back at the v1 blocks. */
	table[0 * 3 + 0] = 16;
	table[0 * 3 + 2] = 40;
	/* Slot 4 clean is a model that lives after the table. */
	table[4 * 3 + 0] = end + 12 + WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS * 4;

	write_trailer(body, end, WHEEL_EXT_VERSION, WHEEL_EXT_SLOTS, table);
	/* The bytes between the model end and N are the pad. */
	CHECK(body[47] == 0xA5 || end == 48);

	CHECK(WheelExtRead(body, (int)sizeof(body), end, got, &why) == 1);
	CHECK(why == NULL);
	CHECK(got[0] == 16);
	CHECK(got[1] == -1);
	CHECK(got[2] == 40);
	CHECK(got[2 * 3 + 0] == -1);
	CHECK(got[4 * 3 + 0] == end + 168);
	CHECK(got[4 * 3 + 1] == -1);
	CHECK(got[4 * 3 + 2] == -1);
	CHECK(body[end] == 0x57 && body[end + 1] == 0x48 &&
		body[end + 2] == 0x4C && body[end + 3] == 0x58);

	for (i = 0; i < WHEEL_EXT_SLOTS; i++)
	{
		if (i == 0 || i == 4)
			continue;

		CHECK(got[i * 3] == -1 && got[i * 3 + 1] == -1 && got[i * 3 + 2] == -1);
	}
}

static void test_reject(void)
{
	unsigned char body[400];
	int v1[3];
	int table[WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS];
	int got[WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS];
	int end, i;
	const char* why = NULL;

	memset(body, 0, sizeof(body));
	v1[0] = -1;
	v1[1] = -1;
	v1[2] = -1;
	write_v1(body, 3, v1);
	end = WheelExtV1End(body, (int)sizeof(body), NULL, NULL);
	CHECK(end == 16);

	for (i = 0; i < WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS; i++)
		table[i] = -1;

	write_trailer(body, end, 2, WHEEL_EXT_SLOTS, table);
	CHECK(WheelExtRead(body, (int)sizeof(body), end, got, &why) == -1);
	CHECK(why != NULL && strstr(why, "version") != NULL);

	why = NULL;
	write_trailer(body, end, 1, 12, table);
	CHECK(WheelExtRead(body, (int)sizeof(body), end, got, &why) == -1);

	why = NULL;
	write_trailer(body, end, 1, WHEEL_EXT_SLOTS, table);
	CHECK(WheelExtRead(body, end + 20, end, got, &why) == -1);
	CHECK(why != NULL && strstr(why, "table") != NULL);

	why = NULL;
	table[7] = -2;
	write_trailer(body, end, 1, WHEEL_EXT_SLOTS, table);
	CHECK(WheelExtRead(body, (int)sizeof(body), end, got, &why) == -1);
	CHECK(why != NULL && strstr(why, "offset") != NULL);

	why = NULL;
	table[7] = 9999;
	write_trailer(body, end, 1, WHEEL_EXT_SLOTS, table);
	CHECK(WheelExtRead(body, (int)sizeof(body), end, got, &why) == -1);

	why = NULL;
	table[7] = -1;
	write_trailer(body, end, 1, WHEEL_EXT_SLOTS, table);
	body[end] = 0;
	CHECK(WheelExtRead(body, (int)sizeof(body), end, got, &why) == 0);

	CHECK(WheelExtV1End(body, 4, NULL, NULL) == -1);
	put_i32(body, 0);
	CHECK(WheelExtV1End(body, 16, NULL, NULL) == -1);
	CHECK(WheelExtRead(NULL, 16, 0, NULL, NULL) == 0);
}

int main(void)
{
	test_v1_only();
	test_absent_slots_and_one_owned();
	test_reject();

	if (gFails != 0)
	{
		printf("%d of %d checks failed\n", gFails, gChecks);
		return 1;
	}

	printf("%d checks passed\n", gChecks);
	return 0;
}
