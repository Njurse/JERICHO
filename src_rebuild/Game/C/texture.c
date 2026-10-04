#include "driver2.h"
#include "texture.h"
#include "system.h"
#include "mission.h"
#include "draw.h"
#include "cars.h"
#include "models.h"	// JERICHO: GetCarImportCity / GetCarImportTextureInfo (cross-city textures)
#include "objanim.h"
#include "ASM/compres.h"

SXYPAIR tpagepos[20] =
{
	{ 640, 0 },
	{ 704, 0 },
	{ 768, 0 },
	{ 832, 0 },
	{ 896, 0 },
	{ 960, 0 },
	{ 512, 256 },
	{ 576, 256 },
	{ 640, 256 },
	{ 704, 256 },
	{ 768, 256 },
	{ 832, 256 },
	{ 896, 256 },
	{ 448, 0 },
	{ 512, 0 },
	{ 576, 0 },
	{ 320, 256 },
	{ 384, 256 },
	{ 448, 256 },

	{ -1, -1 }
};

char specTpages[4][12] = {
	{
		54, 55, 
		66, 67, 
		56, 57,
		68, 69,
		61, 64, 
		61, 64 
	},
	{ 
		38, 39, 
		38, 39, 
		42, 43, 
		44, 45,
		48, 49, 
		48, 49 
	},
	{ 
		18, 19, 
		65, 66, 
		67, 68, 
		11, 12, 
		63, 64, 
		63, 64
	},
	{ 
		66, 67, 
		77, 78, 
		73, 74, 
		75, 76, 
		69, 70, 
		71, 72 
	}
};

char carTpages[4][8] = {
	{ 
		01, 58, 65, 62, 50, 63,
		54, 55
	},
	{ 
		10, 36, 35, 20, 37, 51,
		38, 39
	},
	{
		41, 59, 54, 62, 17, 32,
		18, 19 
	},
	{ 
		55, 59, 57, 68, 58, 60,
		66, 67
	}
};

char *palette_lump;

// JERICHO: the LUMP_PALLET segment's own byte size, recorded at load (main.c). The lump
// used to be handed on with a size of 0, which left its walk with nothing to bound
// against: the header's `total_cluts` is a CLUT count (200 on VEGAS), NOT the number of
// 12-byte RECORDS the lump holds (525), so bounding by it silently stopped the walk
// two-thirds of the way - and every car whose palette row lived in the tail (VEGAS rows 5
// and 6, i.e. car pages 32 and 17) kept an empty palette row and drew its panels from
// whatever the fallback found. The real size is what the byte bound was written for.
int palette_lump_size;

char* texturename_buffer = NULL;
int NoTextureMemory = 0;

u_short texture_pages[128];
u_short texture_cluts[128][32];
u_char tpageloaded[128];

int MaxSpecCluts;
int slotsused;

RECT16 clutpos;

// JERICHO: once the strip's remaining budget has been reported, don't repeat it
// until it gets tight.
static int gJerClutReported = 0;
RECT16 fontclutpos;
RECT16 mapclutpos;
DVECTOR slot_clutpos[19];
DVECTOR slot_tpagepos[19];
u_char tpageslots[TPAGE_SLOTS];

TP *tpage_position = NULL;
TEXINF* tpage_ids[128] = { 0 };
int texamount = 0;
int tpage_amount = 0;
int tpage_texamts[128];

int nspecpages = 0;
int nperms = 0;
XYPAIR *speclist = NULL;
XYPAIR *permlist = NULL;

RECT16 tpage; // stupid naming, absolute ass

short specialSlot;

// [D] [T]
void IncrementClutNum(RECT16 *clut)
{
	clut->x += 16;

	if (clut->x >= 1024) 
	{
		clut->x = 960;
		clut->y += 1;
	}
}

// ---------------------------------------------------------------------------
// JERICHO: the bottom-half VRAM pool (see JER_VRAM_HALF_Y in cars.h).
//
// Everything JERICHO loads - the cross-city import's pages and palettes, and later the
// custom textures that name a real page - is placed HERE, in rows 512..1023, instead of
// being squeezed into the top half's claims. The top half is full (VRAM.md 2: 19 page
// slots, a 32 KiB CLUT column and the sky = 704 KiB of 704), so an import placed there
// has to TAKE something: that is where INV1 ("pinned to a WORLD-pool rectangle") and the
// font collision come from. Placing below instead makes the question go away.
//
// Pages are two page-rows (Y = 512 and 768) of 64-wide columns, and they start at x=0:
// the top half cannot do that (x0..319 is the display buffers, and its 64-wide grid only
// begins at x=320), but down here the display buffers do not reach. The CLUT column
// mirrors the top half's at x 960..1023, so the page grid stops at x=896.
//
// No stock walk can reach this: tpagepos[] uses Y in {0,256}, the CLUT cursors are
// bounded by CD2_CLUT_SAFE_LAST, and IncrementClutNum is clamped by its callers. The
// pool is therefore "owned" by construction - it is not a mark that could be forgotten,
// it is a row range nothing else addresses.
// ---------------------------------------------------------------------------
#define JER_POOL_PAGE_W			64
#define JER_POOL_PAGE_ROW_H		256
#define JER_POOL_CLUT_X	960									// the CLUT column starts here
#define JER_POOL_PAGES_PER_ROW	(JER_POOL_CLUT_X / JER_POOL_PAGE_W)		// 15 -> x 0..895
#define JER_POOL_PAGE_ROWS		2									// Y = 512 and 768
#define JER_POOL_PAGES		(JER_POOL_PAGES_PER_ROW * JER_POOL_PAGE_ROWS)	// 30 pages, 60 KiB

static u_char sJerLowerPoolPageUsed[JER_POOL_PAGES];
static int sJerLowerPoolPagesUsed;
static int sJerLowerPoolClutY = JER_VRAM_HALF_Y;	// next free row of the bottom CLUT column
static int sJerLowerPoolClutUsed;					// rows handed out
static int sJerLowerPoolClutDropped;				// asks that found no room

// Drop every pool claim. A level load calls this, so one level's imports cannot make
// the next level's look pre-used.
void JerLowerPoolReset(void)
{
	memset(sJerLowerPoolPageUsed, 0, sizeof(sJerLowerPoolPageUsed));
	sJerLowerPoolPagesUsed = 0;
	sJerLowerPoolClutY = JER_VRAM_HALF_Y;
	sJerLowerPoolClutUsed = 0;
	sJerLowerPoolClutDropped = 0;
}

// Where lower half pool page `slot` lives. This is the rectangle a caller uploads into, and what
// the page word it hands the model has to resolve back to (bits 4 and 11 of the tpage Y).
void JerLowerPoolPageRect(int slot, RECT16 *r)
{
	r->x = (short)((slot % JER_POOL_PAGES_PER_ROW) * JER_POOL_PAGE_W);
	r->y = (short)(JER_VRAM_HALF_Y + (slot / JER_POOL_PAGES_PER_ROW) * JER_POOL_PAGE_ROW_H);
	r->w = JER_POOL_PAGE_W;
	r->h = JER_POOL_PAGE_ROW_H;
}

// Take the first free lower half pool page. Returns the slot, or -1 when the half is full.
int JerLowerPoolPageAlloc(void)
{
	int i;

	for (i = 0; i < JER_POOL_PAGES; i++)
	{
		if (!sJerLowerPoolPageUsed[i])
		{
			sJerLowerPoolPageUsed[i] = 1;
			sJerLowerPoolPagesUsed++;
			return i;
		}
	}

	return -1;
}

void JerLowerPoolPageFree(int slot)
{
	if (slot < 0 || slot >= JER_POOL_PAGES)
		return;

	if (sJerLowerPoolPageUsed[slot])
	{
		sJerLowerPoolPageUsed[slot] = 0;
		sJerLowerPoolPagesUsed--;
	}
}

// Is (x,y) the top-left of an ALLOCATED lower half pool page? The lower half is JERICHO's alone, so
// an allocated page there is by construction owned by whoever asked for it - no separate
// claim frame is needed, and none of the base game's ownership bookkeeping (which is
// indexed by tpagepos[] slot) can describe it.
int JerLowerPoolPageOwned(int x, int y)
{
	int i;

	for (i = 0; i < JER_POOL_PAGES; i++)
	{
		RECT16 r;

		if (!sJerLowerPoolPageUsed[i])
			continue;

		JerLowerPoolPageRect(i, &r);

		if (r.x == x && r.y == y)
			return 1;
	}

	return 0;
}

// Does tpage word `page` address lower half pool page `slot`? The decode is the engine's own, the
// same one CarImportPageRect and the shader use (x = (page & 0xf) * 64, y = bit4 * 256 +
// bit11 * 512) - NOT the retired 5-bit form.
int JerLowerPoolPageHolds(int slot, int page)
{
	RECT16 r;
	int px, py;

	if (slot < 0 || slot >= JER_POOL_PAGES)
		return 0;

	JerLowerPoolPageRect(slot, &r);

	px = (page & 0xf) * 64;
	py = (((page >> 4) & 1) * 256) + (((page >> 11) & 1) * 512);

	return (px == r.x) && (py == r.y);
}

// Reserve `rows` rows of the bottom CLUT column and return the first row, or -1 when the
// column cannot hold them. The caller decides the row count - for the import it is the
// same (npal + 3) / 4 + 1 the top half's pin band uses, per set.
// JERICHO: ONE allocator for the lower half pool's CLUT column, the cursor below.
//
// There used to be a second, `JerLowerPoolClutAlloc(rows)` - an up-front block
// reservation - and it is GONE on purpose. Two ways to advance one column is exactly how
// the palettes and the pin's page CLUTs ended up sharing rows 512..: the pin band was
// reserved by size and the palette tables walked a cursor, so neither knew about the
// other. What the column needs is one watermark that everything takes from, in order.
//
// The cursor is the shape the palettes need (walk, then commit). The pin band uses the
// same watermark by taking the cursor and advancing it as it walks (texture.c,
// CarImportPin), so a later taker always starts past the earlier one.

// JERICHO: the lower half pool CLUT column as a CURSOR, for a caller that walks it one CLUT at a
// time (IncrementClutNum) instead of knowing its size up front - the imported cities'
// palette upload is exactly that: it walks a lump and only then knows how many rows it
// used. Take the cursor, walk it, commit the rows.
//
// The x is the same 960 as the base column and IncrementClutNum wraps at 1024, so the
// walk needs no special case: 4 CLUTs to a row, then y++.
//
// The rect is 16x1 and MUST stay 16x1: `LoadImage` is
// `GR_CopyVRAM(p, 0, 0, rect->w, rect->h, rect->x, rect->y)` (LIBGPU.C:64), i.e. it copies
// w*h halfwords from the caller's buffer. This first returned 64x4 - the SHAPE of the
// column instead of the shape of one CLUT - so every CLUT copied 256 halfwords: 16 real
// ones plus 240 read past the source and written over the next three VRAM rows. The base
// column's cursor is 16x1 for the same reason (texture.c, `clutpos` init).
void JerLowerPoolClutCursor(RECT16 *out)
{
	out->x = JER_POOL_CLUT_X;
	out->y = (short)sJerLowerPoolClutY;
	out->w = 16;
	out->h = 1;
}

void JerLowerPoolClutAdvance(int rows)
{
	if (rows <= 0)
		return;

	if (sJerLowerPoolClutY + rows > JER_VRAM_TOTAL_ROWS)
	{
		// The column is full. Pin the cursor at the end rather than letting it walk past
		// VRAM; the upload that overran is dropped and counted, not silently wrapped.
		sJerLowerPoolClutDropped++;
		sJerLowerPoolClutY = JER_VRAM_TOTAL_ROWS;
		sJerLowerPoolClutUsed = JER_VRAM_TOTAL_ROWS - JER_VRAM_HALF_Y;
		return;
	}

	sJerLowerPoolClutY += rows;
	sJerLowerPoolClutUsed += rows;
}

int JerLowerPoolPagesUsed(void)			{ return sJerLowerPoolPagesUsed; }
int JerLowerPoolPagesFree(void)			{ return JER_POOL_PAGES - sJerLowerPoolPagesUsed; }
int JerLowerPoolClutRowsUsed(void)		{ return sJerLowerPoolClutUsed; }
int JerLowerPoolClutRowsFree(void)		{ return JER_VRAM_TOTAL_ROWS - sJerLowerPoolClutY; }
int JerLowerPoolClutDropped(void)		{ return sJerLowerPoolClutDropped; }

// JERICHO: read or write a CLUT row IN PLACE, at the address a CLUT id already names.
//
// A live palette editor needs exactly this and nothing else. An in-place write costs NO
// CLUT strip row - `clutpos` is untouched - so it works even when the strip is full, and
// it is full (see VRAM.md 3 and the budget in cars.h). The mechanism is the engine's own,
// already proven at runtime by objanim.c ColourCycle, which rewrites a world CLUT row
// every other frame: read the row, change it, write it back to the SAME address.
//
// Both calls are immediate (StoreImage -> GR_ReadVRAM, LoadImage -> GR_CopyVRAM), so they
// are safe OUTSIDE a render pass - no ordering table, no `current`.
//
// `clut` is a CLUT id as `civ_clut` / `texture_cluts` store it (x = (clut & 0x3f) << 4,
// y = clut >> 6). Only the ONE 16-entry row that id names is touched: a 256-colour CLUT
// is 16 such rows, so writing one row changes one of its palettes.
//
// NOTE for callers: a CLUT row can be SHARED - every car whose poly resolves to the same
// id samples this row. An in-place edit is therefore global. That is what makes it the
// right tool for FINDING OUT which part a row paints, and the wrong one for a per-car
// colour (which needs its own row; JerichoMakeClutRow allocates one).
//
// Return 1 on success, 0 on a zero id or a null buffer.
int JerichoClutReadInPlace(u_short clut, u_short* out16)
{
	RECT16 r;

	if (clut == 0 || out16 == NULL)
		return 0;

	r.x = (clut & 0x3f) << 4;
	r.y = clut >> 6;
	r.w = 16;
	r.h = 1;

	StoreImage(&r, (u_long*)out16);

	return 1;
}

int JerichoClutWriteInPlace(u_short clut, const u_short* in16)
{
	RECT16 r;

	if (clut == 0 || in16 == NULL)
		return 0;

	r.x = (clut & 0x3f) << 4;
	r.y = clut >> 6;
	r.w = 16;
	r.h = 1;

	LoadImage(&r, (u_long*)in16);

	return 1;
}

