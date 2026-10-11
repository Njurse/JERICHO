#!/usr/bin/env python3
"""follow_host.py -- bring in ONE extra game that joins YOUR localhost match and,
with JerAI, tails the host around the map.

The workflow it exists for: you host a multiplayer match as usual (the game's own
Host menu, or mp_host.bat / mp_localpair's host seat), then run this. It starts a
single joining instance on 127.0.0.1:<port> with JerAI enabled in "follow" mode,
so it drives its own car to YOUR car, holds a clearance and idles - a moving
partner to test multiplayer against.

It does NOT host, and it does not start a second host: the match must already be
up. It also does not touch your instance -- the follower gets its OWN run
directory (directory junctions share the game's big trees; only JERICHO/CONFIG is
a private copy), so its JERICHO.log and mp.ini cannot collide with yours.

    python JERICHO/MODS/JerAI/tools/follow_host.py                 # port 1400
    python JERICHO/MODS/JerAI/tools/follow_host.py --port 1400 --client-car slot4
    python JERICHO/MODS/JerAI/tools/follow_host.py --clearance 2500 --range 0
    python JERICHO/MODS/JerAI/tools/follow_host.py --dry            # print, do not launch

Ctrl+C stops the follower (the PID here is real -- it is launched directly, not
through `start`, which would detach it beyond our reach).
"""
import argparse
import math
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# Reuse the mp pair harness's run-dir builder rather than keep a second copy of the
# junction dance: it is the piece that knows a run dir must never be deleted
# through a junction into the real game tree.
MP_TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "mp", "tools"))
if os.path.isdir(MP_TOOLS):
    sys.path.insert(0, MP_TOOLS)

DEFAULT_GAME_DIR = os.path.join("src_rebuild", "bin", "Release_dev")

DRIVE_RE = re.compile(r"\[JerAI\] drive .*?speed=(-?\d+).*?follow=(-?\d+) fdist=(\d+)")


def log(msg):
    print(f"[follow] {msg}", flush=True)


