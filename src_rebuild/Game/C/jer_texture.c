// jer_texture.c — JERICHO custom texture injection, ENGINE side.
//
// The implementation behind jer_texture.h (read that header for the module-facing
// contract). This file is where the engine's rules live:
//
//   * THE ASSET CONVENTION. MODS/<modId>/textures/<name>.tga, resolved at load time
//     with the same dev-repo-then-mirror rule the arena loader uses: a dev build runs
//     from <repo>/src_rebuild/bin/<config>/JERICHO, so the repo's own MODS folder sits
//     four levels up and is preferred, so the file a tool writes is the file the game
//     reads. The result is frozen to an ABSOLUTE path (the game changes its working
//     directory while loading a level).
//
//   * A HANDLE REGISTRY. One (key, target) loads once and is shared; the handle packs
//     a slot index and a generation, so a handle used after jer_texture_free is a
//     clean no-op rather than a stale alias of whatever took the slot.
//
//   * THE DRAW RECIPE (this is the whole reason the header is small). The geometry is
//     not obvious and every part of it was measured:
//
//         rel = (x, -y, z) - camera_position      // raw world frame -> render space
//         Apply_Inv_CameraMatrix(&rel);  gte_SetTransVector(&rel);
//         rot = inv_camera_matrix * RotY(spin)    // or face_camera for a billboard
//         project the 4 card corners; skip if z < 150; addPrim(ot + (z >> 3))
//
//     - the Y negation: the game's raw world frame (a car's hd.where.t) is Y-flipped
//       against render space (cars.c does `pos.vy = -cp->hd.where.t[1]`), and a
//       positive lift in the raw frame moves a card DOWN the screen.
//     - the spin must be a RotY composed with inv_camera_matrix, NOT face_camera:
//       face_camera re-aims the card at the camera every frame, which cancels a spin.
//       face_camera is used only for JER_TEX_DRAW_BILLBOARD.
//     - z >> 3 is the engine's world-prim bucket (debris.c) and the only SAFE shift:
//       SZ saturates at 0xFFFF and 0xFFFF >> 3 == OTSIZE-1, while >>2 can index past
//       the 0x2000-entry ordering table.
//
//   * THE DR_PSYX_TEX OVERRIDE IS A GLOBAL. PsyX's overrideTexture (PsyX_GPU.cpp)
//     applies to EVERY textured primitive parsed after the DR_PSYX_TEX, and a change
//     of texture forces a new GPU draw split. addPrim PREPENDS and the OT is walked
//     head-first, so to get the parse order [override][quad][reset] the three prims
//     are ADDED in the REVERSE order (reset, quad, override) into one bucket, and
//     nothing else is added between them.
//
//   * DRAWING IS ONLY LEGAL IN A RENDER PASS. gJerTextureRenderPass is set by the
//     engine around the DRAW_WORLD / DRAW_OVERLAY fires; a draw call outside one is
//     ignored, so a module calling from the wrong hook cannot corrupt the primtab.

#include "driver2.h"
#include "system.h"
#include "camera.h"
#include "draw.h"
#include "../utils/targa.h"
#include "ASM/rndrasm.h"
#include "JERICHO/include/jericho.h"
#include "JERICHO/include/jer_texture.h"
#include "jer_texture_internal.h"
#include "PsyX/PsyX_render.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define JER_TEX_MAX		64
#define JER_TEX_KEY_MAX		400
#define JER_TEX_PATH_MAX	512

// Engine-internal: non-zero only while the engine is inside a render pass. main.c
// brackets the DRAW_WORLD / DRAW_OVERLAY fires with these.
int gJerTextureRenderPass = 0;

typedef struct JER_TEX_SLOT
{
	int		used;
	int		generation;
	int		target;			// JER_TEX_TARGET_*
	char		key[JER_TEX_KEY_MAX];	// identity: absolute path + target
	char		path[JER_TEX_PATH_MAX];	// absolute source path
	int		width, height;
	unsigned int	glTexture;		// IMAGE target
	unsigned short	tpage, clut;		// PAGE target
	unsigned char	u0, v0, u1, v1, u2, v2, u3, v3;	// UV rect (0..255), both targets
} JER_TEX_SLOT;

static JER_TEX_SLOT gTex[JER_TEX_MAX];

// JERICHO_TEX_DIAG=1 traces the projected card rectangle (see jerTexEmitQuad).
static int gJerTexDiag = -1;		/* -1 = not yet probed */
static int gJerTexDiagDrawn;

