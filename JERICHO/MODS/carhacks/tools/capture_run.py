#!/usr/bin/env python3
"""capture_run.py - run the game in a real window and grab actual frames.

WHY THIS EXISTS: a headless run produces no picture, and a report about rendering
is much more useful with one. The game is an ordinary window, so Pillow's
ImageGrab after a settle delay captures a real frame - there is no engine-side
screenshot facility to hook.

It launches JERICHO_dev.exe DIRECTLY rather than through a launcher, so the PID is
known and the run can be ended deliberately. A launcher's `start` detaches the
process, which makes the PID unknowable - the one thing not to do here.

  python capture_run.py --out shots/ --settle 12 --frames 1 \
      --level havana --car 1 --weather none --time day

  --vramview        also open the VRAM viewer, so its window is in the grab
  --frames N        N shots, N seconds apart (for a look at motion)
  --gif FILE        assemble the frames into a GIF as well

Writes <out>/NNN.png and prints where each went. Requires Pillow.
"""
import argparse
import os
import subprocess
import sys
import time

EXEDIR = os.path.join("src_rebuild", "bin", "Release_dev")
EXE = os.path.join(EXEDIR, "JERICHO_dev.exe")


def grab(path):
    from PIL import ImageGrab
    img = ImageGrab.grab()
    img.save(path)
    return img.size


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="tmp/shots")
    ap.add_argument("--settle", type=float, default=12.0,
                    help="seconds to let the game reach a drawn frame before grabbing")
    ap.add_argument("--gap", type=float, default=3.0, help="seconds between frames")
    ap.add_argument("--frames", type=int, default=1)
    ap.add_argument("--vramview", action="store_true")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="environment for the game, e.g. --env JERICHO_DUMPVRAM=1")
    ap.add_argument("--wait-for-exit", action="store_true",
                    help="let the run finish on its own rather than ending it, so "
                         "end-of-run output (the VRAM dump) is actually written")
    ap.add_argument("--gif")
    ap.add_argument("--tag", default="run")
    ap.add_argument("rest", nargs=argparse.REMAINDER,
                    help="arguments for the game itself, e.g. -- -level havana -car 1")
    a = ap.parse_args(argv)

    if not os.path.exists(EXE):
        print("no %s - build Release_dev first" % EXE)
        return 2

    os.makedirs(a.out, exist_ok=True)
    cmd = [os.path.abspath(EXE)] + [x for x in a.rest if x != "--"]
    if a.vramview:
        cmd.append("-vramview")
    print("launching: %s" % " ".join(cmd))

    env = dict(os.environ)
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
        print("  env %s=%s" % (k, v))

    p = subprocess.Popen(cmd, cwd=os.path.abspath(EXEDIR), env=env)
    shots = []
    try:
        print("settling %.0fs (pid %d) ..." % (a.settle, p.pid))
        time.sleep(a.settle)
        if p.poll() is not None:
            print("the game exited early (code %s) - nothing to grab" % p.returncode)
            return 3
        for i in range(a.frames):
            path = os.path.join(a.out, "%s_%03d.png" % (a.tag, i))
            size = grab(path)
            shots.append(path)
            print("  shot %d: %s  %dx%d" % (i, path, size[0], size[1]))
            if i + 1 < a.frames:
                time.sleep(a.gap)
    finally:
        # the PID is ours, because we launched it ourselves
        if a.wait_for_exit and p.poll() is None:
            # An end-of-run artifact (the VRAM dump) is only written on a real exit,
            # so give it that exit rather than terminating it out from under one.
            print("waiting for the run to end on its own ...")
            try:
                p.wait(timeout=max(60.0, a.settle * 6))
            except subprocess.TimeoutExpired:
                print("  still running; ending it")
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
            print("ended pid %d" % p.pid)

    if a.gif and shots:
        from PIL import Image
        ims = [Image.open(s).convert("P", palette=Image.ADAPTIVE) for s in shots]
        ims[0].save(a.gif, save_all=True, append_images=ims[1:], duration=400, loop=0)
        print("gif: %s (%d frames)" % (a.gif, len(ims)))

    print("done: %d shot(s) in %s" % (len(shots), a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