def set_ini_key(path, key, value):
    """Set `key = value` in a JERICHO .ini, replacing the key if it is there and
    appending it if it is not. Kept simple on purpose: these files are one
    `key = value` per line, and a module re-reads them live."""
    lines = []
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()

    out, seen = [], False
    for line in lines:
        stripped = line.strip()
        if stripped and not stripped.startswith("#") and "=" in stripped:
            name = stripped.split("=", 1)[0].strip()
            if name == key:
                out.append(f"{key} = {value}")
                seen = True
                continue
        out.append(line)

    if not seen:
        out.append(f"{key} = {value}")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def enable_module(modlist, module, value=1):
    """Turn a module on in JERICHO/CONFIG/modlist.ini without disturbing the rest
    of the list. An id the file has never heard of is appended (the loader falls
    back to mod.toml's default-enabled when an id is absent, and JerAI's is
    false)."""
    lines = []
    if os.path.isfile(modlist):
        with open(modlist, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()

    pat = re.compile(r"^(\s*)" + re.escape(module) + r"(\s*)=")
    out, seen = [], False
    for line in lines:
        m = pat.match(line)
        if m:
            out.append(f"{module} = {value}")
            seen = True
        else:
            out.append(line)

    if not seen:
        out.append(f"{module} = {value}")

    with open(modlist, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", default=DEFAULT_GAME_DIR,
                    help=f"the built game directory (default {DEFAULT_GAME_DIR})")
    ap.add_argument("--exe", default="JERICHO_dev.exe")
    ap.add_argument("--join-host", default="127.0.0.1",
                    help="the host to dial (default 127.0.0.1 -- your own machine)")
    ap.add_argument("--port", type=int, default=1400,
                    help="the host's session port (default 1400, what mp_host.bat uses; "
                         "must match the port in the host's mp.ini)")
    ap.add_argument("--run-dir", default=None,
                    help="where the follower's private run directory lives "
                         "(default <game-dir>/.mp-follow)")
    ap.add_argument("--name", default="Follower", help="the joining player's name")
    ap.add_argument("--client-car", default="slot4",
                    help="the follower's car: a model number, slotN, 'random', or 'default' "
                         "(= pass no -mpcar and let the level choose)")
    ap.add_argument("--clearance", type=int, default=None,
                    help="override JerAI's follow_clearance (world units)")
    ap.add_argument("--range", type=int, default=None,
                    help="override JerAI's follow_range; 0 = unlimited (the default)")
    ap.add_argument("--yaw-lead", type=int, default=None,
                    help="override JerAI's yaw_lead (steering damper; lower steers harder)")
    ap.add_argument("--seconds", type=int, default=0,
                    help="stop after this long (0 = run until Ctrl+C)")
    ap.add_argument("--dry", action="store_true",
                    help="prepare the run dir and print the command, but do not launch")
    args = ap.parse_args()

    game_dir = os.path.abspath(args.game_dir)
    if not os.path.isdir(game_dir):
        log(f"no such game directory: {game_dir}")
        return 2

    exe = os.path.join(game_dir, args.exe)
    if not os.path.isfile(exe):
        log(f"no such exe: {exe}  (build it with JERICHO\\build_game.bat)")
        return 2

    run_dir = args.run_dir or os.path.join(game_dir, ".mp-follow")

    try:
        import mp_localpair
    except ImportError:
        log(f"cannot import the mp pair harness from {MP_TOOLS} "
            f"(follow_host.py reuses its run-dir builder)")
        return 2

    log(f"building the follower's run directory: {run_dir}")
    mp_localpair.build_run_dir(game_dir, run_dir, args.name, args.port)

    cfg = os.path.join(run_dir, "JERICHO", "CONFIG")
    os.makedirs(cfg, exist_ok=True)

    # The follower must be an mp client (mp is on by default; make it explicit) and
    # must run JerAI in Joyride. In a session JerAI's own gate does the rest: it is
    # NOT the host, so Joyride targets the host's car.
    enable_module(os.path.join(cfg, "modlist.ini"), "mp", 1)
    enable_module(os.path.join(cfg, "modlist.ini"), "JerAI", 1)

    jer = os.path.join(cfg, "JerAI.ini")
    set_ini_key(jer, "enabled", 1)
    set_ini_key(jer, "mode", 1)          # 1 = Joyride
    set_ini_key(jer, "follow", 1)
    set_ini_key(jer, "follow_car", -1)   # the MP host, not a single-player lever
    set_ini_key(jer, "follow_clearance", args.clearance if args.clearance is not None else 1500)
    set_ini_key(jer, "follow_range", args.range if args.range is not None else 0)
    if args.yaw_lead is not None:
        set_ini_key(jer, "yaw_lead", args.yaw_lead)

    cmd = [exe, "-nointro", "-nofmv", "-join", f"{args.join_host}:{args.port}"]
    if args.client_car and args.client_car.lower() != "default":
        cmd += ["-mpcar", args.client_car]

    env = dict(os.environ)
    env.pop("MP_AUTOSTART", None)   # the client JOINS via -join; that env var is the host's
    env["JERICHO_LOG_FLUSH"] = "1"

    log(f"command: {' '.join(cmd)}")
    if args.dry:
        log("--dry: not launching")
        return 0

    log(f"joining {args.join_host}:{args.port} as '{args.name}' - JerAI (Joyride) will follow the host")
    proc = subprocess.Popen(cmd, cwd=run_dir, env=env)

    logfile = os.path.join(run_dir, "JERICHO.log")
    started = time.time()
    pos = 0
    last = 0.0

    try:
        while True:
            if proc.poll() is not None:
                log(f"the follower exited (code {proc.returncode})")
                break

            if os.path.isfile(logfile):
                with open(logfile, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(pos)
                    for line in f:
                        m = DRIVE_RE.search(line)
                        if m and time.time() - last >= 1.0:
                            last = time.time()
                            log(f"speed={m.group(1):>4}  follow={m.group(2)}  "
                                f"dist={math.isqrt(int(m.group(3))):>6}")
                    pos = f.tell()

            if args.seconds and time.time() - started >= args.seconds:
                log("time is up")
                break

            time.sleep(0.25)
    except KeyboardInterrupt:
        log("stopping the follower")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log(f"log: {logfile}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
