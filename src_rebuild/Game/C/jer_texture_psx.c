// jer_texture_psx.c — the PSX PAGE target for jer_texture (see jer_texture.h).
//
// *** NOT IMPLEMENTED YET. *** This file holds the seam (jer_texture_internal.h) with
// honest stubs so the IMAGE target can land and be verified on its own. The real
// implementation — decode -> quantise to an indexed image + 16/256-colour CLUT ->
// claim a VRAM page (never the streamer's, see carhacks/docs/VRAM.md) -> LoadImage ->
// report the packed tpage/clut ids + UV rect — is the next unit of work.
//
// Until then a PAGE request fails cleanly, which the registry already handles: the
// caller gets JER_TEX_NONE and can fall back to the IMAGE target.

#include "driver2.h"
#include "JERICHO/include/jericho.h"
#include "jer_texture_internal.h"

int jer_texture_psx_load(const char* path, int* width, int* height,
			 unsigned short* tpage, unsigned short* clut, unsigned char* uv)
{
	(void)path;
	(void)width;
	(void)height;
	(void)tpage;
	(void)clut;
	(void)uv;

	jer_log("jer_texture: PAGE target not implemented yet\n");
	return 0;
}

int jer_texture_psx_reload(const char* path, unsigned short tpage, unsigned short clut)
{
	(void)path;
	(void)tpage;
	(void)clut;

	return 0;
}

void jer_texture_psx_release(unsigned short tpage, unsigned short clut)
{
	(void)tpage;
	(void)clut;
}
