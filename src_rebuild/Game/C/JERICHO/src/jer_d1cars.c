/* jer_d1cars.c -- find the baked Driver 1 car data, wherever it already is.
 *
 * BAKE_D1_CARS writes JERICHO/MODS/carhacks/D1CARS/<CITY>.LEV and the engine
 * used to read only that folder, and to call a bake "done" by its BAKE_VERSION
 * stamp. Two things went wrong with that:
 *
 *  - every playtest install is a fresh package (D1CARS is stripped from
 *    packages) with the Driver 1 disc copied in, so it had no carhacks/D1CARS and
 *    looked unbaked even when a good bake sat right next to it: in the legacy
 *    DRIVER/D1CARS (pre-move bakes, still copied along with DRIVER/), or in a
 *    sibling install such as "JERICHO Release";
 *  - the legacy folder counted as "baked" for the prompt, but the engine never
 *    loaded from it, so the cars still did not register.
 *
 * So the bake is now found from the LEV FILES, in this order:
 *
 *   0  JERICHO/MODS/carhacks/D1CARS          (where a bake writes)
 *   1  CONFIG/jericho.ini d1cars_path        (optional, unset by default: a
 *                                             shared folder that holds D1CARS,
 *                                             or the D1CARS folder itself;
 *                                             absolute, or relative to the
 *                                             game folder)
 *   2  DRIVER/D1CARS                          (legacy, pre-move bakes)
 *   3  ../<sibling>/JERICHO/MODS/carhacks/D1CARS for every folder beside this
 *      install (another JERICHO install)
 *
 * A LEV counts only when the car importer could read it (jer_d1cars_lev_valid:
 * the citylump table, DATA1 inside the file, and a LUMP_CAR_MODELS segment with
 * at least one model). The chosen folder is the one with the newest BAKE_VERSION
 * (an unstamped valid bake is 0), then the most valid LEVs, then the earliest in
 * the order above - so a local bake wins whenever it is as good as the others,
 * and a fresh re-bake (which writes slot 0 at the tooling's version) always wins.
 *
 * The engine loads from the chosen folder (system.c sets gD1DataFolder from
 * jer_d1cars_root and maps bank paths spelled for slot 0 onto it), and the bake
 * prompt asks about the chosen folder's version. Nothing is copied or written.
 *
 * Runtime code: plain C, no game headers (paths are relative to the game folder,
 * as jer_root_dir is). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#include "../include/jericho.h"
#include "../include/jer_config.h"
#include "../include/jer_rebake.h"

#define D1_PATH_MAX		1024
#define D1_SIBLINGS_MAX		64	/* folders beside the install looked at */
#define D1_DATA1_MAX		(4 * 1024 * 1024)	/* the importer's own cap (models.c) */
#define D1_LUMP_CAR_MODELS	28	/* models.c CAR_IMPORT_LUMP_MODELS */
#define D1_MODEL_SLOTS		13	/* models.c CAR_IMPORT_MODEL_SLOTS */

/* the files a bake writes; the first seven are the stock rows (system.c), the
 * last is the Euro-43 bank (carhacks / cities.ini) */
static const char* const sD1Cities[] =
{
	"MIAMI", "FRISCO", "LA", "NEWYORK", "NEWCASTLE", "IVIEW", "TRAIN", "BETAMIAMI"
};
#define D1_CITY_COUNT	((int)(sizeof(sD1Cities) / sizeof(sD1Cities[0])))
#define D1_BETA_INDEX	7

static char sRoot[JER_D1CARS_ROOT_MAX];	/* engine-style folder holding D1CARS */
static int sVersion = -1;		/* chosen folder's BAKE_VERSION, -1 = none */
static int sValid;			/* chosen folder's valid LEV count */
static unsigned sValidMask;		/* which of sD1Cities are valid there */
static int sScanned;

