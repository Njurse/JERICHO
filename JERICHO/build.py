#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""JERICHO cross-platform build driver.

This is the single source of truth for every JERICHO build step, on every
OS. Windows and Linux run *this same script*; each OS supplies a backend
(see BACKENDS below) that knows how to generate project files and call the
compiler. The in-game "Compile Mods" flow (jer_compile.c) and the developer
entry points (JERICHO/build_game.bat|.sh, JERICHO/build_mods.bat|.sh) all
call this script, so the build logic lives in exactly one place.

Usage
-----
    build.py game premake            generate the game project files
    build.py game mod <id>           build one compiled-in (deep) module
    build.py game exe                link the game executable
    build.py game exports            (Windows) two-pass exports.def + relink
    build.py game                    premake + every deep mod + the exe
    build.py mods                    build every runtime addon (DLL / .so)
    build.py sdk [<id>]              build one addon against the standalone SDK

Common options
--------------
    --src DIR       the src_rebuild tree (default: auto-detected)
    --config NAME   Release | Release_dev | Debug   (default: Release_dev)
    --jobs N        parallel jobs for make        (default: CPU count)
    --dry-run       print the commands instead of running them

Porting to a new OS
-------------------
Subclass Backend, implement its small method set, and register the class
with @register_backend. Nothing outside this file has to change. When the
OS is not in BACKENDS the driver exits with instructions rather than
silently doing nothing.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

# Directory the driver itself lives in (repo JERICHO/ or <bin>/<cfg>/JERICHO/).
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

DEFAULT_CONFIG = "Release_dev"
CONFIGS = ("Debug", "Release", "Release_dev")


def eprint(*a):
    print(*a, file=sys.stderr, flush=True)


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------

def canonical_config(name):
    """Accept Release / release / release_dev_x64 ... -> the canonical name."""
    key = (name or "").strip().lower().replace("-", "_")
    table = {
        "debug": "Debug", "debug_x64": "Debug", "debug_x86": "Debug",
        "release": "Release", "release_x64": "Release", "release_x86": "Release",
        "release_dev": "Release_dev", "release_dev_x64": "Release_dev",
        "release_dev_x86": "Release_dev", "releasedev": "Release_dev",
    }
    if key in table:
        return table[key]
    raise SystemExit("build.py: unknown configuration %r (expected %s)"
                     % (name, ", ".join(CONFIGS)))


def parse_mod_toml(path):
    """Tiny mod.toml reader: flat key = value pairs, '#' comments."""
    out = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    return out


def jericho_dir(src_dir):
    """The central JERICHO folder that lives beside src_rebuild."""
    return os.path.abspath(os.path.join(src_dir, "..", "JERICHO"))


def mods_dir(src_dir):
    return os.path.join(jericho_dir(src_dir), "MODS")


def scan_mods(src_dir):
    """[(id, toml_dict)] for every folder under JERICHO/MODS with a mod.toml."""
    root = mods_dir(src_dir)
    found = []
    if not os.path.isdir(root):
        return found
    for name in sorted(os.listdir(root)):
        d = os.path.join(root, name)
        toml = os.path.join(d, "mod.toml")
        if os.path.isdir(d) and os.path.isfile(toml):
            found.append((name, parse_mod_toml(toml)))
    return found


def deep_mods(src_dir):
    """Compiled-in modules: mod.toml WITHOUT runtime = "dll"."""
    return [i for i, t in scan_mods(src_dir)
            if (t.get("runtime") or "").lower() != "dll"]


def addon_mods(src_dir):
    """Runtime addons: mod.toml WITH runtime = "dll"."""
    return [i for i, t in scan_mods(src_dir)
            if (t.get("runtime") or "").lower() == "dll"]


