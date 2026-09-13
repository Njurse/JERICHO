#ifndef TEXTURE_H
#define TEXTURE_H

extern char carTpages[4][8];
extern char* texturename_buffer;
extern char* palette_lump;

extern SXYPAIR tpagepos[20];

extern unsigned short texture_pages[128];
extern unsigned short texture_cluts[128][32];
extern int tpage_texamts[128];

extern RECT16 clutpos;
extern RECT16 fontclutpos;
extern RECT16 mapclutpos;

extern unsigned char tpageloaded[128];
extern unsigned char tpageslots[19];

extern DVECTOR slot_clutpos[19];
extern DVECTOR slot_tpagepos[19];

extern RECT16 tpage;

extern short specialSlot;
extern int slotsused;
extern int nperms;
extern int NoTextureMemory;
extern char specTpages[4][12];

extern void ProcessPalletLump(char *lump_ptr, int lump_size); // 0x00019F44

// JERICHO: upload an imported city's car texture sets into texture_pages /
// texture_cluts. Called from inside LoadPermanentTPages.
extern void LoadImportedTPages(void);

// JERICHO: translate an imported vehicle's source-city set number to the index its
// page was actually loaded at (identity when it was not re-indexed). Applied where
// a car's polys are converted into engine form, in cars.c's plotNewCarModel.
extern int CarSetRemap(int set);

// JERICHO: arm/disarm the remap for the car being converted. Must be on only for an
// imported car - a host car whose set number collides with a remapped one needs its
// own page, not the imported city's.
extern void CarSetRemapEnable(int on);

// JERICHO-HOOK: merge a cross-city import's car palettes (civ_clut) so its
// vehicles read their own colours. No-op unless a module asked for an import.
extern void ProcessImportedPalette(void);
extern void load_civ_palettes(RECT16 *cluts); // 0x0001A094

extern void IncrementClutNum(RECT16 *clut); // 0x00080DDC
extern void IncrementTPageNum(RECT16 *tpage); // 0x00080528

extern int LoadTPageAndCluts(RECT16 *tpage, RECT16 *cluts, int tpage2send, char *tpageaddress); // 0x00080E14

extern int Find_TexID(MODEL *model, int t_id); // 0x000805EC
extern TEXINF* GetTEXINFName(char *name, int *tpagenum, int *texturenum); // 0x00080F3C
extern TEXINF* GetTextureInfoName(char *name, TPAN *result); // 0x00080DA0
extern void GetTextureDetails(char *name, TEXTURE_DETAILS *info, int defaultToSea = 1); // 0x00080BB0

extern void update_slotinfo(int tpage, int slot, RECT16 *pos); // 0x00081038

extern void ProcessTextureInfo(char *lump_ptr); // 0x00081080
extern void LoadPermanentTPages(int *sector); // 0x00080688

extern void ReloadIcons(); // 0x00081118

#ifndef PSX
 // [A] - loads TIM files as level textures
void LoadTPageFromTIMs(int tpage2send);
void LoadPermanentTPagesFromTIM();
#endif

#endif