// ---------------------------------------------------------------------------
// handles
// ---------------------------------------------------------------------------

#define JER_TEX_HANDLE(slot, gen)	((JER_TEXTURE)(((unsigned)(gen) << 16) | (unsigned)((slot) + 1)))

static JER_TEX_SLOT* jerTexSlot(JER_TEXTURE tex, int* outSlot)
{
	unsigned idx = (unsigned)tex & 0xFFFFu;
	unsigned gen = (unsigned)tex >> 16;
	int slot = (int)idx - 1;

	if (idx == 0 || slot < 0 || slot >= JER_TEX_MAX)
		return NULL;

	if (!gTex[slot].used || gTex[slot].generation != (int)gen)
		return NULL;

	if (outSlot != NULL)
		*outSlot = slot;

	return &gTex[slot];
}

static void jerTexSetDefaultUVs(JER_TEX_SLOT* s)
{
	s->u0 = 0;   s->v0 = 0;
	s->u1 = 255; s->v1 = 0;
	s->u2 = 0;   s->v2 = 255;
	s->u3 = 255; s->v3 = 255;
}

// EXPORT ANCHOR. The linker drops any function the exe does not reference
// (/OPT:REF, plus whole-program optimisation), and exports.def is generated FROM the
// linker map -- so an SDK entry point no module happens to call YET would silently
// disappear from the surface a mod DLL links against. Naming every public entry point
// here keeps the whole documented API in the image, and therefore in exports.def.
//
// The stores go into a FILE-SCOPE volatile: a local array of addresses is provably
// dead and gets optimised out (which is exactly what happened the first time).
// Membership is the header's, not this list's: add a line when you add an entry point.
static volatile const void* gJerTexApiKeep[16];

static void jerTexKeepPublicSurface(void)
{
	gJerTexApiKeep[0]  = (const void*)jer_texture_load;
	gJerTexApiKeep[1]  = (const void*)jer_texture_load_target;
	gJerTexApiKeep[2]  = (const void*)jer_texture_load_path;
	gJerTexApiKeep[3]  = (const void*)jer_texture_mod_path;
	gJerTexApiKeep[4]  = (const void*)jer_texture_free;
	gJerTexApiKeep[5]  = (const void*)jer_texture_free_all;
	gJerTexApiKeep[6]  = (const void*)jer_texture_reload;
	gJerTexApiKeep[7]  = (const void*)jer_texture_reload_all;
	gJerTexApiKeep[8]  = (const void*)jer_texture_valid;
	gJerTexApiKeep[9]  = (const void*)jer_texture_size;
	gJerTexApiKeep[10] = (const void*)jer_texture_target;
	gJerTexApiKeep[11] = (const void*)jer_texture_tpage;
	gJerTexApiKeep[12] = (const void*)jer_texture_count;
	gJerTexApiKeep[13] = (const void*)jer_texture_draw_card;
	gJerTexApiKeep[14] = (const void*)jer_texture_draw_flat;
	gJerTexApiKeep[15] = (const void*)jer_texture_psx_release;
}

// ---------------------------------------------------------------------------
// path convention
// ---------------------------------------------------------------------------

static int jerTexFileExists(const char* path)
{
	FILE* fp = fopen(path, "rb");

	if (fp == NULL)
		return 0;

	fclose(fp);
	return 1;
}

static void jerTexAbsolute(char* out, int cap, const char* raw)
{
#if defined(_WIN32)
	if (_fullpath(out, raw, cap) == NULL)
		snprintf(out, cap, "%s", raw);
#else
	if (realpath(raw, out) == NULL)
		snprintf(out, cap, "%s", raw);
#endif
}

int jer_texture_mod_path(const char* modId, const char* name, char* out, int cap)
{
	char rel[JER_TEX_PATH_MAX];
	char dev[JER_TEX_PATH_MAX + 64];
	char raw[JER_TEX_PATH_MAX + 64];
	const char* nm = (name != NULL) ? name : "";
	const char* root = jer_root_dir();
	size_t len = strlen(nm);
	int hasExt = (len >= 4) && (_stricmp(nm + len - 4, ".tga") == 0);

	if (out == NULL || cap <= 0 || root == NULL)
		return 0;

	snprintf(rel, sizeof(rel), "MODS/%s/textures/%s%s",
		(modId != NULL) ? modId : "", nm, hasExt ? "" : ".tga");

	// A dev build's JERICHO folder is <repo>/src_rebuild/bin/<config>/JERICHO, so the
	// repo's MODS tree sits four levels up: prefer it, so the file the tools write is
	// the file the game reads. A shipped build has no repo tree and uses the mirror.
	snprintf(dev, sizeof(dev), "%s/../../../../JERICHO/%s", root, rel);

	if (jerTexFileExists(dev))
		snprintf(raw, sizeof(raw), "%s", dev);
	else
		snprintf(raw, sizeof(raw), "%s/%s", root, rel);

	jerTexAbsolute(out, cap, raw);
	return 1;
}

