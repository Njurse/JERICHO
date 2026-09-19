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

char* texturename_buffer = NULL;
int NoTextureMemory = 0;

u_short texture_pages[128];
u_short texture_cluts[128][32];
u_char tpageloaded[128];

int MaxSpecCluts;
int slotsused;

RECT16 clutpos;
RECT16 fontclutpos;
RECT16 mapclutpos;
DVECTOR slot_clutpos[19];
DVECTOR slot_tpagepos[19];
u_char tpageslots[19];

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
	src.y = (short)((sourceClut >> 6) & 0x1ff);
	src.w = 16;
	src.h = 1;

	// out of room in the strip at the right of VRAM
	if (src.y > 511 || clutpos.y > 511)
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

	return addr;
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
static unsigned char sCarPageOwned[19];		// the slot rects an imported page holds

// JERICHO: is this VRAM rectangle one an imported page owns? Exported so the spool's
// own upload paths can respect it too - they write texture_pages[] and call LoadImage
// directly, bypassing the LoadTPageAndCluts guard entirely, which is how the host's
// special-car pages kept landing on top of an imported player car.
int CarPageRectOwned(int x, int y)
{
	int i;

	for (i = 0; i < 19; i++)
	{
		if (sCarPageOwned[i] && tpagepos[i].x == x && tpagepos[i].y == y)
			return 1;
	}

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

static CAR_IMPORT_SETS gCarImportPerms;
static CAR_IMPORT_SETS gCarImportSpecs;
static int gCarImportTexParsed = 0;

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
static void ParseImportedTextureInfo(void)
{
	char* lump;
	char* ptr;
	char* end;
	int size = 0;
	int tpageAmount;
	int i;

	gCarImportPerms.count = 0;
	gCarImportSpecs.count = 0;
	gCarImportTexParsed = 0;

	if (GetCarImportCity() < 0)
		return;

	lump = GetCarImportTextureInfo(&size);

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

		CopyImportSetList((XYPAIR*)ptr, nperms, &gCarImportPerms);
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

		CopyImportSetList((XYPAIR*)ptr, nspec, &gCarImportSpecs);
	}

	gCarImportTexParsed = 1;

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
		int city = GetCarImportCity();
		int base = GetCarImportPageBase();
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

			for (j = 0; j < gCarImportPerms.count; j++)
			{
				if (gCarImportPerms.set[j] == set)
				{
					int cluts = 0;

					found++;

					if (base >= 0 && ReadCarImportFile(base + offset, &cluts, sizeof(cluts)))
						printInfo("cross-city: %s set %d at +%d, %d bytes, %d clut rows\n",
							LevelNames[city], set, offset, gCarImportPerms.bytes[j], cluts);

					break;
				}

				offset += (gCarImportPerms.bytes[j] + CDSECTOR_SIZE - 1) & -CDSECTOR_SIZE;
			}
		}

		printInfo("cross-city: %s page lists - %d permanent sets, %d special sets, %d/%d car sets present\n",
			LevelNames[city], gCarImportPerms.count, gCarImportSpecs.count, found, wanted);
	}
}

