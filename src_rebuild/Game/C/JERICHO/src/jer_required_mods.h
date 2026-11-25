/*
 * jer_required_mods.h -- the mods the game cannot build without.
 *
 * mp, carhacks, crumple and bake are their own repositories (JERICHO/MODS/mods.json,
 * fetched by tools/fetch_mods.py). A mod that was never fetched leaves no folder, jericho_scan_mods() (premake) then returns
 * fewer ids, and the build SUCCEEDS with a registry, a mirrored JERICHO tree
 * and an exe that quietly lack a chunk of the game. Nothing in the build
 * complains, and the first symptom is a missing feature at runtime.
 *
 * premake5.lua stops that at generation time. This header repeats the check at
 * COMPILE time, so an IDE or a plain msbuild run that never regenerates the
 * project files still cannot produce a smaller game.
 *
 * Deliberately NOT listed: gaildrv2 (removed from this superproject; hibernated).
 * bake (formerly d1cars) used to be exempt too, but it is a release mod and the
 * core suite refuses store addons without it, so it is required now.
 *
 * The paths are relative to this file (src/), five levels below the repo root:
 * src -> JERICHO -> C -> Game -> src_rebuild -> <root>.
 */
#ifndef JERICHO_REQUIRED_MODS_H
#define JERICHO_REQUIRED_MODS_H

#if defined(__has_include)
#	if !__has_include("../../../../../JERICHO/MODS/mp/mod.toml")
#		error "mp is its own repository and was not fetched; run: python tools/fetch_mods.py mp"
#	endif

#	if !__has_include("../../../../../JERICHO/MODS/carhacks/mod.toml")
#		error "carhacks is its own repository and was not fetched; run: python tools/fetch_mods.py carhacks"
#	endif

#	if !__has_include("../../../../../JERICHO/MODS/crumple/mod.toml")
#		error "crumple is its own repository and was not fetched; run: python tools/fetch_mods.py crumple"
#	endif

#	if !__has_include("../../../../../JERICHO/MODS/bake/mod.toml")
#		error "bake is its own repository and was not fetched; run: python tools/fetch_mods.py bake"
#	endif
#endif

#endif /* JERICHO_REQUIRED_MODS_H */
