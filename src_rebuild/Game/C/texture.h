#ifndef TEXTURE_H
#define TEXTURE_H

extern char carTpages[4][8];
extern char* texturename_buffer;
extern char* palette_lump;
extern int palette_lump_size;	// JERICHO: the LUMP_PALLET segment's byte size (see texture.c)

extern SXYPAIR tpagepos[20];

extern unsigned short texture_pages[128];
extern unsigned short texture_cluts[128][32];
extern int tpage_texamts[128];

extern RECT16 clutpos;
extern RECT16 fontclutpos;
extern RECT16 mapclutpos;

extern unsigned char tpageloaded[128];
// JERICHO: the base half's SLOT table. 19 entries is the ENGINE's own bound, not a policy
// tuned here: LoadPermanentTPages writes one entry per permanent page, so a level with
// more would already have overrun this array. That is why the slot scans stop at 19 - it
// is an array bound, and "lifting" it would read past the end. What does NOT need the slot
// table is "does the host use this set at all": that is answered from the level's own page
// list (permlist/speclist), which has no such limit.
#define TPAGE_SLOTS 19
extern unsigned char tpageslots[TPAGE_SLOTS];

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

/* JERICHO cross-city hot load: record (pin) the texture pages an imported car needs for
 * a slot built AFTER the level loaded. Idempotent; the palette rows follow when
 * CarImportPin places the pages. */
extern void JerHotLoadCarTpages(void);

/* JERICHO: parse one city's page lists, for a city read in MID-LEVEL. The level-load
 * parse covers only the cities held then, and without this a later city's sets are "not
 * in its page list" - their baked index stays empty and the car draws another page. */
extern void CarImportPageListsForCity(int city);

/* JERICHO: the per-slot cross-city resource manifest - what each resident slot holds: its
 * pins, the pool pages behind them, its civ_clut block and its hot-load geometry, keyed by
 * resident slot (the unit that comes and goes). Filled in where each piece is placed and
 * reset with the import state. A release reads it; nothing reads it for behaviour yet. */
extern void CarSlotResNote(int slot, int city);
extern void CarSlotResNoteGeometry(int slot, int bytes);
extern int  CarSlotResReport(void);

// JERICHO: re-claim and re-upload imported pages whose slot has been taken back by
// streaming. Called from the game loop; cheap unless something actually went wrong.
extern void CarImportPin(void);

// JERICHO: report where the imported sets' pages actually ended up, decoding the
// draw path's tpage/clut values back into VRAM coordinates. Called at the end of a
// debug run - the check that says whether streaming replaced them.
extern void CarImportDumpState(void);

// JERICHO: the texture sets one imported model's own polygons name. Cleared per slot
// before the level's car models are built; buildNewCarFromModel records into it as it
// walks the polys, and LoadImportedTPages imports only what it holds.
extern void CarModelSetsClear(int slot);
extern void CarModelSetsAdd(int slot, int set);
extern int  CarModelSetCount(int slot);
extern int  CarModelSet(int slot, int k);
extern int  CarModelSetUsed(int set);

// JERICHO: clear every bit of cross-city state for a new level - pins, remaps, page
// ownership, the CLUT cursor and the per-slot set lists. Called from InitCarImport,
// which runs before the level's car models are built.
extern void CarImportResetState(void);

// JERICHO: whether a VRAM rectangle (a tpage position) is owned by an imported page.
// The spool's own upload paths must respect this - they bypass LoadTPageAndCluts.
extern int CarPageRectOwned(int x, int y);
extern int CarPageSlotOwned(int slot);

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

// JERICHO: the mid-level counterpart, for ONE city read in after the level loaded (the
// hot load). Defers that city's car-palette lump; the rows still upload lazily on the
// first draw of a car that names them. Touches no other city, and clears nothing --
// ProcessImportedPalette must not be used for this (see cars.c).
extern int CarImportApplyPaletteForCity(int city);
// JERICHO-HOOK: the same merge, but uploading ONLY these civ_clut rows (indexed by row,
// CIV_CLUT_ROWS entries). The import's whole table is 228 CLUTs = 57 column rows, most
// of which belong to car slots the imported model never draws from. Called from
// CarImportPin, the first point the built model's rows exist.
// Returns 1 when a deferred lump was uploaded, 0 when there was none.
extern int ProcessImportedPaletteRows(const unsigned char* rowNeeded);
extern void load_civ_palettes(RECT16 *cluts); // 0x0001A094

