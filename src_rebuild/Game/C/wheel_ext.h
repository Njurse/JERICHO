#ifndef WHEEL_EXT_H
#define WHEEL_EXT_H

/* LUMP_CAR_WHEELS (type 200) per-car trailer. The v1 prefix is unchanged:
 * int32 count (1..3), then count int32 offsets, then the MODEL blocks those
 * offsets name. Offsets are from the first byte of the segment body; -1 means
 * that v1 kind is absent.
 *
 * The v1 payload ends at the highest byte any of those MODEL blocks occupies,
 * padded with 0x00 up to a multiple of 4. The trailer starts on that byte N:
 *
 *   N+0   4 bytes  ASCII 'WHLX' (57 48 4C 58)
 *   N+4   int32    version = 1
 *   N+8   int32    slot_count = 13
 *   N+12  int32[13][3]  slot-major, index (slot * 3 + kind)
 *                      kind is clean=0, damaged=1, fast=2
 *                      -1 = use the v1 offset for that kind
 *
 * Extra MODEL blocks, if any, follow the table (N+168). An offset may instead
 * point back at a v1 block. All integers are little-endian.
 *
 * This header only reads the trailer. The MODEL checks (8..24 polys, first
 * eight FT4, texture_set) stay with the caller, which rejects the whole
 * trailer if any named model fails them.
 */

#define WHEEL_EXT_SLOTS		13
#define WHEEL_EXT_KINDS		3
#define WHEEL_EXT_VERSION	1

/* Byte where the trailer would start, or -1 when the v1 count header itself
 * is unusable. modelEnd returns the first byte past the MODEL at off, or off
 * when that offset cannot be walked. NULL modelEnd only measures the offset
 * table. */
typedef int (*WheelExtModelEndFn)(const unsigned char* body, int size, int off, void* user);

static int WheelExtI32(const unsigned char* p)
{
	return (int)((unsigned int)p[0] | ((unsigned int)p[1] << 8) |
		((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24));
}

static int WheelExtV1End(const unsigned char* body, int size, WheelExtModelEndFn modelEnd, void* user)
{
	int count, end, i;

	if (body == NULL || size < 8)
		return -1;

	count = WheelExtI32(body);

	if (count < 1 || count > WHEEL_EXT_KINDS)
		return -1;

	if (4 + count * 4 > size)
		return -1;

	end = 4 + count * 4;

	for (i = 0; i < count; i++)
	{
		int off = WheelExtI32(body + 4 + i * 4);
		int m;

		if (off < 0 || modelEnd == NULL)
			continue;

		m = modelEnd(body, size, off, user);

		if (m > end)
			end = m;
	}

	return (end + 3) & ~3;
}

/*  1  trailer is well-formed; offsets[slot * 3 + kind] is filled
 *  0  no trailer (magic absent, or not enough bytes to hold one)
 * -1  magic is present but the trailer is not usable. *why is a short reason.
 * An offset of -1 is legal. Any other offset must satisfy 0 <= off < size;
 * the caller still has to validate the MODEL there. */
static int WheelExtRead(const unsigned char* body, int size, int v1End, int* offsets, const char** why)
{
	int version, slots, i;
	int tmp[WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS];
	int table;

	if (why != NULL)
		*why = NULL;

	if (body == NULL || v1End < 0 || size < 0 || v1End > size || v1End + 4 > size)
		return 0;

	if (body[v1End] != 0x57 || body[v1End + 1] != 0x48 ||
		body[v1End + 2] != 0x4C || body[v1End + 3] != 0x58)
		return 0;

	if (v1End + 12 + WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS * 4 > size)
	{
		if (why != NULL)
			*why = "offset table runs past the segment";

		return -1;
	}

	version = WheelExtI32(body + v1End + 4);
	slots = WheelExtI32(body + v1End + 8);

	if (version != WHEEL_EXT_VERSION || slots != WHEEL_EXT_SLOTS)
	{
		if (why != NULL)
			*why = "version or slot count is not 1 x 13";

		return -1;
	}

	table = v1End + 12;

	for (i = 0; i < WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS; i++)
	{
		int off = WheelExtI32(body + table + i * 4);

		if (off != -1 && (off < 0 || off >= size))
		{
			if (why != NULL)
				*why = "an offset is outside the segment";

			return -1;
		}

		tmp[i] = off;
	}

	if (offsets != NULL)
	{
		for (i = 0; i < WHEEL_EXT_SLOTS * WHEEL_EXT_KINDS; i++)
			offsets[i] = tmp[i];
	}

	return 1;
}

#endif
