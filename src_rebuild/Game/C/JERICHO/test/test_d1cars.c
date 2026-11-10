/* Host-side tests for jer_d1cars.c: finding a Driver 1 bake from its LEV files.
 *
 * Builds a sandbox of installs under a temp folder (this install, a legacy
 * DRIVER/D1CARS, a sibling "JERICHO Release", a shared folder) with synthetic
 * LEVs, chdirs into this install the way the game runs, and checks what the
 * scan picks. With D1_REAL_DIR set it also checks real baked LEVs.
 * This directory is excluded from the game build (premake5.lua):
 *
 *   gcc -std=c99 -Wall -Wextra -I src_rebuild/Game/C/JERICHO/include \
 *       -o /tmp/test_d1cars src_rebuild/Game/C/JERICHO/test/test_d1cars.c \
 *       src_rebuild/Game/C/JERICHO/src/jer_d1cars.c && \
 *   D1_REAL_DIR=/path/to/D1CARS /tmp/test_d1cars
 *
 * POSIX only (mkdtemp/chdir); the scan itself is the same code on Windows.
 * Exit status 0 = every check passed. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "jericho.h"
#include "jer_config.h"
#include "jer_rebake.h"

static int gFails, gChecks;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the three runtime calls jer_d1cars.c makes ---- */
static char gConfigPath[1024];

void jer_log(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("  log: ");
	vprintf(fmt, ap);
	va_end(ap);
}

const char* jer_root_dir(void)
{
	return "JERICHO";
}

const char* jer_config_get_str(const char* mod, const char* key, const char* def)
{
	if (strcmp(mod, "jericho") == 0 && strcmp(key, JER_D1CARS_PATH_KEY) == 0 && gConfigPath[0])
		return gConfigPath;
	return def;
}

/* ---- sandbox ---- */
static void Put32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

enum { LEV_GOOD, LEV_NO_MODELS, LEV_EMPTY_TABLE, LEV_SHORT, LEV_JUNK };

/* the importer's layout: lump header, citylump table, DATA1 = 8 bytes then
 * segments; LUMP_CAR_MODELS = 4 bytes + 13 x 3 offsets */
static void WriteLev(const char* path, int kind)
{
	unsigned char buf[2048];
	unsigned data1 = 40, seg = data1 + 8, i;
	unsigned modelsSize = 4 + 13 * 12;
	unsigned total;
	FILE* f;

	memset(buf, 0, sizeof(buf));

	/* a padding segment first, so the walk has to step */
	Put32(buf + seg, 99); Put32(buf + seg + 4, 6);
	seg += 8 + 8;
	Put32(buf + seg, 28); Put32(buf + seg + 4, modelsSize);
	for (i = 0; i < 13; i++)
		Put32(buf + seg + 8 + 4 + i * 12, 0xffffffffu);
	if (kind != LEV_EMPTY_TABLE)
		Put32(buf + seg + 8 + 4 + 5 * 12, 64);	/* model 5 is there */
	if (kind == LEV_NO_MODELS)
		Put32(buf + seg, 27);
	seg += 8 + modelsSize;

	Put32(buf + 8, data1);
	Put32(buf + 12, seg - data1);
	total = seg + 64;

	if (kind == LEV_SHORT)
		total = seg - 20;	/* DATA1 runs past the end */
	if (kind == LEV_JUNK)
		memset(buf, 0x41, sizeof(buf));

	f = fopen(path, "wb");
	fwrite(buf, 1, total, f);
	fclose(f);
}

