#include <stddef.h>
char* decomp_asm_n(char* dest, char* src, int srcLen);

// [D] [T]
char* decomp_asm(char* dest, char* src)
{
	return decomp_asm_n(dest, src, -1);
}

// JERICHO: decomp_asm with both ends bounded. The page is 0x8000 bytes filled downward
// from dest+0x7fff; the stock loop only checked that between tokens, so a crafted final
// token wrote up to 129 bytes below dest. A token is now clipped at dest (its source
// bytes are still consumed, so a well-formed stream decodes and returns exactly as
// before). srcLen >= 0 also bounds the input: NULL if the stream ends before the page
// is full (an imported entry cannot read past its own buffer). srcLen < 0 = unbounded.
char* decomp_asm_n(char* dest, char* src, int srcLen)
{
	char* end = (srcLen >= 0) ? src + srcLen : NULL;
	int at = 0x7fff;

	do {
		if (end != NULL && src >= end)
			return NULL;

		char pix = *src++;

		if ((pix & 0x80) != 0)
		{
			if (end != NULL && src >= end)
				return NULL;

			char p = *src++;

			do {
				if (at >= 0)
					dest[at] = p;
				at--;
			} while (pix++ <= 0);
		}
		else
		{
			do {
				if (end != NULL && src >= end)
					return NULL;

				char c = *src++;

				if (at >= 0)
					dest[at] = c;
				at--;
			} while (pix-- != 0);
		}
	} while (at >= 0);

	return src;
}