// JERICHO-HOOK: build a CLUT row that is a recoloured copy of another row.
//
// Used for per-instance pedestrian palettes: the Tanner body's polys all live on
// one texture page but use more than one CLUT entry, so a team colour needs its
// own recoloured row per entry. `sourceClut` is a clut word as it appears in
// texture_cluts[][]; the new row is taken from the clutpos strip and its address
// returned (0 when there was nothing to copy).
//
// `strength` is 0..256: 0 keeps the original colours, 256 takes the team hue
// outright. `floor5` (0..31) lifts the dark end: each entry's brightness is
// remapped to floor5 + ((31 - floor5) * lum) / 31 before the hue is applied, so a
// near-black entry still comes out as a visible dark team colour instead of
// staying black. floor5 = 0 keeps the source brightness, 31 makes it flat.
//
// Colour order note: PSX 16bpp is stp | b<<10 | g<<5 | r - five bits per channel,
// blue in the HIGH bits (the same packing as the rest of this file, not the
// B<<16|G<<8|R used for polygon colour words).
u_short JerichoMakeClutRow(u_short sourceClut, int r, int g, int b, int strength, int floor5)
{
	RECT16 src;
	u_short entries[16];
	u_short out[16];
	u_short addr;
	int i, k;
	int tr, tg, tb;
	int dyR, dyG, dyB;		// per-channel dye scale (the team hue on a mid entry)
	int flR, flG, flB;		// dark-end lift toward the hue

	if (sourceClut == 0)
		return 0;

	// clut word -> VRAM position (PSX GetClut encoding: y << 6 | x >> 4)
	src.x = (short)((sourceClut & 0x3f) * 16);
	src.y = (short)((sourceClut >> 6) & 0x3ff);	// 10-bit Y: the lower half pool sits at 512..1023
	src.w = 16;
	src.h = 1;

	// out of room in the strip at the right of VRAM, OR past the rows the cross-city
	// import reserves above this point (its own palettes and the pin band). Allocating
	// into those overwrote an imported car's colours with a pedestrian's - the runtime
	// cursor and the import share one VRAM column, so this is the boundary between them.
	if (src.y >= JER_VRAM_TOTAL_ROWS || clutpos.y > 511 || clutpos.y > CAR_CLUT_IMPORT_LIMIT)
		return 0;

	StoreImage(&src, (u_long*)entries);

	k = strength;

	if (k < 0)
		k = 0;
	if (k > 256)
		k = 256;

	if (floor5 < 0)
		floor5 = 0;
	if (floor5 > 31)
		floor5 = 31;

	// the team hue in the same 5-bit space
	tr = (r * 31) / 255;
	tg = (g * 31) / 255;
	tb = (b * 31) / 255;

	/* --- the dye -------------------------------------------------------------
	 * The suit must keep the shading the texture had, so an entry is SCALED by
	 * the team hue rather than REPLACED by it. Replacing (what this did first)
	 * turns every entry into "team hue at this entry's brightness": brightness
	 * survives but the difference in hue and chroma between entries does not, and
	 * the fabric reads as a solid colour - which is exactly what was reported.
	 *
	 * dyeX is the per-channel scale that lands a mid-brightness entry on the hue,
	 * so the RELATIVE levels of the entries are preserved along with the absolute
	 * ones. */
	{
		int hueLum = (tr * 77 + tg * 150 + tb * 29) >> 8;

		if (hueLum < 1)
			hueLum = 1;		// a near-black hue would divide by ~0

		dyR = (tr << 8) / hueLum;
		dyG = (tg << 8) / hueLum;
		dyB = (tb << 8) / hueLum;
	}

	/* and the dark end is LIFTED toward the hue, which is what stops a black
	 * outfit staying black (scaling alone cannot: black x anything is black) */
	flR = (tr * floor5) / 31;
	flG = (tg * floor5) / 31;
	flB = (tb * floor5) / 31;

	for (i = 0; i < 16; i++)
	{
		int r5 = entries[i] & 31;
		int g5 = (entries[i] >> 5) & 31;
		int b5 = (entries[i] >> 10) & 31;
		int lum, nr, ng, nb, w;

		/* Skin shares the OUTFIT's CLUT row: measured on the outfit row, entry 9
		 * is [20,14,12] - a warm tone - while the other entries are neutral greys.
		 * A warm entry is skin, not clothing, so it keeps its own colour; only the
		 * neutral entries take the team hue. */
		if (r5 - (g5 + b5) / 2 > 2)
		{
			out[i] = entries[i];
			continue;
		}

		lum = (r5 * 77 + g5 * 150 + b5 * 29) >> 8;	// 0..31 brightness

		/* the dyed colour: scale, then lift the floor */
		nr = ((r5 * dyR) >> 8) + flR;
		ng = ((g5 * dyG) >> 8) + flG;
		nb = ((b5 * dyB) >> 8) + flB;

		if (nr > 31) nr = 31;
		if (ng > 31) ng = 31;
		if (nb > 31) nb = 31;

		/* a gentle lift on the brightest entries, so a light outfit still reads as
		 * light instead of settling onto the hue - deliberately mild, because a
		 * strong pull toward white is the other way this ends up looking flat.
		 * Scaled by strength, so strength 0 is still an exact identity. */
		w = (lum > 22) ? (lum - 22) * 2 : 0;

		if (w > 0)
		{
			w = (w * k) / 256;

			nr += ((31 - nr) * w) / 31;
			ng += ((31 - ng) * w) / 31;
			nb += ((31 - nb) * w) / 31;
		}

		/* mix the dyed entry with the original by the strength */
		nr = (r5 * (256 - k) + nr * k) >> 8;
		ng = (g5 * (256 - k) + ng * k) >> 8;
		nb = (b5 * (256 - k) + nb * k) >> 8;

		if (nr > 31) nr = 31;
		if (ng > 31) ng = 31;
		if (nb > 31) nb = 31;

		out[i] = (u_short)((entries[i] & 0x8000) | (nb << 10) | (ng << 5) | nr);
	}

	/* Evidence that the suit kept its shading, as numbers rather than a claim:
	 * the brightness RANGE across the entries that were dyed (skin excluded)
	 * before and after, and how many DISTINCT colours survived.
	 *
	 * Both matter, and the second is the one that catches the repaint this
	 * replaced: "team hue at this entry's brightness" kept the brightness range
	 * intact - it would have scored fine on that alone - while collapsing every
	 * entry onto a single hue, which is what "it looks like a solid colour" was.
	 * A dye keeps the entries apart. */
	{
		int lo0 = 31, hi0 = 0, lo1 = 31, hi1 = 0;
		int counted = 0, distinct0 = 0, distinct1 = 0;
		int j;

		for (i = 0; i < 16; i++)
		{
			int r5 = entries[i] & 31;
			int g5 = (entries[i] >> 5) & 31;
			int b5 = (entries[i] >> 10) & 31;
			int l0, l1;

			if (r5 - (g5 + b5) / 2 > 2)
				continue;			/* skin: not touched */

			l0 = (r5 * 77 + g5 * 150 + b5 * 29) >> 8;
			l1 = ((out[i] & 31) * 77 + (((out[i] >> 5) & 31)) * 150 + (((out[i] >> 10) & 31)) * 29) >> 8;

			if (l0 < lo0) lo0 = l0;
			if (l0 > hi0) hi0 = l0;
			if (l1 < lo1) lo1 = l1;
			if (l1 > hi1) hi1 = l1;

			counted++;
		}

		/* distinct colours, ignoring the STP bit and the skin entries */
		for (i = 0; i < 16; i++)
		{
			int a, dup0 = 0, dup1 = 0;

			if ((entries[i] & 31) - (((entries[i] >> 5) & 31) + ((entries[i] >> 10) & 31)) / 2 > 2)
				continue;

			for (a = 0; a < i; a++)
			{
				if ((entries[a] & 31) - (((entries[a] >> 5) & 31) + ((entries[a] >> 10) & 31)) / 2 > 2)
					continue;
				if ((entries[a] & 0x7fff) == (entries[i] & 0x7fff)) dup0 = 1;
				if ((out[a] & 0x7fff) == (out[i] & 0x7fff)) dup1 = 1;
			}

			if (!dup0) distinct0++;
			if (!dup1) distinct1++;
		}

		if (counted > 1 && k > 0)
		{
			printInfo("ped palette dye %d,%d,%d @%d floor=%d: %d entry(s), brightness %d..%d (spread %d) -> %d..%d (spread %d), distinct colours %d -> %d\n",
				r, g, b, k, floor5, counted,
				lo0, hi0, hi0 - lo0, lo1, hi1, hi1 - lo1,
				distinct0, distinct1);
		}
	}

	LoadImage(&clutpos, (u_long*)out);

	addr = GetClut(clutpos.x, clutpos.y);
	IncrementClutNum(&clutpos);

	/* JERICHO: the same strip carries the ped team colours, the imported car palettes
	 * and (next) the per-instance car colours, so say what is left the first time a
	 * row is taken and again when it gets tight. A dyed car needs one row per
	 * textured part, so the number to watch is SLOTS, not lines. */
	{
		int free = jer_clut_slots_free();

		if (!gJerClutReported || free < 16)
		{
			gJerClutReported = 1;
			jer_clut_report("after a dyed CLUT row");
		}
	}

	return addr;
}

// JERICHO: free CLUT slots in the strip, before CAR_CLUT_IMPORT_LIMIT. The x cursor
// walks 0..960 in steps of 16 (64 CLUTs to a strip line) and wraps one line down, so
// what is left is whole lines plus the slot the x cursor is sitting on.
int jer_clut_slots_free(void)
{
	int lines = CAR_CLUT_IMPORT_LIMIT - clutpos.y;

	if (lines < 0)
		return 0;

	return lines * 64 - (clutpos.x / 16);
}

void jer_clut_report(const char* why)
{
	printInfo("[jericho] CLUT strip: y=%d/%d x=%d -> %d free slot(s)%s%s\n",
		clutpos.y, CAR_CLUT_IMPORT_LIMIT, clutpos.x, jer_clut_slots_free(),
		why ? " -- " : "", why ? why : "");
}

// [D] [T]
void IncrementTPageNum(RECT16 *tpage)
{
	int i = 0;

	while (++i)
	{
		// proper tpage position?
		if ((tpage->x == tpagepos[i - 1].x) && 
			(tpage->y == tpagepos[i - 1].y))
		{
			if (tpagepos[i].x == -1)
			{
				// out of tpages
				NoTextureMemory = 100;
			}
			else
			{
				// increment the tpage
				tpage->x = tpagepos[i].x;
				tpage->y = tpagepos[i].y;
			}

			// bust 'outta here, real fly
			break;
		}
		else
		{
			// last tpage?
			if (tpagepos[i].x == -1)
				break;
		}
	}
}

#ifndef PSX
// [A] - loads TIM files as level textures
void LoadTPageFromTIMs(int tpage2send)
{
	int i, j;
	RECT16 tmptpage;
	RECT16 tmpclut;
	SXYPAIR tpage;
	int tpn;
	
	char filename[64];
	TEXINF* details = tpage_ids[tpage2send];

	tpn = texture_pages[tpage2send];

	tpage.x = tpn << 6 & 0x3c0;
	tpage.y = (tpn << 4 & 0x100) + (tpn >> 2 & 0x200);
	
	// try loading TIMs directly
	for(i = 0; i < tpage_texamts[tpage2send]; i++)
	{
		TIMIMAGEHDR* timClut;
		TIMIMAGEHDR* timData;
		char* textureName;
		char* citytypeStr;
		int j;

		switch (GetCityType())
		{
			case CITYTYPE_NIGHT:
				citytypeStr = "N";
				break;
			case CITYTYPE_MULTI_DAY:
				citytypeStr = "M";
				break;
			case CITYTYPE_MULTI_NIGHT:
				citytypeStr = "MN";
				break;
			default:
				citytypeStr = "D";
				break;
			}

		textureName = texturename_buffer + details[i].nameoffset;

		sprintf(filename, "LEVELS\\%s\\%sPAGE_%d\\%s_%d.TIM", LevelNames[GameLevel], citytypeStr, tpage2send, textureName, i);

		if (!FileExists(filename))
			sprintf(filename, "LEVELS\\%s\\PAGE_%d\\%s_%d.TIM", LevelNames[GameLevel], tpage2send, textureName, i);

		if(!FileExists(filename))
			continue;
		
		Loadfile(filename, (char*)_other_buffer);

		// get TIM data
		timClut = (TIMIMAGEHDR*)(_other_buffer + sizeof(TIMHDR));
		timData = (TIMIMAGEHDR*)((char*)timClut + timClut->len);

		// replace tpage
		// upload it to ram
		tmptpage.x = tpage.x + (details[i].x >> 2);
		tmptpage.y = tpage.y + details[i].y;
		tmptpage.w = timData->width;
		tmptpage.h = timData->height;

		LoadImage(&tmptpage, (u_long*)((char*)timData + sizeof(TIMIMAGEHDR)));

		// get through all it's CLUTs
		// and replace
		for (j = 0; j < timClut->height; j++)
		{
			int cpal = GetCarPalIndex(tpage2send);
			int clutN;

			if (j > 0 && cpal > 0)
				clutN = civ_clut[cpal][i][j];
			else
				clutN = texture_cluts[tpage2send][i];

#if 0
			// FIXME:
			// this is a wasteful way handling multiple palettes
			// we just allocate new palettes to ensure that it would not glitch
			if(clutN == 0 || j > 0 && cpal > 0)
			{
				// add new CLUT
				clutN = GetClut(clutpos.x, clutpos.y);
				IncrementClutNum(&clutpos);
				civ_clut[cpal][i][j] = clutN;
			}
#endif
			
			tmpclut.x = (clutN & 0x3f) << 4;
			tmpclut.y = (clutN >> 6);
			tmpclut.w = 16;
			tmpclut.h = 1;

			LoadImage(&tmpclut, (u_long*)((char*)timClut + sizeof(TIMIMAGEHDR) + j * 32));
		}
	}
}
#endif

// [D] [T]
// JERICHO: imported pages take precedence over whatever the engine streams.
//
// Every page upload - the level's own, the world's re-streaming, and ours - funnels
// through LoadTPageAndCluts. So this is the one choke point where "enforce our
// materials over whatever vehicle we are replacing" can be made absolute: an upload
// aimed at a rectangle an imported page already occupies is simply refused.
//
// Brutish on purpose, as asked. The cost is that a world stream landing on that
// rectangle goes nowhere, and the world draws a stale page there - which is the price
// of the imported car keeping its own. Inert unless an import is active: with no
// import, nothing is owned, the guard never fires, and VRAM behaves exactly as before.
static int sCarPageUploading;			// set while WE upload, so ours is not refused

// JERICHO: imported-page claims, kept as a small POOL. A claim records the frame the
// pin last confirmed that slot's rectangle for an imported set (0 = no claim).
// CarPageRectOwned refuses a world upload only while the claim is FRESH - the car has
// been drawn within CAR_PAGE_CLAIM_FRAMES. Once it has not, the world may take the
// rectangle back: the page re-streams from disk (cheap - there is no CD budget on PC),
// and CarImportPin re-uploads the car's page the next time the car IS drawn. A frame
// stamp per slot, rather than a bare owned flag, is what lets this grow into a real
// pool over the host's own car pages too (see the pool note in HACK.md).
#define CAR_PAGE_CLAIM_FRAMES 8

extern int FrameCnt;			// JERICHO: frame clock for the imported-page claim pool

static int sCarPageClaimFrame[19];
static int sCarPageGiveBacks;		// world uploads that reclaimed a stale claim

// JERICHO: is this VRAM rectangle one an imported page owns? Exported so the spool's
// own upload paths can respect it too - they write texture_pages[] and call LoadImage
// directly, bypassing the LoadTPageAndCluts guard entirely, which is how the host's
// special-car pages kept landing on top of an imported player car.
int CarPageRectOwned(int x, int y)
{
	int i;

	// JERICHO: anything in the lower half pool half is JERICHO's. Rows 512..1023 are space no stock
	// path computes - tpagepos[] holds Y in {0,256} - so an upload aimed there is either
	// one of our own pin uploads (which bypass the guard via sCarPageUploading) or a stray
	// that must be refused. Checked before the slot table, which cannot describe it.
	if (y >= JER_VRAM_HALF_Y)
		return JerLowerPoolPageOwned(x, y);

	for (i = 0; i < 19; i++)
	{
		if (sCarPageClaimFrame[i] == 0 || tpagepos[i].x != x || tpagepos[i].y != y)
			continue;

		if (FrameCnt - sCarPageClaimFrame[i] <= CAR_PAGE_CLAIM_FRAMES)
			return 1;			// still the car's: refuse the world

		// Stale: the car has not been drawn for a while, so let the world have the
		// rectangle back. CarImportPin re-uploads the car's page when it is drawn again.
		sCarPageClaimFrame[i] = 0;
		sCarPageGiveBacks++;
	}

	return 0;
}

// JERICHO: the same question asked by SLOT, which is exact where a rectangle is not.
// `slot_tpagepos[]` (what the streamer uploads to) and `tpagepos[]` (what the pin
// places at) are two views that are not guaranteed to agree, so the streamer asks by
// slot - otherwise it could offer a rectangle the pin owns and upload a world page
// over the car.
int CarPageSlotOwned(int slot)
{
	if (slot < 0 || slot >= 19)
		return 0;

	if (sCarPageClaimFrame[slot] == 0)
		return 0;

	if (FrameCnt - sCarPageClaimFrame[slot] <= CAR_PAGE_CLAIM_FRAMES)
		return 1;

	sCarPageClaimFrame[slot] = 0;
	sCarPageGiveBacks++;

	return 0;
}

int LoadTPageAndCluts(RECT16 *tpage, RECT16 *cluts, int tpage2send, char *tpageaddress)
{
	int npalettes;
	int i;
	RECT16 temptpage;

	char* tempBuf;

	// JERICHO-HOOK: an imported page lives at this VRAM rectangle - the engine does
	// not get to replace it. See sCarPageOwned.
	if (!sCarPageUploading && CarPageRectOwned(tpage->x, tpage->y))
		return 0;

	npalettes = *(int *)tpageaddress;
	tpageaddress += 4;

	for (i = 0; i < npalettes; i++)
	{
		LoadImage(cluts, (u_long*)tpageaddress);
		tpageaddress += 32;

		texture_cluts[tpage2send][i] = GetClut(cluts->x, cluts->y);
		
		IncrementClutNum(cluts);
	}

	temptpage.x = tpage->x;
	temptpage.y = tpage->y;
	temptpage.w = tpage->w;
	temptpage.h = 256;

	decomp_asm((char*)_other_buffer, tpageaddress);
	LoadImage(&temptpage, (u_long*)_other_buffer);

	texture_pages[tpage2send] = GetTPage(0, 0, tpage->x, tpage->y);
	IncrementTPageNum(tpage);

	return 1;
}

// UNUSED
int Find_TexID(MODEL *model, int t_id)
{
	char *polylist;
	polylist = GET_MODEL_DATA(char, model, poly_block);

	for (int i = 0; i < model->num_polys; i++)
	{
		switch (*polylist & 0x1F)
		{
			case 4:
			case 5:
			case 6:
			case 7:
			case 20:
			case 21:
			case 22:
			case 23:
				if (polylist[2] == t_id)
					return 1;
		}

		polylist += PolySizes[*polylist];
	}
	
	return 0;
}

// [D] [T]
TEXINF* GetTEXINFName(char *name, int *tpagenum, int *texturenum)
{
	char *nametable;
	int i, j;
	nametable = texturename_buffer;

	for (i = 0; i < tpage_amount; i++)
	{
		int texamt = tpage_texamts[i];
		TEXINF *texinf = tpage_ids[i];

		for (j = 0; j < texamt; j++)
		{
			if (!strcmp(nametable + texinf->nameoffset, name))
			{
				*tpagenum = i;
				*texturenum = j;

				return texinf;
			}

			texinf++;
		}
	}

	return NULL;
}

// [D] [T]
TEXINF* GetTextureInfoName(char *name, TPAN *result)
{
	TEXINF *tex;
	int tpagenum;
	int texturenum;

	tex = GetTEXINFName(name, &tpagenum, &texturenum);

	result->texture_page = tpagenum;
	result->texture_number = texturenum;

	return tex;
}

// [D] [T]
// JERICHO: note the two encodings of `tpageloaded[]` in this file and leave them alone.
// This writes it 0-based (the slot number) while SendTPage writes `slot + 1` and compares
// `slot != tpageloaded[set] - 1`. The mismatch only ever makes SendTPage re-upload a set
// this function loaded (a redundant upload, which the import guard then refuses if the
// rectangle is a car's) - it cannot mis-point a page. Changing it would touch the
// streamer's skip logic for no gain, so the inconsistency is documented instead.
void update_slotinfo(int tpage, int slot, RECT16 *pos)
{
	tpageslots[slot] = tpage;
	tpageloaded[tpage] = slot;
	slot_tpagepos[slot].vx = pos->x;
	slot_tpagepos[slot].vy = pos->y;
}

// [D] [T]
void ProcessTextureInfo(char *lump_ptr)
{
	int i;
	char* ptr;
	tpage_amount =  *(int *)lump_ptr;
	texamount = *(int *)(lump_ptr + 4);
	tpage_position = (TP *)(lump_ptr + 8);

	ptr = (char *)&tpage_position[tpage_amount + 1];

	for (i = 0; i < tpage_amount; i++)
	{
		texamount = *(int *)ptr;
		ptr += 4;

#ifndef PSX
		tpage_ids[i] = (TEXINF *)D_MALLOC(sizeof(TEXINF) * texamount);
		memcpy(tpage_ids[i], ptr, sizeof(TEXINF) * texamount);
#else
		tpage_ids[i] = (TEXINF *)ptr;
#endif

		ptr += (texamount * sizeof(TEXINF));

		tpage_texamts[i] = texamount;
	}

	nperms = *(int *)ptr;
	permlist = (XYPAIR *)(ptr + 4);

	ptr = (char *)&permlist[16];

	nspecpages = *(int *)ptr;
	speclist = (XYPAIR *)(ptr + 4);

	// initialize here on PSX
	InitCyclingPals();
}

#ifndef PSX
extern char g_CurrentLevelFileName[64];

// [A] one-shot texture replacement
void LoadPermanentTPagesFromTIM()
{
	int slot;

	for (slot = 0; slot < 19; slot++)
	{
		if(tpageslots[slot] != 0xFF)
		{
			int tpage = tpageslots[slot];
			LoadTPageFromTIMs(tpage);

#if 0
			// initialize ALL texture palettes
			// this makes damaged textures appear properly
			int pal = GetCarPalIndex(tpage);
			
			if (pal)
			{
				int carpal = GetCarPalIndex(tpage);

				if(carpal > 0)
				{
					for (int i = 0; i < 32; i++)
						civ_clut[carpal][i][0] = texture_cluts[tpage][i];
				}

			}
#endif
		}
	}
}

#endif // !PSX

void load_civ_palettes(RECT16 *cluts)
{
	return;
}

