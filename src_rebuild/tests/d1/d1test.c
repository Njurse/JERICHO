/* d1test.c — HEADLESS Driver 1 validation harness.
 *
 * Exercises the REAL jer_d1 parsing library (included below) against the
 * actual Driver 1 .LEV files, with no window / no SDL / no controller.
 * Run from src_rebuild/bin/Release_dev (where the DRIVER\ data folder sits):
 *
 *     ..\..\tests\d1\d1test.bat
 *
 * Expected output: every "PASS" (failures print "FAIL" and a non-zero exit).
 *
 * The format expectations come from hex-verified probes of the extracted
 * Driver 1 (PS1) assets — see docs/driver1-cities.md §3-4.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jer_d1.h"

static int gFails = 0;

#define CHECK(cond, fmt, ...) do { \
	if (cond) { printf("PASS: " fmt "\n", ##__VA_ARGS__); } \
	else { printf("FAIL: " fmt "\n", ##__VA_ARGS__); gFails++; } \
} while (0)

/* ------------------------------------------------------------------ */
/* the library under test (real implementation, real headers)         */
/* ------------------------------------------------------------------ */
#include "../../Game/C/jer_d1.c"

extern void d1stubs_init(void);

/* ------------------------------------------------------------------ */
/* level-file helpers (mirror SetCityType's header read)              */
/* ------------------------------------------------------------------ */

static int ReadCityLumps(const char* path, int cl[4][2])
{
	FILE* f = fopen(path, "rb");
	unsigned char head[8];
	int i;

	if (f == NULL)
		return 0;

	if (fread(head, 1, 8, f) != 8)
	{
		fclose(f);
		return 0;
	}

	/* lump type must be 37 (LUMPDESC) */
	if (head[0] != 37 || head[1] != 0 || head[2] != 0 || head[3] != 0)
	{
		fclose(f);
		return 0;
	}

	for (i = 0; i < 4; i++)
	{
		unsigned char b[8];

		if (fread(b, 1, 8, f) != 8)
		{
			fclose(f);
			return 0;
		}

		cl[i][0] = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
		cl[i][1] = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
	}

	fclose(f);
	return 1;
}

/* walk one section (loadtime or inmem) with the D1 lump count and return
 * the lump types in order; maxLumps bounds the output */