// ---------------------------------------------------------------------------
// load / free / reload
// ---------------------------------------------------------------------------

static JER_TEXTURE jerTexStore(const char* path, int target, const char* key,
			      int width, int height, unsigned int glTexture,
			      unsigned short tpage, unsigned short clut,
			      const unsigned char* uv)
{
	int i, slot = -1;
	int gen;
	JER_TEX_SLOT* s;

	for (i = 0; i < JER_TEX_MAX; i++)
	{
		if (!gTex[i].used)
		{
			slot = i;
			break;
		}
	}

	if (slot < 0)
	{
		jer_log("jer_texture: out of texture slots (%d)\n", JER_TEX_MAX);
		return JER_TEX_NONE;
	}

	gen = gTex[slot].generation + 1;	// bumped before the memset clears it

	s = &gTex[slot];
	memset(s, 0, sizeof(*s));

	s->used = 1;
	s->generation = gen;
	s->target = target;
	s->width = width;
	s->height = height;
	s->glTexture = glTexture;
	s->tpage = tpage;
	s->clut = clut;

	snprintf(s->key, sizeof(s->key), "%s", (key != NULL) ? key : path);
	snprintf(s->path, sizeof(s->path), "%s", path);

	if (uv != NULL)
	{
		s->u0 = uv[0]; s->v0 = uv[1];
		s->u1 = uv[2]; s->v1 = uv[3];
		s->u2 = uv[4]; s->v2 = uv[5];
		s->u3 = uv[6]; s->v3 = uv[7];
	}
	else
	{
		jerTexSetDefaultUVs(s);
	}

	return JER_TEX_HANDLE(slot, s->generation);
}

JER_TEXTURE jer_texture_load_path(const char* path, int target)
{
	u_char* data = NULL;
	int w = 0, h = 0, bpp = 0;
	char key[JER_TEX_KEY_MAX];
	char fixed[JER_TEX_PATH_MAX];
	unsigned int gl;
	int i;

	if (path == NULL)
		return JER_TEX_NONE;

	jerTexKeepPublicSurface();	// see the export-anchor note above

	jerTexAbsolute(fixed, sizeof(fixed), path);

	snprintf(key, sizeof(key), "%s|%d", fixed, target);

	for (i = 0; i < JER_TEX_MAX; i++)
	{
		if (gTex[i].used && strcmp(gTex[i].key, key) == 0)
			return JER_TEX_HANDLE(i, gTex[i].generation);
	}

	if (target == JER_TEX_TARGET_PAGE)
	{
		int w = 0, h = 0;
		unsigned short tp = 0, cl = 0;
		unsigned char uv[8];

		if (!jer_texture_psx_load(fixed, &w, &h, &tp, &cl, uv))
			return JER_TEX_NONE;

		return jerTexStore(fixed, JER_TEX_TARGET_PAGE, key, w, h, 0, tp, cl, uv);
	}

	if (!LoadTGAImage(fixed, &data, w, h, bpp) || data == NULL)
	{
		jer_log("jer_texture: cannot read %s\n", fixed);
		if (data != NULL)
			free(data);
		return JER_TEX_NONE;
	}

	if (bpp != 32)
	{
		jer_log("jer_texture: %s is %d-bit, need a 32-bit TGA (with alpha)\n", fixed, bpp);
		free(data);
		return JER_TEX_NONE;
	}

	gl = (unsigned int)GR_CreateRGBATexture(w, h, data);
	free(data);

	if (gl == 0)
	{
		jer_log("jer_texture: GPU upload failed for %s\n", fixed);
		return JER_TEX_NONE;
	}

	return jerTexStore(fixed, JER_TEX_TARGET_IMAGE, key, w, h, gl, 0, 0, NULL);
}