// ---------------------------------------------------------------------------
// JERICHO cross-city textures
//
// A vehicle imported from another city names ITS city's texture sets in its
// polygons. This level never loaded those pages, so their texture_pages[] and
// texture_cluts[] entries are still the dummy (960,0)/(960,16) values
// LoadPermanentTPages fills in first - which is why a foreign car renders as
// nothing at all. An imported city's page lists come from its own
// LUMP_TEXTUREINFO lump, which models.c located while reading that city's level
// file.
//
// This mirrors ProcessTextureInfo's walk but writes into import-side state: the
// host level's own tables are never taken over.
#define CAR_IMPORT_MAX_SETS	16

typedef struct
{
	int count;			// entries used
	int set[CAR_IMPORT_MAX_SETS];	// texture-set index (XYPAIR.x)
	int bytes[CAR_IMPORT_MAX_SETS];	// byte size of that set's data (XYPAIR.y)
} CAR_IMPORT_SETS;

// ONE list per city: a level can hold more than one city's car data, and each
// city's page list is its own (the same set number means a different page in
// another city). Indexed by city, so a set is resolved against ITS OWN list.
static CAR_IMPORT_SETS gCarImportPerms[4];

/* JERICHO: what cross-city resources each resident SLOT holds.
 *
 * Nothing mapped a slot to what it owns, which is why a car's resources could not be
 * given back: the pin table is keyed by set (a record, not a car), the pool pages by
 * pin, the palette block by CITY, and the hot-load geometry by nothing at all. This is
 * that map, filled in where each piece is placed (CarSlotResNote after the pin walk,
 * CarSlotResNoteGeometry from the build) and reset with the rest of the import state.
 * A release needs it; nothing reads it for behaviour yet.
 *
 * Keyed by resident slot, because that is the unit that comes and goes: a player picks,
 * a peer joins, a peer leaves - each is a slot acquiring or giving up these. */
typedef struct
{
	int	used;			/* 0 = nothing cross-city on this slot */
	int	city;			/* -1 = the level's own car */
	int	pins;			/* pins recorded for this slot */
	int	poolPages;		/* of those, how many hold a lower-half pool page */
	int	clutRowBase;		/* this city's civ_clut block, -1 = none */
	int	clutRows;
	int	geometryBytes;		/* the hot-load pool block, 0 = built at level load */
} CAR_SLOT_RES;

static CAR_SLOT_RES sCarSlotRes[MAX_CAR_RESIDENT_MODELS];
static CAR_IMPORT_SETS gCarImportSpecs[4];
static int gCarImportTexParsed[4];

static void CopyImportSetList(const XYPAIR* list, int n, CAR_IMPORT_SETS* out)
{
	int i;

	out->count = 0;

	if (list == NULL || n <= 0)
		return;

	if (n > CAR_IMPORT_MAX_SETS)
		n = CAR_IMPORT_MAX_SETS;

	for (i = 0; i < n; i++)
	{
		out->set[i] = list[i].x;
		out->bytes[i] = list[i].y;
	}

	out->count = n;
}

// Parse the imported city's page lists. Same layout as ProcessTextureInfo:
// [tpage_amount][texamount][TP array][one length-prefixed TEXINF array per
// tpage][nperms][permlist][16-entry region][nspecpages][speclist].
// A no-op when nothing is imported, and it fails safe on anything malformed.
// Parse ONE city's page lists. Same layout as ProcessTextureInfo:
// [tpage_amount][texamount][TP array][one length-prefixed TEXINF array per
// tpage][nperms][permlist][16-entry region][nspecpages][speclist].
// A no-op when that city is not held, and it fails safe on anything malformed.
static void ParseImportedTextureInfoForCity(int city)
{
	char* lump;
	char* ptr;
	char* end;
	int size = 0;
	int tpageAmount;
	int i;

	gCarImportPerms[city].count = 0;
	gCarImportSpecs[city].count = 0;
	gCarImportTexParsed[city] = 0;

	if (!CarImportCityHeld(city))
		return;

	lump = GetCarImportTextureInfoForCity(city, &size);

	if (lump == NULL || size < 16)
		return;

	end = lump + size;
	tpageAmount = *(int*)lump;

	ptr = (char*)&((TP*)(lump + 8))[tpageAmount + 1];

	// one length-prefixed TEXINF array per texture page
	for (i = 0; i < tpageAmount; i++)
	{
		int texamount;

		if (ptr + 4 > end)
			return;

		texamount = *(int*)ptr;
		ptr += 4;

		if (texamount < 0 || ptr + (size_t)texamount * sizeof(TEXINF) > end)
			return;

		ptr += texamount * sizeof(TEXINF);
	}

	if (ptr + 4 > end)
		return;

	{
		int nperms = *(int*)ptr;

		ptr += 4;

		if (nperms < 0 || ptr + (size_t)nperms * sizeof(XYPAIR) > end)
			return;

		CopyImportSetList((XYPAIR*)ptr, nperms, &gCarImportPerms[city]);
	}

	// the permanent list occupies a fixed 16-entry region
	ptr = (char*)&((XYPAIR*)ptr)[16];

	if (ptr + 4 > end)
		return;

	{
		int nspec = *(int*)ptr;

		ptr += 4;

		if (nspec < 0 || ptr + (size_t)nspec * sizeof(XYPAIR) > end)
			return;

		CopyImportSetList((XYPAIR*)ptr, nspec, &gCarImportSpecs[city]);
	}

	gCarImportTexParsed[city] = 1;

	// Say whether this city's CAR sets are among the loaded page lists - those
	// are the sets an imported vehicle's polygons name. Entries 6..7 of
	// carTpages are filled in at run time for the CURRENT level, so only the
	// six static ones can be checked here.
	//
	// Also prove the bytes are reachable: the permanent page data is
	// concatenated after DATA1 in that city's file, one entry per listing, each
	// sector-aligned - so a set's offset is the running total of the aligned
	// sizes before it, which is exactly how LoadPermanentTPages carves them.
	{
		int base = GetCarImportPageBaseForCity(city);
		int wanted = 0;
		int found = 0;

		for (i = 0; i < 6; i++)
		{
			int set = carTpages[city][i];
			int j;
			int offset = 0;

			if (set == 0)
				continue;

			wanted++;

			for (j = 0; j < gCarImportPerms[city].count; j++)
			{
				if (gCarImportPerms[city].set[j] == set)
				{
					int cluts = 0;

					found++;

					if (base >= 0 && ReadCarImportFileForCity(city, base + offset, &cluts, sizeof(cluts)))
						printInfo("cross-city: %s set %d at +%d, %d bytes, %d clut rows\n",
							LevelNames[city], set, offset, gCarImportPerms[city].bytes[j], cluts);

					break;
				}

				offset += (gCarImportPerms[city].bytes[j] + CDSECTOR_SIZE - 1) & -CDSECTOR_SIZE;
			}
		}

		printInfo("cross-city: %s page lists - %d permanent sets, %d special sets, %d/%d car sets present\n",
			LevelNames[city], gCarImportPerms[city].count, gCarImportSpecs[city].count, found, wanted);
	}
}

// Parse EVERY held city's page list. A set is resolved against ITS OWN city's
// list, because the same set number means a different page in another city - so
// one city's list cannot stand in for another's.
static void ParseImportedTextureInfo(void)
{
	int city;

	for (city = 0; city < 4; city++)
		ParseImportedTextureInfoForCity(city);
}

// JERICHO: parse ONE city's page list, for a city read in MID-LEVEL (the hot load).
//
// ParseImportedTextureInfo runs once, at level load, for the cities held THEN. A city
// read in later never got its lists, so the pin walk could not find its sets' pages and
// skipped them ("HAVANA set 37 is not in its page list - skipped") - the index stayed
// baked with nothing behind it, so the model drew whatever that page held: "the palette
// is correct but the texture still seems wrong ... for havana". Called from
// InitCarImportMidLevel, next to the read that makes the city held in the first place.
void CarImportPageListsForCity(int city)
{
	ParseImportedTextureInfoForCity(city);
}

// Whether the level's own page load already claimed this texture set. Scans the
// slot table rather than tpageloaded, because slot 0 is a valid slot and so
// indistinguishable from "not loaded" in that array.
static int LevelTookTPage(int tpage)
{
	int i;

	// TPAGE_SLOTS, not a policy limit: tpageslots is 19 entries and this is the array
	// bound. It is NOT the reason a host set could read as unused - HostUsesTPage asks the
	// level's own page list (permlist), which covers every permanent page regardless of
	// how many slots the table can hold.
	for (i = 0; i < slotsused && i < TPAGE_SLOTS; i++)
	{
		if (tpageslots[i] == tpage)
			return 1;
	}

	return 0;
}

// Whether a set is already in a collected list.
static int SetInList(int* sets, int n, int set)
{
	int i;

	for (i = 0; i < n; i++)
	{
		if (sets[i] == set)
			return 1;
	}

	return 0;
}

// The distinct texture sets a car model paints with. polyList[1] is the set - the
// draw path indexes texture_pages[texture_set] with it (cars.c) - and only the
// textured poly types carry one. PolySizes walks the packed list exactly as the
// renderer does.
static int CollectModelSets(MODEL* model, int* sets, int n, int max)
{
	char* polylist;
	int i;

	extern int PolySizes[56];

	if (model == NULL)
		return n;

	polylist = GET_MODEL_DATA(char, model, poly_block);

	for (i = 0; i < model->num_polys && n < max; i++)
	{
		switch (*polylist & 0x1F)
		{
			case 4: case 5: case 6: case 7:
			case 20: case 21: case 22: case 23:
			{
				int set = (u_char)polylist[1];

				if (set != 0 && !SetInList(sets, n, set))
					sets[n++] = set;

				break;
			}
		}

		polylist += PolySizes[*polylist & 0x1f];
	}

	return n;
}

// Whether a texture set belongs to the level's own city. Its car pages are loaded,
// or streamed on demand, by the level itself, so an import must never take one.
// specTpages is checked because carTpages[GameLevel][6..7] are only filled in
// further down LoadPermanentTPages - specTpages[GameLevel] covers those numbers.
static int HostOwnsCarTPage(int tpage)
{
	int i;

	for (i = 0; i < 8; i++)
	{
		if (carTpages[GameLevel][i] == tpage)
			return 1;
	}

	for (i = 0; i < 12; i++)
	{
		if (specTpages[GameLevel][i] == tpage)
			return 1;
	}

	return 0;
}

// JERICHO: does the HOST level draw with this texture set AT ALL?
//
// This is the question an import must ask before it writes a page, and the old test was
// narrower in two ways that both left the host repainted:
//
//   - it knew only the host's CAR pages (carTpages/specTpages), so a set the host uses for
//     a PEDESTRIAN or a scenery prop read as "the host does not use this" and the import
//     wrote its page IN PLACE. Pedestrians read texture_pages[]/texture_cluts[] with no
//     city awareness at all (draw.c, and their colour row is civ_clut[0], motion_c.c), so
//     those peds came out with the imported car's pixels - the "the host's own pedestrians
//     and regional traffic are mangled" report. Consistently, not occasionally: a set the
//     host shares with the guest city ALWAYS collides.
//   - LevelTookTPage only scanned the resolved slot table, whose loop caps at 19, so a
//     permanent page past slot 19 also read as unused.
//
// permlist IS the host's own page list (LoadPermanentTPages loads exactly those, and
// update_slotinfo puts them in tpageslots), and speclist is its special-page list, so
// with the car tables this answers for every set the host draws with - and it subsumes
// LevelTookTPage, whose only other sources are those two lists plus a spool special.
// JERICHO: WHICH of the host's lists says it uses this set - ordered so the answer says
// whether the OLD test would have caught it:
//
//   "host car page" / "resolved slot"  -> the narrow test (LevelTookTPage, whose loop caps
//                                         at 19, or HostOwnsCarTPage) already said yes.
//                                         No behaviour change for these.
//   "specs" / "perms/ped-scenery"      -> ONLY the wide test catches it. perms is the
//                                         level's own page list, where PEDESTRIANS and
//                                         scenery live; specs is its special-page list.
//                                         These are the sets the import used to write IN
//                                         PLACE, repainting the host's pedestrians with the
//                                         imported car's pixels - measured as common: 1-4
//                                         of every 9 candidates, host-dependent.
//
// The union is the same either way; the order only makes the *reason* readable.
static const char *HostUseReason(int tpage)
{
	int i;

	if (HostOwnsCarTPage(tpage))
		return "host car page";

	if (LevelTookTPage(tpage))
		return "resolved slot";

	for (i = 0; i < nspecpages; i++)
	{
		if (speclist[i].x == tpage)
			return "specs";
	}

	for (i = 0; i < nperms; i++)
	{
		if (permlist[i].x == tpage)
			return "perms/ped-scenery";
	}

	return "-";
}

static int HostUsesTPage(int tpage)
{
	return HostUseReason(tpage)[0] != '-';
}

// JERICHO: index remap for imported vehicles.
//
// A texture set number can only mean one thing at a time, and the host city already
// means something by some of the numbers the source city uses. Rather than skip
// those sets - which left an imported car drawing host textures on part of its body
// - the imported page goes to a free set index and the car's own polygons are
// translated onto it as they are converted into engine form (cars.c, in
// plotNewCarModel). Nothing of the host's is touched.
// Enough for BOTH imported cities: each contributes its own sets that the level
// already resolved, so one city's six was the old ceiling and a second city
// overflows 8. These are small int arrays - the cost is a few hundred bytes.
#define CAR_REMAP_MAX 32

static int sRemapFrom[CAR_REMAP_MAX];
static int sRemapTo[CAR_REMAP_MAX];
static int sRemapCount;

// Set while an imported vehicle's polys are being converted, so CarSetRemap only
// applies to THAT car. It has to be per-car: the remap maps e.g. 54 -> 110 because
// the imported car's 54 means the source city's page, but a HOST car whose set 54
// means the host's own page must still read 54. Chicago's own car set list contains
// 54, so a global remap would have retextured host cars with the imported city's
// pages - the same class of bug as the civ_clut clobber.
static int sCarSetRemapActive;

void CarSetRemapEnable(int on)
{
	sCarSetRemapActive = on;
}

// Translate a source-city set number to the index its page was loaded at. Identity
// unless we are converting an imported car, so host cars and import-free levels are
// unaffected.
//
// The index is decided HERE, once, and shared by the two places that need it: the model
// BUILD (which bakes it into every poly - CAR_BAKE_TPAGE) and the PIN (which fills
// texture_pages at draw time). They must agree, because the polys read
// texture_pages[that index]. They used to be computed separately, and that is why a
// special body's third set was baked as the host's index 1 while the pin filled index
// 110: measured, 48 of 254 polys read the host's page (704,0) instead of the imported one.
int CarImportDstSet(int set);	// defined below, once the free-index allocator is in scope
int CarImportPinDstSet(int set);	// the same decision, for the pin (outside the build)

int CarSetRemap(int set)
{
	return CarImportDstSet(set);
}

// First set index above everything a city uses that is still free. City car sets
// live in the low numbers (10..68 in all four), so the top of the 128-entry table is
// unused - and tpageloaded stays zero for any index never loaded. The host's car and
// special sets are excluded explicitly as well, so a destination can never collide
// with a meaning the host needs.
//
// JERICHO: reservation matters. `tpageloaded[]` only becomes non-zero when a page is
// PLACED, and placement is draw-time - so two sets imported during one load both saw
// 110 free and BOTH took it. That collapsed the car's two pages onto one index: the
// second's pixels and CLUTs replaced the first's, and the car sampled one page for both
// parts (the "broken textures on the modded car" report). The reservation list is what
// makes the answer stable between the re-index and the placement.
static int sReservedSet[CAR_REMAP_MAX];
static int sReservedCount;

static int SetIndexReserved(int i)
{
	int k;

	for (k = 0; k < sReservedCount; k++)
	{
		if (sReservedSet[k] == i)
			return 1;
	}

	return 0;
}

static int FindFreeSetIndex(void)
{
	int i;

	for (i = 110; i < 128; i++)
	{
		if (tpageloaded[i] != 0 || SetIndexReserved(i))
			continue;

		// JERICHO: never hand an import an index the HOST draws with.
		//
		// The two car-table loops that used to be here were the whole test, and they left
		// the level's OWN page list unasked (permlist/speclist - scenery and pedestrians).
		// HostUsesTPage asks all of it, so this is the invariant stated where the index is
		// actually chosen, rather than relying on tpageloaded's bookkeeping above being
		// complete. If an import ever ends up on a host set, this is the line that should
		// have stopped it.
		if (HostUsesTPage(i))
			continue;

		return i;
	}

	return 0;
}

// The decision itself, shared by the build (which bakes it) and the pin (which fills it).
// The `sCarSetRemapActive` guard lives in CarImportDstSet, NOT here: the pin runs long
// after the build has finished, and the first cut of this had the guard here - so the
// pin's lookup returned the set unchanged, recorded index 1 while the polys had baked
// 110, and the 32 polys that name set 1 kept reading the host's page.
static int CarImportDstSetCore(int set)
{
	int i, free;

	for (i = 0; i < sRemapCount; i++)
	{
		if (sRemapFrom[i] == set)
			return sRemapTo[i];
	}

	// A set the level has already resolved keeps its meaning (the host city owns that
	// number), so the imported page goes to a free index instead - allocated at BUILD
	// time so the bake cannot disagree with the pin. A set the level never resolved is
	// left as it is: the pin replaces texture_pages[set] with the imported page, and the
	// polys read exactly that.
	//
	// REVERTED FROM "always take an import index", and the reason matters more than the
	// code: that version was meant to stop an imported car landing on a world page (the
	// HAVANA "0 in the pool" case), and instead the user came back with "the frontend
	// completely fails to work correctly and is still accepting input but visually does
	// not update at all after returning to the frontend menu". Taking a remap index for
	// every set put imported pages into the index space the FRONTEND draws from (the
	// attract screen and the menus draw cars), so the menus stopped updating while the
	// input still worked. The imported-page problem needs a fix that cannot reach the
	// frontend's pages - see the pool/authority notes in MP_ADAPTER.md - not a wider grab.
	//
	// JERICHO: the test is HostUsesTPage - "does the host draw with this set at all?" -
	// not the old "is it one of the 19 resolved slots, or one of the host's car pages".
	// See HostUsesTPage for what the narrow version cost (repainted pedestrians).
	if (!HostUsesTPage(set))
		return set;

	free = FindFreeSetIndex();

	if (free == 0 || sRemapCount >= CAR_REMAP_MAX)
		return set;			// no room: keep the host's page for this part, as before

	sRemapFrom[sRemapCount] = set;
	sRemapTo[sRemapCount] = free;
	sRemapCount++;

	if (sReservedCount < CAR_REMAP_MAX)
		sReservedSet[sReservedCount++] = free;	// keep the next set out of it

	printInfo("cross-city: set %d is the level's own - re-indexed to %d for the imported car (decided once, so the bake and the pin agree)\n",
		set, free);

	return free;
}

