/* paletteedit.c — live car-palette editing for Caine's Crossfire.
 *
 * Why this exists
 * ---------------
 * Working out WHICH palette entry paints WHICH part of a car is not something that can be
 * reasoned out of the data - a car model's polys each carry their own CLUT reference, and
 * the palette rows are shared. The quick way is to change an entry and look at the car.
 * This file is the game side of that: it applies a small override file to live CLUT rows,
 * and it can dump what the live rows actually are.
 *
 * The engine does the hard part. `JerichoClutWriteInPlace` (texture.h) rewrites a CLUT row
 * at the address the CLUT id already names - an IMMEDIATE VRAM write that costs no CLUT
 * strip row, so it works even with the strip full (VRAM.md 3). That is the mechanism
 * objanim.c ColourCycle has used at runtime for years, just aimed at car palettes.
 *
 *   JERICHO/CONFIG/cc_palette.txt, one override per line, '#' comments:
 *
 *     set <row> <texid> <palette> <entry> <rrggbb>   # one CLUT entry
 *     solo <row> <texid> <palette> <entry>           # everything else black (find a part)
 *     flash <row> <texid> <palette> <entry>          # white/black blink every 15 frames
 *     clear                                          # everything back as it was
 *
 * `row` is a civ_clut row (0..7 the level's own car slots, 8..15 a cross-city import's),
 * `texid` 0..31, `palette` 0..5 (car paint variant), `entry` 0..15 within the CLUT.
 *
 * In-place edits are GLOBAL: every car that resolves to the same CLUT id samples that row.
 * That is the point for finding parts out, and the reason an AUTHORED colour (one car,
 * one colour) is a different mechanism - it needs its own row, allocated by
 * JerichoMakeClutRow. See PALETTES.md.
 *
 * The file is re-read while you play (every CC_PAL_REFRESH frames, by mtime+size), so the
 * companion editor (tools/paletteedit.py) can drive this live.
 */

#include "driver2.h"
#include "cars.h"
#include "texture.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"

#include "cainescrossfire.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#endif

// ---------------------------------------------------------------------------
// the engine entry points we build on (texture.h)
// ---------------------------------------------------------------------------

extern int JerichoClutReadInPlace(u_short clut, u_short* out16);
extern int JerichoClutWriteInPlace(u_short clut, const u_short* in16);
extern const char* jer_root_dir(void);

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

#define CC_PAL_MAX_STEPS	256		// overrides in one file
#define CC_PAL_REFRESH		15		// frames between re-reads (~1/2s at 30fps)
#define CC_PAL_DUMP_EVERY	90		// frames between map dumps (~3s at 30fps)

typedef struct {
	int row, texid, palette, entry;
	int action;				// CC_PAL_SET / SOLO / FLASH
	u_short colour;				// packed 5-5-5
} CC_PAL_STEP;

enum { CC_PAL_SET = 0, CC_PAL_SOLO, CC_PAL_FLASH };

static CC_PAL_STEP sSteps[CC_PAL_MAX_STEPS];
static int sCount;
static int sFrame;
static unsigned char sSaved[CC_PAL_MAX_STEPS][16];	// the row as it was, per step
static int sSavedOk[CC_PAL_MAX_STEPS];
static unsigned long sFileStamp;
static int sFlashOn;
static int sClearOnly;			// the file asked for everything to be put back
static int sWantVerify;			// read one edited entry back, once per parse, to prove it landed
static int sApplyNow;			// apply on the next frame, not at the next refresh tick

// ---------------------------------------------------------------------------
// colour words
// ---------------------------------------------------------------------------

