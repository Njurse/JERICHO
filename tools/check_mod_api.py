#!/usr/bin/env python3
"""check_mod_api.py - keep runtime modules on the stable host API.

    python3 tools/check_mod_api.py [--root <repo>] [--gap] [--quiet]

A `runtime = "dll"` addon must be portable: it may include the JERICHO SDK
headers and its own headers, but it must not reach into the engine's headers or
data globals. A compiled-in module may do both, but that is exactly why it
cannot be installed as a runtime addon.

The checker:

  * finds every module under JERICHO/MODS/*/mod.toml;
  * reads `runtime` from the manifest (comments ignored);
  * for a runtime DLL, refuses an `#include` of an engine header, a non-SDK
    include outside the module, and a known engine function or data global;
  * with `--gap`, prints the direct-engine use of every compiled-in module as a
    tab-separated `module  kind  symbol` table, which is the input to the
    external-mod gap document.

The symbol list is deliberately conservative: it contains the engine entry
points and globals modules have actually reached for, not every symbol in the
tree. Add a symbol here when a module starts using it; the check then catches
the next runtime module that tries the same shortcut.

Exit: 0 clean, 1 problems, 2 usage.
"""

from __future__ import annotations

import io
import os
import re
import sys

PROBLEMS: list[str] = []

# The core suite stays compiled in. This is the boundary the external-mod
# conversion must not cross: these modules own engine data/flow that is not a
# stable addon surface.
CORE_MODULES = {"mp", "carhacks", "d1cars"}

# Engine headers that a runtime addon must not include. The scanner also builds
# this from the tree; the hints keep the planted-fault CI test working when the
# test tree only carries the SDK headers.
ENGINE_HEADER_HINTS = {
    "driver2.h", "cars.h", "players.h", "camera.h", "map.h", "dr2roads.h",
    "dr2math.h", "dr2types.h", "draw.h", "models.h", "pedest.h", "pres.h",
    "main.h", "handling.h", "wheelforces.h", "bcollide.h", "objcoll.h",
    "cell.h", "cop_ai.h", "felony.h", "pad.h", "civ_ai.h", "cutscene.h",
    "event.h", "overmap.h", "spool.h", "system.h", "texture.h", "fonts.h",
    "gamesnd.h", "sound.h", "job_fx.h", "denting.h", "loadsave.h",
    "glaunch.h", "scores.h", "replays.h", "overlay.h", "debris.h",
    "gm_sound.h", "weather.h", "sky.h", "shadow.h", "psx.h",
}

# Engine functions and data globals compiled-in modules use directly. `kind`
# is function or global; the value is the category used by --gap.
ENGINE_SYMBOLS: dict[str, tuple[str, str]] = {}


def sym(kind: str, category: str, *names: str) -> None:
    for name in names:
        ENGINE_SYMBOLS[name] = (kind, category)


sym("global", "car", "car_data", "active_car_list", "num_active_cars",
    "gCarCleanModelPtr", "modelpointers", "residentCarModels")
sym("global", "player", "player", "MainPlayer", "MaxPlayerDamage",
    "NumPlayers", "GameType", "gCurrentMissionNumber")
sym("global", "camera", "camera_position", "gCameraAngle",
    "gCameraDefaultScrZ", "gCameraAngle")
sym("global", "render", "matrixtable", "OT", "CurrentOT", "plotContext")
sym("global", "world", "pPed", "MapData", "roadInfo", "Straights",
    "Curves", "Junctions", "NumDriver2Straights", "NumDriver2Curves",
    "NumDriver2Junctions")
sym("function", "car", "RebuildCarMatrix", "SetCarMatrix",
    "UpdateCarMatrix", "CreateCar", "DeleteCar", "InitCar", "ApplyDamage",
    "DamageCar", "SetCarControl", "SetCarPosition", "SetCarVelocity",
    "SetCarAngularVelocity", "SetCarOrientation", "GetCarMatrix",
    "GetWheelState", "SetWheelDamage", "SetZoneDamage")