// What the imported car's polys are baked with, and what the pin fills.
int CarImportDstSet(int set)
{
	return sCarSetRemapActive ? CarImportDstSetCore(set) : set;
}

// For the pin, which runs outside the build: the mapping is the same one the build used,
// so the page it fills is the page the polys read.
int CarImportPinDstSet(int set)
{
	return CarImportDstSetCore(set);
}


// JERICHO: pinning for imported pages.
//
// The slot table does not stay as the import left it: a later load pass memsets
// tpageloaded and resets tpageslots, after which the imported pages are unclaimed -
// and they sit at tpagepos[slot], the very VRAM rectangles the engine's own slots
// stream into. So a region page overwrites their pixels while the imported car keeps
// sampling the coordinates, which reads as wrong UVs or wrong colours.
//
// So each imported page is remembered, and re-uploaded whenever the slot table no
// longer shows it as ours. The page bytes come back from the source city's level
// file, which is already open-able (ReadCarImportFile) - no need to hold megabytes.
// JERICHO: how many imported pages can be pinned at once. A 3-city mashup asks for 13
// (measured: 8 pinned + 5 DROPPED), and a dropped pin is not a miss - the index was
// already allocated by CarImportDstSetCore and baked into the model's polys, so its
// polys read the dummy (960,0), a live host slot. The ceiling is the free-index window
// CarImportDstSetCore allocates from (110..127, 18 indices): past that there is no index
// to pin anyway.
#define CAR_PIN_MAX 16

// JERICHO: the pool must be able to hold EVERY pin. That is what makes the world-side
// fallback in CarPageFindSlot unreachable rather than merely unlikely: a pin takes a lower
// half pool page first, and there are never more pins than pool pages (16 <= 30). If this
// ever stops holding, an import would have to evict a WORLD texture for its own page - the
// "buildings show the car's texture" corruption - so fail the build rather than ship it.
#if CAR_PIN_MAX > JER_POOL_PAGES
#error "CAR_PIN_MAX exceeds the lower half pool (JER_POOL_PAGES): an import could evict a world page"
#endif

static int sPinCount;
static int sPinDropped;			// JERICHO: pages asked for after the table filled - see CarPinRecord

// JERICHO: the sets CarPinRecord had to DROP, because the table was full.
//
// A dropped set still has an index that CarImportDstSetCore allocated and the model's
// polys baked, so the POLYS STILL READ ITS PALETTE ROW. That row is marked for upload from
// `sPinSet[]` - which a dropped set never enters - so before this, a drop meant a
// `civ_clut` row that nothing wrote, and the car wore whatever was there. The page is lost
// either way; the colour is not, so the drops are recorded and not merely counted.
#define CAR_DROP_MAX	16
static int sPinDroppedSet[CAR_DROP_MAX];
static int sPinDroppedCity[CAR_DROP_MAX];
static int sPinDroppedKept;		// recorded (vs counted only, once CAR_DROP_MAX is reached)
static int sPalDone;			// JERICHO: the deferred palette upload has run for THIS level
static unsigned char sPalRowsDone[CIV_CLUT_ROWS];	// JERICHO: which civ_clut rows are already
							// IN VRAM this level. The upload used to be a
							// one-shot latch, so a pin recorded after the
							// first call never got its rows; it is now
							// driven by "needed and not yet done".
static int sPalUploaded;		// JERICHO: how many CLUT slots it wrote (0 = nothing, the interesting failure)
static int sPinSet[CAR_PIN_MAX];		// the set number the CAR asks for
static int sPinIndex[CAR_PIN_MAX];		// the index its page is loaded at
static int sPinSlot[CAR_PIN_MAX];		// the slot it lives in, -1 while unplaced
static int sPinPool[CAR_PIN_MAX];		// JERICHO: the lower half pool page it lives in
						// instead, or -1 when it is in the base half. An
						// lower half pool pin has no slot: rows 512..1023 are not
						// tpagepos[] and must stay out of tpageslots /
						// tpageloaded / slot_clutpos, which are the world
						// streamer's slot space (spool.c indexes
						// slot_clutpos with tpageloaded[x] - 1).
static int sPinOffset[CAR_PIN_MAX];		// where its bytes are in the source city's file
static int sPinSize[CAR_PIN_MAX];
static int sPinCity[CAR_PIN_MAX];		// WHICH city's level file those bytes come from --
						// a level can hold more than one city's car data now,
						// so the page must be read from ITS OWN file
static int sPinPreferred[CAR_PIN_MAX];		// the rectangle the REPLACED car's page used, or -1
static RECT16 sPinClutCursor;			// walking CLUT-row cursor for the imported pages
static int sPinEvictions;			// world pages taken back this run, for the dump
static int sPinUnusedTakes;			// wasted host car pages taken instead - the #3 win
static int sJerPinPoolFull;			// JERICHO: pins that found the lower half pool full
static int sJerPinNoPage;			// JERICHO: pins refused a world rectangle (should stay 0 - see CAR_PIN_MAX)
static int sPinReloads;				// times we re-uploaded a page we had already placed - the thrash meter
static int sPinRowLeaks;			// imported sets whose row came back a HOST row (refused, and logged)
static int sPinBandSafe;			// the pin band starts inside the CLUT-safe area (above the font)

// JERICHO: the texture sets each imported model's OWN polygons name. buildNewCarFromModel
// collects them as it walks the poly stream (the engine's own, reliable PolySizes walk);
// LoadImportedTPages then imports only these instead of a whole city's six civilian sets.
// Without it a civilian import asks for 6 pages while the level leaves 5, so one page
// stays unplaced - and an unplaced page leaves texture_pages/texture_cluts at the DUMMY
// (960,0)/(960,16), which is inside a live page, so the car shows another page's pixels.
#define CAR_MODEL_SETS_MAX 16

static int sModelSetCount[MAX_CAR_RESIDENT_MODELS];
static unsigned char sModelSet[MAX_CAR_RESIDENT_MODELS][CAR_MODEL_SETS_MAX];

void CarModelSetsClear(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	sModelSetCount[slot] = 0;
}

void CarModelSetsAdd(int slot, int set)
{
	int i;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS || set <= 0 || set > 127)
		return;

	for (i = 0; i < sModelSetCount[slot]; i++)
	{
		if (sModelSet[slot][i] == set)
			return;		// already have it
	}

	if (sModelSetCount[slot] >= CAR_MODEL_SETS_MAX)
		return;

	sModelSet[slot][sModelSetCount[slot]++] = (unsigned char)set;
}

int CarModelSetCount(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	return sModelSetCount[slot];
}

int CarModelSet(int slot, int k)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS || k < 0 || k >= sModelSetCount[slot])
		return 0;

	return sModelSet[slot][k];
}

// JERICHO: is this texture set named by ANY built model (host or imported)? A loaded
// car page no model names is wasted VRAM - the first thing the pool should take.
int CarModelSetUsed(int set)
{
	int s, k;

	for (s = 0; s < MAX_CAR_RESIDENT_MODELS; s++)
	{
		for (k = 0; k < sModelSetCount[s]; k++)
		{
			if (sModelSet[s][k] == set)
				return 1;
		}
	}

	return 0;
}

static void CarPinRecord(int set, int index, int offset, int size, int preferred, int city, int slot)
{
	/* Idempotent: a set of a city is pinned ONCE. The level load records every imported
	 * car's sets, and a hot load re-runs that same walk (a car added after the level was
	 * built has a slot and its own sets, but no pin record) -- so without this the second
	 * pass would pin each existing set twice, and CarImportPin would place two copies. */
	{
		int p;

		for (p = 0; p < sPinCount; p++)
		{
			if (sPinSet[p] == set && sPinCity[p] == city)
				return;		/* already recorded: the pool copy is the one in use */
		}
	}

	if (sPinCount >= CAR_PIN_MAX)
	{
		// JERICHO: this used to return silently, which is what turned "the import ran out
		// of pins" into "this car wears the host's page": CarImportDstSetCore had already
		// allocated `index` and the model's polys baked it, so an index nothing fills keeps
		// the initialisation dummy GetTPage(0,0,960,0) - VRAM (960,0), which is a LIVE
		// slot. Say so, and count it, so the gap between what is asked for and what the
		// table holds is a number rather than a guess.
		sPinDropped++;

		if (sPinDroppedKept < CAR_DROP_MAX)
		{
			sPinDroppedSet[sPinDroppedKept] = set;
			sPinDroppedCity[sPinDroppedKept] = city;
			sPinDroppedKept++;
		}

		if (sPinDropped <= 4)
		{
			int drow = CarPalIndexInCityFor(set, city);

			printInfo("cross-city: NO PIN LEFT for %s set %d (index %d) - the table holds %d; its polys will read the dummy page (960,0), which is a live host slot, but its PALETTE ROW (%d) still gets uploaded\n",
				LevelNames[city], set, index, CAR_PIN_MAX, drow);
		}

		return;
	}

	sPinSet[sPinCount] = set;
	sPinIndex[sPinCount] = index;
	sPinSlot[sPinCount] = slot;		// which CAR this page belongs to (the manifest's key)
	sPinPool[sPinCount] = -1;
	sPinOffset[sPinCount] = offset;
	sPinSize[sPinCount] = size;
	sPinCity[sPinCount] = city;
	sPinPreferred[sPinCount] = preferred;
	sPinCount++;
}

// Called from the game loop. Cheap when nothing is wrong - a handful of compares -
// and only re-reads the file when a page really has been taken.
// Which slot an imported page should live in: a genuinely free one if there is one,
// otherwise a streamed WORLD page to evict.
//
// Evicting the world is safe because the world is demand-paged - it re-streams
// whatever it needs whenever it needs it - but the eviction has to CLEAR
// tpageloaded[held], because that is the engine's own "this is not loaded" marker and
// what makes it reload on next use. Without that the world would keep rendering the
// rectangle we just took.
//
// Never taken: slots below nperms (the level's permanent pages) and anything a host
// car or the host's special car needs. Breaking those is the thing this work exists
// to fix.
static int CarPageFindSlot(int allowWorld)
{
	int i, k;
	static int sVictim;

	// JERICHO: a slot free outright - but only one inside the LEVEL's own range. The free
	// slots in [slotsused,19) are the WORLD's stream pool: LoadInAreaTSets treats any
	// slot not already holding a set it needs as available, so taking one there makes the
	// world stream into a rectangle we hold. A free slot below nperms is a permanent page
	// and must never be taken either. That leaves [nperms, slotsused).
	for (i = 0; i < 19; i++)
	{
		int idx = (sVictim + i) % 19;

		if (idx >= nperms && idx < slotsused && tpageslots[idx] == 0xFF)
		{
			sVictim = idx;
			return idx;		// free outright, and not a rectangle the world will want
		}
	}

	// JERICHO: FIRST choice - a HOST car page that no built model names. The level
	// loads its whole carTpages/specTpages list up front, but a model only draws the
	// sets IT names, so a car page nothing names is pure waste (measured: Rio wastes
	// 2 of its 8). Taking one costs the world and the traffic nothing at all.
	//
	// This pass deliberately ignores nperms: the wasted pages sit in the level's own
	// range (Rio's were slots 5 and 9), which is exactly where the world-eviction pass
	// below refuses to look.
	for (i = 0; i < 19; i++)
	{
		int idx = (sVictim + i) % 19;
		int held = tpageslots[idx];
		int iscar = 0;

		if (held == 0xFF)
			continue;

		for (k = 0; k < 8; k++)
			if (carTpages[GameLevel][k] == held) { iscar = 1; break; }
		if (!iscar)
		{
			for (k = 0; k < 12; k++)
				if (specTpages[GameLevel][k] == held) { iscar = 1; break; }
		}

		if (!iscar || CarModelSetUsed(held))
			continue;

		tpageloaded[held] = 0;

		sVictim = idx;
		sPinUnusedTakes++;

		if (sPinUnusedTakes <= 6)
			printInfo("cross-city: paging - taking UNUSED host car page set %d from slot %d for an imported page\n", held, idx);

		return idx;
	}

	// JERICHO: the LAST resort - evict a world texture for an imported page. The import
	// never asks for this (see the call site): it is here so the placement rules live in
	// one place, and it is unreachable while CAR_PIN_MAX <= JER_POOL_PAGES.
	if (!allowWorld)
		return -1;

	for (i = 0; i < 19; i++)
	{
		int idx = (sVictim + i) % 19;
		int held = tpageslots[idx];

		if (idx < nperms || held == 0xFF)
			continue;

		// JERICHO: never evict a page WE imported. Our own sets would otherwise fight
		// each other for one rectangle - the model-9 run logged 'evicting world set 66'
		// where 66 was a page we had just placed - and a page swapped out from under the
		// car that needs it is exactly what "the UVs are bleeding" looks like.
		for (k = 0; k < sPinCount; k++)
		{
			if (sPinIndex[k] == held)
				break;
		}

		if (k != sPinCount)
			continue;

		for (k = 0; k < 8; k++)
		{
			if (carTpages[GameLevel][k] == held)
				break;
		}

		if (k != 8)
			continue;		// a host car needs it

		for (k = 0; k < 12; k++)
		{
			if (specTpages[GameLevel][k] == held)
				break;
		}

		if (k != 12)
			continue;		// the host's special car needs it

		tpageloaded[held] = 0;	// tell the engine to re-stream this one when it next wants it

		sVictim = idx;
		sPinEvictions++;

		if (sPinEvictions <= 6)
			printInfo("cross-city: paging - evicting world set %d from slot %d for an imported page\n", held, idx);

		return idx;
	}

	return -1;
}

// JERICHO: may an imported page take this rectangle as its PREFERRED one? Only if it
// currently holds one of the HOST SPECIAL car's own two pages (`carTpages[GameLevel][6]`
// and `[7]`, set at load) - the rectangle of the car an import replaces, which nothing
// else needs.
//
// This check is the fix for a real leak: the preferred slot used to be taken blindly,
// and it was `SPECIAL_CAR_SLOT + k` - the RESIDENT-MODEL constant (7), not the runtime
// texture slot the special pages actually live in (11-13 depending on the level). So the
// car's page was pinned onto two of the level's PERMANENT page rectangles and a building
// drew the car's texture. Anything that is not the replaced car's own page now goes
// through CarPageFindSlot, which knows the placement rules.
static int CarPinPreferredAllowed(int slot)
{
	int held;

	if (slot < 0 || slot >= 19)
		return 0;

	held = tpageslots[slot];

	if (held == 0xFF)
		return 0;

	return (held == carTpages[GameLevel][6] || held == carTpages[GameLevel][7]);
}

static void VramAccountReport(void);
static int LevelClutRowsNeeded(void);		// JERICHO: the level's own max CLUT rows

/* JERICHO: fill in the manifest for `slot` from the live tables. Called after the pin
 * walk, which is when that slot's pins and their pool pages are known. */
void CarSlotResNote(int slot, int city)
{
	CAR_SLOT_RES* r;
	int p;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	r = &sCarSlotRes[slot];

	r->used = 1;
	r->city = city;
	r->pins = 0;
	r->poolPages = 0;

	for (p = 0; p < sPinCount; p++)
	{
		if (sPinSlot[p] != slot)
			continue;

		r->pins++;

		if (sPinPool[p] >= 0)
			r->poolPages++;
	}

	r->clutRowBase = (city >= 0) ? CarImportPaletteBlockBase(city) : -1;
	r->clutRows = (r->clutRowBase >= 0) ? CIV_CLUT_BLOCK_ROWS : 0;

	printInfo("cross-city: slot %d holds %s: %d pin(s) (%d in the pool), civ_clut %s, geometry %d bytes\n",
		slot, (city >= 0) ? LevelNames[city] : "the level's own car",
		r->pins, r->poolPages,
		(r->clutRowBase >= 0) ? "blocked" : "none", r->geometryBytes);
}

/* JERICHO: the geometry side of the manifest, from the build that made it. */
void CarSlotResNoteGeometry(int slot, int bytes)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	sCarSlotRes[slot].used = 1;
	sCarSlotRes[slot].geometryBytes = bytes;
}

/* JERICHO: the whole manifest, for the log at level end and for anything that needs to
 * know what is still held. Returns how many slots hold cross-city resources. */
int CarSlotResReport(void)
{
	int slot, n = 0;

	for (slot = 0; slot < MAX_CAR_RESIDENT_MODELS; slot++)
	{
		CAR_SLOT_RES* r = &sCarSlotRes[slot];

		if (!r->used || (r->pins == 0 && r->geometryBytes == 0))
			continue;

		n++;
		printInfo("cross-city: held - slot %d (%s): %d pin(s), %d pool page(s), civ_clut %s, geometry %d bytes\n",
			slot, (r->city >= 0) ? LevelNames[r->city] : "the level's own car",
			r->pins, r->poolPages,
			(r->clutRowBase >= 0) ? "blocked" : "none", r->geometryBytes);
	}

	return n;
}

/* JERICHO cross-city hot load: record (and therefore pin) the texture pages an imported
 * car needs, for a slot built AFTER the level loaded.
 *
 * LoadImportedTPages is the level-load walk that does this for every slot. A slot that
 * appeared later has a city and its own sets (buildNewCarFromModel filled them) but no
 * pin record, so CarImportPin has nothing to place for it and its polys read whatever
 * their baked index happens to hold -- measured as "the model retained the local city's
 * materials". Re-running that walk is exactly the right unit of work: it derives each
 * slot's sets from the model, and CarPinRecord is now idempotent, so the cars already
 * pinned are left alone. The rows follow by themselves -- CarImportPin uploads a page's
 * CLUT rows when it places the page. */
void JerHotLoadCarTpages(void)
{
	LoadImportedTPages();
}

