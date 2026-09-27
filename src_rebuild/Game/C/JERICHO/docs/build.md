# Building JERICHO — the cross-platform driver

Every JERICHO build step goes through one script, **`JERICHO/build.py`**.
Windows and Linux run the *same* driver; each OS supplies a **backend** that
knows how to generate project files and call the compiler. The in-game
**Compile Mods** flow and the developer entry points are thin shims over it, so
the build logic lives in exactly one place.

```
JERICHO/build.py                       the driver (Python 3, stdlib only)
JERICHO/build_game.bat / build_game.sh  shims: build / rebuild a deep mod, or the game
JERICHO/build_mods.bat / build_mods.sh  shims: build every runtime addon (DLL / .so)
```

The old `.bat` files were the whole toolchain (vswhere + MSBuild + premake
invocations baked in). They are now **shims**: they find Python, forward their
arguments to the driver, and pass its exit code back.

## Running it

```
python3 JERICHO/build.py <target> [step] [id] [options]
```

| Target / step | What it does |
|---|---|
| `game premake` | generate the game project files (the `mod_<id>` static libs are auto-scanned from `JERICHO/MODS`) |
| `game mod <id>` | build one compiled-in (deep) module |
| `game exe` | link the game executable |
| `game exports` | **Windows only** — two-pass `exports.def` (empty def → link for the `.map` → `gen_exports` → relink) |
| `game` / `game all` | `premake` + every deep mod + the exe |
| `mods` | build every `runtime = "dll"` addon and mirror it next to the exe |
| `sdk [<id>]` | build one addon against the standalone SDK (`JERICHO/sdk`) |

Options: `--src DIR` (default: auto-detected), `--config NAME`
(`Release` | `Release_dev` | `Debug`, and the `*_x64` spellings),
`--jobs N`, `--dry-run` (print the commands, run nothing).

`--dry-run` is the safe way to see what a step would do on an unfamiliar
machine — it resolves the whole command line and prints it.

### The shims keep their old interface

The in-game runtime (`jer_compile.c`) and existing shortcuts call
`build_game.bat <src_rebuild_dir> <config> <step> [id]`. That signature is
unchanged; the shim forwards it to `build.py`. When `<src_rebuild_dir>` is
empty the driver auto-detects the tree, so the same shims work whether they sit
in the repo (`JERICHO/`) or beside the exe (`bin/<cfg>/JERICHO/`).

## Backends — and porting to a new platform

`build.py` picks a backend from `os.name` / `sys.platform`. Two ship today:

| OS | Project files | Compiler | Addon binary | Symbol export |
|---|---|---|---|---|
| Windows | `premake5 vs2019` | MSBuild | `<id>.dll` | `exports.def` (`/DEF:`) |
| Linux | `premake5 gmake2` | `make config=<cfg>_x64` | `<id>.so` | `-Wl,--export-dynamic` |

To port, subclass `Backend`, implement its small method set, and register the
class — nothing else in the tree changes:

```python
@register_backend
class MyBackend(Backend):
    key = "myos"
    lib_ext = ".dylib"
    def premake_action(self):        return "..."
    def premake(self, ctx, mods_only=False):  ...
    def build_deep_mod(self, ctx, mod_id):    ...
    def build_exe(self, ctx):                 ...
    def build_addons(self, ctx):              ...
    def build_sdk_addon(self, ctx, include, sdk_dir, src_c, out): ...
```

If the running OS has no backend, the driver **exits with the template above**
rather than silently doing nothing. The two platform-coupled decisions a port
must make are exactly these:

1. **How to generate project files and invoke the compiler** (the backend
   methods above).
2. **How the host exe exposes its symbols to a loadable addon** — the
   `build_exports` step is Windows-only because MSVC needs an import library;
   every other OS either exports its symbols from the exe (Linux:
   `--export-dynamic`) or the platform's loader resolves them another way.

The runtime side of (2) is the loader in `jer_loader.c` (see
[README.md](README.md) for the platform matrix); the *build* side is this file.

## How the pieces fit

- **Deep mods** (compiled into the exe): `premake5.lua` auto-scans `JERICHO/MODS`
  for folders whose `mod.toml` does **not** say `runtime = "dll"` and emits one
  `mod_<id>` static library each, linked into the game. `game mod <id>` builds
  one of those; `game exe` relinks.
- **Runtime addons** (loaded, never linked into the exe): `premake5_mods.lua`
  builds one shared library per `runtime = "dll"` mod; `mods` builds them and
  mirrors the binaries into the runtime `JERICHO/MODS`.
- **No mod list anywhere.** Both pipelines auto-scan, so installing a mod is
  dropping a folder.

## Linux specifics

The Linux backend is `premake5 gmake2` + `make`. The one thing that differs
from Windows throughout the JERICHO build is **how an addon reaches the game's
symbols**:

- **Windows** builds an *import library* for the exe (`REDRIVER2.lib`, from
  `/DEF:exports.def`) and every addon links against it.
- **Linux has no import library.** The exe is linked with
  `-Wl,--export-dynamic` (premake5.lua), so its symbols are present in the
  running process, and each addon is linked with unresolved symbols allowed
  (`-Wl,--allow-shlib-undefined`) and resolved at `dlopen()` time.

Consequences worth knowing:

- The addon artifact is `<id>.so` (the loader looks for exactly that name, so
  `premake5_mods.lua` sets `targetprefix ""` to defeat premake's `lib` prefix).
- There is **no `exports.def` step on Linux** — `game exports` is Windows-only
  and exits with an explanation elsewhere. The committed `exports.def` is never
  handed to a Linux link (it is inside `filter "system:Windows"`).
- The make configs are the lowercase `*_x64` names: the game uses
  `release_x64` / `release_dev_x64` / `debug_x64`; the addon workspace is
  `release_x64` only (it is `{ Release } x { x64 }`).

A fresh Linux checkout:

```
./linux_dev_prepare.sh          # premake5 + deps check + gmake2 makefiles
python3 JERICHO/build.py game   # build the game with the deep mods
python3 JERICHO/build.py mods   # build the runtime addons (.so)
```



- [README.md](README.md) — JERICHO overview, the mod model, the platform matrix.
- [`sdk/README.md`](../../../JERICHO/sdk/README.md) — the standalone addon SDK.