static int WalkSection(const char* path, int ofs, int* types, int maxLumps)
{
	FILE* f = fopen(path, "rb");
	int count;
	int n = 0;
	int pos;

	if (f == NULL)
		return -1;

	fseek(f, ofs + 8, SEEK_SET);	/* skip the section-wrapper lump header */
	fread(&count, 4, 1, f);

	if (count < 1 || count > 255)
	{
		fclose(f);
		return -2;
	}

	pos = ofs + 12;

	while (n < count && n < maxLumps)
	{
		unsigned char h[8];
		int type;
		int size;

		fseek(f, pos, SEEK_SET);

		if (fread(h, 1, 8, f) != 8)
			break;

		type = h[0] | (h[1] << 8) | (h[2] << 16) | (h[3] << 24);
		size = h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24);

		types[n++] = type;
		pos = pos + 8 + ((size + 3) & ~3);
	}

	fclose(f);
	return n;
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static void TestCity(const char* city, int expectedCl[4][2],
	int expectedMap[5], int expectedLoadtime[16], int expectedInmem[16])
{
	char path[96];
	int cl[4][2];
	int types[16];
	int n;
	int i;

	sprintf(path, "DRIVER\\LEVELS\\%s.LEV", city);

	/* 1. header + citylumps */
	CHECK(ReadCityLumps(path, cl), "%s: LEV header (lump 37 + citylumps) readable", city);

	if (ReadCityLumps(path, cl))
	{
		for (i = 0; i < 4; i++)
		{
			if (cl[i][0] != expectedCl[i][0] || cl[i][1] != expectedCl[i][1])
			{
				CHECK(0, "%s: citylump[%d] = (%d,%d), expected (%d,%d)",
					city, i, cl[i][0], cl[i][1], expectedCl[i][0], expectedCl[i][1]);
				break;
			}
		}
		CHECK(i == 4, "%s: citylump offsets match", city);
	}

	/* 2. loadtime lump walk */
	n = WalkSection(path, cl[0][0], types, 16);
	CHECK(n == expectedLoadtime[0], "%s: loadtime lump count = %d (expected %d)", city, n, expectedLoadtime[0]);

	if (n == expectedLoadtime[0])
	{
		int ok = 1;

		for (i = 0; i < n; i++)
		{
			if (types[i] != expectedLoadtime[i + 1])
			{
				ok = 0;
				break;
			}
		}

		CHECK(ok, "%s: loadtime lump types [5,34,12,7,25,2,22x4,28]", city);
	}

	/* 3. inmem lump walk */
	n = WalkSection(path, cl[2][0], types, 16);
	CHECK(n == expectedInmem[0], "%s: inmem lump count = %d (expected %d)", city, n, expectedInmem[0]);

	if (n == expectedInmem[0])
	{
		int ok = 1;

		for (i = 0; i < n; i++)
		{
			if (types[i] != expectedInmem[i + 1])
			{
				ok = 0;
				break;
			}
		}

		CHECK(ok, "%s: inmem lump types [1,26,8,9,16,17,20,10,24,33,21]", city);
	}

	/* 4. map header */
	{
		FILE* f = fopen(path, "rb");
		int map[5];
		unsigned char b[24];

		if (f != NULL)
		{
			/* MAP lump = second lump in loadtime (index 5 of the walk);
			 * locate it by re-walking */
			int pos;
			int count;
			int j;

			fseek(f, cl[0][0] + 8, SEEK_SET);
			fread(&count, 4, 1, f);
			pos = cl[0][0] + 12;

			for (j = 0; j < count; j++)
			{
				unsigned char h[8];
				int type, size;

				fseek(f, pos, SEEK_SET);
				fread(h, 1, 8, f);
				type = h[0] | (h[1] << 8) | (h[2] << 16) | (h[3] << 24);
				size = h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24);

				if (type == 2)	/* LUMP_MAP */
				{
					fread(b, 1, 20, f);
					map[0] = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
					map[1] = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
					map[2] = b[8] | (b[9] << 8) | (b[10] << 16) | (b[11] << 24);
					map[3] = b[12] | (b[13] << 8) | (b[14] << 16) | (b[15] << 24);
					map[4] = b[16] | (b[17] << 8) | (b[18] << 16) | (b[19] << 24);
					break;
				}

				pos += 8 + ((size + 3) & ~3);
			}

			CHECK(map[0] == expectedMap[0] && map[1] == expectedMap[1] &&
				map[2] == expectedMap[2] && map[3] == expectedMap[3] &&
				map[4] == expectedMap[4],
				"%s: map header cells=%dx%d cell=%d regions=%d rsize=%d",
				city, map[0], map[1], map[2], map[3], map[4]);

			/* stash for later use */
			cells_across = map[0];
			cells_down = map[1];
			units_across_halved = map[0] / 2 * map[2];
			units_down_halved = map[1] / 2 * map[2];

			fclose(f);
		}
	}
}

static void TestSpoolinfo(const char* city, int expectedRegions, int expectedSlots[4])
{
	char path[96];
	FILE* f;
	int cl[4][2];
	int count;
	int pos;
	int type = -1;
	int size = 0;
	int i;

	unsigned char lump[4096];
	int n = 0;

	sprintf(path, "DRIVER\\LEVELS\\%s.LEV", city);

	if (!ReadCityLumps(path, cl))
		return;

	f = fopen(path, "rb");
	if (f == NULL)
		return;

	/* find lump 26 (SPOOLINFO) in the inmem section */
	fseek(f, cl[2][0] + 8, SEEK_SET);
	fread(&count, 4, 1, f);
	pos = cl[2][0] + 12;

	for (i = 0; i < count; i++)
	{
		unsigned char h[8];

		fseek(f, pos, SEEK_SET);
		fread(h, 1, 8, f);
		type = h[0] | (h[1] << 8) | (h[2] << 16) | (h[3] << 24);
		size = h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24);

		if (type == 26)
		{
			n = size;
			break;
		}

		pos += 8 + ((size + 3) & ~3);
	}

	CHECK(n > 0 && n <= 4096, "%s: SPOOLINFO lump found (%d bytes)", city, n);

	if (n > 0 && n <= 4096)
	{
		int p = 0;
		int modelBuf, musamb, numAreas;
		int slots[4], objs[4], pvs[4];
		int nOffsets, spoolBytes;

		fseek(f, pos + 8, SEEK_SET);
		fread(lump, 1, n, f);

#define GI() (lump[p] | (lump[p+1] << 8) | (lump[p+2] << 16) | (lump[p+3] << 24)), p += 4

		modelBuf = GI();
		musamb = GI();
		p += musamb;
		numAreas = GI();
		p += numAreas * 16 + numAreas * 16;
		slots[0] = GI(); slots[1] = GI(); slots[2] = GI(); slots[3] = GI();
		objs[0] = GI(); objs[1] = GI(); objs[2] = GI(); objs[3] = GI();
		pvs[0] = GI(); pvs[1] = GI(); pvs[2] = GI(); pvs[3] = GI();
		nOffsets = GI();
		p += nOffsets * 2;
		spoolBytes = GI();
		p += spoolBytes;

#undef GI

		CHECK(nOffsets == expectedRegions,
			"%s: spoolinfo region offsets = %d (map regions %d)", city, nOffsets, expectedRegions);
		CHECK(p == n, "%s: spoolinfo parses to the byte (%d == %d)", city, p, n);
		CHECK(slots[0] == expectedSlots[0] && slots[1] == expectedSlots[1] &&
			slots[2] == expectedSlots[2] && slots[3] == expectedSlots[3],
			"%s: cell slots %d,%d,%d,%d", city, slots[0], slots[1], slots[2], slots[3]);
	}

	fclose(f);
}

