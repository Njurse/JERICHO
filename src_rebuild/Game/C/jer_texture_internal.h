#ifndef JER_TEXTURE_INTERNAL_H
#define JER_TEXTURE_INTERNAL_H

/* jer_texture_internal — the seam between the two halves of the texture subsystem.
 *
 * jer_texture.c owns the registry, the asset convention and the DRAW recipe
 * (target-agnostic). jer_texture_psx.c owns the PSX PAGE target: quantising a decoded
 * image, claiming a VRAM page, uploading it, and giving it back. This header is the
 * only thing they share. It is NOT a module header.
 *
 * The PAGE side deals in raw ids rather than handles because a handle is the
 * registry's (jer_texture.c's) business; storing one from here would invert the
 * dependency. So it fills in what it produced and the registry records it. */

/* Load `path` (a 32-bit TGA) into a PSX texture page + CLUT.
 *
 * On success fills: width/height (the image's pixel size), tpage/clut (the packed ids
 * a POLY_FT4 stores), and the 8 UV bytes as u0,v0,u1,v1,u2,v2,u3,v3 in the engine's
 * TL,TR,BL,BR order, 0..255 within the page.
 *
 * Returns 1 on success. Returns 0 when the image cannot be decoded, is not 32-bit, or
 * no VRAM page can be given up (VRAM is effectively full on a played level, so this is
 * a normal outcome, not a bug — the caller falls back to the IMAGE target). */
int jer_texture_psx_load(const char* path, int* width, int* height,
			 unsigned short* tpage, unsigned short* clut, unsigned char* uv);

/* Re-read `path` into the page this texture already owns (same tpage/clut). Returns 1
 * on success; 0 leaves the existing page contents alone. */
int jer_texture_psx_reload(const char* path, unsigned short tpage, unsigned short clut);

/* Give a page/CLUT pair back to the allocator. A no-op for an unknown pair. */
void jer_texture_psx_release(unsigned short tpage, unsigned short clut);

#endif /* JER_TEXTURE_INTERNAL_H */