def find_src_dir(explicit):
    """Locate the src_rebuild tree (the folder holding premake5.lua)."""
    if explicit:
        d = os.path.abspath(explicit)
        if not os.path.isfile(os.path.join(d, "premake5.lua")):
            raise SystemExit("build.py: %s is not a src_rebuild tree "
                             "(no premake5.lua)" % d)
        return d

    # 1) the repo layout: JERICHO/ is a sibling of src_rebuild/
    cand = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "src_rebuild"))
    if os.path.isfile(os.path.join(cand, "premake5.lua")):
        return cand

    # 2) walk up from the driver's folder (covers <bin>/<cfg>/JERICHO)
    d = SCRIPT_DIR
    for _ in range(8):
        if os.path.isfile(os.path.join(d, "premake5.lua")):
            return d
        nxt = os.path.join(d, "src_rebuild", "premake5.lua")
        if os.path.isfile(nxt):
            return os.path.abspath(os.path.join(d, "src_rebuild"))
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None


def find_premake(src_dir):
    name = "premake5.exe" if os.name == "nt" else "premake5"
    for cand in (os.path.join(src_dir, name),
                 os.path.join(os.path.dirname(src_dir), name),
                 os.path.join(SCRIPT_DIR, name)):
        if os.path.isfile(cand):
            return os.path.abspath(cand)
    found = shutil.which("premake5") or shutil.which("premake5.exe")
    if found:
        return found
    raise SystemExit(
        "build.py: premake5 not found. Put it in %s or on PATH "
        "(Linux: download premake-5.0.0-beta1-linux.tar.gz)." % src_dir)


def find_vswhere():
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    pf = os.environ.get("ProgramFiles", r"C:\Program Files")
    for d in (pf86, pf):
        c = os.path.join(d, "Microsoft Visual Studio", "Installer", "vswhere.exe")
        if os.path.isfile(c):
            return c
    return shutil.which("vswhere")


def _vswhere_find(requires, pattern):
    vswhere = find_vswhere()
    if not vswhere:
        return None
    try:
        out = subprocess.run([vswhere, "-latest", "-products", "*",
                              "-requires", requires, "-find", pattern],
                             capture_output=True, text=True)
    except OSError:
        return None
    for line in out.stdout.splitlines():
        line = line.strip()
        if line and os.path.isfile(line):
            return line
    return None


def find_msbuild():
    found = _vswhere_find("Microsoft.Component.MSBuild",
                          r"MSBuild\**\Bin\MSBuild.exe")
    return found or shutil.which("msbuild")


