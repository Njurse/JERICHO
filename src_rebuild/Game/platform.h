#ifndef PLATFORM_H
#define PLATFORM_H

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

#include "psyx_compat.h"

#ifndef PSX
#include "../utils/fs.h"
#endif

#if defined(__GNUC__) && !defined(_WIN32)
/* _stricmp is MSVC's case-insensitive compare; on POSIX it would be strcasecmp.
 * Do NOT reach for <strings.h> to get that: this include path SHADOWS it with
 * PsyCross/include/psx/strings.h, a PSX shim that includes <string.h> and
 * <ctype.h> and declares neither, so the include silently declares nothing and
 * the build fails with "strcasecmp was not declared in this scope". Compare
 * here instead - no header needed, and no libc declaration to disagree with. */
static inline int _stricmp(const char* s1, const char* s2)
{
	for (;;)
	{
		unsigned char a = (unsigned char)*s1++;
		unsigned char b = (unsigned char)*s2++;

		if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');

		if (a != b || a == 0)
			return (int)a - (int)b;
	}
}
#endif

#endif // PLATFORM_H