void CarImportPin(void)
{
	int i;
	// JERICHO: the import's car palettes are uploaded HERE, not during the level load.
	//
	// The palette lump is a whole foreign city's (228 CLUTs, 57 column rows) and the rows
	// to keep are the ones the BUILT model draws from - which do not exist until its poly
	// stream has been walked. Deferring the upload to this point is what lets it be
	// filtered: measured, the model uses 2 of the import bank's 8 rows, so ~43 of the 57
	// rows are given back, which is more than the 19 the CLUT column is short of the level
	// font (cars.h, VRAM.md §6).
	//
	// Once per LEVEL, not once per process: this used to be a function-static that
	// CarImportResetState never cleared, so a second level in the same session never
	// uploaded a guest city's palettes at all - and its deferred lumps were the previous
	// level's. See sPalDone at file scope.
	{
		unsigned char rowNeeded[CIV_CLUT_ROWS];
		unsigned char fresh[CIV_CLUT_ROWS];
		int r, nfresh = 0;

		memset(rowNeeded, 0, sizeof(rowNeeded));

		for (i = 0; i < sPinCount; i++)
		{
			// JERICHO: the pin knows which city this page came from (sPinCity), so ask
			// that city's table directly. GetCarPalIndex would instead search every
			// held city and take the first that has the page number - which with
			// three cities is the first city, leaving the others' blocks empty.
			int row = CarPalIndexInCityFor(sPinSet[i], sPinCity[i]);

			if (row >= CIV_CLUT_IMPORT_ROW && row < CIV_CLUT_ROWS)
				rowNeeded[row] = 1;
		}

		// JERICHO: AND the sets CarPinRecord had to DROP for want of table room. Their
		// page is lost - the polys will read the dummy page - but the model's polys still
		// baked an index whose CLUT comes from civ_clut, so the ROW is still read. Leaving
		// it unmarked is what made a drop show up as a car wearing a colour nothing wrote.
		for (i = 0; i < sPinDroppedKept; i++)
		{
			int drow = CarPalIndexInCityFor(sPinDroppedSet[i], sPinDroppedCity[i]);

			if (drow >= CIV_CLUT_IMPORT_ROW && drow < CIV_CLUT_ROWS)
				rowNeeded[drow] = 1;
		}

		// JERICHO: AND every row of every held city's block, not only the rows the models
		// built so far happen to name. The import is as-needed, and a city's block is 8 of
		// the column's 32 rows, so uploading all of it is affordable - and it is what makes
		// a spawned car's COLOUR VARIANT reachable, because a variant is a COLUMN within a
		// row: civ_ai picks ap.palette 0..5 on spawn, and the car built first is not
		// necessarily the one that ends up wearing that row. With only the named rows kept,
		// exactly the columns some earlier model happened to name exist, and the row filter
		// was solving a space problem the lower half pool no longer has.
		//
		// This is NOT what fixes a colourless imported special: that car reads rows whose
		// data is absent from its lump entirely (see PALETTES.md). It fixes variants.
		for (i = 0; i < 4; i++)
		{
			int base = CarImportPaletteBlockBase(i), r2, end;

			if (base < 0)
				continue;

			end = base + CIV_CLUT_BLOCK_ROWS;
			if (end > CIV_CLUT_ROWS)
				end = CIV_CLUT_ROWS;

			for (r2 = base; r2 < end; r2++)
				rowNeeded[r2] = 1;
		}

		// JERICHO: what is needed that has NOT already been uploaded.
		//
		// This used to be gated by a one-shot latch (`if (!sPalDone && sPinCount > 0)`), so
		// a pin recorded AFTER the first call never got its rows at all: the latch was
		// already set and the new row was simply never asked for. Driving on "needed and not
		// yet done" instead means a late pin costs a re-upload of the rows it adds - and
		// nothing when it adds none, which is the common case and why this stays cheap.
		for (r = 0; r < CIV_CLUT_ROWS; r++)
		{
			fresh[r] = (rowNeeded[r] && !sPalRowsDone[r]) ? 1 : 0;
			nfresh += fresh[r];
		}

		if (nfresh > 0)
		{
			// JERICHO: how many rows this level asked for. A zero here was the signature of
			// the 2-row-band bug (rows fell outside the band and the upload wrote nothing),
			// so it belongs in the census rather than being inferred from a column total
			// that has lied before.
			sPalDone = 1;

			for (r = 0; r < CIV_CLUT_ROWS; r++)
				if (rowNeeded[r])
					sPalUploaded += 1;

			// Remember what is now in VRAM BEFORE the upload, so a row that is asked for
			// again later is not re-uploaded (and does not consume a second lower half pool row).
			memcpy(sPalRowsDone, rowNeeded, sizeof(sPalRowsDone));

			ProcessImportedPaletteRows(fresh);

			// JERICHO: re-run the census NOW. LoadPermanentTPages reports the CLUT column
			// during the level load, which is before this upload - so it would keep saying
			// "no overflow" about a column that this upload then ran past. The number to
			// read is the one after everything the import puts in the column is in it.
			VramAccountReport();

			printInfo("cross-city: palette rows needed %d, newly uploaded %d (the rest were already in VRAM)\n",
				sPalUploaded, nfresh);
		}
	}

	// JERICHO-DIAG: once, what the 19 VRAM slots actually hold. #3 may only take a
	// rectangle without hurting the world if it holds a CAR page no live model is
	// using - everything else is a streamed world page. This is the map for that.
	{
		static int sDumped;

		if (!sDumped && sPinCount > 0)
		{
			int s, k, carpages = 0, unused = 0;

			sDumped = 1;

			for (s = 0; s < 19; s++)
			{
				int held = tpageslots[s];
				int iscar = 0;

				if (held == 0xFF)
				{
					printInfo("cross-city: slot %2d at (%3d,%3d): FREE\n", s, tpagepos[s].x, tpagepos[s].y);
					continue;
				}

				for (k = 0; k < 6; k++)
					if (carTpages[GameLevel][k] == held) { iscar = 1; break; }
				for (k = 0; k < 12; k++)
					if (specTpages[GameLevel][k] == held) { iscar = 1; break; }

				printInfo("cross-city: slot %2d at (%3d,%3d): set %3d %s (loaded=%d%s)\n",
					s, tpagepos[s].x, tpagepos[s].y, held,
					iscar ? "HOST CAR PAGE" : "world       ", tpageloaded[held] != 0,
					(iscar && !CarModelSetUsed(held)) ? " UNUSED" : "");

				if (iscar)
				{
					carpages++;
					if (!CarModelSetUsed(held))
						unused++;
				}
			}

			printInfo("cross-city: %d of 19 slots hold host car pages, %d of them named by no model\n", carpages, unused);
		}
	}

	for (i = 0; i < sPinCount; i++)
	{
		char* buf;
		RECT16 tpage, clut;
		int slot;
		int pool = -1;

		// JERICHO: a lower half pool pin cannot be taken back. Rows 512..1023 are not
		// tpagepos[], so the world streamer cannot target them and no host page can
		// land on one - there is nothing to refresh and nothing to re-upload.
		if (sPinPool[i] >= 0)
			continue;

		if (sPinSlot[i] >= 0 && tpageslots[sPinSlot[i]] == sPinIndex[i] && tpageloaded[sPinIndex[i]] != 0)
		{
			// Still ours: refresh the claim so a car that keeps being drawn keeps its
			// rectangle, while one that stops being drawn is released (CarPageRectOwned).
			sCarPageClaimFrame[sPinSlot[i]] = FrameCnt;
			continue;
		}

		// JERICHO: THE LOWER HALF POOL FIRST. Rows 512..1023 are space nothing else in the engine
		// computes - not the world streamer (tpagepos[] holds Y in {0,256}), not the
		// CLUT cursors (bounded by CD2_CLUT_SAFE_LAST) - so a page placed here takes
		// nothing from the world and nothing from a host car. That IS the fix: INV1
		// ("pinned to a WORLD-pool rectangle") and "buildings show the car's texture"
		// are both what happens when an import has to TAKE a rectangle. The slot passes
		// below stay as the fallback for when the lower half pool is full (30 pages, and an import
		// needs a handful per set).
		//
		// This choice belongs here rather than in CarPageFindSlot: that returns a
		// tpagepos[] INDEX and the rectangle is derived from it, so a lower half pool page has no
		// index to return.
		pool = JerLowerPoolPageAlloc();

		if (pool >= 0)
		{
			JerLowerPoolPageRect(pool, &tpage);
			slot = -1;
		}
		else
		{
			// JERICHO: the pool is FULL, so this page has to come out of space the WORLD also
			// knows about. That is exactly the long-body worry: a model that names more pages
			// than the pool has free can put a car page on a rectangle the world streams into,
			// and the world then draws the car's texture. Counted here, and the slot passes
			// below try the import's OWN rectangles (its previous one, else the replaced car's)
			// first, so a genuine world slot stays the last resort rather than the first guess.
			sJerPinPoolFull++;

			if (sJerPinPoolFull <= 4)
			{
				printInfo("cross-city: pin - lower half pool FULL (%d of %d pages in use); set %d must come from world-side space\n",
					sJerLowerPoolPagesUsed, JER_POOL_PAGES, sPinSet[i]);
			}

			// Where to put it: the slot it had before (taking that rectangle back), else the
			// rectangle the REPLACED car's page used, else a free one, else a world stream to
			// evict.
			//
			// That middle option is the important one. Picking merely the first free slot -
			// which is by definition a slot the world streams into - put imported textures on
			// rectangles buildings were using, so buildings showed the car's texture. An
			// import REPLACES a car, so it should take that car's own rectangles and leave the
			// world alone.
			slot = sPinSlot[i];

			if (!CarPinPreferredAllowed(slot))
				slot = sPinPreferred[i];

			if (!CarPinPreferredAllowed(slot))
			{
				// JERICHO: NOT the world. CarPageFindSlot's last resort evicts a WORLD texture for
				// the import's page, and that is what makes a building draw a car's colours. It is
				// unreachable while CAR_PIN_MAX <= JER_POOL_PAGES - every pin fits in the pool, and
				// the pool is tried first - and passing 0 keeps it that way even if those numbers
				// move: an import gets the pool page, a wasted host car page, or a free slot, and
				// never a world rectangle. Refusing costs a wrong-looking car; taking one costs the
				// world.
				slot = CarPageFindSlot(0);

				if (slot < 0)
				{
					sJerPinNoPage++;

					if (sJerPinNoPage <= 6)
					{
						printInfo("cross-city: pin - set %d has no rectangle it may take (pool full, no wasted car page free): leaving it for a later frame rather than evicting a world page\n",
							sPinSet[i]);
					}
				}
			}

			if (slot < 0)
				continue;		// nothing evictable this frame; try again next frame
		}

		buf = (char*)malloc(sPinSize[i]);

		if (buf == NULL)
		{
			if (pool >= 0)
				JerLowerPoolPageFree(pool);

			continue;
		}

		/* The page's bytes come from ITS OWN city's level file -- a level can hold
		 * more than one city's car data, so neither the file nor the page base may
		 * come from the level-wide singleton. */
		if (!ReadCarImportFileForCity(sPinCity[i], GetCarImportPageBaseForCity(sPinCity[i]) + sPinOffset[i], buf, sPinSize[i]))
		{
			free(buf);

			if (pool >= 0)
				JerLowerPoolPageFree(pool);

			continue;
		}

		if (pool < 0)
		{
			tpage.x = tpagepos[slot].x;
			tpage.y = tpagepos[slot].y;
			tpage.w = 64;
			tpage.h = 256;
		}

		// CLUT rows come from a LOCAL walker across the imported sets, and it starts ABOVE
		// the rows the level's slots use.
		//
		// Two things forced that. Using slot_clutpos[slot] was wrong for the preferred
		// rectangles - the replaced special car's slots 7/8 - because those entries are
		// never assigned (the tail loop only fills slots from slotsused onward), so the
		// CLUTs went to (0,0) and the car sampled CLUT 0: the 'crazy colors' report. Then
		// starting the walker at clutpos collided with the SLOTS' own band, which also
		// begins at clutpos and walks 8 rows per slot - so streamed pages kept overwriting
		// our palette, which is why the palette check said MISMATCH with real positions.
		if (sPinClutCursor.x == 0 && sPinClutCursor.y == 0)
		{
			// JERICHO: the import's CLUT rows come from the lower half pool's own column - x960..1023,
			// rows 512..1023 - NOT from the base half's strip.
			//
			// The strip is the scarce resource, and this was the last thing still competing
			// for it. It holds ~86 rows above the level font (pres.c:584) and the level's own
			// layout plus the streamed-slot band take most of that, so a 3-city mix ran out
			// and sets were refused. Below row 512 there is no font and no slot band: 512
			// rows, mirrored at the same x, so IncrementClutNum walks it unchanged.
			//
			// AND it starts where the lower half pool column is ACTUALLY free - i.e. past the guest
			// palette tables, which are uploaded just above this - and advances the SAME
			// watermark (see the walk's tail below).
			//
			// Both used to start at JER_VRAM_HALF_Y with INDEPENDENT cursors -
			// `sPinClutCursor` here, `sJerLowerPoolClutY` in the palette upload - so the pin's
			// page CLUTs were written straight over rows 512.. and the imported car's
			// flat/GT PANEL polys read page-CLUT data: a broken palette on every panel.
			// Measured before the fix: the palettes took rows 512..545 and the band then
			// started at 512 again.
			RECT16 poolClut;
			int firstFree;

			JerLowerPoolClutCursor(&poolClut);
			firstFree = poolClut.y;

			sPinClutCursor.x = 960;
			sPinClutCursor.y = firstFree;
			sPinClutCursor.w = 16;
			sPinClutCursor.h = 1;

			sPinBandSafe = 0;	// the lower half pool column, not the base half's strip: nothing to overflow into

			printInfo("cross-city: imported CLUT rows start at y=%d (level layout ends at %d, %d slots spare, safe area ends at %d)\n",
				firstFree, clutpos.y, 19 - slotsused, CD2_CLUT_SAFE_LAST);
		}

		clut = sPinClutCursor;

		// JERICHO: the CLUT walker advances forward and WRAPS at the bottom of VRAM
		// (IncrementClutNum), landing back at the top in the TEXTURE area. A latent
		// safety net: with the slot starved as it is (see below) the walk never gets
		// that far, but any set whose rows would run past the CLUT-safe area is left
		// unplaced rather than painting over a texture page OR over the level font.
		{
			int npal = *(int*)buf;
			int need = (npal + 3) / 4 + 1;	// CLUT rows -> VRAM rows, 4 per row, +1 for a mid-row start
			// JERICHO: the boundary is the END OF VRAM now, not the level font. The pool
			// column runs x960..1023 / y512..1023 and IncrementClutNum wraps the row at the
			// bottom, so the only thing worth refusing is a set that would run off the end of
			// the buffer and be carried back into a real texture page. With 512 rows against
			// a handful per car this is a formality - but it is the honest one, and it no
			// longer has anything to do with the glyphs.
			int limit = JER_VRAM_TOTAL_ROWS;
			if (sPinClutCursor.y + need > limit)
			{
				printInfo("cross-city: %s set %d left unplaced - %d CLUT rows from y=%d would %s\n",
					LevelNames[sPinCity[i]], sPinSet[i], npal, sPinClutCursor.y,
					sPinBandSafe ? "reach the level font (the safe area ends there)" : "wrap into a texture page");

				free(buf);

				if (pool >= 0)
					JerLowerPoolPageFree(pool);

				continue;
			}
		}

		// Ours to write: bypass the ownership guard for this upload, then mark the
		// rectangle owned so the engine's own uploads to it are refused from here on.
		sCarPageUploading = 1;
		sPinReloads++;	// counts re-uploads, i.e. how often the engine took a page back
		LoadTPageAndCluts(&tpage, &clut, sPinIndex[i], buf);
		sCarPageUploading = 0;

		if (clut.x != sPinClutCursor.x || clut.y != sPinClutCursor.y)
		{
			// The walker advanced. Keep the lower half pool watermark level with it, so the next
			// walk - another pin's page CLUTs, or a guest palette table - starts PAST this
			// band instead of on top of it. Without this the two cursors drift apart and
			// the palettes get overwritten (a broken palette on every panel).
			int before = sPinClutCursor.y;

			sPinClutCursor = clut;	// the walker advanced: remember where it got to

			if (clut.y > before)
				JerLowerPoolClutAdvance(clut.y - before);
		}

		if (pool >= 0)
		{
			// An lower half pool pin. Deliberately NOT recorded in tpageslots / tpageloaded /
			// slot_clutpos - those describe the world streamer's slot space, and a page in
			// rows 512..1023 is not one of its slots. CarPageRectOwned knows the lower half pool
			// separately, so the engine's own uploads are still refused.
			sPinPool[i] = pool;
		}
		else
		{
			sCarPageClaimFrame[slot] = FrameCnt;

			tpageslots[slot] = (u_char)sPinIndex[i];
			tpageloaded[sPinIndex[i]] = (u_char)slot;

			sPinSlot[i] = slot;
		}

		// JERICHO: buildNewCarFromModel caches each set's palette-0 CLUT into its
		// civ_clut row - but that build runs at load time, before this upload, so for
		// an imported set it cached the (960,16) dummy. The page's CLUTs are only
		// really in VRAM now, so re-point the row's base entry at them. Without this
		// the car renders with whatever the dummy area happens to hold.
		{
			int row = CarPalIndexInCityFor(sPinSet[i], sPinCity[i]);
			int j;

			// JERICHO: never write a HOST row for an imported set. A row below the import
			// bank means either (0..7) the set IS a host page - re-pointing then hands a host
			// palette the imported page's CLUTs, i.e. the import repaints a local car - or
			// (-1) the set is in neither of the source city's tables, so there is no row for
			// it at all. Both are refused and said out loud: a silent skip is how this class
			// of leak stayed invisible. (-1 rather than 0 is the more precise answer, and is
			// what CarPalIndexInCityFor now returns when the city genuinely has no row.)
			if (row < CIV_CLUT_IMPORT_ROW)
			{
				// JERICHO: log EVERY refusal, and count them.
				//
				// This was capped at four lines, which made the FIFTH set's refusal
				// invisible - and tools/crosscheck.py's INV2 reads refusals from this very
				// log, so it reported "the pin did NOT refuse a host row" for a set the
				// engine had just correctly refused (measured: the 3-city mix's set 1, which
				// is pinned last and was therefore the fifth). An assertion that can be
				// suppressed by volume is not an assertion. The volume is a handful of lines
				// per level.
				sPinRowLeaks++;

				printInfo("cross-city: pin - set %d resolves to civ_clut row %d (below the import bank: a host row, or no row at all): not re-pointing, palette leak avoided\n",
					sPinSet[i], row);
			}
			else
			{
				for (j = 0; j < 32; j++)
					civ_clut[row][j][0] = texture_cluts[sPinIndex[i]][j];
			}
		}

		free(buf);
	}
}

// JERICHO: report where the imported sets' pages actually ended up, by decoding the
// tpage/clut values the draw path will read back into VRAM coordinates.
//
// This is the streaming check. Imported pages are uploaded during the level load,
// and the engine streams region pages into the same slot table as you drive - so a
// page can be replaced by something else entirely, which a car sampling it looks
// exactly like wrong UVs or wrong colours. Logging the values at the end of a run
// says whether what was placed is still what is there.
//
// tpage packing (libgpu.h): x = ((v)      & 0xf) << 6; y = ((v >> 4) & 1) * 256 + ((v >> 11) & 1) * 512.
// clut packing:  x = ((v) & 0x3f) << 4;  y = v >> 6.
static void CarImportDumpPageRefs(void);
static void CarImportPageRect(unsigned int page, int* px, int* py);	// defined below, used by the dump above
void CarPalRowReport(void);		// JERICHO: the civ_clut row ownership map (cars.c)