static unsigned D1Le32(const unsigned char* p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

int jer_d1cars_lev_valid(const char* path)
{
	unsigned char hdr[8 + 32];
	unsigned char* data1;
	unsigned data1Off, data1Size;
	long fileSize;
	FILE* f;
	int ok = 0;

	if (path == NULL || path[0] == 0)
		return 0;

	f = fopen(path, "rb");
	if (f == NULL)
		return 0;

	/* a lump header, then the citylump table: (offset, size) pairs, DATA1 first
	 * (models.c JerLoadCarImportFromFile reads exactly this) */
	if (fseek(f, 0, SEEK_END) != 0 || (fileSize = ftell(f)) < (long)sizeof(hdr) ||
		fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return 0;
	}

	data1Off = D1Le32(hdr + 8);
	data1Size = D1Le32(hdr + 12);

	if (data1Off == 0 || data1Size <= 8 || data1Size > D1_DATA1_MAX ||
		(unsigned long)data1Off + data1Size > (unsigned long)fileSize)
	{
		fclose(f);
		return 0;
	}

	data1 = (unsigned char*)malloc(data1Size);
	if (data1 == NULL)
	{
		fclose(f);
		return 0;
	}

	if (fseek(f, (long)data1Off, SEEK_SET) == 0 && fread(data1, 1, data1Size, f) == data1Size)
	{
		/* DATA1's body is a container: segments (type, size, body) from 8 bytes in */
		unsigned off = 8;

		while (off + 8 <= data1Size)
		{
			unsigned type = D1Le32(data1 + off);
			unsigned segSize = D1Le32(data1 + off + 4);

			if (segSize > data1Size - off - 8)
				break;

			if (type == D1_LUMP_CAR_MODELS)
			{
				/* 4 bytes, then 13 x (clean, damaged, low) offsets; a model is
				 * there when its clean offset is not -1 and lands inside */
				const unsigned char* body = data1 + off + 8;
				int m;

				for (m = 0; m < D1_MODEL_SLOTS && 4 + (unsigned)(m * 12 + 4) <= segSize; m++)
				{
					unsigned clean = D1Le32(body + 4 + m * 12);

					if (clean != 0xffffffffu && clean < segSize)
					{
						ok = 1;
						break;
					}
				}

				break;
			}

			off += 8 + ((segSize + 3) & ~3u);
		}
	}

	free(data1);
	fclose(f);
	return ok;
}

/* BAKE_VERSION in a folder: the first number in the file, -1 when absent */
static int D1StampIn(const char* dir)
{
	char path[D1_PATH_MAX];
	char buf[64];
	size_t n;
	char* p;
	FILE* f;

	snprintf(path, sizeof(path), "%s/BAKE_VERSION", dir);
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;

	for (p = buf; *p != 0 && !isdigit((unsigned char)*p); p++)
		;
	return *p ? atoi(p) : -1;
}

typedef struct D1_CANDIDATE
{
	char root[JER_D1CARS_ROOT_MAX];	/* engine-style, holds D1CARS, ends in '\\' */
	char dir[D1_PATH_MAX];		/* the D1CARS folder, '/' separators */
	int valid;
	unsigned mask;
	int version;
} D1_CANDIDATE;

/* "a/b\\c" -> "a\\b\\c\\": the engine's table style (FS_FixPathSlashes undoes it
 * on Linux), always ending in a separator */
static int D1EngineRoot(const char* fsRoot, char* out, int outSize)
{
	int i, n = 0;

	for (i = 0; fsRoot[i] != 0; i++)
	{
		if (n >= outSize - 2)
			return 0;
		out[n++] = (fsRoot[i] == '/') ? '\\' : fsRoot[i];
	}

	if (n > 0 && out[n - 1] != '\\')
		out[n++] = '\\';

	out[n] = 0;
	return 1;
}

/* Look at one folder that may HOLD a D1CARS folder (fsRoot, '/' or '\\'
 * separators, "" = the game folder). Fills c and answers its valid LEV count. */
static int D1Probe(const char* fsRoot, D1_CANDIDATE* c)
{
	char path[D1_PATH_MAX + 32];
	int i, len;

	memset(c, 0, sizeof(*c));
	c->version = -1;

	len = (int)strlen(fsRoot);
	if (len > 0 && (fsRoot[len - 1] == '/' || fsRoot[len - 1] == '\\'))
		len--;

	if (len + 8 >= (int)sizeof(c->dir) || !D1EngineRoot(fsRoot, c->root, sizeof(c->root)))
		return 0;	/* too long for the engine's path buffers: not usable */

	snprintf(c->dir, sizeof(c->dir), "%.*s%sD1CARS", len, fsRoot, len > 0 ? "/" : "");

	for (i = 0; i < D1_CITY_COUNT; i++)
	{
		snprintf(path, sizeof(path), "%s/%s.LEV", c->dir, sD1Cities[i]);

		if (jer_d1cars_lev_valid(path))
		{
			c->valid++;
			c->mask |= 1u << i;
		}
	}

	if (c->valid > 0)
	{
		c->version = D1StampIn(c->dir);
		if (c->version < 0)
			c->version = 0;		/* unstamped: older than any stamped bake */
	}

	return c->valid;
}

/* better = newer bake, then more valid LEVs; equal keeps the earlier one */
static int D1Better(const D1_CANDIDATE* a, const D1_CANDIDATE* b)
{
	if (a->valid <= 0)
		return 0;
	if (b->valid <= 0)
		return 1;
	if (a->version != b->version)
		return a->version > b->version;
	return a->valid > b->valid;
}

/* the canonical spelling a bank or stock row uses; also the answer when nothing is found */
#define D1_CANON_FS	"/MODS/carhacks"

static void D1Consider(const char* label, const char* fsRoot, D1_CANDIDATE* best, int* bestFrom, int from)
{
	D1_CANDIDATE c;

	if (D1Probe(fsRoot, &c) > 0)
	{
		jer_log("[d1cars] %s: %s has %d valid LEV(s), BAKE_VERSION %d\n", label, c.dir, c.valid, c.version);

		if (D1Better(&c, best))
		{
			*best = c;
			*bestFrom = from;
		}
	}
}

/* is `dir` (relative to the game folder) this install's own folder? */
static int D1IsSelf(const char* dir)
{
#if defined(_WIN32)
	char a[MAX_PATH], b[MAX_PATH];

	if (GetFullPathNameA(dir, MAX_PATH, a, NULL) == 0 || GetFullPathNameA(".", MAX_PATH, b, NULL) == 0)
		return 0;
	return _stricmp(a, b) == 0;
#else
	struct stat a, b;

	if (stat(dir, &a) != 0 || stat(".", &b) != 0)
		return 0;
	return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
}

static void D1ConsiderSibling(const char* name, D1_CANDIDATE* best, int* bestFrom)
{
	char inst[D1_PATH_MAX];
	char fsRoot[D1_PATH_MAX];

	if (name[0] == '.')
		return;

	snprintf(inst, sizeof(inst), "../%s", name);
	if (D1IsSelf(inst))
		return;

	snprintf(fsRoot, sizeof(fsRoot), "../%s/JERICHO/MODS/carhacks", name);
	D1Consider("sibling install", fsRoot, best, bestFrom, 3);
}

static void D1ScanSiblings(D1_CANDIDATE* best, int* bestFrom)
{
	int seen = 0;
#if defined(_WIN32)
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA("..\\*", &fd);

	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && seen++ < D1_SIBLINGS_MAX)
			D1ConsiderSibling(fd.cFileName, best, bestFrom);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR* d = opendir("..");
	struct dirent* e;

	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL && seen < D1_SIBLINGS_MAX)
	{
		char p[D1_PATH_MAX];
		struct stat st;

		snprintf(p, sizeof(p), "../%s", e->d_name);
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
		{
			seen++;
			D1ConsiderSibling(e->d_name, best, bestFrom);
		}
	}
	closedir(d);
