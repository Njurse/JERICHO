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

#ifdef __GNUC__
#ifndef _WIN32
/* strcasecmp is POSIX, not C: <string.h> does not declare it, <strings.h>
 * does. The mapping below therefore needs this include, or every TU that
 * includes this header and uses _stricmp fails to build on Linux. */
#include <strings.h>
#endif
#define _stricmp(s1, s2) strcasecmp(s1, s2)
#endif

#endif // PLATFORM_H