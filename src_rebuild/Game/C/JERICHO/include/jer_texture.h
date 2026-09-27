#ifndef JER_TEXTURE_H
#define JER_TEXTURE_H

/* jer_texture — custom texture injection for JERICHO modules.
 *
 * THE ONE WAY to get an image you authored onto something in the world. A module
 * ships a TGA beside it, asks the engine for a handle, and draws it; the engine owns
 * the loading, the upload and the draw. Nothing about texture pages, CLUTs, GL texture
 * ids or the ordering table belongs in a module.
 *
 * ---------------------------------------------------------------------------
 * ASSET CONVENTION
 * ---------------------------------------------------------------------------
 * Textures live in the module's OWN folder, under a `textures/` subdirectory:
 *
 *     JERICHO/MODS/<modId>/textures/<name>.tga
 *
 * so a module called `cainescrossfire` loading "icons/health" reads
 *
 *     JERICHO/MODS/cainescrossfire/textures/icons/health.tga
 *
 * `name` is a slash-separated relative path, WITHOUT the extension (pass the
 * extension too if you have a file that is not a .tga — see jer_texture_load_path).
 * The file is a 32-bit TGA: an uncompressed or RLE true-colour TGA with an 8-bit
 * alpha channel (the format LoadTGAImage already reads, and the only one that
 * carries alpha for a cut-out icon). A dev build runs from
 * <repo>/src_rebuild/bin/<config>/JERICHO, so the resolver prefers the repo's own
 * MODS folder four levels up and falls back to the mirror next to the exe — the same
 * rule the arena loader uses, so the file a tool writes is the file the game reads.
 *
 * ---------------------------------------------------------------------------
 * TWO TARGETS
 * ---------------------------------------------------------------------------
 * A texture is drawn one of two ways, chosen by the loader:
 *
 *   JER_TEX_TARGET_IMAGE — a real RGBA texture uploaded to the GPU and sampled
 *     directly (a "hires" texture). Any size, full colour, alpha blending, no VRAM
 *     cost. This is the default and what an icon wants.
 *
 *   JER_TEX_TARGET_PAGE  — the image is quantised to an indexed PSX texture PAGE plus
 *     a 16- or 256-colour CLUT in VRAM and drawn as a normal textured poly, so it
 *     looks exactly like the rest of the game. VRAM is effectively FULL on a played
 *     level (see the mod's carhacks/VRAM.md), so a page is taken from an existing
 *     claim, and the load FAILS cleanly when nothing can be given up. Use this when
 *     the texture has to sit in a level's palette world rather than on top of it.
 *
 * Both targets draw through the same calls below, so a module does not care which it
 * got except when it wants the raw tpage/clut (jer_texture_tpage).
 *
 * ---------------------------------------------------------------------------
 * DRAWING
 * ---------------------------------------------------------------------------
 * Draw from JER_EVENT_DRAW_WORLD (the mid-render hook, while the camera matrices are
 * live) or JER_EVENT_DRAW_OVERLAY. Calling out of a render pass is a no-op, so a
 * module never has to know whether the camera is ready.
 *
 * The world position is the GAME's raw world frame — the same numbers a car's
 * `hd.where.t[0..2]` or an arena pickup's x/z carry. The API applies the engine's
 * Y-flip internally (the trap that costs an afternoon: render-space Y is the NEGATIVE
 * of that frame, and camera-space +Y runs DOWN the screen).
 *
 * The implementation lives in the GAME (Game/C/jer_texture.c + jer_texture_psx.c):
 * only the engine can decode, upload and put a primitive in the ordering table.
 */