/* ------------------------------------------------------------------ */
/* deep region test: spool a few regions like LoadRegionData's D1      */
/* branch, then query the surface                                      */
/* ------------------------------------------------------------------ */

static void TestRegions(const char* city, int numRegions, int expectedAc, int expectedDn)
{
	char path[96];
	FILE* f;
	int cl[4][2];
	int count;
	int pos;
	int i;

	unsigned char lump[4096];
	int p = 0;
	int nOffsets;
	int spoolBytes;
	unsigned char spool[4096];
	int regionIdx[512];
	int regionBarrel[512];
	int numValid = 0;

	sprintf(path, "DRIVER\\LEVELS\\%s.LEV", city);

	if (!ReadCityLumps(path, cl))
		return;

	f = fopen(path, "rb");
	if (f == NULL)
		return;

	/* --- parse spoolinfo (same as TestSpoolinfo) --- */
	fseek(f, cl[2][0] + 8, SEEK_SET);
	fread(&count, 4, 1, f);
	pos = cl[2][0] + 12;

	for (i = 0; i < count; i++)
	{
		unsigned char h[8];
		int type, size;

		fseek(f, pos, SEEK_SET);
		fread(h, 1, 8, f);
		type = h[0] | (h[1] << 8) | (h[2] << 16) | (h[3] << 24);
		size = h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24);

		if (type == 26)
		{
			fseek(f, pos + 8, SEEK_SET);
			fread(lump, 1, size > 4096 ? 4096 : size, f);
			break;
		}

		pos += 8 + ((size + 3) & ~3);
	}

	p = 0;

	/* --- parse spoolinfo: offsets table + Spool table --- */
	{
		int q = 4;
		int musamb = lump[q] | (lump[q+1] << 8) | (lump[q+2] << 16) | (lump[q+3] << 24); q += 4;
		int numAreas;

		q += musamb;

		numAreas = lump[q] | (lump[q+1] << 8) | (lump[q+2] << 16) | (lump[q+3] << 24); q += 4;
		q += numAreas * 32;
		q += 48;	/* slots objs pvs */

		nOffsets = lump[q] | (lump[q+1] << 8) | (lump[q+2] << 16) | (lump[q+3] << 24); q += 4;

		for (i = 0; i < nOffsets && i < 512; i++)
		{
			regionIdx[i] = lump[q] | (lump[q+1] << 8);
			q += 2;
		}

		spoolBytes = lump[q] | (lump[q+1] << 8) | (lump[q+2] << 16) | (lump[q+3] << 24); q += 4;
		memcpy(spool, lump + q, spoolBytes > 4096 ? 4096 : spoolBytes);
	}

	/* --- roadmap lump (7) from loadtime: unit scale 750 for D1 --- */
	{
		unsigned char rl[8];
		int rwidth = 0, rheight = 0;
		int q2 = cl[0][0] + 12;

		fseek(f, cl[0][0] + 8, SEEK_SET);
		fread(&count, 4, 1, f);

		for (i = 0; i < count; i++)
		{
			unsigned char h[8];
			int type, size;

			fseek(f, q2, SEEK_SET);
			fread(h, 1, 8, f);
			type = h[0] | (h[1] << 8) | (h[2] << 16) | (h[3] << 24);
			size = h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24);

			if (type == 7)
			{
				fread(rl, 1, 8, f);
				rwidth = rl[0] | (rl[1] << 8) | (rl[2] << 16) | (rl[3] << 24);
				rheight = rl[4] | (rl[5] << 8) | (rl[6] << 16) | (rl[7] << 24);
				break;
			}

			q2 += 8 + ((size + 3) & ~3);
		}

		roadMapLumpData.width = rwidth;
		roadMapLumpData.height = rheight;
		roadMapLumpData.unitXMid = (rwidth + 1) * 750;
		roadMapLumpData.unitZMid = rheight * 750;

		CHECK(rwidth > 0 && rheight > 0,
			"%s: roadmap lump %dx%d (unit 750)", city, rwidth, rheight);
	}

	/* --- spool one non-empty region per barrel --- */
	{
		int barrel;
		int cellsDone = 0;
		int across = expectedAc / 30;
		int foundBarrels = 0;

		for (barrel = 0; barrel < 4; barrel++)
		{
			int ridx = -1;
			int k;
			int off, pvs, cd0, cd1, cd2, roadm, roadh;
			unsigned char s[12];
			unsigned char buf[64 * 2048];
			int rofs;
			int regionX, regionZ, wantBarrel;

			for (k = 0; k < nOffsets && k < 512; k++)
			{
				if (regionIdx[k] == 0xFFFF)
					continue;

				regionX = k % across;
				regionZ = k / across;
				wantBarrel = (regionX & 1) + (regionZ & 1) * 2;

				if (wantBarrel == barrel)
				{
					ridx = regionIdx[k];
					break;
				}
			}

			if (ridx < 0 || ridx + 12 > spoolBytes)
				continue;

			memcpy(s, spool + ridx, 12);
			off = s[0] | (s[1] << 8);
			pvs = s[4];
			cd0 = s[5]; cd1 = s[6]; cd2 = s[7];
			roadm = s[10]; roadh = s[11];

			if (jer_d1_region_begin(barrel, roadm, roadh, cd2) != 0)
			{
				CHECK(0, "%s: jer_d1_region_begin(barrel %d) failed", city, barrel);
				continue;
			}

			rofs = cl[3][0] + off * 2048;

			/* roadm RLE */
			fseek(f, rofs, SEEK_SET);
			if (roadm > 0)
			{
				fread(buf, 1, roadm * 2048, f);
				memcpy(jer_d1_region_roadm_scratch(barrel), buf, roadm * 2048);
			}

			/* roadh RLE */
			fseek(f, rofs + roadm * 2048, SEEK_SET);
			if (roadh > 0)
			{
				fread(buf, 1, roadh * 2048, f);
				memcpy(jer_d1_region_roadh_scratch(barrel), buf, roadh * 2048);
			}

			/* cell objects (raw 16-byte) */
			fseek(f, rofs + (roadm + roadh + cd1 + cd0) * 2048, SEEK_SET);
			if (cd2 > 0)
			{
				fread(buf, 1, cd2 * 2048, f);
				memcpy(jer_d1_region_cell_object_scratch(barrel), buf, cd2 * 2048);
			}

			jer_d1_region_finish(barrel);

			/* validate the decoded road map */
			{
				const u_int* rm = (const u_int*)jer_d1_region_roadmap(barrel);
				const ushort* sr = (const ushort*)jer_d1_region_surfaceroads(barrel);
				int bad = 0;
				int k;

				if (rm == NULL || sr == NULL)
				{
					CHECK(0, "%s: barrel %d road map missing", city, barrel);
					continue;
				}

				for (k = 0; k < 3600; k++)
				{
					int h = *(const short*)&rm[k];
					int type = (rm[k] >> 16) & 1023;

					if (h > 20000 || h < -20000)
						bad++;
					if (type > 900)
						bad++;
				}

				CHECK(bad == 0, "%s: barrel %d road map cells sane (height/type, bad=%d)", city, barrel, bad);
				cellsDone += 3600;
			}

			/* cell objects were converted into the packed buffer */
			(void)pvs;
			foundBarrels++;
		}

		CHECK(foundBarrels >= 4, "%s: spooled %d/4 barrels", city, foundBarrels);
		CHECK(cellsDone >= 4 * 3600, "%s: 4 barrels road-mapped (%d cells)", city, cellsDone);
	}

	/* --- surface queries inside the spooled regions
	 * (regions 15/16/24/25 for MIAMI: x5-6,z1 and x4-5,z2) --- */
	{
		int samples[][2] = {
			{ 45000, -495000 },	/* region 15 (x5,z1) */
			{ 135000, -495000 },	/* region 16 (x6,z1) */
			{ -45000, -405000 },	/* region 24 (x4,z2) */
			{ 45000, -405000 },	/* region 25 (x5,z2) */
			{ 105000, -405000 },	/* near region 26 (x6,z2) */
		};
		int k;
		int hits = 0;

		for (k = 0; k < (int)(sizeof(samples) / sizeof(samples[0])); k++)
		{
			int nx, ny, nz, y, surface;

			if (jer_d1_find_surface(samples[k][0], 0, samples[k][1],
				&nx, &ny, &nz, &y, &surface) == 1)
			{
				CHECK(y > -30000 && y < 30000, "%s: surface y=%d at (%d,%d)",
					city, y, samples[k][0], samples[k][1]);
				hits++;
			}
		}

		CHECK(hits >= 4, "%s: jer_d1_find_surface hits %d/5 samples", city, hits);
	}

	fclose(f);
}