// Is ANY city held for this level? The per-city replacement for "is there a guest
// city at all" - the old single-city test (GetCarImportCity() < 0) only answered
// for the FIRST held city.
static int CarImportAnyHeld(void)
{
	int c;

	for (c = 0; c < 4; c++)
		if (CarImportCityHeld(c))
			return 1;

	return 0;
}

void CarImportDumpState(void)
{
	int i, k;

	if (CarImportAnyHeld() == 0 && sRemapCount == 0)
		return;

	printInfo("cross-city: final page state (%d pinned, %d wasted car pages taken, %d world pages evicted, %d page re-uploads, %d claims given back, %d pins DROPPED, %d palette rows uploaded, %d palette rows REFUSED)\n", sPinCount, sPinUnusedTakes, sPinEvictions, sPinReloads, sCarPageGiveBacks, sPinDropped, sPalUploaded, sPinRowLeaks);

	// JERICHO: the pool budget against what the models actually ask for.
	//
	// The lower half pool is 30 pages at rows 512..1023, and an import places its pages
	// THERE precisely so it takes nothing from the world. So "can an import exhaust it?" is
	// answered by two numbers: how many pages each BUILT model names (its sets - the pages
	// that must be resident together), and how much of the pool the run used. A long body
	// names more sets than a short one, which is why a limo or a bus would be first to run out.
	{
		char setbuf[160];
		int pos;

		for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		{
			if (sModelSetCount[i] == 0)
				continue;

			pos = 0;
			setbuf[0] = 0;

			for (k = 0; k < sModelSetCount[i]; k++)
			{
				int n = snprintf(setbuf + pos, sizeof(setbuf) - pos, "%s%d", (pos > 0) ? "," : "", sModelSet[i][k]);

				if (n <= 0 || pos + n >= (int)sizeof(setbuf))
					break;

				pos += n;
			}

			printInfo("cross-city: model sets - resident slot %d names %d set(s): %s\n",
				i, sModelSetCount[i], setbuf);
		}

		printInfo("cross-city: lower half pool at exit: %d of %d pages used (%d free); %d pin(s) had to leave the pool, %d pin(s) refused a world rectangle (want 0)\n",
			sJerLowerPoolPagesUsed, JER_POOL_PAGES, JER_POOL_PAGES - sJerLowerPoolPagesUsed,
			sJerPinPoolFull, sJerPinNoPage);
	}

	// JERICHO: which of the import bank's rows the imported model actually uses.
	//
	// This is the number that sizes the CLUT reclaim (VRAM.md §6). The palette lump is a
	// WHOLE foreign city's table - 228 CLUTs, 57 column rows - and it is uploaded for
	// every row in the bank whether the imported model draws from that row or not. The
	// bank is 8 rows (8..15, one per car slot in that city) and an import is ONE model,
	// so the rows below are what a partial upload would have to keep. If this ever says
	// 8 of 8 the reclaim is worth nothing and that has to be known before writing it.
	{
		int seen[CIV_CLUT_ROWS];
		int nrow = 0, r, k2;

		memset(seen, 0, sizeof(seen));

		for (k2 = 0; k2 < sPinCount; k2++)
		{
			int row = CarPalIndexInCityFor(sPinSet[k2], sPinCity[k2]);

			if (row >= CIV_CLUT_IMPORT_ROW && row < CIV_CLUT_ROWS && !seen[row])
			{
				seen[row] = 1;
				nrow++;
			}
		}

		if (nrow > 0)
		{
			char list[64];
			int at = 0;

			list[0] = 0;

			for (r = CIV_CLUT_IMPORT_ROW; r < CIV_CLUT_ROWS; r++)
			{
				if (seen[r])
					at += snprintf(list + at, sizeof(list) - at, "%s%d", (at > 0) ? "," : "", r);
			}

			printInfo("cross-city: the imported model uses %d of the import bank's %d civ_clut rows (rows %s, bank is %d..%d) - %d idle\n",
				nrow, CIV_CLUT_ROWS - CIV_CLUT_IMPORT_ROW, list, CIV_CLUT_IMPORT_ROW, CIV_CLUT_ROWS - 1,
				(CIV_CLUT_ROWS - CIV_CLUT_IMPORT_ROW) - nrow);
		}
	}

	// Every pinned set and the rectangle it occupies, decoded from the tpage/clut values
	// the draw path will read. This is what the VRAM dump is aimed at: run with
	// JERICHO_DUMPVRAM=1 and feed these rectangles to tools/vramdump.py. If the car's
	// rectangle no longer looks like a page, something streamed over it.
	for (k = 0; k < sPinCount; k++)
	{
		int pslot = sPinSlot[k];
		unsigned int page = texture_pages[sPinIndex[k]];
		unsigned int clut = texture_cluts[sPinIndex[k]][0];

		// Decode the rectangle from the page word the DRAW path will read, rather than from
		// tpagepos[slot]: a lower half pool pin has no slot (rows 512..1023 are not tpagepos[]), and
		// this is what the car actually samples, so it reports the truth for both halves.
		{
			int px = -1, py = -1;

			CarImportPageRect(page, &px, &py);

			printInfo("cross-city:   pinned set %d index %d: slot=%d%s, rect=(%d,%d), page=%04x, clut0=%04x=(%d,%d)\n",
				sPinSet[k], sPinIndex[k], pslot,
				(sPinPool[k] >= 0) ? " (pool)" : "",
				px, py,
				page, clut, (int)((clut & 0x3f) << 4), (int)(clut >> 6));
		}
	}

	for (k = 0; k < sRemapCount; k++)
	{
		unsigned int page = texture_pages[sRemapTo[k]];
		unsigned int clut = texture_cluts[sRemapTo[k]][0];

		printInfo("cross-city:   set %d is at index %d: page=%04x => (%d,%d), clut0=%04x => (%d,%d)\n",
			sRemapFrom[k], sRemapTo[k], page,
			(int)((page & 0xf) << 6), (int)(((page >> 4) & 1) * 256 + ((page >> 11) & 1) * 512),
			clut, (int)((clut & 0x3f) << 4), (int)(clut >> 6));
	}

	// and the pages we imported under their own numbers. Iterating the PINS, not the
	// remaps: a remap can exist for a set whose upload was refused for want of a spare
	// slot, and calling that 'replaced' was a false report (it was never placed).
	for (i = 0; i < sPinCount; i++)
	{
		// An lower half pool pin keeps tpageloaded[] clear ON PURPOSE - rows 512..1023 are not the
		// world streamer's slot space - so this test would report every one of them as
		// replaced. Nothing can replace a lower half pool page; check it first.
		if (sPinPool[i] >= 0)
			continue;

		if (tpageloaded[sPinIndex[i]] == 0)
			printInfo("cross-city:   index %d no longer looks loaded - something replaced it\n", sPinIndex[i]);
	}

	// The decisive one: which slot index each imported page occupies, and whether the
	// slot table still says that slot is ours. An imported page sits at tpagepos[slot]
	// - the same VRAM rectangle the engine's own slot occupies - so if streaming has
	// taken that slot back, the next streamed page lands on our pixels while the car
	// keeps sampling the coordinates. That is what 'the UVs look off' actually is.
	for (i = 0; i < slotsused; i++)
	{
		if (tpageloaded[tpageslots[i]] != 0 && tpageslots[i] >= 110)
			printInfo("cross-city:   slot %d still holds imported set %d - intact\n", i, tpageslots[i]);
	}

	for (i = 0; i < sRemapCount; i++)
	{
		printInfo("cross-city:   imported index %d sits at slot rect (%d,%d) - shared with slot %d\n",
			sRemapTo[i], tpagepos[sRemapTo[i] - 100].x, tpagepos[sRemapTo[i] - 100].y, sRemapTo[i] - 100);
	}

	CarImportDumpPageRefs();
	CarPalRowReport();
}

// JERICHO-HOOK: what the IMPORTED MODEL actually samples, poly by poly.
//
// The palette survives a pin because the CLUT is re-pointed at draw time: CarImportPin
// writes civ_clut and the car reads it through pciv_clut on every draw. A PAGE has no such
// indirection. buildNewCarFromModel BAKES the page id into every poly from
// texture_pages[CarSetRemap(set)] at build time (cars.c:1293-1375), and CarImportPin only
// fills texture_pages[sPinIndex] later, at draw time. So "the palette is right but the
// texture is wrong" is exactly the shape of a stale page id: the poly keeps the id it was
// built with, while the pixels that id decodes to are whatever is at that rectangle now.
//
// For every imported slot this prints the three things that decide it: the distinct page
// ids the polys carry and the VRAM rectangle each decodes to; the u/v window the model
// addresses (so a window sitting in the wrong band of a page - wheel wells across the body
// - is numbers rather than a screenshot); and what the pin actually put in texture_pages
// for that set, and where. A poly page id matching no pinned set is the fault; one whose
// rectangle a live slot now owns is the other half of it.
static void CarImportPageRect(unsigned int page, int* px, int* py)
{
	// PSX tpage id: bits 0-3 = x in 64-texel units, bit 4 = y bit 8, bit 11 = y bit 9.
	if (px) *px = (int)(page & 0xf) * 64;
	if (py) *py = (int)(((page >> 4) & 1) * 256 + ((page >> 11) & 1) * 512);
}

static void CarImportDumpOneModel(const char* which, int slot, CAR_MODEL* m)
{
	unsigned int seen[8];
	int count[8], pinnedCount[8];
	int nseen = 0, npoly = 0, k, list, nOnPinned = 0;
	int umin = 0xffff, umax = -1, vmin = 0xffff, vmax = -1;

	if (m == NULL)
		return;

	for (k = 0; k < 8; k++)
		seen[k] = 0, count[k] = 0, pinnedCount[k] = 0;

	for (list = 0; list < 3; list++)
	{
		CAR_POLY* polys = (list == 0) ? m->pFT3 : (list == 1) ? m->pGT3 : m->pB3;
		int num = (list == 0) ? m->numFT3 : (list == 1) ? m->numGT3 : m->numB3;

		for (k = 0; k < num; k++)
		{
			CAR_POLY* poly = &polys[k];
			unsigned int id = (unsigned int)((poly->tpage_uv1 >> 16) & 0xffff);
			int j, u, v;

			npoly++;

			for (j = 0; j < nseen; j++)
				if (seen[j] == id)
					break;
			if (j == nseen && nseen < 8)
			{
				seen[nseen] = id;
				count[nseen] = 0;
				nseen++;
			}
			if (j < nseen)
				count[j]++;

			u = poly->clut_uv0 & 0xff;			if (u < umin) umin = u; if (u > umax) umax = u;
			v = (poly->clut_uv0 >> 8) & 0xff;	if (v < vmin) vmin = v; if (v > vmax) vmax = v;
			u = poly->uv3_uv2 & 0xff;			if (u < umin) umin = u; if (u > umax) umax = u;
			v = (poly->uv3_uv2 >> 8) & 0xff;	if (v < vmin) vmin = v; if (v > vmax) vmax = v;
		}
	}

	printInfo("cross-city: imported slot %d %s model: %d polys, u=%d..%d v=%d..%d\n",
		slot, which, npoly, umin, umax, vmin, vmax);

	for (k = 0; k < nseen; k++)
	{
		unsigned int page = texture_pages[seen[k] & 0xff];
		int px, py, pinned = -1, j;

		// An imported model bakes the INDEX (CAR_BAKE_TPAGE), so seen[k] is an index into
		// texture_pages - resolve it the way the draw path does (CAR_TPAGE_OF) and compare
		// against the index the pin filled. If those agree, the car reads the imported page.
		CarImportPageRect(page, &px, &py);

		for (j = 0; j < sPinCount; j++)
			if ((unsigned int)sPinIndex[j] == (seen[k] & 0xff))
				pinned = j;

		if (pinned >= 0)
			nOnPinned += count[k];

		pinnedCount[k] = (pinned >= 0);

		printInfo("cross-city:   poly tpage index %d -> texture_pages=%04x rect (%d,%d), %d poly(s) - %s\n",
			(int)(seen[k] & 0xff), page, px, py, count[k],
			(pinned >= 0) ? "a pinned index"
			: "NOT a pinned index - the model names a set the import never took");

		if (pinned >= 0 && sPinSlot[pinned] >= 0)
			printInfo("cross-city:     ...pinned into slot %d at (%d,%d) - %s\n",
				sPinSlot[pinned], tpagepos[sPinSlot[pinned]].x, tpagepos[sPinSlot[pinned]].y,
				(tpagepos[sPinSlot[pinned]].x == px && tpagepos[sPinSlot[pinned]].y == py)
					? "THE SAME RECTANGLE the poly resolves to"
					: "a DIFFERENT rectangle (the table moved after this frame's draw)");

		// JERICHO: WHICH set is it, and why did nothing fill it? The index alone cannot
		// say, because CarImportDstSetCore folds a source set into its destination. Ask
		// the three places that decide: the import's own remap table (it meant to take
		// this set), a guest city's page list (the loader should have loaded it), the
		// host's own resolution (the import deliberately kept the host's page), or
		// nothing at all (which is the fault).
		if (pinned < 0)
		{
			int r, c, s2, found = 0;

			for (r = 0; r < sRemapCount; r++)
			{
				// Both directions matter: seen[k] is usually a DESTINATION the import
				// allocated (110..127), which is the interesting case - the import
				// decided to take this set, so an unfilled index means the pin never
				// got to it.
				if (sRemapTo[r] == (int)(seen[k] & 0xff))
				{
					printInfo("cross-city:     index %d IS an index the import allocated, for source set %d\n",
						(int)(seen[k] & 0xff), sRemapFrom[r]);
					found = 1;
				}
				else if (sRemapFrom[r] == (int)(seen[k] & 0xff))
				{
					printInfo("cross-city:     index %d is a source set the import remapped -> %d\n",
						(int)(seen[k] & 0xff), sRemapTo[r]);
					found = 1;
				}
			}

			for (c = 0; c < 4; c++)
			{
				for (s2 = 0; s2 < gCarImportPerms[c].count; s2++)
					if (gCarImportPerms[c].set[s2] == (int)(seen[k] & 0xff))
					{ printInfo("cross-city:     source set %d: in %s's page list at perms[%d]\n",
						(int)(seen[k] & 0xff), LevelNames[c], s2); found = 1; }

				for (s2 = 0; s2 < gCarImportSpecs[c].count; s2++)
					if (gCarImportSpecs[c].set[s2] == (int)(seen[k] & 0xff))
					{ printInfo("cross-city:     source set %d: in %s's SPECIAL list at specs[%d]\n",
						(int)(seen[k] & 0xff), LevelNames[c], s2); found = 1; }
			}

			if (HostOwnsCarTPage((int)(seen[k] & 0xff)))
				printInfo("cross-city:     index %d is a page the HOST already resolved\n", (int)(seen[k] & 0xff));

			if (!found)
				printInfo("cross-city:     index %d is in NO city's page list and was never remapped - nothing could ever fill it\n",
					(int)(seen[k] & 0xff));
		}
	}

	// the one-line regression check: a poly whose index is not a pinned one is drawing a
	// rectangle the import never filled, which is exactly the fault this instrument exists
	// to catch (before the fix this read 0 of 254)
	if (npoly > 0)
		printInfo("cross-city: page check - slot %d %s: %d of %d polys resolve to a pinned imported page%s\n",
			slot, which, nOnPinned, npoly,
			(nOnPinned == npoly) ? " (all of them)"
			: " - the rest name a set the import has no page for");
}

static void CarImportDumpPageRefs(void)
{
	int slot, n;

	for (slot = 0; slot < MAX_CAR_RESIDENT_MODELS; slot++)
	{
		if (GetCarModelSourceCity(slot) < 0)
			continue;

		CarImportDumpOneModel("clean", slot, &NewCarModel[slot]);
		CarImportDumpOneModel("low", slot, &NewLowCarModel[slot]);
	}

	for (n = 0; n < sPinCount; n++)
	{
		int px, py;

		if (sPinIndex[n] < 0 || sPinIndex[n] >= 128 || sPinSet[n] < 0)
			continue;

		CarImportPageRect(texture_pages[sPinIndex[n]], &px, &py);
		printInfo("cross-city:   pinned set %d index %d: texture_pages=%04x => (%d,%d)%s\n",
			sPinSet[n], sPinIndex[n], texture_pages[sPinIndex[n]], px, py,
			(sPinPool[n] >= 0) ? " [pool]" : (sPinSlot[n] >= 0) ? " [placed]" : " [NOT PLACED]");
	}
}
// from that city's level file draws with its own textures instead of the host's.
// Called from LoadPermanentTPages while its tpage/clutpos/slotsused accounting is
// live, so the imported sets are treated exactly like the level's permanent
// pages: they take the next VRAM page position and the next CLUT rows, and the
// streamed slots are pushed along behind them. No-op without an import.
// JERICHO-HOOK: upload the imported city's car texture sets so a vehicle built
// from that city's level file draws with its own textures instead of the host's.
//
// One page per car set, recovered from the imported file at the offset the page
// list gives it (entries concatenated, each sector-aligned). Anything doubtful is
// skipped and logged - a texture is never worth a crash or a corrupted VRAM.
// JERICHO: cross-city state is PER LEVEL, and it used to survive a level change.
// sPinCount/sRemapCount only ever appended, sCarPageOwned kept its 1s, and
// sPinClutCursor stayed where the last level left it. So a second level loaded in
// the same process inherited the first level's imported rectangles as "owned":
// CarPageRectOwned then returned 1 for them and LoadTPageAndCluts / the two spool
// sites refused the WORLD's uploads at exactly those rectangles - the world drew
// stale pages there. Clear everything here, once per level.
//
// Called from InitCarImport (models.c), which runs BEFORE the level's car models are
// built - so the per-slot set lists start empty and buildNewCarFromModel fills them
// for the imported slots.
// JERICHO cross-city UNLOAD, for ONE slot: give back everything that slot holds, so it can be
// used again (a peer leaving, or the local player replacing their car). The manifest says what
// that is; this walks the real tables.
//
// Order matters: the slot's SETS are known only from its pins, so they are collected before
// the pins are removed, and the geometry goes last because the sets live in the built model.
// Anything still driving this slot must be rebuilt afterwards -- mp does that through its own
// swap path -- and a caller that is not replacing the car should also take the slot out of its
// resident set (that is the module's side, not the engine's).
int JerReleaseCarSlot(int slot)
{
	int sets[16];
	int i, k, nsets = 0, released = 0;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	/* 1. the pins recorded for this slot, and the pool pages behind them */
	for (i = 0; i < sPinCount; i++)
	{
		if (sPinSlot[i] != slot)
			continue;

		if (sPinPool[i] >= 0)
		{
			JerLowerPoolPageFree(sPinPool[i]);
			released++;
		}

		if (nsets < 16)
			sets[nsets++] = sPinSet[i];

		for (k = i; k < sPinCount - 1; k++)
		{
			sPinSet[k] = sPinSet[k + 1];
			sPinIndex[k] = sPinIndex[k + 1];
			sPinSlot[k] = sPinSlot[k + 1];
			sPinPool[k] = sPinPool[k + 1];
			sPinOffset[k] = sPinOffset[k + 1];
			sPinSize[k] = sPinSize[k + 1];
			sPinCity[k] = sPinCity[k + 1];
		}

		sPinCount--;
		i--;			/* the pin that shifted in is examined next */
	}

	/* 2. the baked index each of those sets was given (and its reservation, which exists
	 * precisely so the next import does not take the same index) */
	for (i = 0; i < nsets; i++)
	{
		int r;

		for (r = 0; r < sRemapCount; r++)
		{
			int t;

			if (sRemapFrom[r] != sets[i])
				continue;

			/* drop the reservation this set's index holds */
			for (t = 0; t < sReservedCount; t++)
			{
				if (sReservedSet[t] != sRemapTo[r])
					continue;

				for (k = t; k < sReservedCount - 1; k++)
					sReservedSet[k] = sReservedSet[k + 1];

				sReservedCount--;
				break;
			}

			for (k = r; k < sRemapCount - 1; k++)
			{
				sRemapFrom[k] = sRemapFrom[k + 1];
				sRemapTo[k] = sRemapTo[k + 1];
			}

			sRemapCount--;
			released++;
			break;
		}
	}

	/* 3. the geometry, and the manifest entry that described it */
	if (JerReleaseCarGeometry(slot))
		released++;

	sCarSlotRes[slot].used = 0;
	sCarSlotRes[slot].city = -1;
	sCarSlotRes[slot].pins = 0;
	sCarSlotRes[slot].poolPages = 0;
	sCarSlotRes[slot].clutRowBase = -1;
	sCarSlotRes[slot].clutRows = 0;
	sCarSlotRes[slot].geometryBytes = 0;

	if (released > 0)
		printInfo("cross-city: released slot %d - %d thing(s) given back (pins, pool pages, baked index, geometry)\n",
			slot, released);

	return released;
}

