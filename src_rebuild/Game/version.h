#ifndef GAME_VERSION_N
#ifdef JERICHO_BUILD_VERSION
/* JERICHO's own release number, from git describe at build time (premake5.lua).
 * The "8.0" this used to say is the REDRIVER2 engine the fork began from, not an
 * application version - reporting it as one is how a log gets read as the wrong
 * build. JERICHO_BUILD_VERSION is defined for the exe, the runtime and every
 * module, so every one of them reports the same number. */
#define GAME_VERSION_N		JERICHO_BUILD_VERSION
#else
#define GAME_VERSION_N		"0.0.0"
#endif
#endif

#ifndef GAME_VERSION_RES
#define GAME_VERSION_RES	8,0
#endif

#define GAME_TITLE			"JERICHO"
#define GAME_VERSION		GAME_TITLE " " GAME_VERSION_N
