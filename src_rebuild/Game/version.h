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

/* JERICHO: the build flavour, spelled out. GAME_VERSION_N is git describe, which
 * already ends in "-dirty" when the tree differed from its commit (CI refreshes the
 * index first, so a clean CI build never does). On top of that, every build that is
 * not the shipping Release -- Release_dev, Debug -- says "-dev", so a screenshot or
 * a window title is never ambiguous about which binary it came from. */
#if defined(JERICHO_RELEASE_MODS) && !defined(_DEBUG)
#define GAME_BUILD_SUFFIX	""
#else
#define GAME_BUILD_SUFFIX	"-dev"
#endif

#define GAME_VERSION_FULL	GAME_VERSION GAME_BUILD_SUFFIX	/* "JERICHO 0.10.4-3-gabc1234[-dirty][-dev]" */