int main(int argc, char** argv)
{
	int miamiCl[4][2] = { {2048,110592}, {112640,401408}, {514048,176128}, {690176,4370432} };
	int friscoCl[4][2] = { {2048,88064}, {90112,440320}, {530432,231424}, {761856,6502400} };
	int laCl[4][2] = { {2048,92160}, {94208,372736}, {466944,165888}, {632832,4995072} };
	int nyCl[4][2] = { {2048,100352}, {102400,485376}, {587776,243712}, {831488,6023168} };

	int miamiMap[5] = { 300, 420, 3000, 140, 30 };
	int friscoMap[5] = { 540, 540, 3000, 324, 30 };
	int laMap[5] = { 540, 540, 3000, 324, 30 };
	int nyMap[5] = { 480, 480, 3000, 256, 30 };

	int d1Load[16] = { 11, 5, 34, 12, 7, 25, 2, 22, 22, 22, 22, 28 };
	int d1Inmem[16] = { 11, 1, 26, 8, 9, 16, 17, 20, 10, 24, 33, 21 };

	int miamiSlots[4] = { 4096, 4096, 4608, 4608 };
	int friscoSlots[4] = { 2048, 2560, 2560, 3072 };
	int laSlots[4] = { 3584, 3584, 2560, 3072 };
	int nySlots[4] = { 3072, 3072, 2560, 3072 };

	(void)argc;
	(void)argv;

	d1stubs_init();
	setvbuf(stdout, NULL, _IONBF, 0);	/* crash-safe output */

	printf("=== Driver 1 headless validation ===\n\n");

	/* library basics */
	CHECK(strcmp(jer_d1_level_file(0), "LEVELS\\MIAMI.LEV") == 0, "level file table");
	CHECK(strcmp(jer_d1_level_name(2), "Los Angeles") == 0, "level name table");
	CHECK(jer_d1_folder() != NULL && strstr(jer_d1_folder(), "DRIVER") != NULL, "D1 data folder: '%s'", jer_d1_folder());
	CHECK(jer_d1_available() == 1, "D1 assets available (run from bin/Release_dev)");
	{
		int c11 = 11;
		int cneg = -1;

		CHECK(jer_d1_lump_count(&c11) == 11, "lump count ok");
		CHECK(jer_d1_lump_count(&cneg) == -1, "lump count rejects garbage");
	}

	/* city formats */
	TestCity("MIAMI", miamiCl, miamiMap, d1Load, d1Inmem);
	TestCity("FRISCO", friscoCl, friscoMap, d1Load, d1Inmem);
	TestCity("LA", laCl, laMap, d1Load, d1Inmem);
	TestCity("NEWYORK", nyCl, nyMap, d1Load, d1Inmem);

	/* spoolinfo */
	TestSpoolinfo("MIAMI", 140, miamiSlots);
	TestSpoolinfo("FRISCO", 324, friscoSlots);
	TestSpoolinfo("LA", 324, laSlots);
	TestSpoolinfo("NEWYORK", 256, nySlots);

	/* deep region + surface test */
	TestRegions("MIAMI", 140, 300, 420);

	printf("\n=== %s (%d failures) ===\n", gFails == 0 ? "ALL PASS" : "FAILURES", gFails);
	return gFails == 0 ? 0 : 1;
}
