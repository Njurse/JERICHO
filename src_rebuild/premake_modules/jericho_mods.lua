-- premake_modules/jericho_mods.lua
--
-- Shared JERICHO module discovery for both build pipelines:
--   * premake5.lua        — the game build: scans nothing, modules are NOT
--                           compiled in anymore (they load at runtime).
--   * premake5_mods.lua   — the mods-only build: emits one DLL project per
--                           installed module, never touches the game exe.
--
-- A folder under JERICHO/MODS counts as a module when it carries a
-- mod.toml; the folder name becomes the module id and the entry symbol
-- (jer_module_<id>_entry).

-- Scan JERICHO/MODS for module folders. Returns a sorted list of ids.
function jericho_scan_mods()
	local mods = {}
	local dirs = os.matchdirs("../JERICHO/MODS/*")

	table.sort(dirs)	-- stable, deterministic order

	for _, d in ipairs(dirs) do
		local id = d:gsub("[/\\]+$", ""):match("[^/\\]+$")

		if id and os.isfile("../JERICHO/MODS/" .. id .. "/mod.toml") then
			if id:match("^[A-Za-z_][A-Za-z0-9_]*$") then
				table.insert(mods, id)
			else
				print("** JERICHO: skipping '" .. id .. "' — module ids must match [A-Za-z_][A-Za-z0-9_]* (the folder name becomes the C entry symbol)")
			end
		end
	end

	return mods
end