#ifdef __cplusplus
extern "C" {
#endif


/* A loaded texture. JER_TEX_NONE (0) means "nothing" — an unloaded, failed or freed
 * handle, which every call treats as a no-op. */
typedef unsigned int JER_TEXTURE;

#define JER_TEX_NONE	((JER_TEXTURE)0)

/* Longest texture name, terminator included. */
#define JER_TEX_NAME_MAX	128

/* ------------------------------------------------------------------ */
/* Targets                                                             */
/* ------------------------------------------------------------------ */

enum
{
	JER_TEX_TARGET_AUTO = -1,	/* let the engine choose (currently PAGE for a mod
						   that named a page, else IMAGE) */
	JER_TEX_TARGET_IMAGE = 0,	/* GPU RGBA texture: any size, full colour */
	JER_TEX_TARGET_PAGE = 1		/* PSX VRAM page + CLUT: authentic, needs VRAM */
};

/* ------------------------------------------------------------------ */
/* Draw flags                                                          */
/* ------------------------------------------------------------------ */

enum
{
	JER_TEX_DRAW_NONE = 0,

	/* Draw the reverse-wound twin as well, so a spinning card is not invisible from
	 * behind. Cheap (one more quad); ON is what a pickup wants. */
	JER_TEX_DRAW_DOUBLE = 1 << 0,

	/* Ignore `spin` and yaw the card towards the camera instead — a true billboard.
	 * A pickup that spins should NOT set this (it would cancel the spin). */
	JER_TEX_DRAW_BILLBOARD = 1 << 1,

	/* Draw the front face only, with the texture MIRRORED, so an upright card reads
	 * the right way round when it is showing its back. Implied by JER_TEX_DRAW_DOUBLE
	 * when the card is more than 90 degrees from the camera. */
	JER_TEX_DRAW_MIRROR = 1 << 2,

	/* Skip the depth sort: put the card in the nearest OT bucket so nothing occludes
	 * it (a marker you must always see). Off = sorted with the world, so a car or a
	 * wall in front really does hide it. */
	JER_TEX_DRAW_NO_OCCLUDE = 1 << 3
};

/* ------------------------------------------------------------------ */
/* Loading                                                             */
/* ------------------------------------------------------------------ */

/* Load <root>/MODS/<modId>/textures/<name>.tga into the default target (currently
 * JER_TEX_TARGET_IMAGE). Returns JER_TEX_NONE when the file is missing, is not a
 * 32-bit TGA, or the target could not provide storage. Safe to call repeatedly: the
 * same (modId, name, target) returns the same handle and loads once. */
JER_TEXTURE jer_texture_load(const char* modId, const char* name);

/* As jer_texture_load, but with an explicit VRAM/GPU target. */
JER_TEXTURE jer_texture_load_target(const char* modId, const char* name, int target);

/* Load an explicit path (absolute, or relative to the game's working directory) with
 * an explicit target. `path` may include the extension. This is the escape hatch for
 * a texture that does not follow the convention; prefer jer_texture_load. */
JER_TEXTURE jer_texture_load_path(const char* path, int target);

/* Build the conventional path for (modId, name) into `out` (NUL-terminated, cap
 * bytes). Returns 1 on success, 0 when it does not fit. Exposed so a tool or a module
 * can report where it is going to look. `out` takes the ABSOLUTE path, frozen at the
 * call, because the game changes its working directory while loading a level. */
int jer_texture_mod_path(const char* modId, const char* name, char* out, int cap);

/* As jer_texture_mod_path, for JERICHO's OWN art: <root>/CORE/<name>, repo copy first.
 * The menu background lives there. */
int jer_texture_core_path(const char* name, char* out, int cap);

/* Free a texture (a no-op on JER_TEX_NONE). Every OTHER handle stays valid, but a
 * handle used after freeing is a no-op rather than a crash. */
void jer_texture_free(JER_TEXTURE tex);

/* Free every loaded texture. The engine also does this on JER_EVENT_SHUTDOWN. */
void jer_texture_free_all(void);

/* Re-read and re-upload a texture from disk — for iterating on art without a restart.
 * Returns 1 on success, 0 when the file is now missing/bad (the old texture is kept). */
int jer_texture_reload(JER_TEXTURE tex);

/* Re-read every loaded texture. Fired for you on JER_EVENT_GAME_START and
 * JER_EVENT_FRONTEND_ENTERED, so a texture edited between runs is picked up. */
void jer_texture_reload_all(void);

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

/* Non-zero when `tex` is a live, drawable handle. */
int jer_texture_valid(JER_TEXTURE tex);

/* The texture's pixel size (either out pointer may be NULL). Returns 1 on success. */
int jer_texture_size(JER_TEXTURE tex, int* width, int* height);

/* Which target `tex` actually landed on (a JER_TEX_TARGET_*), or -1 for an invalid
 * handle. A PAGE request that had to be given up reports IMAGE, so a module can see
 * that it got the fallback rather than the authentic look. */
int jer_texture_target(JER_TEXTURE tex);

/* The raw PSX page ids for a PAGE-target texture, for a module that draws its own
 * POLY_FT4. Writes the packed tpage id and the CLUT id (see the engine's
 * TEXTURE_DETAILS). Returns 1 on success, 0 for an IMAGE-target or invalid handle. */
int jer_texture_tpage(JER_TEXTURE tex, unsigned short* tpage, unsigned short* clut);

/* How many textures are resident (diagnostics). */
int jer_texture_count(void);

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* Draw `tex` as an upright, flat CARD in the world.
 *
 *   x, y, z   the card's centre, in the game's raw world frame
 *   halfW     half-width in world units (the card is 2*halfW wide)
 *   halfH     half-height in world units
 *   spin      yaw in PSX angle units, 0..4095 (128 == 11.25 degrees). The card turns
 *             about the world vertical, so it goes edge-on twice per turn — that is
 *             what reads as "spinning". Ignored with JER_TEX_DRAW_BILLBOARD.
 *   flags     a mask of JER_TEX_DRAW_*
 *
 * No-op outside a render pass, on a freed handle, or when the card is behind the
 * camera. Draws at most two quads (front + reverse-wound back with JER_TEX_DRAW_DOUBLE).
 */
void jer_texture_draw_card(JER_TEXTURE tex, int x, int y, int z,
			   int halfW, int halfH, int spin, int flags);

/* Draw `tex` on an axis-aligned RECTANGLE in the world, lying in the horizontal plane
 * (a decal on the ground) — the flat sibling of jer_texture_draw_card. `yaw` turns the
 * rectangle about the vertical. Same flags. */
void jer_texture_draw_flat(JER_TEXTURE tex, int x, int y, int z,
			   int halfW, int halfL, int yaw, int flags);

/* Blit `tex` over a rectangle in the FRAME BUFFER — a screen-space draw with no camera
 * involved, for a menu background or any full-screen art. `x, y` is the top-left corner
 * and `w, h` the size, in the current draw buffer's coordinates, so any texture can be
 * scaled to the display without resizing the image.
 *
 * `otBucket` is the caller's own ordering-table bucket and is REQUIRED: the frontend's
 * table is only 16 entries, so a world-sized index would write past the end of it. The
 * menu background passes the bucket its stock art used (11), which the DT walks before
 * the lower buckets - so the menu text lands on top. */
void jer_texture_draw_screen(JER_TEXTURE tex, int x, int y, int w, int h, int otBucket);

#ifdef __cplusplus
}
#endif

/* Engine-internal: non-zero while a render pass is live. The engine brackets the
 * DRAW_WORLD / DRAW_OVERLAY fires with it, and the frontend brackets its own
 * display-buffer draws, so a draw made from the wrong place is ignored rather than
 * writing into a primitive table nothing is rendering. Declared OUTSIDE the extern "C"
 * block on purpose: the engine defines it as a C++ symbol. Modules do not touch it. */
extern int gJerTextureRenderPass;

#endif /* JER_TEXTURE_H */