// JERICHO cross-city UNLOAD, everything: the state a level installs, given back while there is
// NO level -- the frontend, after leaving a session. CarImportResetState covers the pins, the
// remap, the reservations and the pool, but NOT the two things that are per CITY: the parsed
// page lists and the deferred palette lumps (pointers INTO the import buffer). Those are
// exactly the state that outlives a level, and reading them with no map loaded is the reported
// access violation when a player leaves and rejoins with a car from another city.
void JerReleaseAllCrossCity(void)
{
	int slot, city;

	for (slot = 0; slot < MAX_CAR_RESIDENT_MODELS; slot++)
	{
		if (sCarSlotRes[slot].used)
			JerReleaseCarSlot(slot);
	}

	CarImportResetState();

	/* the per-city state CarImportResetState does not know about */
	CarImportPaletteReset();

	for (city = 0; city < 4; city++)
	{
		gCarImportPerms[city].count = 0;
		gCarImportSpecs[city].count = 0;
		gCarImportTexParsed[city] = 0;
	}

	JerReleaseCarImport();

	printInfo("cross-city: released everything - no map is loaded, so nothing cross-city is held\n");
}

void CarImportResetState(void)
{
	int i;

	sPinCount = 0;

	/* the manifest describes what was placed, so it goes with the placement state */
	memset(sCarSlotRes, 0, sizeof(sCarSlotRes));
	sRemapCount = 0;
	sReservedCount = 0;
	sPinEvictions = 0;
	sPinUnusedTakes = 0;
	sPinReloads = 0;

	// JERICHO: the bottom-half pool is per LEVEL. Its pages and CLUT rows are handed out
	// by the pins above, so a new level must start with all of them free, or the lower half pool
	// would fill up across a session and quietly push later imports back into the base
	// half (or refuse them).
	JerLowerPoolReset();

	// JERICHO: the palette upload's own lifecycle. These were never cleared, so the upload
	// ran once per PROCESS rather than once per level: a second level kept sPalDone set,
	// never re-uploaded, and dereferenced the previous level's deferred lumps. The
	// counters were the same - they accumulated across levels and read as churn.
	sPalDone = 0;
	memset(sPalRowsDone, 0, sizeof(sPalRowsDone));
	sPalUploaded = 0;
	sPinDropped = 0;
	sPinDroppedKept = 0;		// the recorded subsets (and their cities) go with it
	sPinRowLeaks = 0;
	sPinBandSafe = 0;
	CarImportPaletteReset();

	for (i = 0; i < 19; i++)
		sCarPageClaimFrame[i] = 0;

	sCarPageGiveBacks = 0;

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		sModelSetCount[i] = 0;

	sPinClutCursor.x = 0;
	sPinClutCursor.y = 0;
	sPinClutCursor.w = 0;
	sPinClutCursor.h = 0;
}

void LoadImportedTPages(void)
{
	int city = GetCarImportCity();
	int base = GetCarImportPageBaseForCity(city);
	int sets[64];
	int setSlot[64];		// WHICH resident slot asked for it: the manifest is per slot
	int setCity[64];		// WHICH city each set belongs to: a page list is per city, and
					// the same set number means a different page in another
	int pref[64];		// preferred slot per set: the rectangle the replaced car used, or -1
	int nsets = 0;
	int i, j;

	// (placement moved to draw time - see CarImportPin/CarPageFindSlot - so the load
	// no longer tracks slots, positions or CLUT rows)

	// JERICHO-DIAG: what the walk actually sees, one line per resident slot. Local
	// slots are the control - their cars render textured today, so if the walk finds
	// no sets for THOSE, the walk is wrong rather than the import being absent.
	// Runs before the no-import early-out on purpose, so a stock level gives the
	// control in a single run.
	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		MODEL* m = gCarCleanModelPtr[i];

		if (m == NULL)
		{
			printInfo("cross-city: scan slot %d: no model (src=%d)\n", i, GetCarModelSourceCity(i));
			continue;
		}

		{
			char* pb = GET_MODEL_DATA(char, m, poly_block);
			int found[64];
			int n, k;

			// Run the SAME walk the import uses, on every slot including local ones.
			// A local model that renders textured must yield sets here; if it does
			// not, the walk is what is broken, not the import.
			n = CollectModelSets(m, found, 0, 64);

			printInfo("cross-city: scan slot %d: model=%p polys=%d polyblock=%p src=%d -> %d set(s):",
				i, (void*)m, m->num_polys, (void*)pb, GetCarModelSourceCity(i), n);

			for (k = 0; k < n && k < 12; k++)
				printInfo(" %d", found[k]);

			printInfo("   [bytes %02x %02x %02x %02x]\n",
				(u_char)pb[0], (u_char)pb[1], (u_char)pb[2], (u_char)pb[3]);

			// JERICHO-DIAG: the RAW structure, so the real encoding can be read off
			// instead of assumed. First 40 bytes, then the first 10 polygons as the walk
			// sees them - type byte, the step PolySizes gives it, and the running total.
			// If the total runs past the model's own data the advance is wrong, and the
			// pattern of type bytes says what the mask should have been.
			if (i == 0 && pb != NULL && m->num_polys > 0)
			{
				int total = 0;

				printInfo("cross-city:   raw:");
				for (k = 0; k < 40; k++)
					printInfo(" %02x", (u_char)pb[k]);
				printInfo("\n");

				printInfo("cross-city:   walk:");
				for (k = 0; k < 10; k++)
				{
					int step = PolySizes[(u_char)(pb[total] & 0x1f)];
					printInfo(" [%d:t=%02x step=%d]", k, (u_char)pb[total], step);
					total += step;
				}
				printInfo("  -> 10 polys span %d bytes; block end guess %d\n", total, sizeof(void*) * 0);
			}
		}
	}

	if (city < 0 || base < 0)
		return;

	// Which sets to bring across: the ENGINE'S OWN ANSWER, not a guess.
	//
	// The engine never scans polygons to decide this. For its own cars it loads the
	// whole carTpages list for the level - six civilian sets - and for a special
	// body it takes that body's two entries out of specTpages. Do exactly the same
	// for the source city, which is what makes a cross-city vehicle load like the
	// city's own vehicles do.
	//
	// The polygon walk (CollectModelSets) is no longer a source of truth: run against
	// LOCAL models, whose cars render textured, it reported sets that do not exist in
	// that city (12 and 255 against Havana's {10,20,35,36,37,38,39,51}), so it
	// misreads the layout. It survives only as the diagnostic that shows this.
	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		int src = GetCarModelSourceCity(i);
		int body, k;

		if (src < 0)
			continue;

		body = residentCarModels[i];

		if (body > 5)
		{
			// Special body: its own two pages, taken the same way LoadPermanentTPages
			// takes them for the host level's special car.
			int spec = (body - 8) * 2;

			for (k = 0; k < 2; k++)
			{
				int set = (spec + k >= 0 && spec + k < 12) ? specTpages[src][spec + k] : 0;

				if (set != 0 && nsets < 64 && !SetInList(sets, nsets, set))
				{
					// This page replaces the host's special car's OWN rectangle, so nothing has
					// to be evicted for it. Picking a mere free slot instead is what put
					// imported textures onto rectangles buildings were using. `specialSlot` is
					// the runtime TEXTURE slot the host special pages were loaded into - not
					// SPECIAL_CAR_SLOT, which is a resident-MODEL index (7) and lands on the
					// level's permanent pages.
					pref[nsets] = (specialSlot + k < 19) ? (specialSlot + k) : -1;
					setCity[nsets] = src;
					setSlot[nsets] = i;
					sets[nsets++] = set;
				}
			}

			// ...AND the sets the model's own polygons name, on top of the body's two.
			//
			// A special body's polys can name more than its specTpages pair: measured on
			// an imported body, 48 of 254 polys carried tpage indices that were NOT the
			// body's two - they resolved to the HOST's pages (704,0) and (640,0) - because
			// those polys name small set numbers the source city resolves through its own
			// tables. Taking them here means every poly the model paints with gets an
			// imported page instead of a host one, which is what the loud line below
			// reports when it still cannot.
			{
				int count = CarModelSetCount(i);
				int own = 0;

				for (k = 0; k < count; k++)
				{
					int set = CarModelSet(i, k);

					if (set != 0 && nsets < 64 && !SetInList(sets, nsets, set))
					{
						pref[nsets] = -1;	// no natural rectangle: a spare slot, as for civilians
						setCity[nsets] = src;
						setSlot[nsets] = i;
						sets[nsets++] = set;
						own++;
					}
				}

				if (count > 0)
					printInfo("cross-city:   special body %d (slot %d) names %d set(s) of its own; %d beyond its two spec pages\n",
						body, i, count, own);
			}
		}
		else
		{
			// Civilian body: only the sets THIS model's own polygons name, collected by
			// buildNewCarFromModel as it walked them. Pulling the whole carTpages list (6
			// sets) asked for 6 pages where the level leaves 5, so one stayed unplaced -
			// and an unplaced page leaves texture_pages/texture_cluts at the DUMMY,
			// which sits inside a live page, so the car showed another page's pixels.
			int count = CarModelSetCount(i);

			if (count <= 0)
			{
				// Walk found nothing (or the model was not built yet): keep the old
				// whole-list behaviour rather than import nothing at all.
				count = 6;
			}

			for (k = 0; k < count; k++)
			{
				int set = (count == 6) ? carTpages[src][k] : CarModelSet(i, k);

				if (set != 0 && nsets < 64 && !SetInList(sets, nsets, set))
				{
					pref[nsets] = -1;	// civilian body: no single natural rectangle, so the
										// level's spare slots are used as before
					setCity[nsets] = src;
					setSlot[nsets] = i;
					sets[nsets++] = set;
				}
			}
		}
	}

	printInfo("cross-city: %s - %d set(s) wanted from carTpages/specTpages\n", LevelNames[city], nsets);

	// Each set goes into a slot the level left FREE, at that slot's own already
	// assigned position - and now at DRAW time rather than load time: the load only
	// records what each imported car needs (CarPinRecord), and CarImportPin pages it in
	// before the cars are drawn, evicting a world slot if none is free. So nothing of
	// the level's moves: not a page position, not a CLUT row, not the tpage/clutpos
	// cursors. Walking those cursors is what corrupted walls and car colours before.

	for (i = 0; i < nsets; i++)
	{
		int set = sets[i];
		int sc = setCity[i];		// THIS set's city: its page list is THAT city's
		int dstSet = set;
		int remapFrom = -1;
		int remapTo = 0;
		int offset = 0;
		int size = 0;
		int npalettes;
		char* buf;

		if (set == 0 || SetInList(sets, i, set))
			continue;

		// JERICHO-DIAG: every candidate, before any guard can hide it - which slot it
		// would take, the position that slot resolves to, and whether the host owns
		// the set. This is what tells a genuine capacity wall from a bogus refusal.
		printInfo("cross-city: candidate %s set %d hostOwns=%d (%s)\n",
			LevelNames[sc], set, HostUsesTPage(set) ? 1 : 0, HostUseReason(set));

		// The host city keeps its own meaning for a set number: a set index holds one
		// meaning at a time. So a set the level already resolved goes to a free index and
		// the car's polys are translated onto it (CarSetRemap, applied in
		// buildNewCarFromModel as the polys are converted). Without this the part kept the
		// host's texture.
		//
		// The decision is NOT made here: CarImportDstSet owns it, and the build already
		// called it - so the index the polys were baked with is the index filled below.
		// Making the choice twice is what left a special body's third set baked as the
		// host's index while the pin filled a different one.
		dstSet = CarImportPinDstSet(set);

		if (dstSet != set)
		{
			// CarImportDstSet allocated and recorded it at build time; recording again here
			// would add a second mapping for the same set.
			remapFrom = -1;
			printInfo("cross-city: %s set %d -> index %d, taken from the build-time remap (the car's polys already point at it)\n",
				LevelNames[sc], set, dstSet);
		}
		else
		{
			printInfo("cross-city: %s set %d keeps its own index (the level never resolved it)\n",
				LevelNames[sc], set);
		}



		// locate it in THAT city's page list
		for (j = 0; j < gCarImportPerms[sc].count; j++)
		{
			if (gCarImportPerms[sc].set[j] == set)
			{
				size = gCarImportPerms[sc].bytes[j];
				break;
			}

			offset += (gCarImportPerms[sc].bytes[j] + CDSECTOR_SIZE - 1) & -CDSECTOR_SIZE;
		}

		// Not among the permanent pages? Then it may be a SPECIAL page - and those are
		// exactly what a special body needs: carTpages gives a civilian car six sets,
		// but a special body takes its own two out of specTpages. Their data follows
		// the perm block in the file, each entry sector-aligned, so the walk simply
		// continues where the perm block ends. Without this the player's imported car
		// loaded no textures at all: 'set 77 -> slot 14' twice, nothing uploaded,
		// because 77/78 are in the spec list and the search never looked there.
		if (size <= 8 && gCarImportSpecs[sc].count > 0)
		{
			int specOffset = 0;

			for (j = 0; j < gCarImportPerms[sc].count; j++)
				specOffset += (gCarImportPerms[sc].bytes[j] + CDSECTOR_SIZE - 1) & -CDSECTOR_SIZE;

			for (j = 0; j < gCarImportSpecs[sc].count; j++)
			{
				if (gCarImportSpecs[sc].set[j] == set)
				{
					size = gCarImportSpecs[sc].bytes[j];
					offset = specOffset;
					break;
				}

				specOffset += (gCarImportSpecs[sc].bytes[j] + CDSECTOR_SIZE - 1) & -CDSECTOR_SIZE;
			}
		}

		if (size <= 8)
		{
			printInfo("cross-city: %s set %d is not in its page list - skipped\n", LevelNames[sc], set);
			continue;
		}

		buf = (char*)malloc(size);

		// The page's bytes come from THIS set's city, so neither the file nor the page
		// base may come from the level-wide singleton.
		if (buf == NULL || !ReadCarImportFileForCity(sc, GetCarImportPageBaseForCity(sc) + offset, buf, size))
		{
			printInfo("cross-city: %s set %d could not be read (%d bytes) - skipped\n", LevelNames[sc], set, size);

			if (buf)
				free(buf);

			continue;
		}

		// An entry starts with its CLUT-row count; anything outside the 32 rows
		// texture_cluts can hold means a bad offset, and uploading it would smear VRAM.
		// So the bytes are read here to VALIDATE the entry only: placement happens at
		// draw time (CarImportPin), which is what frees the load from needing a spare
		// slot at all - and lets a page take a world slot back when there is none.
		npalettes = *(int*)buf;

		free(buf);

		if (npalettes <= 0 || npalettes > 32)
		{
			printInfo("cross-city: %s set %d looks corrupt (%d clut rows) - skipped\n", LevelNames[sc], set, npalettes);
			continue;
		}

		// The remap becomes real together with the pin: the page WILL be placed (at draw
		// time, evicting the world if need be), short of a failed read.
		if (remapFrom >= 0 && sRemapCount < CAR_REMAP_MAX)
		{
			sRemapFrom[sRemapCount] = remapFrom;
			sRemapTo[sRemapCount] = remapTo;
			sRemapCount++;
		}

		CarPinRecord(set, dstSet, offset, size, pref[i], sc, setSlot[i]);

		printInfo("cross-city: %s set %d -> index %d, %d bytes at +%d, %d clut rows (paged in at draw time, evicting the world if needed)\n",
			LevelNames[sc], set, dstSet, size, offset, npalettes);
	}

	/* The manifest (CAR_SLOT_RES): now that every slot's pins are recorded, say what
	 * each imported slot holds. This is the map a release needs -- nothing reads it for
	 * behaviour yet. */
	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		int src = GetCarModelSourceCity(i);

		if (src >= 0)
			CarSlotResNote(i, src);
	}
}

// ---------------------------------------------------------------------------
// JERICHO VRAM accounting (measurement only - it changes no upload)
//
// The engine has no allocator and no free-space query: every upload is a hard-coded or
// slot-walk rectangle, and `NoTextureMemory` is only a lockout for when the page walk
// runs off `tpagepos[]` (texture.c:354-386). So "how much VRAM is left" was
// unanswerable - which is how a reserved region sits empty for years and how a
// CLUT-strip squeeze stays invisible. tools/vrammap.py is the offline version of this;
// this is the same answer, in the run.
//
// The dynamic claims (page slots, the level's CLUT column) are READ from the engine's
// own state - `slot_tpagepos[]` and `clutpos`, the same variables the uploads use - so
// they cannot drift from reality the way a hand-maintained list would. Only rectangles
// that are fixed no matter what a level loads are listed as constants.
// ---------------------------------------------------------------------------

