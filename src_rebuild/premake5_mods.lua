-- premake5_mods.lua — JERICHO mods-only build.
--
-- Generates one DLL project per installed module (auto-scanned from
-- JERICHO/MODS via premake_modules/jericho_mods.lua) into a separate
-- solution (build_mods/REDRIVER2_MODS.sln) that NEVER touches the game
-- exe. Each module links against the game's import library
-- (src_rebuild/bin/<cfg>/REDRIVER2.lib — generated once when the exe is
-- built with /DEF:exports.def) so it can call game functions and read
-- game globals directly.
--
-- Usage (from src_rebuild/):
--     premake5.exe --file=premake5_mods.lua vs2019
--     msbuild build_mods/REDRIVER2_MODS.sln /p:Configuration=Release /p:Platform=x64
--
-- DLLs land in JERICHO/MODS/<id>/<id>.dll, where the runtime loader
-- (jer_loader.c) picks them up — no exe rebuild, no mod list anywhere.

require "premake_modules/jericho_mods"

-- resolve like premake5.lua does (kept minimal — only what mods need)
SDL2_DIR = os.getenv("SDL2_DIR") or "dependencies/SDL2-2.30.2"
OPENAL_DIR = os.getenv("OPENAL_DIR") or "dependencies/openal-soft-1.23.1-bin"
JPEG_DIR = os.getenv("JPEG_DIR") or "dependencies/jpeg-9d"
GAME_REGION = os.getenv("GAME_REGION") or "NTSC_VERSION"

local ALL_MODS = jericho_scan_mods()

-- Only API addons are built here: a mod.toml declaring runtime = "dll".
-- Deep mods (no such declaration) are compiled into the game by
-- premake5.lua — MSVC cannot import game data globals into a DLL, so
-- those must stay linked in. See premake_modules/jericho_mods.lua.
local MODS = {}
for _, id in ipairs(ALL_MODS) do
	local toml = io.open("../JERICHO/MODS/" .. id .. "/mod.toml", "r")
	local isAddon = false

	if toml then
		local line = toml:read("*l")

		while line do
			if line:match("^%s*runtime%s*=%s*\"dll\"") then
				isAddon = true
				break
			end

			line = toml:read("*l")
		end

		toml:close()
	end

	if isAddon then
		table.insert(MODS, id)
	else
		print("** JERICHO: " .. id .. " is a compiled-in mod — skipped by the DLL build (add runtime = \"dll\" to its mod.toml to make it a DLL addon)")
	end
end

workspace "REDRIVER2_MODS"
	location "build_mods"
	configurations { "Release" }
	platforms { "x64" }

	-- the game exe export surface (bin/<cfg>/REDRIVER2.lib) is built in the
	-- same configuration family; Release is the canonical mod target
	configuration "Release"
		defines { "NDEBUG", "JERICHO_MODULE_BUILD" }
		optimize "Speed"
		symbols "On"	-- debuggable mods

for _, JER_MOD in ipairs(MODS) do
	filter {}

	project ("mod_" .. JER_MOD)
		kind "SharedLib"
		language "c++"

		-- include the JERICHO SDK + game headers + the mod's own folder
		includedirs {
			"Game",
			"Game/C",
			"Game/C/JERICHO/include",
			"../JERICHO/MODS/" .. JER_MOD,
			SDL2_DIR .. "/include",
			OPENAL_DIR .. "/include",
			JPEG_DIR .. "/",
			"PsyCross/include",
			"PsyCross/include/psx",
		}

		files {
			("../JERICHO/MODS/" .. JER_MOD .. "/**.c"),
			("../JERICHO/MODS/" .. JER_MOD .. "/**.h"),
		}

		defines {
			GAME_REGION,
			"BUILD_CONFIGURATION_STRING=\"%{cfg.buildcfg}\"",
			"JERICHO_MODULE_BUILD",
		}

		-- link against the game exe's exported symbols
		libdirs { "bin/%{cfg.buildcfg}" }
		links { "REDRIVER2" }

		-- output next to mod.toml so the runtime loader finds <id>.dll
		-- (paths here are relative to the premake script dir, src_rebuild)
		targetdir ("../JERICHO/MODS/" .. JER_MOD)
		targetname (JER_MOD)

		filter "system:Windows"
			disablewarnings { "4996", "4244", "4018", "4267", "4101", "4013" }

		filter "system:linux"
			buildoptions { "-Wno-unused-parameter", "-Wno-unused-variable" }

		-- the game compiles all C as C++ (headers rely on C++ struct-tag
		-- rules); modules must match
		filter { "files:**.c", "files:**.C" }
			compileas "C++"
end

filter {}