static void MkdirP(const char* path)
{
	char tmp[1024];
	char* p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++)
	{
		if (*p == '/')
		{
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static const char* const kStock[] = { "MIAMI", "FRISCO", "LA", "NEWYORK", "NEWCASTLE", "IVIEW", "TRAIN" };

/* a D1CARS folder with the stock seven (+ BETAMIAMI) and a stamp (-1 = none) */
static void MakeBake(const char* dir, int stamp, int beta, int kind)
{
	char path[1024];
	int i;

	MkdirP(dir);
	for (i = 0; i < 7; i++)
	{
		snprintf(path, sizeof(path), "%s/%s.LEV", dir, kStock[i]);
		WriteLev(path, kind);
	}
	if (beta)
	{
		snprintf(path, sizeof(path), "%s/BETAMIAMI.LEV", dir);
		WriteLev(path, kind);
	}
	if (stamp >= 0)
	{
		FILE* f;
		snprintf(path, sizeof(path), "%s/BAKE_VERSION", dir);
		f = fopen(path, "w");
		fprintf(f, "%d\n", stamp);
		fclose(f);
	}
}

static void RmRf(const char* path)
{
	char cmd[1200];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", path);
}

int main(void)
{
	char base[] = "/tmp/d1cars_XXXXXX";
	char self[1100], path[1200];
	const char* real = getenv("D1_REAL_DIR");

	if (mkdtemp(base) == NULL)
		return 2;

	/* ---- the validator ---- */
	snprintf(path, sizeof(path), "%s/good.LEV", base);	WriteLev(path, LEV_GOOD);	CHECK(jer_d1cars_lev_valid(path) == 1);
	snprintf(path, sizeof(path), "%s/nomodels.LEV", base);	WriteLev(path, LEV_NO_MODELS);	CHECK(jer_d1cars_lev_valid(path) == 0);
	snprintf(path, sizeof(path), "%s/empty.LEV", base);	WriteLev(path, LEV_EMPTY_TABLE);	CHECK(jer_d1cars_lev_valid(path) == 0);
	snprintf(path, sizeof(path), "%s/short.LEV", base);	WriteLev(path, LEV_SHORT);	CHECK(jer_d1cars_lev_valid(path) == 0);
	snprintf(path, sizeof(path), "%s/junk.LEV", base);	WriteLev(path, LEV_JUNK);	CHECK(jer_d1cars_lev_valid(path) == 0);
	snprintf(path, sizeof(path), "%s/absent.LEV", base);	CHECK(jer_d1cars_lev_valid(path) == 0);
	CHECK(jer_d1cars_lev_valid(NULL) == 0);
	CHECK(jer_d1cars_lev_valid("") == 0);

	if (real != NULL && real[0])
	{
		int i, n = 0;
		for (i = 0; i < 7; i++)
		{
			snprintf(path, sizeof(path), "%s/%s.LEV", real, kStock[i]);
			if (jer_d1cars_lev_valid(path))
				n++;
			else
				printf("  real %s is not valid\n", path);
		}
		printf("  real bake %s: %d/7 valid\n", real, n);
		CHECK(n == 7);
	}

	/* ---- the scan, from inside an install ---- */
	snprintf(self, sizeof(self), "%s/JERICHO Playtest 0.23.4", base);
	snprintf(path, sizeof(path), "%s/JERICHO/MODS/carhacks", self);
	MkdirP(path);
	if (chdir(self) != 0)
		return 2;

	/* 1. nothing anywhere */
	CHECK(jer_d1cars_scan() == 0);
	CHECK(jer_d1cars_baked_version() == -1);
	CHECK(strcmp(jer_d1cars_root(), "JERICHO\\MODS\\carhacks\\") == 0);
	CHECK(jer_d1cars_has("MIAMI") == 0);

	/* 2. a stamp over junk is not a bake (the old check said it was) */
	MakeBake("DRIVER/D1CARS", 3, 0, LEV_JUNK);
	CHECK(jer_d1cars_scan() == 0);
	CHECK(jer_d1cars_baked_version() == -1);

	/* 3. the legacy bake every playtest install carries: v3, the seven */
	MakeBake("DRIVER/D1CARS", 3, 0, LEV_GOOD);
	CHECK(jer_d1cars_scan() == 7);
	CHECK(jer_d1cars_baked_version() == 3);
	CHECK(strcmp(jer_d1cars_root(), "JERICHO\\..\\DRIVER\\") == 0);
	CHECK(jer_d1cars_has("MIAMI") == 1);
	CHECK(jer_d1cars_has("BETAMIAMI") == 0);

	/* 4. a newer sibling ("JERICHO Release": v4 with Beta Miami) wins */
	snprintf(path, sizeof(path), "%s/JERICHO Release/JERICHO/MODS/carhacks/D1CARS", base);
	MakeBake(path, 4, 1, LEV_GOOD);
	CHECK(jer_d1cars_scan() == 8);
	CHECK(jer_d1cars_baked_version() == 4);
	CHECK(strcmp(jer_d1cars_root(), "..\\JERICHO Release\\JERICHO\\MODS\\carhacks\\") == 0);
	CHECK(jer_d1cars_has("BETAMIAMI") == 1);

	/* the root + "D1CARS\\MIAMI.LEV" is a path the game can open from here */
	{
		char p[512];
		char* c;
		snprintf(p, sizeof(p), "%sD1CARS\\MIAMI.LEV", jer_d1cars_root());
		for (c = p; *c; c++)
			if (*c == '\\')
				*c = '/';
		CHECK(jer_d1cars_lev_valid(p) == 1);
	}

	/* 5. this install's own bake wins a tie with the sibling (same version, same
	 * count), and a fresh local bake (the tooling's version) always wins */
	MakeBake("JERICHO/MODS/carhacks/D1CARS", 4, 1, LEV_GOOD);
	CHECK(jer_d1cars_scan() == 8);
	CHECK(strcmp(jer_d1cars_root(), "JERICHO\\MODS\\carhacks\\") == 0);
	MakeBake("JERICHO/MODS/carhacks/D1CARS", 11, 0, LEV_GOOD);
	snprintf(path, sizeof(path), "JERICHO/MODS/carhacks/D1CARS/BETAMIAMI.LEV");
	remove(path);
	CHECK(jer_d1cars_scan() == 7);
	CHECK(jer_d1cars_baked_version() == 11);
	CHECK(strcmp(jer_d1cars_root(), "JERICHO\\MODS\\carhacks\\") == 0);
	CHECK(jer_d1cars_has("BETAMIAMI") == 0);

	/* 6. the shared d1cars_path, absolute, naming the D1CARS folder itself */
	snprintf(path, sizeof(path), "%s/shared/D1CARS", base);
	MakeBake(path, 12, 1, LEV_GOOD);
	snprintf(gConfigPath, sizeof(gConfigPath), "%s/shared/D1CARS/", base);
	CHECK(jer_d1cars_scan() == 8);
	CHECK(jer_d1cars_baked_version() == 12);
	snprintf(path, sizeof(path), "%s/shared/", base);
	{
		/* engine style: backslashes */
		char want[1200];
		char* c;
		snprintf(want, sizeof(want), "%s", path);
		for (c = want; *c; c++)
			if (*c == '/')
				*c = '\\';
		CHECK(strcmp(jer_d1cars_root(), want) == 0);
	}
	/* ...or the folder holding it, relative to the game folder */
	snprintf(gConfigPath, sizeof(gConfigPath), "../shared");
	CHECK(jer_d1cars_scan() == 8);
	CHECK(strcmp(jer_d1cars_root(), "..\\shared\\") == 0);
	gConfigPath[0] = 0;

	/* 7. our own install is not its own sibling: with only the local bake, the
	 * log names it once, as carhacks */
	snprintf(path, sizeof(path), "%s/JERICHO Release", base);
	RmRf(path);
	RmRf("DRIVER");
	CHECK(jer_d1cars_scan() == 7);
	CHECK(strcmp(jer_d1cars_root(), "JERICHO\\MODS\\carhacks\\") == 0);

	if (chdir("/") != 0)
		return 2;
	RmRf(base);

	printf("%d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
}