#define VRAM_CELL	64							// accounting granularity: 64x64 texels
#define VRAM_COLS	(1024 / VRAM_CELL)
#define VRAM_ROWS	(JER_VRAM_TOTAL_ROWS / VRAM_CELL)	// 16: the whole 1024-row buffer, not just the top half
#define VRAM_CELL_KB	(VRAM_CELL * VRAM_CELL * 2 / 1024)	// 8 KiB

typedef struct
{
	const char *name;
	int x, y, w, h;
	const char *why;
} VRAM_FIXED;

static const VRAM_FIXED sVramFixed[] =
{
	{ "framebuffer A", 0,   0,   320, 256, "system.c:724-725 - a display buffer, not texture memory" },
	{ "framebuffer B", 0,   256, 320, 256, "system.c:724-725 - double buffered, so 320 KiB of the 1 MiB" },
	{ "sky",           320, 0,   128, 256, "sky.c:280-282 - two page widths" },
	{ "level font",    960, 466, 64,  46,  "pres.c:584-589" },
	{ "font clut",     976, 256, 16,  1,   "pres.c:550-553" },
	{ "map clut",      960, 256, 16,  1,   "texture.c:2074-2077" },
	{ "CD icon",       960, 433, 16,  32,  "spool.c:333-348" },
};

#define JERICHO_PAL_DIAG(_what) printInfo("cross-city: clut cursor after %s: y=%d (the level font image owns 466..511, cars.h)\n", _what, clutpos.y)

static void VramAccountReport(void)
{
	static unsigned char cells[VRAM_ROWS][VRAM_COLS];	// static: this is not a small stack
	int used = 0, texused = 0, freecells, i, x, y;
	int bw = 0, bh = 0, bx = 0, by = 0, best = 0;
	const int texcol = 320 / VRAM_CELL;			// the display buffers own x0..319

	memset(cells, 0, sizeof(cells));

// mark every cell a rectangle touches (rounded up: a partial cell is not free)
#define VRAM_MARK(px, py, pw, ph)												\
	do {																		\
		int _x0 = (px) / VRAM_CELL, _y0 = (py) / VRAM_CELL;						\
		int _x1 = ((px) + (pw) + VRAM_CELL - 1) / VRAM_CELL;					\
		int _y1 = ((py) + (ph) + VRAM_CELL - 1) / VRAM_CELL;					\
		for (y = _y0; y < _y1 && y < VRAM_ROWS; y++)							\
			for (x = _x0; x < _x1 && x < VRAM_COLS; x++)						\
				if (x >= 0 && y >= 0) cells[y][x] = 1;							\
	} while (0)

	// the page slots, read from the array the streamer itself uploads to
	for (i = 0; i < 19; i++)
		VRAM_MARK(slot_tpagepos[i].vx, slot_tpagepos[i].vy, 64, 256);

	// the CLUT column the level committed (its own cursor, rounded up to the cell)
	if (clutpos.y > 256)
		VRAM_MARK(960, 256, 64, clutpos.y - 256);

	for (i = 0; i < (int)(sizeof(sVramFixed) / sizeof(sVramFixed[0])); i++)
		VRAM_MARK(sVramFixed[i].x, sVramFixed[i].y, sVramFixed[i].w, sVramFixed[i].h);

	for (y = 0; y < VRAM_ROWS; y++)
		for (x = 0; x < VRAM_COLS; x++)
			if (cells[y][x])
			{
				used++;
				if (x >= texcol) texused++;		// inside the texture area (x320..1023)
			}

	freecells = VRAM_ROWS * VRAM_COLS - used;

	// the largest free rectangle INSIDE the texture area: whole-VRAM 'free' is always 0
	// here (the display buffers see to that), so the useful answer is the texture pool's
	{
		int r0, r1, c0, c1, r, c;

		for (r0 = 0; r0 < VRAM_ROWS; r0++)
			for (r1 = r0; r1 < VRAM_ROWS; r1++)
				for (c0 = texcol; c0 < VRAM_COLS; c0++)
					for (c1 = c0; c1 < VRAM_COLS; c1++)
					{
						int ok = 1;

						for (r = r0; r <= r1 && ok; r++)
							for (c = c0; c <= c1 && ok; c++)
								if (cells[r][c]) ok = 0;

						if (ok && (r1 - r0 + 1) * (c1 - c0 + 1) > best)
						{
							best = (r1 - r0 + 1) * (c1 - c0 + 1);
							bx = c0 * VRAM_CELL; by = r0 * VRAM_CELL;
							bw = (c1 - c0 + 1) * VRAM_CELL; bh = (r1 - r0 + 1) * VRAM_CELL;
						}
					}
	}

#undef VRAM_MARK

	// One line, because this is read in a log next to the page state. The texture area is
	// the half that can be argued about: the framebuffers are 320 KiB nobody can use.
	//
	// JERICHO: the CLUT column is reported against its SAFE area (rows 256..CD2_CLUT_SAFE_LAST),
	// not against the whole 256-row column. The area below CD2_CLUT_SAFE_LAST is the level
	// font image, so counting it as "free" is what let a 19-row overflow look like spare
	// room - the numbers said 84 free while the glyphs were being painted over. When the
	// layout does overflow, say by how much; that is the number to drive to zero.
	{
		int clutrows = (clutpos.y > CD2_CLUT_SAFE_FIRST) ? (clutpos.y - CD2_CLUT_SAFE_FIRST) : 0;
		int clutfree = CD2_CLUT_SAFE_LAST + 1 - clutpos.y;
		int clutover = -clutfree;

		if (clutfree < 0)
			clutfree = 0;

		printInfo("JERICHO-VRAM: texture used=%d/%d KiB (slots %d + clut %d + sky %d); clut strip %d rows used, %d safe free%s; vram free=%d KiB of %d; largest free in texture area=(%d,%d) %dx%d = %d KiB\n",
			texused * VRAM_CELL_KB, (VRAM_COLS - texcol) * VRAM_ROWS * VRAM_CELL_KB,
			19 * 32, 64 * 256 * 2 / 1024, 64,
			clutrows, clutfree,
			(clutover > 0) ? " - OVERFLOW into the level font" : ", no overflow",
			freecells * VRAM_CELL_KB, VRAM_ROWS * VRAM_COLS * VRAM_CELL_KB, bx, by, bw, bh, best * VRAM_CELL_KB);

		if (clutover > 0)
			printInfo("JERICHO-VRAM: WARNING - the CLUT column reaches y=%d, %d row(s) into the level font image (%d..511). See cars.h CD2_CLUT_SAFE_LAST and VRAM.md 6.\n",
				clutpos.y, clutover, CD2_CLUT_SAFE_LAST + 1);

		// JERICHO: the bottom-half pool, next to the top half's answer. Rows 512..1023 are
		// space no stock path addresses, so the whole budget is free until JERICHO content
		// claims it - this line is the "did anything land in the new half, and is it full?"
		// measurement.
		printInfo("JERICHO-VRAM: lower half pool rows %d..%d: pages %d used of %d (%d free), clut rows %d used (%d free)%s\n",
			JER_VRAM_HALF_Y, JER_VRAM_TOTAL_ROWS - 1,
			JerLowerPoolPagesUsed(), JerLowerPoolPagesUsed() + JerLowerPoolPagesFree(),
			JerLowerPoolPagesFree(),
			JerLowerPoolClutRowsUsed(), JerLowerPoolClutRowsFree(),
			(JerLowerPoolClutDropped() > 0) ? " - DROPPED asks" : "");
	}

	// JERICHO: how many CLUT rows ONE streamed slot can need -- the max over the
	// level's own pages of (palettes / 4 + 1), which is exactly what SendTPage
	// writes into a slot's band (spool.c:495). The band reserves a flat 8 per slot;
	// this is the number that says whether it can reserve less (VRAM.md 6, option 1)
	// -- and whether 8 was ever enough.
	{
		int reserve = LevelClutRowsNeeded();
		int minRows = 0, ti;

		for (ti = 0; ti < tpage_amount; ti++)
		{
			int rows;

			if (tpage_texamts[ti] <= 0)
				continue;

			rows = tpage_texamts[ti] / 4 + 1;

			if (minRows == 0 || rows < minRows)
				minRows = rows;
		}

		printInfo("JERICHO-CLUT: the level's pages need %d..%d CLUT rows each; the streamed-slot band reserves %d\n",
			minRows, reserve, (reserve < 1) ? 8 : reserve);
	}
}

// JERICHO: the most CLUT rows ANY of the level's own pages needs -- i.e. the widest
// thing a streamed slot can be asked to hold. spool.c orders exactly
// (npalettes / 4 + 1) rows per slot, so reserving less than this lets one slot's
// CLUTs write over the next slot's. The band used to reserve a flat 8, which is one
// short for LASVEGAS (its own pages need 9) -- VRAM.md 6.
static int LevelClutRowsNeeded(void)
{
	int ti, rows, maxRows = 0;

	for (ti = 0; ti < tpage_amount; ti++)
	{
		if (tpage_texamts[ti] <= 0)
			continue;

		rows = tpage_texamts[ti] / 4 + 1;

		if (rows > maxRows)
			maxRows = rows;
	}

	return maxRows;
}

// [D] [T]
void LoadPermanentTPages(int *sector)
{
	int nsectors;
	char *tpagebuffer;
	int tloop, tset, i;
	int specmodel;
	int page1, page2;

	// init tpage and cluts
	MaxSpecCluts = 0;

	// JERICHO-HOOK: read the imported city's page lists (no-op with no import) so
	// its car sets can be registered below.
	ParseImportedTextureInfo();

	for (tloop = 0; tloop < 128; tloop++)
		texture_pages[tloop] = GetTPage(0, 0, 960, 0);

	for (tloop = 0; tloop < 128; tloop++)
	{
		for (tset = 0; tset < 32; tset++)
			texture_cluts[tloop][tset] = GetClut(960, 16);
	}

	slotsused = 0;
	memset(tpageloaded, 0, sizeof(tpageloaded));

	clutpos.x = 960;
	clutpos.y = 256;
	clutpos.w = 16;
	clutpos.h = 1;

	mapclutpos.x = 960;
	mapclutpos.y = 256;
	mapclutpos.w = 16;
	mapclutpos.h = 1;

	tpage.x = tpagepos[0].x;
	tpage.y = tpagepos[0].y;
	tpage.w = 64;
	tpage.h = 256;

	IncrementClutNum(&clutpos);
	fontclutpos = clutpos;
	
	IncrementClutNum(&clutpos);
	ProcessPalletLump(palette_lump, palette_lump_size);
	JERICHO_PAL_DIAG("host palettes");
	ProcessImportedPalette();	// JERICHO-HOOK: a cross-city import's own palettes
	JERICHO_PAL_DIAG("import palettes");

	load_civ_palettes(&clutpos);
	JERICHO_PAL_DIAG("load_civ_palettes");

	tpagebuffer = (char*)mallocptr;
	nsectors = 0;

	// JERICHO: state the invariant the slot table depends on, out loud. The loop below
	// writes ONE slot per permanent page and tpageslots holds TPAGE_SLOTS, so a level with
	// more would overrun it - silently, because the write is an index into a fixed array
	// two hundred lines from the bound. It has never happened (nperms is far below), and
	// saying so here means the next reader does not have to infer it from a loop bound.
	if (nperms > TPAGE_SLOTS)
		printInfo("JERICHO-WARN: level has %d permanent pages but tpageslots holds %d - the load will overrun the slot table\n", nperms, TPAGE_SLOTS);

	for (i = 0; i < nperms; i++)
		nsectors += (permlist[i].y + 2047) / CDSECTOR_SIZE;

	loadsectors(tpagebuffer, *sector, nsectors);

	*sector += nsectors;

	for (i = 0; i < nperms; i++)
	{
		int tp = permlist[i].x;

		update_slotinfo(tp, slotsused, &tpage);
		LoadTPageAndCluts(&tpage, &clutpos, tp, tpagebuffer);
		slotsused++;

		tpagebuffer += (permlist[i].y + 2047) & -CDSECTOR_SIZE;
	}

	// JERICHO-HOOK: the imported city's car texture sets are brought in later, by
	// LoadImportedTPages() from LoadGameLevel. They cannot go here: the models
	// have to exist before we can ask them which sets they paint with, and the .TIM
	// override pass after this rewrites every slot that is not 0xFF. Claims belong
	// after that, not before it.
	tpagebuffer = (char*)mallocptr;

	slot_clutpos[slotsused].vx = clutpos.x;
	slot_clutpos[slotsused].vy = clutpos.y;

	// init special slot texture
	specmodel = (residentCarModels[SPECIAL_CAR_SLOT] - 8) * 2;
	specialSlot = (short)slotsused;

	// get special slot tpage
	page1 = specTpages[GameLevel][specmodel];
	page2 = specTpages[GameLevel][specmodel + 1];

	carTpages[GameLevel][6] = page1;
	carTpages[GameLevel][7] = page2;

	if (nspecpages != 0)
	{
		int temp, clutsloaded;
		
		temp = 0;
		clutsloaded = 0;

		nsectors = 0;

		for (i = 0; i < nspecpages; i++)
			nsectors += (speclist[i].y + 2047) / CDSECTOR_SIZE;

		loadsectors(tpagebuffer, *sector, nsectors);

		*sector += nsectors;
		
		for (i = 0; i < nspecpages; i++)
		{
			int tp, npalettes;
			npalettes = *(int *)tpagebuffer;

			temp += npalettes;

			if ((i & 1) != 0)
			{
				if (temp > MaxSpecCluts)
					MaxSpecCluts = temp;

				temp = 0;
			}

			tp = speclist[i].x;

			// find a special car TPAGEs
			if (page1 == tp || page2 == tp)
			{
				update_slotinfo(tp, slotsused, &tpage);
				LoadTPageAndCluts(&tpage, &clutpos, tp, tpagebuffer);
				slotsused++;

				clutsloaded += npalettes;
			}

			tpagebuffer += (speclist[i].y + 2047) & -CDSECTOR_SIZE;
		}

		while (clutsloaded < MaxSpecCluts)
		{
			// JERICHO: stop at the last row above the level font image rather than
			// ticking the cursor into the glyphs (CD2_CLUT_SAFE_LAST, VRAM.md 6).
			if (clutpos.y >= CD2_CLUT_SAFE_LAST)
				break;

			IncrementClutNum(&clutpos);
			clutsloaded++;
		}
	}

	if (clutpos.x != 960) 
	{
		clutpos.x = 960;

		// JERICHO: same ceiling. The band below is clamped too, but a cursor already
		// sitting at the ceiling must not step into the level font.
		if (clutpos.y < CD2_CLUT_SAFE_LAST)
			clutpos.y++;
	}

	// init all slots
	// JERICHO: reserve what this level's widest page actually needs rather than a
	// flat 8 rows, and never past the last row above the level font image.
	int slotRows = LevelClutRowsNeeded();
	if (slotRows < 1)
		slotRows = 8;			// no page data at all: keep the historic reserve
	if (slotRows > CD2_CLUT_SAFE_LAST - clutpos.y + 1)
		slotRows = CD2_CLUT_SAFE_LAST - clutpos.y + 1;
	if (slotRows < 0)
		slotRows = 0;

	for (i = slotsused; i < 19; i++)
	{
		tpageslots[i] = 0xFF;

		slot_clutpos[i].vx = clutpos.x;
		slot_clutpos[i].vy = clutpos.y;

		slot_tpagepos[i].vx = tpage.x;
		slot_tpagepos[i].vy = tpage.y;

		IncrementTPageNum(&tpage);
		clutpos.y += slotRows;
	}
	JERICHO_PAL_DIAG("level slot walk (the level's own max rows per streamed slot)");

	// JERICHO-HOOK: the level's own page state, for the cross-city invariant. An
	// import must leave every one of these exactly as it is here - measured with an
	// import on and off, and compared. This line is what proves it.
	//
	// The civ_clut checksums are the same idea for the palette table
	// (u_short civ_clut[CIV_CLUT_ROWS][32][6], CIV_CLUT_ROWS 16 and the last eight rows
	// an import's - PALETTES.md §1): those rows hold the colours every car in the level
	// draws with, so an import must not disturb a single entry.
	//
	// TWO sums, deliberately. `civclut` covers rows 0..7 - the HOST's rows - and is the
	// number to compare ACROSS builds, because it has meant the same thing since it was
	// introduced (a stock run must print the same value before and after a change).
	// `civclut16` covers all CIV_CLUT_ROWS rows so the import bank is visible too, but it
	// is only comparable within one build: widening a hash changes its value without
	// anything being wrong. Skipping this split is how "the hash moved" nearly got read
	// as a regression.
	{
		unsigned int clutSum = 0;
		unsigned int clutSum16 = 0;

		for (i = 0; i < CIV_CLUT_ROWS * 32 * 6; i++)
		{
			if (i < 8 * 32 * 6)
				clutSum = clutSum * 31 + ((u_short*)civ_clut)[i];

			clutSum16 = clutSum16 * 31 + ((u_short*)civ_clut)[i];
		}

		printInfo("cross-city: level page state - slotsused=%d nperms=%d nspecpages=%d tpage=(%d,%d) clutpos=(%d,%d) civclut=%08x civclut16=%08x\n",
			slotsused, nperms, nspecpages, tpage.x, tpage.y, clutpos.x, clutpos.y, clutSum, clutSum16);
	}

	// JERICHO-HOOK: and where that left VRAM (measurement only). The fixed claims in the
	// table above are committed a little later in a level's life (sky, font, CD icon), so
	// they are counted here from their constants rather than waited for.
	VramAccountReport();
}

// [D] [T]
void ReloadIcons(void)
{
	ReportMode(0);
	ReportMode(1);
}

// [D] [T]
void GetTextureDetails(char *name, TEXTURE_DETAILS *info, int defaultToSea)
{
	int i, j;
	int texamt;
	char *nametable;
	TEXINF *texinf;
	
	nametable = texturename_buffer;

	for (i = 0; i < tpage_amount; i++)
	{
		texamt = tpage_texamts[i];
		texinf = tpage_ids[i];

		for (j = 0; j < texamt; j++)
		{
			if (!strcmp(nametable + texinf->nameoffset, name))
			{
				info->tpageid = texture_pages[i];
				info->clutid = texture_cluts[i][j];
				info->texture_number = j;
				info->texture_page = i;

				setUVWH(&info->coords, texinf->x, texinf->y, texinf->width - 1, texinf->height - 1);

				// bust 'outta here, real fly
				return;
			}

			texinf++;
		}
	}

	info->tpageid = 0;
	info->clutid = 0;
	info->texture_number = 0;
	info->texture_page = 0;
	if (defaultToSea)
		GetTextureDetails("SEA", info);	// weird but ok, ok...
}