JER_TEXTURE jer_texture_load_target(const char* modId, const char* name, int target)
{
	char path[JER_TEX_PATH_MAX];

	if (!jer_texture_mod_path(modId, name, path, sizeof(path)))
		return JER_TEX_NONE;

	return jer_texture_load_path(path, target);
}

JER_TEXTURE jer_texture_load(const char* modId, const char* name)
{
	return jer_texture_load_target(modId, name, JER_TEX_TARGET_IMAGE);
}

void jer_texture_free(JER_TEXTURE tex)
{
	JER_TEX_SLOT* s = jerTexSlot(tex, NULL);

	if (s == NULL)
		return;

	// IMAGE textures own a GPU texture; PAGE textures own a VRAM claim.
	if (s->target == JER_TEX_TARGET_IMAGE && s->glTexture != 0)
		GR_DestroyTexture(s->glTexture);
	else if (s->target == JER_TEX_TARGET_PAGE)
		jer_texture_psx_release(s->tpage, s->clut);
	s->used = 0;
	s->glTexture = 0;
	s->tpage = s->clut = 0;
}

void jer_texture_free_all(void)
{
	int i, n = 0;

	for (i = 0; i < JER_TEX_MAX; i++)
	{
		if (gTex[i].used)
		{
			jer_texture_free(JER_TEX_HANDLE(i, gTex[i].generation));
			n++;
		}
	}

	if (n > 0)
		jer_log("jer_texture: released %d texture(s)\n", n);
}

// Re-read one texture's bytes into its existing storage.
//
// The IMAGE target replaces the GPU texture (the old one is destroyed); a PAGE
// re-upload is handled by the PSX side. On any failure the existing texture is left
// alone, so a transient bad file does not blank a live texture.
int jer_texture_reload(JER_TEXTURE tex)
{
	JER_TEX_SLOT* s = jerTexSlot(tex, NULL);
	u_char* data = NULL;
	int w = 0, h = 0, bpp = 0;
	unsigned int gl;

	if (s == NULL)
		return 0;

	if (s->target == JER_TEX_TARGET_PAGE)
		return jer_texture_psx_reload(s->path, s->tpage, s->clut);

	if (!LoadTGAImage(s->path, &data, w, h, bpp) || data == NULL)
	{
		if (data != NULL)
			free(data);
		return 0;
	}

	if (bpp != 32)
	{
		free(data);
		return 0;
	}

	gl = (unsigned int)GR_CreateRGBATexture(w, h, data);
	free(data);

	if (gl == 0)
		return 0;

	if (s->glTexture != 0)
		GR_DestroyTexture(s->glTexture);

	s->glTexture = gl;
	s->width = w;
	s->height = h;

	return 1;
}

void jer_texture_reload_all(void)
{
	int i, n = 0;

	for (i = 0; i < JER_TEX_MAX; i++)
	{
		if (gTex[i].used && jer_texture_reload(JER_TEX_HANDLE(i, gTex[i].generation)))
			n++;
	}

	if (n > 0)
		jer_log("jer_texture: re-read %d texture(s) from disk\n", n);
}

// ---------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------

int jer_texture_valid(JER_TEXTURE tex)
{
	return jerTexSlot(tex, NULL) != NULL;
}

int jer_texture_size(JER_TEXTURE tex, int* width, int* height)
{
	JER_TEX_SLOT* s = jerTexSlot(tex, NULL);

	if (s == NULL)
		return 0;

	if (width != NULL)
		*width = s->width;

	if (height != NULL)
		*height = s->height;

	return 1;
}

int jer_texture_target(JER_TEXTURE tex)
{
	JER_TEX_SLOT* s = jerTexSlot(tex, NULL);

	return (s != NULL) ? s->target : -1;
}

int jer_texture_tpage(JER_TEXTURE tex, unsigned short* tpage, unsigned short* clut)
{
	JER_TEX_SLOT* s = jerTexSlot(tex, NULL);

	if (s == NULL || s->target != JER_TEX_TARGET_PAGE)
		return 0;

	if (tpage != NULL)
		*tpage = s->tpage;

	if (clut != NULL)
		*clut = s->clut;

	return 1;
}