def find_vcvars64():
    found = _vswhere_find("Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                          r"VC\Auxiliary\Build\vcvars64.bat")
    return found


# --------------------------------------------------------------------------
# Execution context handed to the backends
# --------------------------------------------------------------------------

class Ctx:
    def __init__(self, src, config, dry_run, jobs, backend):
        self.src = src
        self.config = config
        self.dry_run = dry_run
        self.jobs = jobs
        self.backend = backend

    def run(self, cmd, cwd=None, env=None):
        """Print and run a command. Returns its exit code (0 in dry-run)."""
        where = "" if cwd is None else "   (cwd %s)" % cwd
        print("[build] %s%s" % (" ".join(cmd), where), flush=True)
        if self.dry_run:
            return 0
        try:
            return subprocess.call(cmd, cwd=cwd, env=env)
        except FileNotFoundError as exc:
            eprint("[build] ERROR: %s" % exc)
            return 127
        except OSError as exc:
            eprint("[build] ERROR: %s" % exc)
            return 126


# --------------------------------------------------------------------------
# Backends — one per OS family. Add a class + @register_backend to port.
# --------------------------------------------------------------------------

BACKENDS = {}


def register_backend(cls):
    BACKENDS[cls.key] = cls
    return cls


def os_key():
    if os.name == "nt":
        return "windows"
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "darwin"
    return sys.platform


def backend_for_os():
    key = os_key()
    if key not in BACKENDS:
        raise SystemExit(
            "build.py: no build backend for this OS (%s).\n"
            "To port: subclass Backend, implement its methods, and add\n"
            "    @register_backend\n"
            "    class MyBackend(Backend): key = %r; ...\n"
            "See the 'Porting to a new platform' JERICHO docs." % (key, key))
    return BACKENDS[key]()


class Backend:
    key = None
    lib_ext = ".so"

    # --- project generation -------------------------------------------------
    def premake_action(self):                       # pragma: no cover - abstract
        raise NotImplementedError

    def premake(self, ctx, mods_only=False):        # pragma: no cover - abstract
        raise NotImplementedError

    # --- per-step builds ----------------------------------------------------
    def build_deep_mod(self, ctx, mod_id):          # pragma: no cover - abstract
        raise NotImplementedError

    def build_exe(self, ctx):                       # pragma: no cover - abstract
        raise NotImplementedError

    def build_exports(self, ctx):
        raise SystemExit("build.py: exports.def is a Windows/MSVC mechanism; "
                         "on %s the exe exports its symbols with the linker's "
                         "--export-dynamic and no .def is needed." % self.key)

    def build_addons(self, ctx):                    # pragma: no cover - abstract
        raise NotImplementedError

    def build_sdk_addon(self, ctx, include, sdk_dir, src_c, out):  # pragma: no cover
        raise NotImplementedError

    def make_config(self, ctx):
        return ctx.config


@register_backend
class WindowsBackend(Backend):
    key = "windows"
    lib_ext = ".dll"

    def premake_action(self):
        # vs2019 matches the original in-game flow; override for CI/locally.
        return os.environ.get("JERICHO_PREMAKE_ACTION", "vs2019")

    def _premake_env(self, ctx):
        env = dict(os.environ)
        deps = (("SDL2_DIR", "SDL2-2.30.2"),
                ("OPENAL_DIR", "openal-soft-1.23.1-bin"),
                ("JPEG_DIR", "jpeg-9d"))
        for var, sub in deps:
            p = os.path.join(ctx.src, "dependencies", sub)
            if os.path.isdir(p):
                env[var] = p
        return env

    def premake(self, ctx, mods_only=False):
        premake = find_premake(ctx.src)
        if mods_only:
            cmd = [premake, "--file=premake5_mods.lua", self.premake_action()]
        else:
            cmd = [premake, self.premake_action()]
        return ctx.run(cmd, cwd=ctx.src, env=self._premake_env(ctx))

    def _msbuild(self):
        mb = find_msbuild()
        if not mb:
            raise SystemExit("build.py: MSBuild not found - install the "
                             "Visual Studio 'Desktop development with C++' "
                             "workload, or run from a Developer Command Prompt.")
        return mb

    def _build_proj(self, ctx, proj, config):
        return ctx.run([self._msbuild(), proj,
                        "/p:Configuration=" + config, "/p:Platform=x64",
                        "/m", "/v:m", "/nologo"], cwd=ctx.src)

    def build_deep_mod(self, ctx, mod_id):
        return self._build_proj(ctx, os.path.join("build", "mod_%s.vcxproj" % mod_id),
                                ctx.config)

    def build_exe(self, ctx):
        return self._build_proj(ctx, os.path.join("build", "REDRIVER2.vcxproj"),
                                ctx.config)

    def build_exports(self, ctx):
        # Two-pass so the .def matches the build's own symbol set:
        #   1. link with a throwaway empty def -> writes the .map
        #   2. gen_exports reads the .map -> exports.def, then relink.
        exe = {"Release": "REDRIVER2", "Release_dev": "REDRIVER2_dev",
               "Debug": "REDRIVER2_dbg"}[ctx.config]
        def_path = os.path.join(ctx.src, "exports.def")
        gen = os.path.join(ctx.src, "bin", ctx.config, "gen_exports.exe")
        mapf = os.path.join(ctx.src, "bin", ctx.config, exe + ".map")

        print("[build] (pass 1) truncate %s to EXPORTS and link for the map" % def_path,
              flush=True)
        if not ctx.dry_run:
            with open(def_path, "w") as f:
                f.write("EXPORTS\n")
        rc = self.build_exe(ctx)
        if rc:
            return rc
        rc = ctx.run([gen, mapf, def_path], cwd=ctx.src)
        if rc:
            return rc
        print("[build] (pass 2) regenerate the def from the map, then relink",
              flush=True)
        return self.build_exe(ctx)

    def build_addons(self, ctx):
        rc = self.premake(ctx, mods_only=True)
        if rc:
            return rc
        return self._build_proj(ctx, os.path.join("build_mods", "REDRIVER2_MODS.sln"),
                                "Release")

    def build_sdk_addon(self, ctx, include, sdk_dir, src_c, out):
        lib = os.path.join(sdk_dir, "lib", "x64", "Release", "REDRIVER2.lib")
        game_inc = os.path.join(ctx.src, "Game")
        game_c = os.path.join(ctx.src, "Game", "C")
        inner = ('cl /nologo /LD /TP /EHsc /I "%s" /I "%s" /I "%s" "%s" '
                 '/link /LIBPATH:"%s" REDRIVER2.lib /OUT:"%s"'
                 % (include, game_inc, game_c, src_c, os.path.dirname(lib), out))
        vcvars = find_vcvars64()
        if vcvars:
            inner = 'call "%s" >nul && %s' % (vcvars, inner)
        return ctx.run(["cmd", "/c", inner], cwd=ctx.src)


@register_backend
class LinuxBackend(Backend):
    key = "linux"
    lib_ext = ".so"

    def make_config(self, ctx):
        return {"Debug": "debug_x64", "Release": "release_x64",
                "Release_dev": "release_dev_x64"}[ctx.config]

    def premake_action(self):
        return "gmake2"

    def premake(self, ctx, mods_only=False):
        premake = find_premake(ctx.src)
        if mods_only:
            cmd = [premake, "--file=premake5_mods.lua", "gmake2"]
        else:
            cmd = [premake, "gmake2"]
        return ctx.run(cmd, cwd=ctx.src)

    def _make(self, ctx, build_dir, target, config):
        make = shutil.which("make") or "make"
        cmd = [make, "-C", build_dir, "config=" + config]
        if target:
            cmd.append(target)
        cmd.append("-j%d" % ctx.jobs)
        return ctx.run(cmd, cwd=ctx.src)

    def build_deep_mod(self, ctx, mod_id):
        return self._make(ctx, "build", "mod_" + mod_id, self.make_config(ctx))

    def build_exe(self, ctx):
        return self._make(ctx, "build", "REDRIVER2", self.make_config(ctx))

    def build_addons(self, ctx):
        rc = self.premake(ctx, mods_only=True)
        if rc:
            return rc
        # the mods workspace is { Release } x { x64 } only -> release_x64
        return self._make(ctx, "build_mods", None, "release_x64")

    def build_sdk_addon(self, ctx, include, sdk_dir, src_c, out):
        cc = os.environ.get("CC", "cc")
        # an addon .so resolves the game's symbols at dlopen time, so
        # undefined symbols are fine here (the exe is --export-dynamic).
        return ctx.run([cc, "-shared", "-fPIC", "-O2", "-o", out, src_c,
                        "-I", include], cwd=ctx.src)


# --------------------------------------------------------------------------
# addon artifact copy (shared by all backends)
# --------------------------------------------------------------------------

def copy_addons(ctx):
    ext = ctx.backend.lib_ext
    root = mods_dir(ctx.src)
    targets = []

    # the runtime JERICHO mirrored next to the exe (mods always build Release)
    rt = os.path.join(ctx.src, "bin", "Release", "JERICHO", "MODS")
    if os.path.isdir(os.path.dirname(rt)):
        targets.append(rt)

    # when the driver itself runs from a runtime JERICHO folder, land them
    # right there too (the loader looks beside the exe).
    if os.path.basename(SCRIPT_DIR) == "JERICHO":
        own = os.path.join(SCRIPT_DIR, "MODS")
        if os.path.abspath(own) != os.path.abspath(root):
            targets.append(own)

    if not targets:
        return 0

    for mid in addon_mods(ctx.src):
        built = os.path.join(root, mid, mid + ext)
        if not os.path.isfile(built):
            continue
        for t in targets:
            dst = os.path.join(t, mid)
            if ctx.dry_run:
                print("[build] (dry-run) copy %s -> %s" % (built, dst), flush=True)
                continue
            os.makedirs(dst, exist_ok=True)
            shutil.copy2(built, dst)
    return 0


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def do_game(ctx, step, mod_id):
    b = ctx.backend
    if step == "premake":
        return b.premake(ctx)
    if step == "mod":
        if not mod_id:
            raise SystemExit("build.py: 'game mod' needs a module id")
        return b.build_deep_mod(ctx, mod_id)
    if step == "exe":
        return b.build_exe(ctx)
    if step == "exports":
        return b.build_exports(ctx)
    if step in (None, "", "all"):
        rc = b.premake(ctx)
        if rc:
            return rc
        for mid in deep_mods(ctx.src):
            print("[build] deep module: %s" % mid, flush=True)
            rc = b.build_deep_mod(ctx, mid)
            if rc:
                return rc
        return b.build_exe(ctx)
    raise SystemExit("build.py: unknown game step %r "
                     "(premake|mod <id>|exe|exports|all)" % step)


def do_mods(ctx):
    rc = ctx.backend.build_addons(ctx)
    if rc:
        return rc
    return copy_addons(ctx)


def do_sdk(ctx, mod_id):
    b = ctx.backend
    sdk_dir = os.path.join(jericho_dir(ctx.src), "sdk")
    include = os.path.join(sdk_dir, "include")

    ids = [mod_id] if mod_id else addon_mods(ctx.src)
    if not ids:
        print("[build] sdk: no runtime=\"dll\" addons found", flush=True)
        return 0

    for mid in ids:
        src_c = os.path.join(mods_dir(ctx.src), mid, mid + ".c")
        if not os.path.isfile(src_c):
            eprint("[build] sdk: %s has no %s.c - skipped" % (mid, mid))
            continue
        out = os.path.join(mods_dir(ctx.src), mid, mid + b.lib_ext)
        print("[build] sdk addon: %s -> %s" % (mid, out), flush=True)
        rc = b.build_sdk_addon(ctx, include, sdk_dir, src_c, out)
        if rc:
            return rc
    return 0


# --------------------------------------------------------------------------

def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)

    p = argparse.ArgumentParser(
        prog="build.py",
        description="JERICHO cross-platform build driver.")
    p.add_argument("target", nargs="?", default="game",
                   choices=["game", "mods", "sdk"])
    p.add_argument("step", nargs="?", default=None,
                   help="premake|mod|exe|exports|all (game); <id> (sdk)")
    p.add_argument("id", nargs="?", default=None, help="module id for 'game mod <id>'")
    p.add_argument("--src", default=None, help="the src_rebuild tree")
    p.add_argument("--config", default=os.environ.get("JERICHO_BUILD_CONFIG", DEFAULT_CONFIG))
    p.add_argument("--jobs", type=int, default=0)
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args(argv)

    # tolerate an empty or quote-wrapped --src (a batch shim can hand us "").
    src_arg = (args.src or "").strip().strip('"')
    src = find_src_dir(src_arg)
    if src is None:
        raise SystemExit("build.py: could not find src_rebuild (pass --src).")

    config = canonical_config(args.config)
    jobs = args.jobs if args.jobs > 0 else (os.cpu_count() or 4)
    ctx = Ctx(src, config, args.dry_run, jobs, backend_for_os())

    print("[build] JERICHO driver | os=%s config=%s jobs=%d src=%s%s"
          % (ctx.backend.key, config, jobs, src, "  [dry-run]" if args.dry_run else ""),
          flush=True)

    if args.target == "game":
        return do_game(ctx, args.step, args.id)
    if args.target == "mods":
        return do_mods(ctx)
    if args.target == "sdk":
        return do_sdk(ctx, args.step)
    raise SystemExit("build.py: unknown target %r" % args.target)


if __name__ == "__main__":
    sys.exit(main())
