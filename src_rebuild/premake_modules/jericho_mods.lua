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

-- Where the runtime JERICHO/MODS folder is, when the driver says it differs from
-- the repo's. In a dev tree the game runs from bin/<cfg>/, so the mod store
-- installs a downloaded mod THERE; JERICHO/build.py passes that folder here.
function jericho_extra_mods_dir()
	local d = os.getenv("JERICHO_EXTRA_MODS")

	if d == nil or d == "" then
		return nil
	end

	d = d:gsub("\\", "/"):gsub("/+$", "")
	return d
end

-- The folder a module's sources live in. The repo copy wins when both exist
-- (it is the source of truth while developing); otherwise the runtime copy the
-- store installed is used.
function jericho_mod_dir(id)
	local extra = jericho_extra_mods_dir()

	if extra ~= nil then
		local p = extra .. "/" .. id

		if os.isfile(p .. "/mod.toml") and not os.isfile("../JERICHO/MODS/" .. id .. "/mod.toml") then
			return p
		end
	end

	return "../JERICHO/MODS/" .. id
end

-- Scan the repo's JERICHO/MODS and, when set, the runtime JERICHO_EXTRA_MODS.
-- Returns a sorted list of ids (a repo id shadows the same id in the extra
-- root, so a module is never declared twice).
function jericho_scan_mods()
	local mods = {}
	local seen = {}

	local function add_root(root)
		local dirs = os.matchdirs(root .. "/*")

		table.sort(dirs)	-- stable, deterministic order

		for _, d in ipairs(dirs) do
			local id = d:gsub("[/\\]+$", ""):match("[^/\\]+$")

			if id and os.isfile(root .. "/" .. id .. "/mod.toml") then
				if id:match("^[A-Za-z_][A-Za-z0-9_]*$") then
					if not seen[id] then
						seen[id] = true
						table.insert(mods, id)
					end
				else
					print("** JERICHO: skipping '" .. id .. "' — module ids must match [A-Za-z_][A-Za-z0-9_]* (the folder name becomes the C entry symbol)")
				end
			end
		end
	end

	add_root("../JERICHO/MODS")

	local extra = jericho_extra_mods_dir()

	if extra ~= nil then
		add_root(extra)
	end

	return mods
end