// Whether the level's own page load already claimed this texture set. Scans the
// slot table rather than tpageloaded, because slot 0 is a valid slot and so
// indistinguishable from "not loaded" in that array.
static int LevelTookTPage(int tpage)
{
	int i;

	for (i = 0; i < slotsused && i < 19; i++)
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

// JERICHO: index remap for imported vehicles.
//
// A texture set number can only mean one thing at a time, and the host city already
// means something by some of the numbers the source city uses. Rather than skip
// those sets - which left an imported car drawing host textures on part of its body
// - the imported page goes to a free set index and the car's own polygons are
// translated onto it as they are converted into engine form (cars.c, in
// plotNewCarModel). Nothing of the host's is touched.
#define CAR_REMAP_MAX 8

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
int CarSetRemap(int set)
{
	int i;

	if (!sCarSetRemapActive)
		return set;

	for (i = 0; i < sRemapCount; i++)
	{
		if (sRemapFrom[i] == set)
			return sRemapTo[i];
	}

	return set;
}

// First set index above everything a city uses that is still free. City car sets
// live in the low numbers (10..68 in all four), so the top of the 128-entry table is
// unused - and tpageloaded stays zero for any index never loaded. The host's car and
// special sets are excluded explicitly as well, so a destination can never collide
// with a meaning the host needs.
static int FindFreeSetIndex(void)
{
	int i, k;

	for (i = 110; i < 128; i++)
	{
		if (tpageloaded[i] != 0)
			continue;

		for (k = 0; k < 8; k++)
		{
			if (carTpages[GameLevel][k] == i)
				break;
		}

		if (k != 8)
			continue;

		for (k = 0; k < 12; k++)
		{
			if (specTpages[GameLevel][k] == i)
				break;
		}

		if (k != 12)
			continue;

		return i;
	}

	return 0;
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
#define CAR_PIN_MAX 8

static int sPinCount;
static int sPinSet[CAR_PIN_MAX];		// the set number the CAR asks for
static int sPinIndex[CAR_PIN_MAX];		// the index its page is loaded at
static int sPinSlot[CAR_PIN_MAX];		// the slot it lives in, -1 while unplaced
static int sPinOffset[CAR_PIN_MAX];		// where its bytes are in the source city's file
static int sPinSize[CAR_PIN_MAX];
static int sPinPreferred[CAR_PIN_MAX];		// the rectangle the REPLACED car's page used, or -1
static RECT16 sPinClutCursor;			// walking CLUT-row cursor for the imported pages
static int sPinEvictions;			// world pages taken back this run, for the dump
static int sPinReloads;				// times we re-uploaded a page we had already placed - the thrash meter

static void CarPinRecord(int set, int index, int offset, int size, int preferred)
{
	if (sPinCount >= CAR_PIN_MAX)
		return;

	sPinSet[sPinCount] = set;
	sPinIndex[sPinCount] = index;
	sPinSlot[sPinCount] = -1;		// placed at draw time
	sPinOffset[sPinCount] = offset;
	sPinSize[sPinCount] = size;
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
static int CarPageFindSlot(void)
{
	int i, k;
	static int sVictim;

	for (i = 0; i < 19; i++)
	{
		int idx = (sVictim + i) % 19;

		if (idx >= nperms && tpageslots[idx] == 0xFF)
		{
			sVictim = idx;
			return idx;		// free outright
		}
	}

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

void CarImportPin(void)
{
	int i;

	for (i = 0; i < sPinCount; i++)
	{
		char* buf;
		RECT16 tpage, clut;
		int slot;

		if (sPinSlot[i] >= 0 && tpageslots[sPinSlot[i]] == sPinIndex[i] && tpageloaded[sPinIndex[i]] != 0)
			continue;		// still ours, nothing to do

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

		if (slot < 0 || slot >= 19)
			slot = sPinPreferred[i];

		if (slot < 0 || slot >= 19)
			slot = CarPageFindSlot();

		if (slot < 0)
			continue;		// nothing evictable this frame; try again next frame

		buf = (char*)malloc(sPinSize[i]);

		if (buf == NULL)
			continue;

		if (!ReadCarImportFile(GetCarImportPageBase() + sPinOffset[i], buf, sPinSize[i]))
		{
			free(buf);
			continue;
		}

		tpage.x = tpagepos[slot].x;
		tpage.y = tpagepos[slot].y;
		tpage.w = 64;
		tpage.h = 256;

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
			int firstFree = clutpos.y + 8 * (19 - slotsused) + 4;

			if (firstFree < 256)
				firstFree = 480;
			if (firstFree > 500)
				firstFree = 500;

			sPinClutCursor.x = 960;
			sPinClutCursor.y = firstFree;
			sPinClutCursor.w = 16;
			sPinClutCursor.h = 1;

			printInfo("cross-city: imported CLUT rows start at y=%d (level layout ends at %d, %d slots spare)\n",
				firstFree, clutpos.y, 19 - slotsused);
		}

		clut = sPinClutCursor;

		// Ours to write: bypass the ownership guard for this upload, then mark the
		// rectangle owned so the engine's own uploads to it are refused from here on.
		sCarPageUploading = 1;
		sPinReloads++;	// counts re-uploads, i.e. how often the engine took a page back
		LoadTPageAndCluts(&tpage, &clut, sPinIndex[i], buf);
		sCarPageUploading = 0;

		if (clut.x != sPinClutCursor.x || clut.y != sPinClutCursor.y)
			sPinClutCursor = clut;	// the walker advanced: remember where it got to

		sCarPageOwned[slot] = 1;

		tpageslots[slot] = (u_char)sPinIndex[i];
		tpageloaded[sPinIndex[i]] = (u_char)slot;

		sPinSlot[i] = slot;

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
void CarImportDumpState(void)
{
	int i, k;

	if (GetCarImportCity() < 0 && sRemapCount == 0)
		return;

	printInfo("cross-city: final page state (%d pinned, %d world pages evicted, %d page re-uploads)\n", sPinCount, sPinEvictions, sPinReloads);

	// Every pinned set and the rectangle it occupies, decoded from the tpage/clut values
	// the draw path will read. This is what the VRAM dump is aimed at: run with
	// JERICHO_DUMPVRAM=1 and feed these rectangles to tools/vramdump.py. If the car's
	// rectangle no longer looks like a page, something streamed over it.
	for (k = 0; k < sPinCount; k++)
	{
		int pslot = sPinSlot[k];
		unsigned int page = texture_pages[sPinIndex[k]];
		unsigned int clut = texture_cluts[sPinIndex[k]][0];

		printInfo("cross-city:   pinned set %d index %d: slot=%d, rect=(%d,%d), page=%04x, clut0=%04x=(%d,%d)\n",
			sPinSet[k], sPinIndex[k], pslot,
			(pslot >= 0 && pslot < 19) ? tpagepos[pslot].x : -1,
			(pslot >= 0 && pslot < 19) ? tpagepos[pslot].y : -1,
			page, clut, (int)((clut & 0x3f) << 4), (int)(clut >> 6));
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
void LoadImportedTPages(void)
{
	int city = GetCarImportCity();
	int base = GetCarImportPageBase();
	int sets[64];
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
					// imported textures onto rectangles buildings were using.
					pref[nsets] = SPECIAL_CAR_SLOT + k;
					sets[nsets++] = set;
				}
			}
		}
		else
		{
			// Civilian body: the city's civilian car sets.
			for (k = 0; k < 6; k++)
			{
				int set = carTpages[src][k];

				if (set != 0 && nsets < 64 && !SetInList(sets, nsets, set))
				{
					pref[nsets] = -1;	// civilian body: no single natural rectangle, so the
										// level's spare slots are used as before
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
		printInfo("cross-city: candidate %s set %d hostOwns=%d\n",
			LevelNames[city], set, (LevelTookTPage(set) || HostOwnsCarTPage(set)) ? 1 : 0);

		// The host city keeps its own meaning for a set number: a set index holds one
		// meaning at a time. So the imported page goes to a free index instead and the
		// car's polys are translated onto it (CarSetRemap, applied in plotNewCarModel
		// as they are converted). Without this the part kept the host's texture.
		if (LevelTookTPage(set) || HostOwnsCarTPage(set))
		{
			int free = FindFreeSetIndex();

			if (free == 0 || sRemapCount >= CAR_REMAP_MAX)
			{
				printInfo("cross-city: set %d is the level's own and there is no free index - left alone\n", set);
				continue;
			}

			// NOT recorded yet: the remap only counts if the page actually gets loaded.
			// Recording it here left the car's polys pointing at an index with nothing in
			// it whenever the upload was later refused (no spare slot), which draws as an
			// empty texture instead of falling back to the host's page for that part.
			remapFrom = set;
			remapTo = free;

			printInfo("cross-city: set %d is the level's own - re-indexing it to %d for the imported car\n", set, free);

			dstSet = free;
		}

		// locate it in the imported city's page list
		for (j = 0; j < gCarImportPerms.count; j++)
		{
			if (gCarImportPerms.set[j] == set)
			{
				size = gCarImportPerms.bytes[j];
				break;
			}

			offset += (gCarImportPerms.bytes[j] + CDSECTOR_SIZE - 1) & -CDSECTOR_SIZE;
		}

		// Not among the permanent pages? Then it may be a SPECIAL page - and those are
		// exactly what a special body needs: carTpages gives a civilian car six sets,
		// but a special body takes its own two out of specTpages. Their data follows
		// the perm block in the file, each entry sector-aligned, so the walk simply
		// continues where the perm block ends. Without this the player's imported car
		// loaded no textures at all: 'set 77 -> slot 14' twice, nothing uploaded,
		// because 77/78 are in the spec list and the search never looked there.
		if (size <= 8 && gCarImportSpecs.count > 0)
		{
			int specOffset = 0;

			for (j = 0; j < gCarImportPerms.count; j++)
				specOffset += (gCarImportPerms.bytes[j] + CDSECTOR_SIZE - 1) & -CDSECTOR_SIZE;

			for (j = 0; j < gCarImportSpecs.count; j++)
			{
				if (gCarImportSpecs.set[j] == set)
				{
					size = gCarImportSpecs.bytes[j];
					offset = specOffset;
					break;
				}

				specOffset += (gCarImportSpecs.bytes[j] + CDSECTOR_SIZE - 1) & -CDSECTOR_SIZE;
			}
		}

		if (size <= 8)
		{
			printInfo("cross-city: %s set %d is not in its page list - skipped\n", LevelNames[city], set);
			continue;
		}

		// no spare slot: refuse, rather than steal one the level streams into
		if (0)
		{
			;
		}

		buf = (char*)malloc(size);

		if (buf == NULL || !ReadCarImportFile(base + offset, buf, size))
		{
			printInfo("cross-city: %s set %d could not be read (%d bytes) - skipped\n", LevelNames[city], set, size);

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
			printInfo("cross-city: %s set %d looks corrupt (%d clut rows) - skipped\n", LevelNames[city], set, npalettes);
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

		CarPinRecord(set, dstSet, offset, size, pref[i]);

		printInfo("cross-city: %s set %d -> index %d, %d bytes at +%d, %d clut rows (paged in at draw time, evicting the world if needed)\n",
			LevelNames[city], set, dstSet, size, offset, npalettes);
	}
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
	ProcessPalletLump(palette_lump, 0);
	ProcessImportedPalette();	// JERICHO-HOOK: a cross-city import's own palettes

	load_civ_palettes(&clutpos);

	tpagebuffer = (char*)mallocptr;
	nsectors = 0;

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
			IncrementClutNum(&clutpos);
			clutsloaded++;
		}
	}

	if (clutpos.x != 960) 
	{
		clutpos.x = 960;
		clutpos.y++;
	}

	// init all slots
	for (i = slotsused; i < 19; i++)
	{
		tpageslots[i] = 0xFF;

		slot_clutpos[i].vx = clutpos.x;
		slot_clutpos[i].vy = clutpos.y;

		slot_tpagepos[i].vx = tpage.x;
		slot_tpagepos[i].vy = tpage.y;

		IncrementTPageNum(&tpage);
		clutpos.y += 8;
	}

	// JERICHO-HOOK: the level's own page state, for the cross-city invariant. An
	// import must leave every one of these exactly as it is here - measured with an
	// import on and off, and compared. This line is what proves it.
	//
	// The civ_clut checksum is the same idea for the palette table
	// (u_short civ_clut[8][32][6]): those 8 rows hold the colours every car in the
	// level draws with, so an import must not disturb a single entry. A checksum
	// makes that checkable instead of assumed.
	{
		unsigned int clutSum = 0;

		for (i = 0; i < 8 * 32 * 6; i++)
			clutSum = clutSum * 31 + ((u_short*)civ_clut)[i];

		printInfo("cross-city: level page state - slotsused=%d nperms=%d nspecpages=%d tpage=(%d,%d) clutpos=(%d,%d) civclut=%08x\n",
			slotsused, nperms, nspecpages, tpage.x, tpage.y, clutpos.x, clutpos.y, clutSum);
	}
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





