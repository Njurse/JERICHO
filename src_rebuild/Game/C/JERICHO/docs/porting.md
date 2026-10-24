# Porting JERICHO to a new platform

JERICHO is deliberately split so that porting it is small. There are exactly
**three** things a new operating system has to provide, and they live in three
places:

| Concern | Where | What a port does |
|---|---|---|
| **Runtime host layer** — load a module binary, scan a folder, run a child process, find the exe | `src_rebuild/Game/C/JERICHO/src/jer_host.{h,c}` | add one back-end block |
| **Build back-end** — generate project files, invoke the compiler, name the addon artifact | `JERICHO/build.py` | add one `Backend` subclass |
| **Who may compile in-game** — the Mods-menu row and the mod-store rebuild prompt | `src_rebuild/Game/Frontend/FEmain.c` (`JERICHO_FE_HAS_COMPILE_MODS`) | define it for the platform's dev builds |

Everything on the **module loading and Compile Mods path** — the loader, the
module manager, the compile driver — is written against those interfaces and
has **no platform branches**. In particular, `jer_loader.c` and `jer_compile.c`
include no platform SDK header, and neither sees a DLL handle, a `DIR*` or a
PID. (The mod store, the updater and the rebake helper have their own small
Win32/POSIX pairs; they are independent of this layer.)

## 1. The runtime layer: `jer_host.h`

`jer_host.h` is the whole contract. The loader (`jer_loader.c`) and the compile
driver (`jer_compile.c`) call only these:

```c
const char* jer_host_name(void);                 /* "win32" / "posix" / ... */

int   jer_host_lib_variant(const char* id, int variant, char* out, int max);
void* jer_host_lib_open(const char* path);
void* jer_host_lib_sym(void* handle, const char* sym);
void  jer_host_lib_close(void* handle);
const char* jer_host_last_error(void);

typedef void (*JER_HOST_DIR_FN)(const char* name, int isDir, void* user);
int   jer_host_dir_scan(const char* path, JER_HOST_DIR_FN cb, void* user);

typedef struct JER_HOST_PROC JER_HOST_PROC;
JER_HOST_PROC* jer_host_spawn(const char* cmdline, const char* logPath);
int   jer_host_proc_poll(JER_HOST_PROC* proc, int* exitCode);
void  jer_host_proc_free(JER_HOST_PROC* proc);
void  jer_host_sleep_ms(int ms);

int   jer_host_exe_path(char* out, int max);
int   jer_host_exe_locks_while_running(void);
const char* jer_host_script_ext(void);
int   jer_host_file_exists(const char* path);
long long jer_host_file_mtime(const char* path);
```

Three of these carry the only real platform *semantics*:

- **`jer_host_lib_variant`** — the on-disk names a compiled addon may have,
  walked in order (variant 0, 1, …). Windows has one: `<id>.dll`. Linux has
  two: `<id>.so`, then `lib<id>.so`. A new OS lists whatever its toolchain
  emits, and the loader tries each without knowing where it is.
- **`jer_host_exe_locks_while_running`** — `1` when the linker cannot overwrite
  the *running* executable, so the deep-mod build must move it aside first
  (Windows). `0` when the linker relinks it in place (POSIX). This is the one
  behaviour the compile driver branches on, through this capability rather than
  an `#if`.
- **`jer_host_script_ext`** — `bat` on Windows, `sh` elsewhere. The compile
  driver builds `JERICHO/build_game.<ext>` and `JERICHO/build_restart.<ext>`
  from it, so the same C code drives either shim.

`jer_host_spawn` runs a command line through the platform shell (`cmd.exe /c`,
`sh -c`), appending stdout+stderr to a log and giving it empty stdin, and
polls non-blockingly — so a build never freezes the game loop.

### Adding a runtime back-end

`jer_host.c` selects its block with:

```c
#if defined(_WIN32)              → win32
#elif defined(__EMSCRIPTEN__) || defined(__ANDROID__)  → stub
#elif defined(__APPLE__)         → mac
#elif defined(__unix__)          → posix
#else                            → stub
#endif
```

A new OS is a new `#elif` plus a block implementing the functions above. The
`stub` block is the template: every call fails cleanly (no runtime module
loading), which is correct for platforms with no dynamic linker.

## 2. The build back-end: `build.py`

`JERICHO/build.py` generates project files and drives the compiler. Porting the
build is a `Backend` subclass — see
[build.md](build.md#backends--and-porting-to-a-new-platform) for the exact
methods. The driver exits with that template when the OS is unknown, so a port
cannot be silently skipped.

A new platform's back-end must also answer the question the Windows back-end
answers with `exports.def`: **how does the host executable expose its symbols
to a loadable addon?** Windows needs an import library (`/DEF:exports.def`);
Linux exports them from the exe itself (`-Wl,--export-dynamic`). Whatever the
mechanism, it lives in the back-end, not in engine code.

Two platform spellings matter in the back-end:

- The exe's **make target** is the premake project name (`JERICHO`), while its
  **file name** depends on the configuration (`jericho`, `JERICHO_dev`).
- Runtime addons are built with `targetprefix ""` so the loader's `<id>.so`
  name is exact.

## 3. In-game compiling

A platform that ships the toolchain should define
`JERICHO_FE_HAS_COMPILE_MODS` for its **dev** builds (not Release). That is
what makes `jer_mods_compile_available()` true, which in turn shows the Mods
menu's **Compile Mods** row and the mod-store prompt after a compiled-in mod is
installed. Linux dev builds define it alongside Windows now.

The detached helper is `JERICHO/build_restart.bat` on Windows and
`build_restart.sh` on POSIX (selected by `jer_host_script_ext`). It waits for
the game's PID, rebuilds the runtime addons and the deep modules, relinks or
replaces the exe, and starts it again. On POSIX the linker may overwrite the
running image, so the helper does not rename it aside; Windows still does.

### Where an installed mod is compiled from

The game's root is `<exeDir>/JERICHO`, so the Mod Repo installs a downloaded
mod into `<exeDir>/JERICHO/MODS/<id>`. The repo's build scans
`src_rebuild/../JERICHO/MODS`, so the driver passes the runtime folder to
premake as an extra root (`JERICHO_EXTRA_MODS`, set by `--runtime`). A port
must keep that hand-off, or a mod installed at run time will be invisible to
the rebuild.

## 4. The mod-list pipeline (unchanged)

Nothing here is platform-specific — it is worth stating so a porter does not
look for it:

- `premake5.lua` auto-scans `JERICHO/MODS` (plus the runtime root above) and
  compiles each deep mod into the exe as a `mod_<id>` static library.
- `premake5_mods.lua` builds each `runtime = "dll"` addon as a shared library.
- `jer_loader.c` finds `<root>/MODS/<id>/` via `jer_host_dir_scan`, loads
  `<id>.dll` / `<id>.so` via `jer_host_lib_open`, and resolves
  `jer_module_<id>_entry` via `jer_host_lib_sym`.

## See also

- [build.md](build.md) — the driver, its subcommands and back-ends.
- [README.md](README.md) — the platform matrix and the mod model.
- `jer_host.h` — the contract itself, with the port rules in its header.