sym("function", "world", "MapHeight", "CellEmpty", "GetSurfaceIndex",
    "FindRoad", "GetRoadAt", "GetFirstPackedCop", "QuickUnpackCellObject",
    "GetTunnel")
sym("function", "ai", "copsAreInPursuit", "CopsCanSeePlayer",
    "GetPlayerFelony", "NoteFelony")
sym("function", "render", "DrawModel", "RenderModel", "DrawCar",
    "DrawCarWheels", "ImmediateDrawModel", "PlotModel", "DrawSprite",
    "DrawTexturedQuad", "LoadTexture", "CreateTexture", "SetDispEnv",
    "DrawTargetBlip", "DrawPlayerDot")
sym("function", "fx", "Setup_Sparks", "Setup_Debris", "AddLeaf", "AddSmoke",
    "AddExplosion")
sym("function", "sound", "Start3DSoundVolPitch", "Stop3DSound",
    "GetFreeChannel", "SetPlayerOwnsChannel")
sym("function", "gameflow", "SetPadVibration", "EnablePause",
    "SetMissionOver", "SetNextScreen", "SetGameType", "Fade")
sym("function", "math", "Fob", "RSIN", "RCOS", "ratan2", "FixHalfRound")

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
RUNTIME_RE = re.compile(r'^\s*runtime\s*=\s*"([^"]*)"', re.M)
HOST_FN_RE = re.compile(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(")


def read(path: str) -> str:
    with io.open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def strip_comments(text: str) -> str:
    """Remove C comments but keep string/char literals (needed for #include)."""
    out = []
    i = 0
    state = "normal"
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "normal":
            if c == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = "normal"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = "normal"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        # string / char
        out.append(c)
        if c == "\\" and i + 1 < n:
            out.append(text[i + 1])
            i += 2
            continue
        if (state == "string" and c == '"') or (state == "char" and c == "'"):
            state = "normal"
        i += 1
    return "".join(out)


def strip_c(text: str) -> str:
    """Replace comments, strings and char literals with spaces, keeping lines."""
    out = []
    i = 0
    state = "normal"
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "normal":
            if c == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "string"
                out.append(" ")
                i += 1
                continue
            if c == "'":
                state = "char"
                out.append(" ")
                i += 1
                continue
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = "normal"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = "normal"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        # string / char
        if c == "\\":
            out.append("  ")
            i += 2
            continue
        if (state == "string" and c == '"') or (state == "char" and c == "'"):
            state = "normal"
            out.append(" ")
            i += 1
            continue
        out.append("\n" if c == "\n" else " ")
        i += 1
    return "".join(out)


def manifest(path: str) -> tuple[str, bool]:
    text = read(path)
    clean = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
    m = RUNTIME_RE.search(clean)
    return (m.group(1).strip().lower() if m else "", m is not None and m.group(1).strip().lower() == "dll")


def source_files(mod_dir: str) -> list[str]:
    files = []
    for base, dirs, names in os.walk(mod_dir):
        dirs[:] = [d for d in dirs if d not in (".git", "obj", "lib", "build", "bin")]
        for name in names:
            if name.endswith((".c", ".h", ".cpp", ".cc", ".inc")):
                files.append(os.path.join(base, name))
    return sorted(files)


def header_index(root: str) -> tuple[set[str], set[str]]:
    sdk = set()
    sdk_dir = os.path.join(root, "JERICHO", "sdk", "include")
    if os.path.isdir(sdk_dir):
        sdk = {n for n in os.listdir(sdk_dir) if n.endswith(".h")}

    engine = set(ENGINE_HEADER_HINTS)
    for rel in ("src_rebuild/Game/C", "src_rebuild/Game", "src_rebuild/PsyCross/include"):
        base = os.path.join(root, *rel.split("/"))
        if not os.path.isdir(base):
            continue
        for dirpath, dirs, names in os.walk(base):
            dirs[:] = [d for d in dirs if d not in (".git", "obj", "lib", "build", "bin", "dependencies")]
            for name in names:
                if name.endswith(".h"):
                    engine.add(name)
    engine -= sdk
    return sdk, engine


def host_functions(root: str) -> set[str]:
    path = os.path.join(root, "src_rebuild", "Game", "C", "JERICHO", "include", "jer_host_api.h")
    if not os.path.isfile(path):
        return set()
    return set(HOST_FN_RE.findall(read(path)))


def module_dirs(root: str) -> list[str]:
    base = os.path.join(root, "JERICHO", "MODS")
    if not os.path.isdir(base):
        return []
    return sorted(
        os.path.join(base, n) for n in os.listdir(base)
        if os.path.isfile(os.path.join(base, n, "mod.toml"))
    )


def check(root: str, quiet: bool, show_gap: bool) -> int:
    sdk, engine_headers = header_index(root)
    host = host_functions(root)
    gap: list[tuple[str, str, str]] = []
    runtime_modules = 0
    seen: dict[str, bool] = {}

    for mod_dir in module_dirs(root):
        mid = os.path.basename(mod_dir)
        runtime, is_dll = manifest(os.path.join(mod_dir, "mod.toml"))
        seen[mid] = is_dll
        if is_dll:
            runtime_modules += 1

        for path in source_files(mod_dir):
            rel = os.path.relpath(path, mod_dir).replace("\\", "/")
            raw = read(path)
            text = strip_c(raw)
            includes = INCLUDE_RE.findall(strip_comments(raw))

            for inc in includes:
                base = os.path.basename(inc).lower()
                local = os.path.isfile(os.path.join(mod_dir, inc)) or \
                    os.path.isfile(os.path.join(mod_dir, os.path.basename(inc)))
                if base in sdk or local:
                    continue
                if is_dll:
                    if base in engine_headers:
                        PROBLEMS.append("%s/%s: runtime DLL includes engine header '%s'" % (mid, rel, inc))
                    elif "/" in inc or "\\" in inc:
                        PROBLEMS.append("%s/%s: runtime DLL includes non-SDK path '%s'" % (mid, rel, inc))

            for name, (kind, category) in ENGINE_SYMBOLS.items():
                # A member named like an engine global (api->player, a->player)
                # is not a direct engine reference; require the identifier not
                # to be preceded by '.' or the '>' of '->'.
                if not re.search(r"(?<![\w.>])\b%s\b" % re.escape(name), text):
                    continue
                if is_dll:
                    PROBLEMS.append("%s/%s: runtime DLL uses engine %s '%s' (%s)" % (mid, rel, kind, name, category))
                else:
                    gap.append((mid, kind, name))

    for core in sorted(CORE_MODULES):
        if core not in seen:
            PROBLEMS.append("core module '%s' is missing from JERICHO/MODS" % core)
        elif seen[core]:
            PROBLEMS.append("core module '%s' declares runtime = \"dll\"; core must stay compiled-in" % core)

    if show_gap:
        for mid, kind, name in sorted(gap):
            print("%s\t%s\t%s" % (mid, kind, name))

    if not quiet:
        print("check_mod_api: %d runtime DLL(s), %d compiled-in module(s), %d host function(s)"
              % (runtime_modules, len(module_dirs(root)) - runtime_modules, len(host)))
        for p in PROBLEMS:
            print("check_mod_api: " + p)

    if PROBLEMS:
        print("check_mod_api: %d problem(s)" % len(PROBLEMS))
        return 1
    if not quiet:
        print("check_mod_api: clean")
    return 0


def main(argv: list[str]) -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    quiet = False
    show_gap = False
    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg == "--root":
            if i + 1 >= len(argv):
                print("check_mod_api: --root needs a path", file=sys.stderr)
                return 2
            root = argv[i + 1]
            i += 2
            continue
        if arg == "--quiet":
            quiet = True
        elif arg == "--gap":
            show_gap = True
        elif arg in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        else:
            print("check_mod_api: unknown option %s" % arg, file=sys.stderr)
            return 2
        i += 1
    return check(os.path.abspath(root), quiet, show_gap)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