// PSX 16bpp: stp | b<<10 | g<<5 | r, five bits a channel, BLUE in the high bits (the
// packing this file's neighbours use - NOT the B<<16|G<<8|R of polygon colour words).
static u_short cd2PalPack(int r, int g, int b)
{
	return (u_short)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

static void cd2PalSplit(u_short c, int* r, int* g, int* b)
{
	*r = (c & 31) << 3;
	*g = ((c >> 5) & 31) << 3;
	*b = ((c >> 10) & 31) << 3;
}

// ---------------------------------------------------------------------------
// resolving a (row, texid, palette) to the CLUT id actually in VRAM
// ---------------------------------------------------------------------------

// civ_clut[carid][texture_id][palette] is the id a GT poly's draw reads - the draw does
// `pciv_clut[(clut_uv0 >> 16) + cp->ap.palette]`, and clut_uv0's high word is
// `carid*192 + texture_id*6` - so `palette` here is that same COLUMN, the same number a
// vehicle profile's `palette` gives. (The palette LUMP writes column `palette + 1`, i.e.
// its own palette 0 lands in column 1; that off-by-one lives in ProcessPalletLumpForCity
// and is deliberately not repeated here.)
static u_short cd2PalClutOf(int row, int texid, int palette)
{
	if (row < 0 || row >= CIV_CLUT_ROWS || texid < 0 || texid >= 32)
		return 0;

	if (palette < 0 || palette >= 6)
		palette = 0;

	return civ_clut[row][texid][palette];
}

// ---------------------------------------------------------------------------
// applying
// ---------------------------------------------------------------------------

static void cd2PalApplyStep(const CC_PAL_STEP* s)
{
	u_short clut;
	u_short entries[16];
	int i;

	clut = cd2PalClutOf(s->row, s->texid, s->palette);

	if (clut == 0)
		return;				// nothing loaded there - not an error, just empty

	// Remember the row the FIRST time we touch it, so `clear` can put it back.
	{
		int idx = (int)(s - sSteps);

		if (idx >= 0 && idx < CC_PAL_MAX_STEPS && !sSavedOk[idx])
		{
			if (JerichoClutReadInPlace(clut, (u_short*)sSaved[idx]))
				sSavedOk[idx] = 1;
		}
	}

	if (!JerichoClutReadInPlace(clut, entries))
		return;

	switch (s->action)
	{
	case CC_PAL_SOLO:
		for (i = 0; i < 16; i++)
			entries[i] = 0;
		entries[s->entry & 15] = 0x7FFF;
		break;

	case CC_PAL_FLASH:
		for (i = 0; i < 16; i++)
			entries[i] = 0;
		entries[s->entry & 15] = sFlashOn ? 0x7FFF : 0;
		break;

	default:
		entries[s->entry & 15] = s->colour;
		break;
	}

	JerichoClutWriteInPlace(clut, entries);
}

// Put every row we touched back the way it was.
static void cd2PalClearAll(void)
{
	int i;

	for (i = 0; i < sCount; i++)
	{
		u_short clut;
		u_short entries[16];

		if (!sSavedOk[i])
			continue;

		clut = cd2PalClutOf(sSteps[i].row, sSteps[i].texid, sSteps[i].palette);

		if (clut == 0)
			continue;

		memcpy(entries, sSaved[i], sizeof(entries));
		JerichoClutWriteInPlace(clut, entries);

		sSavedOk[i] = 0;
	}
}

// ---------------------------------------------------------------------------
// the file
// ---------------------------------------------------------------------------

// A CONTENT hash, not the size. The editor writes while you drag, and the common edit -
// swapping one colour for another - does NOT change the file's length, so a size stamp
// silently misses it (which is exactly what the first live test showed: two different
// colours, same byte count, no re-read). The file is tiny, so hashing it is cheap.
static unsigned long cd2PalStamp(const char* path)
{
	FILE* fp = fopen(path, "rb");
	unsigned long hash = 2166136261ul;
	int c;

	if (fp == NULL)
		return 0;

	while ((c = fgetc(fp)) != EOF)
		hash = (hash ^ (unsigned long)(c & 0xff)) * 16777619ul;

	fclose(fp);

	return hash | 1;	// never 0: 0 means "no file"
}

static int cd2PalParseColour(const char* s)
{
	int v = 0, i;

	for (i = 0; i < 6; i++)
	{
		int c = s[i];

		if (c == 0)
			return -1;

		if (c >= '0' && c <= '9') c -= '0';
		else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
		else if (c >= 'A' && c <= 'F') c -= 'A' - 10;
		else return -1;

		v = (v << 4) | c;
	}

	return v;
}

static int cd2PalParse(void)
{
	char path[256];
	char line[192];
	FILE* fp;
	int n = 0;

	sprintf(path, "%s/CONFIG/cc_palette.txt", jer_root_dir());

	sCount = 0;
	sClearOnly = 0;

	fp = fopen(path, "rb");
	if (fp == NULL)
		return 0;

	while (fgets(line, sizeof(line), fp) != NULL && n < CC_PAL_MAX_STEPS)
	{
		char* s = line;
		char val[8];
		int row, texid, palette, entry;

		while (*s == ' ' || *s == '\t')
			s++;

		if (*s == '#' || *s == '\r' || *s == '\n' || *s == 0)
			continue;

		// NB: the colour must go to its OWN buffer - sscanf'ing into `line` while `s`
		// points into it is undefined behaviour (and did read back what it had just
		// written the first time this was tried).
		// `clear` means "stop overriding" - put every row we changed back the way it was.
		// It is a whole-file action, not a step, so it short-circuits the rest.
		if (strncmp(s, "clear", 5) == 0)
		{
			sClearOnly = 1;
			break;
		}

		if (sscanf(s, "set %d %d %d %d %7s", &row, &texid, &palette, &entry, val) == 5)		{
			int rgb = cd2PalParseColour(val);

			if (rgb < 0)
				continue;

			sSteps[n].action = CC_PAL_SET;
			sSteps[n].colour = cd2PalPack((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
		}
		else if (sscanf(s, "solo %d %d %d %d", &row, &texid, &palette, &entry) == 4)
			sSteps[n].action = CC_PAL_SOLO;
		else if (sscanf(s, "flash %d %d %d %d", &row, &texid, &palette, &entry) == 4)
			sSteps[n].action = CC_PAL_FLASH;
		else
			continue;

		sSteps[n].row = row;
		sSteps[n].texid = texid;
		sSteps[n].palette = palette;
		sSteps[n].entry = entry;

		n++;
	}

	fclose(fp);

	sCount = n;

	if (sClearOnly)
	{
		cd2PalClearAll();
		sCount = 0;
		printInfo("[cc_pal] clear - every overridden row put back\n");
	}
	else if (n > 0)
	{
		// Say what was taken so a file typo is visible rather than silent: the syntax is
		// free-form enough that a misspelt line would otherwise just vanish.
		printInfo("[cc_pal] %d override(s) from CONFIG/cc_palette.txt (first: row %d texid %d palette %d entry %d action %d)\n",
			n, sSteps[0].row, sSteps[0].texid, sSteps[0].palette, sSteps[0].entry, sSteps[0].action);

		sWantVerify = 1;
		sApplyNow = 1;
	}

	return n;
}

// ---------------------------------------------------------------------------
// the map dump the editor reads
// ---------------------------------------------------------------------------

// A plain-text map of what is actually live, regenerated periodically while the editor is
// in use. It has to be produced HERE rather than derived offline, for two reasons: the
// CLUT ids are only resolved once a level has loaded (and an import pins them at draw
// time), and which rows a car draws from is a property of its BUILT model - a .MDL file
// does not know it (an import rebuilds one, and clut_uv0 is baked at build time).
//
// Poly counts come from the same walk CarImportDumpOneModel does: every poly carries the
// clut index it will read in `clut_uv0`'s high word, which decodes to
// `carid * 192 + texture_id * 6`. A row with no polys is not drawn at all, and saying so
// is most of the value - it is what turns 512 candidates into a handful.
static void cd2PalDump(void)
{
	char path[256];
	FILE* f;
	int i, row, texid, list, k;
	int ncars = 0;

	sprintf(path, "%s/CONFIG/cc_palette_map.txt", jer_root_dir());

	f = fopen(path, "wb");

	if (f == NULL)
		return;

	fprintf(f, "# cainescrossfire palette map - generated LIVE by the game\n");
	fprintf(f, "# read by tools/paletteedit.py; regenerated every %d frames.\n", CC_PAL_DUMP_EVERY);
	fprintf(f, "# car  <slot> <model> <palette> <role>\n");
    fprintf(f, "# clut <carid> <texid> <palette> <clut_hex> <vram_x> <vram_y> <polys> <ymin> <ymax> <r g b> x16\n");

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		CAR_MODEL* m;
		int counts[CIV_CLUT_ROWS][32];		// 2 KiB - the module's stack is fine
		int ymin[CIV_CLUT_ROWS][32];		// model-space Y extent per group: the part's height
		int ymax[CIV_CLUT_ROWS][32];
		int model = cp->ap.model;

		if (cp->controlType == 0)
			continue;			// empty slot

		if (model < 0 || model >= MAX_CAR_RESIDENT_MODELS)
			continue;

		fprintf(f, "car %d %d %d %s\n", i, model, cp->ap.palette,
			(i == 0) ? "player" : "ai");

		ncars++;

		m = &NewCarModel[model];

		memset(counts, 0, sizeof(counts));

		for (row = 0; row < CIV_CLUT_ROWS; row++)
			for (texid = 0; texid < 32; texid++)
				ymin[row][texid] = 0x7fff, ymax[row][texid] = -0x7fff;

		for (list = 0; list < 3; list++)
		{
			CAR_POLY* polys = (list == 0) ? m->pFT3 : (list == 1) ? m->pGT3 : m->pB3;
			int num = (list == 0) ? m->numFT3 : (list == 1) ? m->numGT3 : m->numB3;

			// A GT3/B3 is a quad, an FT3 a triangle. That is the usual PSX split and it is
			// what the draw path reads (vindices' low bytes, one per vertex).
			int nvert = (list == 0) ? 3 : 4;

			for (k = 0; k < num; k++)
			{
				CAR_POLY* poly = &polys[k];
				int idx = (int)(poly->clut_uv0 >> 16);
				int carid = idx / 192;
				int tid = (idx % 192) / 6;
				unsigned int vi = (unsigned int)poly->vindices;
				int v;

				if (carid < 0 || carid >= CIV_CLUT_ROWS || tid < 0 || tid >= 32)
					continue;

				counts[carid][tid]++;

				// The Y extent is what tells a roof/landau band apart from the body when both
				// share a texture id: a part high on the model is a different part.
				for (v = 0; v < nvert; v++)
				{
					int vy = m->vlist[(vi >> (8 * v)) & 0xff].vy;

					if (vy < ymin[carid][tid]) ymin[carid][tid] = vy;
					if (vy > ymax[carid][tid]) ymax[carid][tid] = vy;
				}
			}
		}

		for (row = 0; row < CIV_CLUT_ROWS; row++)
		{
			for (texid = 0; texid < 32; texid++)
			{
				u_short clut, entries[16];
				int e, pal = cp->ap.palette;

				if (counts[row][texid] == 0)
					continue;

				clut = cd2PalClutOf(row, texid, pal);

				if (clut == 0 || !JerichoClutReadInPlace(clut, entries))
					continue;

				fprintf(f, "clut %d %d %d %04x %d %d %d %d %d",
					row, texid, pal, clut, (clut & 0x3f) << 4, clut >> 6, counts[row][texid],
					(ymin[row][texid] <= ymax[row][texid]) ? ymin[row][texid] : 0,
					(ymin[row][texid] <= ymax[row][texid]) ? ymax[row][texid] : 0);

				for (e = 0; e < 16; e++)
					fprintf(f, " %d %d %d", (entries[e] & 31) << 3, ((entries[e] >> 5) & 31) << 3, ((entries[e] >> 10) & 31) << 3);

				fprintf(f, "\n");
			}
		}
	}

	fprintf(f, "# end - %d live car(s)\n", ncars);

	fclose(f);
}

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------

static int cd2PalOnGameStart(void* ud, void* args)
{
	char path[256];

	(void)ud;
	(void)args;

	sFrame = 0;
	sFlashOn = 0;

	memset(sSavedOk, 0, sizeof(sSavedOk));

	sprintf(path, "%s/CONFIG/cc_palette.txt", jer_root_dir());
	sFileStamp = cd2PalStamp(path);

	cd2PalParse();

	// Give the editor something to read straight away, whatever the trigger for the
	// periodic dump.
	cd2PalDump();

	return JER_RESULT_CONTINUE;
}

static int cd2PalOnFrame(void* ud, void* args)
{
	char path[256];
	unsigned long stamp;
	int i;

	(void)ud;
	(void)args;

	sFrame++;

	// The map dump is for the editor, so it only runs when the editor is in play: the env
	// var (how the editor's --launch starts the game) or an override file already present.
	if (sFrame % CC_PAL_DUMP_EVERY == 0)
	{
		char dpath[256];

		sprintf(dpath, "%s/CONFIG/cc_palette.txt", jer_root_dir());

		if (getenv("JERICHO_PAL_MAP") != NULL || cd2PalStamp(dpath) != 0)
			cd2PalDump();
	}

	// Re-read only when the file actually changed - the editor writes as you drag, and
	// stat+size every frame would be silly.
	if (sFrame % CC_PAL_REFRESH == 0)
	{
		sprintf(path, "%s/CONFIG/cc_palette.txt", jer_root_dir());
		stamp = cd2PalStamp(path);

		if (stamp != sFileStamp)
		{
			sFileStamp = stamp;

			cd2PalClearAll();	// put back what we changed before re-applying
			printInfo("[cc_pal] file changed - re-reading live\n");
			cd2PalParse();
		}
	}

	if (sCount <= 0)
		return JER_RESULT_CONTINUE;

	// flash needs a blink, so it re-applies every 15 frames
	sFlashOn ^= 1;

	// A freshly parsed file applies THIS frame. Waiting for the next refresh tick would
	// mean up to half a second of lag on every edit - and would make the read-back below
	// report the colour we had not written yet (which is how the first live test passed a
	// no-op off as a success).
	{
		int doApply = sApplyNow || (sFrame % CC_PAL_REFRESH == 0);

		sApplyNow = 0;

		for (i = 0; i < sCount; i++)
		{
			if (sSteps[i].action == CC_PAL_FLASH)
			{
				if (sFrame % 15 == 0)
					cd2PalApplyStep(&sSteps[i]);
			}
			else if (doApply && (sSteps[i].action == CC_PAL_SET || sSteps[i].action == CC_PAL_SOLO))
			{
				// re-apply every refresh: the engine may have re-uploaded the row (an
				// imported city's palettes are pinned at launch, a dyed row is built once)
				cd2PalApplyStep(&sSteps[i]);
			}
		}
	}

	// Once per parse, read one edited entry back out of VRAM and say what it is. This is
	// the difference between "the file was parsed" and "the colour is on the car" - and a
	// silent no-op (a wrong row, an unloaded column) is exactly what a palette editor
	// must not do.
	if (sWantVerify)
	{
		u_short clut = cd2PalClutOf(sSteps[0].row, sSteps[0].texid, sSteps[0].palette);
		u_short entries[16];

		sWantVerify = 0;

		if (clut != 0 && JerichoClutReadInPlace(clut, entries))
		{
			int r, g, b;

			cd2PalSplit(entries[sSteps[0].entry & 15], &r, &g, &b);

			printInfo("[cc_pal] verified: clut %04x entry %d = %d,%d,%d (live)\n",
				clut, sSteps[0].entry & 15, r, g, b);
		}
		else
		{
			printInfo("[cc_pal] WARNING: step 0 resolved to no CLUT (row %d texid %d palette %d) - nothing was changed\n",
				sSteps[0].row, sSteps[0].texid, sSteps[0].palette);
		}
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// self-test: prove the engine entry points, and that they cost no strip row
// ---------------------------------------------------------------------------

// JERICHO_CLUT_TEST=1 runs this once per level: read a live car CLUT row, change one
// entry, read it back, and report the strip's free-slot count either side. It is how the
// in-place mechanism is verified without a human watching a screen.
static int cd2PalSelfTest(void* ud, void* args)
{
	u_short clut = 0, entries[16], after[16];
	int freeBefore, freeAfter, found = 0;
	int r, g, b, row, texid;

	(void)ud;
	(void)args;

	// The first populated car CLUT in the level's own bank (rows 0..7 = the level's car
	// slots). Reading civ_clut is exactly what a poly's draw does.
	for (row = 1; row < CIV_CLUT_IMPORT_ROW && !found; row++)
	{
		for (texid = 0; texid < 32 && !found; texid++)
		{
			if (civ_clut[row][texid][1] != 0)
			{
				clut = civ_clut[row][texid][1];
				found = 1;
			}
		}
	}

	if (!found)
		return JER_RESULT_CONTINUE;

	freeBefore = jer_clut_slots_free();

	if (!JerichoClutReadInPlace(clut, entries))
		return JER_RESULT_CONTINUE;

	cd2PalSplit(entries[1], &r, &g, &b);

	entries[1] = cd2PalPack(255, 0, 255);		// magenta, so it is unmistakable
	JerichoClutWriteInPlace(clut, entries);
	JerichoClutReadInPlace(clut, after);

	freeAfter = jer_clut_slots_free();

	{
		int r2, g2, b2;

		cd2PalSplit(after[1], &r2, &g2, &b2);

		printInfo("[cc_pal] self-test: clut %04x (row %d texid %d) entry1 was %d,%d,%d now %d,%d,%d; strip free %d -> %d (%s)\n",
			clut, row - 1, texid - 1, r, g, b, r2, g2, b2, freeBefore, freeAfter,
			(freeBefore == freeAfter) ? "no row consumed" : "ROW CONSUMED - BUG");
	}

	// Put the entry back: the test's whole point is that it is reversible with no
	// allocation, and leaving a car magenta would be a nasty surprise for the next run.
	after[1] = cd2PalPack(r, g, b);
	JerichoClutWriteInPlace(clut, after);

	return JER_RESULT_CONTINUE;
}

void cd2PaletteEditRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2PalOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2PalOnFrame, NULL, 0);

	if (getenv("JERICHO_CLUT_TEST") != NULL)
		ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2PalSelfTest, NULL, 0);

	{
		char path[256];

		sprintf(path, "%s/CONFIG/cc_palette.txt", jer_root_dir());

		if (cd2PalStamp(path) != 0)
			ctx->jer_log(ctx, "[cc_pal] live palette overrides active (CONFIG/cc_palette.txt)\n");
	}
}
