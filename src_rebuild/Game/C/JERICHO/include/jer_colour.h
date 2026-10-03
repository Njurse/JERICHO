#ifndef JER_COLOUR_H
#define JER_COLOUR_H

/* jer_colour — JERICHO's canonical colour.
 *
 * One representation for "a colour" so a module never has to re-derive the
 * engine's two packings or remember which byte holds red. A JER_COLOUR is plain
 * 8-bit RGB, 0..255, in red/green/blue order:
 *
 *     JER_COLOUR c = jer_colour_make(255, 128, 0);   // each channel clamped
 *
 * The engine stores colours two ways, and they are NOT the same:
 *
 *     polygon / GTE word   =  B<<16 | G<<8 | R          (RED is the LOW byte)
 *     CLUT entry (16bpp)   =  stp | b<<10 | g<<5 | r    (5 bits per channel)
 *
 * jer_colour_pack_word / _pack_clut produce each and the _unpack_* read one
 * back, so the byte order lives HERE and nowhere else. Red in the high byte
 * silently swaps red and blue -- the bug this exists to stop (see
 * JERICHO-colour-byte-order). Channels are held as int so a tint or a lerp does
 * not truncate; the pack helpers narrow explicitly.
 *
 * This is plain data + pure helpers: no engine state, no side effects. The car
 * path (jer_car_palette.h) and the ped/suit path (jer_ped_palette.h) can both
 * speak it.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JER_COLOUR
{
	int r, g, b;	/* 0..255 */
} JER_COLOUR;

/* One channel clamped to 0..255. */
static inline int jer_colour_clamp1(int v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return v;
}

/* A colour with every channel clamped to 0..255. */
static inline JER_COLOUR jer_colour_make(int r, int g, int b)
{
	JER_COLOUR c;
	c.r = jer_colour_clamp1(r);
	c.g = jer_colour_clamp1(g);
	c.b = jer_colour_clamp1(b);
	return c;
}

static inline int jer_colour_eq(JER_COLOUR a, JER_COLOUR b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* Polygon / GTE colour word: B<<16 | G<<8 | R (red is the LOW byte). */
static inline unsigned int jer_colour_pack_word(JER_COLOUR c)
{
	return ((unsigned int)jer_colour_clamp1(c.b) << 16)
	     | ((unsigned int)jer_colour_clamp1(c.g) << 8)
	     |  (unsigned int)jer_colour_clamp1(c.r);
}

static inline JER_COLOUR jer_colour_unpack_word(unsigned int w)
{
	return jer_colour_make((int)(w & 0xffu), (int)((w >> 8) & 0xffu), (int)((w >> 16) & 0xffu));
}

/* CLUT entry (PSX 16bpp): stp(0) | b<<10 | g<<5 | r, 5 bits per channel. */
static inline unsigned int jer_colour_pack_clut(JER_COLOUR c)
{
	unsigned int r5 = (unsigned int)(jer_colour_clamp1(c.r) * 31 / 255);
	unsigned int g5 = (unsigned int)(jer_colour_clamp1(c.g) * 31 / 255);
	unsigned int b5 = (unsigned int)(jer_colour_clamp1(c.b) * 31 / 255);

	return (b5 << 10) | (g5 << 5) | r5;
}

static inline JER_COLOUR jer_colour_unpack_clut(unsigned int w)
{
	int r5 = (int)(w & 31u);
	int g5 = (int)((w >> 5) & 31u);
	int b5 = (int)((w >> 10) & 31u);

	return jer_colour_make(r5 * 255 / 31, g5 * 255 / 31, b5 * 255 / 31);
}

#ifdef __cplusplus
}
#endif

#endif /* JER_COLOUR_H */
