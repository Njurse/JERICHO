/*
 * jer_required_mods.h -- the mods the game cannot build without.
 *
 * mp, carhacks and crumple are git submodules. A submodule that was never
 * fetched leaves an empty folder, jericho_scan_mods() (premake) then returns
 * fewer ids, and the build SUCCEEDS with a registry, a mirrored JERICHO tree
 * and an exe that quietly lack a chunk of the game. Nothing in the build
 * complains, and the first symptom is a missing feature at runtime.
 *
 * premake5.lua stops that at generation time. This header repeats the check at
 * COMPILE time, so an IDE or a plain msbuild run that never regenerates the
 * project files still cannot produce a smaller game.
 *
 * Deliberately NOT listed: gaildrv2 (its repository is not published, so CI
 * cannot fetch it) and d1cars -- both are allowed to be absent and degrade the
 * way they always have.
 *
 * The paths are relative to this file (src/), five levels below the repo root:
 * src -> JERICHO -> C -> Game -> src_rebuild -> <root>.
 */
#ifndef JERICHO_REQUIRED_MODS_H
#define JERICHO_REQUIRED_MODS_H

#if defined(__has_include)
#	if !__has_include("../../../../../JERICHO/MODS/mp/mod.toml")
#		error "mp is a submodule and was not fetched; run: git submodule update --init JERICHO/MODS/mp"
#	endif

#	if !__has_include("../../../../../JERICHO/MODS/carhacks/mod.toml")
#		error "carhacks is a submodule and was not fetched; run: git submodule update --init JERICHO/MODS/carhacks"
#	endif

#	if !__has_include("../../../../../JERICHO/MODS/crumple/mod.toml")
#		error "crumple is a submodule and was not fetched; run: git submodule update --init JERICHO/MODS/crumple"
#	endif
#endif

#endif /* JERICHO_REQUIRED_MODS_H */