extern void IncrementClutNum(RECT16 *clut); // 0x00080DDC

// JERICHO-HOOK: allocate a CLUT row that is a recoloured copy of another row
// (per-instance pedestrian palettes). `floor5` (0..31) lifts the dark end so a
// dark outfit still reads as the team colour. Entries that are themselves warm
// (red-biased) are copied through untouched, because skin shares the outfit's
// CLUT row - see pedest.c's PedPalRowIsOutfit. Returns the new clut word, or 0.
extern u_short JerichoMakeClutRow(u_short sourceClut, int r, int g, int b, int strength, int floor5);
// JERICHO-HOOK: read/write a CLUT row IN PLACE at the address a CLUT id already names.
// Costs no CLUT strip row (clutpos is untouched), so it works with the strip full. The
// row may be SHARED by every car that resolves to the same id, so an in-place edit is
// global - which is what makes it the way to find out which part a row paints.
// Both are immediate (GR_ReadVRAM / GR_CopyVRAM), safe outside a render pass.
// Return 1 on success, 0 on a zero id or null buffer.
extern int JerichoClutReadInPlace(u_short clut, u_short* out16);
extern int JerichoClutWriteInPlace(u_short clut, const u_short* in16);

/* JERICHO: the CLUT-strip budget, shared by the ped team colours, the imported car
 * palettes and the per-instance car colours. One dyed row = one 16-entry CLUT, and a
 * dyed car takes one per textured part, so these answer "how many custom-coloured
 * cars still fit" before anything is written. */
extern int jer_clut_slots_free(void);
extern void jer_clut_report(const char* why);
extern void IncrementTPageNum(RECT16 *tpage); // 0x00080528

extern int LoadTPageAndCluts(RECT16 *tpage, RECT16 *cluts, int tpage2send, char *tpageaddress); // 0x00080E14

extern int Find_TexID(MODEL *model, int t_id); // 0x000805EC
extern TEXINF* GetTEXINFName(char *name, int *tpagenum, int *texturenum); // 0x00080F3C
extern TEXINF* GetTextureInfoName(char *name, TPAN *result); // 0x00080DA0
extern void GetTextureDetails(char *name, TEXTURE_DETAILS *info, int defaultToSea = 1); // 0x00080BB0

extern void update_slotinfo(int tpage, int slot, RECT16 *pos); // 0x00081038

// JERICHO: the bottom-half VRAM pool (rows 512..1023) - the space the base game never
// addresses. Everything JERICHO loads is placed here rather than taken from the top
// half's claims. See texture.c and JER_VRAM_HALF_Y in cars.h.
void JerLowerPoolReset(void);
void JerLowerPoolPageRect(int slot, RECT16 *r);
int  JerLowerPoolPageAlloc(void);
void JerLowerPoolPageFree(int slot);
int  JerLowerPoolPageHolds(int slot, int page);
int  JerLowerPoolPageOwned(int x, int y);
void JerLowerPoolClutCursor(RECT16 *out);	// walk it (IncrementClutNum), then Advance
void JerLowerPoolClutAdvance(int rows);
int  JerLowerPoolPagesUsed(void);
int  JerLowerPoolPagesFree(void);
int  JerLowerPoolClutRowsUsed(void);
int  JerLowerPoolClutRowsFree(void);
int  JerLowerPoolClutDropped(void);

extern void ProcessTextureInfo(char *lump_ptr); // 0x00081080
extern void LoadPermanentTPages(int *sector); // 0x00080688

extern void ReloadIcons(); // 0x00081118

#ifndef PSX
 // [A] - loads TIM files as level textures
void LoadTPageFromTIMs(int tpage2send);
void LoadPermanentTPagesFromTIM();
#endif

#endif
