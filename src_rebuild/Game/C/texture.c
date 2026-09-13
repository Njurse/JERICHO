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
int LoadTPageAndCluts(RECT16 *tpage, RECT16 *cluts, int tpage2send, char *tpageaddress)
{
	int npalettes;
	int i;
	RECT16 temptpage;

	char* tempBuf;

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