int jer_texture_count(void)
{
	int i, n = 0;

	for (i = 0; i < JER_TEX_MAX; i++)
	{
		if (gTex[i].used)
			n++;
	}

	return n;
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

// One textured, alpha-blended quad plus its override bookkeeping, put into the world
// OT at a depth bucket. `verts` are the 4 corners in the current GTE local frame, in
// the engine's FT4 order (TL, TR, BL, BR). `mirror` swaps the U coordinates so an
// icon seen from behind does not read backwards.
static void jerTexEmitQuad(JER_TEX_SLOT* s, const SVECTOR* verts, int mirror, int noOcclude)
{
	POLY_FT4* poly;
	char* base;
	int z;

	base = current->primptr;
	poly = (POLY_FT4*)base;

	setPolyFT4(poly);

	// Alpha. Under the PsyX override texture the format is forced to RGBA, and the
	// blend mode comes from the tpage's ABR bits: tpage 0 with setSemiTrans(1) gives
	// BM_AVERAGE == GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA, i.e. a real cut-out icon.
	setSemiTrans(poly, 1);
	setRGB0(poly, 255, 255, 255);

	gte_ldv0(&verts[0]);
	gte_rtps();
	gte_stsxy(&poly->x0);
	gte_stsz(&z);

	// the engine's near-clip guard for world prims (debris.c): below 150 the projected
	// coords are meaningless (and this is what makes the first frame's not-yet-placed
	// camera harmless).
	if (z < 150)
	{
		current->primptr = base;	// give the quad back
		return;
	}

	gte_ldv3(&verts[1], &verts[2], &verts[3]);
	gte_rtpt();
	gte_stsxy3(&poly->x1, &poly->x2, &poly->x3);

	// Trace the projected rectangle. Off unless JERICHO_TEX_DIAG=1. This is how the
	// draw recipe was validated: a 4-quadrant test icon (textures/icons/_uvtest.tga)
	// rendered with the right colour in each corner proves the texture mapping, the
	// geometry and the orientation in one shot — which a photographic icon cannot,
	// because a low-contrast image correlates just as well with a random scene patch.
	if (gJerTexDiag < 0)
	{
		const char* d = getenv("JERICHO_TEX_DIAG");

		gJerTexDiag = (d != NULL && *d != '0') ? 1 : 0;
	}

	if (gJerTexDiag != 0 && gJerTexDiagDrawn < 400)
	{
		gJerTexDiagDrawn++;
		jer_log("jer_texture diag: n=%d z=%d tex=%dx%d scr=(%d,%d)(%d,%d)(%d,%d)(%d,%d)\n",
			gJerTexDiagDrawn, z, s->width, s->height,
			(short)poly->x0, (short)poly->y0, (short)poly->x1, (short)poly->y1,
			(short)poly->x2, (short)poly->y2, (short)poly->x3, (short)poly->y3);
	}

	if (mirror)
	{
		poly->u0 = s->u1; poly->v0 = s->v1;
		poly->u1 = s->u0; poly->v1 = s->v0;
		poly->u2 = s->u3; poly->v2 = s->v3;
		poly->u3 = s->u2; poly->v3 = s->v2;
	}
	else
	{
		poly->u0 = s->u0; poly->v0 = s->v0;
		poly->u1 = s->u1; poly->v1 = s->v1;
		poly->u2 = s->u2; poly->v2 = s->v2;
		poly->u3 = s->u3; poly->v3 = s->v3;
	}

	poly->tpage = (s->target == JER_TEX_TARGET_PAGE) ? s->tpage : 0;
	poly->clut = (s->target == JER_TEX_TARGET_PAGE) ? s->clut : 0;

	// OT bucket: >>3 is the engine's world-prim convention AND the only safe shift
	// (SZ saturates at 0xFFFF, and 0xFFFF >> 3 == OTSIZE-1).
	z = noOcclude ? 0 : (z >> 3);

	if (s->target == JER_TEX_TARGET_PAGE)
	{
		// A real VRAM page: the poly is self-describing, no override to manage.
		addPrim(current->ot + z, poly);
		current->primptr = base + sizeof(POLY_FT4);
		return;
	}

	{
		DR_PSYX_TEX* tex = (DR_PSYX_TEX*)(base + sizeof(POLY_FT4));
		DR_PSYX_TEX* rst = (DR_PSYX_TEX*)(base + sizeof(POLY_FT4) + sizeof(DR_PSYX_TEX));

		SetPsyXTexture(tex, s->glTexture, 255, 255);
		SetPsyXTexture(rst, 0, 0, 0);

		// ADD in reverse parse order: addPrim prepends, so the chain ends up
		// [override][quad][reset] and the override covers exactly one quad.
		addPrim(current->ot + z, rst);
		addPrim(current->ot + z, poly);
		addPrim(current->ot + z, tex);

		current->primptr = base + sizeof(POLY_FT4) + 2 * sizeof(DR_PSYX_TEX);
	}
}

// Does the card face away from the camera? `camrel` is the card's world-relative
// position (before Apply_Inv_CameraMatrix), `yaw` its spin. Used to auto-mirror the
// icon so it does not read backwards when the card shows its back. (PsyX does not
// backface-cull, so a single quad is already visible from both sides.)
static int jerTexFacingAway(const VECTOR* camrel, int yaw)
{
	// the card's front normal after a Y rotation by `yaw`, in the same fixed-point
	// space RCOS/RSIN produce
	int nx = (int)(((long long)RSIN(yaw) * 4096) >> 12);
	int nz = (int)(((long long)RCOS(yaw) * 4096) >> 12);

	long long dot = (long long)camrel->vx * nx + (long long)camrel->vz * nz;

	return dot < 0;
}

static void jerTexDraw(JER_TEX_SLOT* s, int x, int y, int z,
		       int halfW, int halfH, int yaw, int flags, int flat)
{
	VECTOR camrel, probe;
	MATRIX ss, work;
	SVECTOR v[4];
	int mirror, noOcclude;

	if (!gJerTextureRenderPass || s == NULL)
		return;

	// raw world frame -> render space (Y negated), then camera-relative
	probe.vx = x - camera_position.vx;
	probe.vy = -y - camera_position.vy;
	probe.vz = z - camera_position.vz;

	camrel = probe;

	Apply_Inv_CameraMatrix(&camrel);
	gte_SetTransVector(&camrel);

	if ((flags & JER_TEX_DRAW_BILLBOARD) != 0)
	{
		gte_SetRotMatrix(&face_camera);
	}
	else
	{
		// memset first: a rotation matrix only names 5 of its 9 entries, and the rest
		// must be zero or the quad projects to garbage.
		memset(&ss, 0, sizeof(ss));
		ss.m[1][1] = ONE;
		ss.m[0][0] = RCOS(yaw);
		ss.m[2][0] = RSIN(yaw);
		ss.m[0][2] = -ss.m[2][0];
		ss.m[2][2] = ss.m[0][0];

		MulMatrix0(&inv_camera_matrix, &ss, &work);
		gte_SetRotMatrix(&work);
	}

	// the local rectangle. A "card" stands upright (X/Y); a "flat" decal lies in the
	// horizontal plane (X/Z). Both in the FT4 order TL, TR, BL, BR.
	if (flat)
	{
		v[0].vx = -halfW; v[0].vy = 0; v[0].vz = -halfH;
		v[1].vx =  halfW; v[1].vy = 0; v[1].vz = -halfH;
		v[2].vx = -halfW; v[2].vy = 0; v[2].vz =  halfH;
		v[3].vx =  halfW; v[3].vy = 0; v[3].vz =  halfH;
	}
	else
	{
		v[0].vx = -halfW; v[0].vy = -halfH; v[0].vz = 0;
		v[1].vx =  halfW; v[1].vy = -halfH; v[1].vz = 0;
		v[2].vx = -halfW; v[2].vy =  halfH; v[2].vz = 0;
		v[3].vx =  halfW; v[3].vy =  halfH; v[3].vz = 0;
	}

	if ((flags & JER_TEX_DRAW_MIRROR) != 0)
		mirror = 1;
	else if ((flags & JER_TEX_DRAW_BILLBOARD) != 0)
		mirror = 0;			// a billboard always faces us
	else
		mirror = jerTexFacingAway(&probe, yaw);

	noOcclude = (flags & JER_TEX_DRAW_NO_OCCLUDE) != 0;

	jerTexEmitQuad(s, v, mirror, noOcclude);
}

void jer_texture_draw_card(JER_TEXTURE tex, int x, int y, int z,
			   int halfW, int halfH, int spin, int flags)
{
	jerTexDraw(jerTexSlot(tex, NULL), x, y, z, halfW, halfH, spin, flags, 0);
}

void jer_texture_draw_flat(JER_TEXTURE tex, int x, int y, int z,
			   int halfW, int halfL, int yaw, int flags)
{
	jerTexDraw(jerTexSlot(tex, NULL), x, y, z, halfW, halfL, yaw, flags, 1);
}