#endif
}

int jer_d1cars_scan(void)
{
	static const char* const kFrom[] = { "carhacks", "config", "legacy", "sibling" };
	char fsRoot[D1_PATH_MAX];
	D1_CANDIDATE best;
	int bestFrom = -1;
	const char* shared;

	memset(&best, 0, sizeof(best));
	best.version = -1;

	/* 0: where a bake writes */
	snprintf(fsRoot, sizeof(fsRoot), "%s" D1_CANON_FS, jer_root_dir());
	D1Consider("carhacks", fsRoot, &best, &bestFrom, 0);

	/* 1: an optional shared folder (jericho.ini d1cars_path; read, never written) */
	shared = jer_config_get_str("jericho", JER_D1CARS_PATH_KEY, "");
	if (shared != NULL && shared[0] != 0)
	{
		int n = (int)strlen(shared);

		snprintf(fsRoot, sizeof(fsRoot), "%s", shared);

		/* the D1CARS folder itself is accepted too: use the folder above it */
		while (n > 0 && (fsRoot[n - 1] == '/' || fsRoot[n - 1] == '\\'))
			fsRoot[--n] = 0;
		if (n >= 6 && (n == 6 || fsRoot[n - 7] == '/' || fsRoot[n - 7] == '\\') &&
			strncmp(fsRoot + n - 6, "D1CARS", 6) == 0)
			fsRoot[n - 6] = 0;

		D1Consider("config d1cars_path", fsRoot, &best, &bestFrom, 1);
	}

	/* 2: a bake from before the move, beside DRIVER2 */
	snprintf(fsRoot, sizeof(fsRoot), "%s/../DRIVER", jer_root_dir());
	D1Consider("legacy", fsRoot, &best, &bestFrom, 2);

	/* 3: another install beside this one */
	D1ScanSiblings(&best, &bestFrom);

	sScanned = 1;

	if (bestFrom < 0)
	{
		snprintf(fsRoot, sizeof(fsRoot), "%s" D1_CANON_FS, jer_root_dir());
		D1EngineRoot(fsRoot, sRoot, sizeof(sRoot));
		sVersion = -1;
		sValid = 0;
		sValidMask = 0;
		jer_log("[d1cars] no baked Driver 1 cars found (carhacks/D1CARS, d1cars_path, DRIVER/D1CARS, sibling installs)\n");
		return 0;
	}

	memcpy(sRoot, best.root, sizeof(sRoot));
	sVersion = best.version;
	sValid = best.valid;
	sValidMask = best.mask;

	jer_log("[d1cars] using %s (%s): %d valid LEV(s), BAKE_VERSION %d%s\n",
		best.dir, kFrom[bestFrom], best.valid, best.version,
		(best.mask & (1u << D1_BETA_INDEX)) ? ", Beta Miami included" : "");

	return sValid;
}

const char* jer_d1cars_root(void)
{
	if (!sScanned)
		jer_d1cars_scan();
	return sRoot;
}

int jer_d1cars_baked_version(void)
{
	if (!sScanned)
		jer_d1cars_scan();
	return sVersion;
}

int jer_d1cars_has(const char* city)
{
	int i;

	if (!sScanned)
		jer_d1cars_scan();

	for (i = 0; city != NULL && i < D1_CITY_COUNT; i++)
	{
		if (strcmp(sD1Cities[i], city) == 0)
			return (sValidMask >> i) & 1u;
	}

	return 0;
}
