#!/usr/bin/env python3
"""jerai_steer_stats.py -- read a JERICHO.log and report how the JerAI steering
loop actually behaved.

Why this exists: the once-a-second "[JerAI] drive" line cannot show a saw - it
samples 30 frames apart while the wheel is turned on and off in ones and twos.
Setting `steer_trace = N` in JERICHO/CONFIG/JerAI.ini makes the module log a
compact "[JerAI] steer" line every N frames instead, and this summarises them.

    python JERICHO/MODS/JerAI/tools/jerai_steer_stats.py [logfile]

The numbers that matter:

  mean|diff|   the average heading error. Small (tens) = tracking; hundreds =
               the car is persistently pointing well away from what it aims at.
  wa           the ACTUAL front wheel angle in game units (352 = full regular
               lock). A mean near 352 means the controller is saturated.
  flips        how often the commanded steer direction reverses. A "saw" is a
               high flip rate; a slow serpentine is a low one with a big lock.
  off          cross-track: how far the car sits off its own planned line.
  la vs aimd   the look-ahead asked for vs where the aim landed. aimd much
               larger than la is the signature of a car well off the line
               (the straight-line distance to a point la ahead ALONG the line
               exceeds la).
  pdir         the path's own direction at the aim, so diff can be split into
               "the car is not aligned with the path" and "the aim sits off to
               one side of the car".
"""
import re
import statistics as st
import sys

STEER = re.compile(
    r"t=(\d+) pad=([0-9a-f]+) dir=(-?\d+) diff=(-?\d+) pred=(-?\d+) bias=(-?\d+) "
    r"spd=(-?\d+) tgt=(-?\d+) wa=(-?\d+) la=(\d+) aimd=(\d+)"
    r"(?: pdir=(-?\d+))? off=(\d+)")
GOAL = re.compile(r" gx=(-?\d+) gz=(-?\d+) wp=(\d+)")

ACCEL, BRAKE, LEFT, RIGHT, HB = 0x40, 0x80, 0x8000, 0x2000, 0x10


def wrap(a):
    return ((a + 2048) & 0xfff) - 2048


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "JERICHO.log"
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if "[JerAI] steer " not in line:
            continue
        m = STEER.search(line)
        if not m:
            continue
        g = m.groups()
        r = {"t": int(g[0]), "pad": int(g[1], 16), "dir": int(g[2]), "diff": int(g[3]),
             "pred": int(g[4]), "bias": int(g[5]), "spd": int(g[6]), "tgt": int(g[7]),
             "wa": int(g[8]), "la": int(g[9]), "aimd": int(g[10]), "off": int(g[12])}
        gm = GOAL.search(line)
        if gm:
            r["gx"], r["gz"], r["wp"] = int(gm.group(1)), int(gm.group(2)), int(gm.group(3))
        rows.append(r)

    if not rows:
        print(f"no '[JerAI] steer' lines in {path}")
        print("set steer_trace = 3 in JERICHO/CONFIG/JerAI.ini and run the game")
        return 2

    n = len(rows)
    moving = [r for r in rows if abs(r["spd"]) > 20]
    seq = [r["dir"] for r in moving]
    flips = sum(1 for a, b in zip(seq, seq[1:]) if a != b and a and b)
    goal_changes = sum(1 for a, b in zip(rows, rows[1:])
                       if "gx" in a and "gx" in b and (a["gx"], a["gz"]) != (b["gx"], b["gz"]))

    def pct(pred):
        return 100.0 * sum(1 for r in rows if pred(r)) / n

    print(f"{path}: {n} steer samples, ticks {rows[0]['t']}..{rows[-1]['t']}")
    print(f"  mean|diff| {st.mean(abs(r['diff']) for r in rows):6.0f}"
          f"   mean|pred| {st.mean(abs(r['pred']) for r in rows):6.0f}"
          f"   mean|wa| {st.mean(abs(r['wa']) for r in rows):6.0f}"
          f"   mean spd {st.mean(abs(r['spd']) for r in rows):5.0f}")
    print(f"  steer on {pct(lambda r: r['dir'] != 0):3.0f}%"
          f"   full lock {pct(lambda r: abs(r['wa']) >= 350):3.0f}%"
          f"   accel {pct(lambda r: r['pad'] & ACCEL):3.0f}%"
          f"   brake {pct(lambda r: r['pad'] & BRAKE):3.0f}%"
          f"   handbrake {pct(lambda r: r['pad'] & HB):3.0f}%")
    print(f"  flips: {flips} over {len(seq)} moving samples")
    print(f"  cross-track off: mean {st.mean(r['off'] for r in rows):5.0f}"
          f"  median {st.median([r['off'] for r in rows]):5.0f}"
          f"  max {max(r['off'] for r in rows)}")
    print(f"  look-ahead la {st.mean(r['la'] for r in rows):5.0f}"
          f"  -> aimd {st.mean(r['aimd'] for r in rows):5.0f}"
          f"   aimd > la in {pct(lambda r: r['aimd'] > r['la']):3.0f}%")
    if "wp" in rows[0]:
        wps = {}
        for r in rows:
            wps[r["wp"]] = wps.get(r["wp"], 0) + 1
        print(f"  waypoints {dict(sorted(wps.items()))}"
              f"   goal changes {goal_changes}"
              f" (one every {n / max(1, goal_changes):.0f} samples)")
    print()
    print("  |diff| bucket      n   mean|wa|  full_lock%  steer on%")
    for lo, hi in [(0, 30), (30, 70), (70, 150), (150, 400), (400, 99999)]:
        sel = [r for r in rows if lo <= abs(r["diff"]) < hi]
        if not sel:
            continue
        print(f"   {lo:>4}-{hi:<6} {len(sel):>6}"
              f"   {st.mean(abs(r['wa']) for r in sel):>8.0f}"
              f"  {100.0*sum(1 for r in sel if abs(r['wa'])>=350)/len(sel):>9.0f}%"
              f"  {100.0*sum(1 for r in sel if r['dir']!=0)/len(sel):>9.0f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
