#!/usr/bin/env python3
"""build_sound_bank.py - review, build and check Caine's Crossfire's sound bank.

The module plays its weapon, pickup and reticle sounds from ONE PSX dynamic bank
(sounds/weapons.sbk). A bank is:

    int32 sample count
    count x 16-byte (address, length, loop, rate) entries
    VAG/ADPCM payload

which is exactly what the engine's LoadSoundBankDynamic() appends into a
SOUND_BANK_* destination (see src_rebuild/Game/C/sound.c). The sample ORDER is
the ABI between this script and the module: cainescrossfire.h's CD2_SND_* enum
names the same slots (1-based; 0 = none), so do not reorder SOUND_SPECS without
updating the header.

Commands:

    python build_sound_bank.py --review    # measure the source WAVs, no writes
    python build_sound_bank.py --build     # write sounds/weapons.sbk
    python build_sound_bank.py --check     # decode the bank back and verify it
    python build_sound_bank.py --all       # build, then check

The encoder is the repo's bake/tools/common/vag.py - the same one the car banks
use - so a source WAV is levelled, looped and encoded the same way a car's
engine/idle loop is.
"""

import argparse
import os
import shutil
import struct
import sys
import wave
from pathlib import Path

MOD_ROOT = Path(__file__).resolve().parents[1]
JERICHO_ROOT = MOD_ROOT.parents[1]
BAKE_COMMON = JERICHO_ROOT / "MODS" / "bake" / "tools" / "common"

sys.path.insert(0, str(BAKE_COMMON))

import vag  # noqa: E402

SOUNDS = MOD_ROOT / "sounds"
BANK = SOUNDS / "weapons.sbk"
MANIFEST = SOUNDS / "weapons.manifest.txt"

# The bank's fixed order. `id` matches CD2_SND_* in cainescrossfire.h (the enum
# starts at 1; the bank offset is id - 1). `loop` samples are encoded with VAG
# loop flags and are meant to be stopped by the caller; every other sample is a
# one-shot. `optional` means the bank may omit it (only the LAST slot may be
# optional, or every later index would shift).
SOUND_SPECS = [
    {"id": "MG_LOOP", "file": "machine_guns.wav", "loop": True,
     "role": "machine gun / SMG held fire"},
    {"id": "MG_SHOT", "file": "machine_guns.wav", "loop": False,
     "role": "machine gun / SMG one-shot (AI + fallback)",
     "max_seconds": 0.55},
    {"id": "FREEZE_FIRE", "file": "freeze_fire.wav", "loop": False,
     "role": "freeze missile launch"},
    {"id": "FREEZE_IMPACT", "file": "freeze_impact.wav", "loop": False,
     "role": "freeze missile impact"},
    {"id": "FREEZE_TRAVEL", "file": "freeze_ray_travel.wav", "loop": True,
     "role": "freeze missile travel"},
    {"id": "EXPLOSION1", "file": "explosion1.wav", "loop": False,
     "role": "impact accent 1"},
    {"id": "EXPLOSION2", "file": "explosion2.wav", "loop": False,
     "role": "impact accent 2"},
    {"id": "EXPLOSION3", "file": "explosion3.wav", "loop": False,
     "role": "impact accent 3"},
    {"id": "EXPLOSION4", "file": "explosion4.wav", "loop": False,
     "role": "impact accent 4"},
    {"id": "PICKUP_WEAPON", "file": "pickup_weapon.wav", "loop": False,
     "role": "weapon pickup"},
    {"id": "PICKUP_HEALTH", "file": "pickup_health.wav", "loop": False,
     "role": "health pickup"},
    {"id": "RETICLE_SELECTED", "file": "reticle_selected.wav", "loop": False,
     "role": "Fixer lock acquired"},
    {"id": "RETICLE_CHARGE", "file": "reticle_charge.wav", "loop": True,
     "role": "Fixer charge loop (pitch ramps)"},
    {"id": "RETICLE_FULL", "file": "reticle_full.wav", "loop": False,
     "role": "Fixer full charge / fire cue"},
    {"id": "SCREAM", "file": "scream.wav", "loop": False,
     "role": "weapon kill"},
    {"id": "SNIPER_SHOT", "file": "gun_sniper_rifle_shot.wav", "loop": False,
     "role": "sniper shot (optional, fixed to the LAST slot)",
     "optional": True},
]

# The starting per-weapon assignment. The bank is data; the CD2_WEAPON_DEF rows
# carry the final values, so this table is the intent the C side implements and
# can be retuned without touching the encoder. "-" = keep the weapon's stock
# sample; "proposed" = a first assignment, not a source-named one.
WEAPON_MAP = [
    ("MG", "MG_LOOP held / MG_SHOT one-shot", "player loop + AI/fallback"),
    ("SMG", "MG_LOOP at a higher pitch / MG_SHOT", "player burst + AI"),
    ("FREEZE", "FREEZE_FIRE / FREEZE_TRAVEL / FREEZE_IMPACT",
     "launch / projectile loop / impact"),
    ("MISSILE", "EXPLOSION1", "proposed impact accent"),
    ("SEEKER", "EXPLOSION2", "proposed impact accent"),
    ("CLUSTER", "EXPLOSION3 parent / EXPLOSION2 bomblets", "proposed impact accents"),
    ("ZOOMY", "EXPLOSION2", "proposed impact accent"),
    ("MINE", "EXPLOSION4", "proposed impact accent"),
    ("SHOTGUN", "MG_SHOT pitched down", "proposed fire sound"),
    ("FIXER", "SNIPER_SHOT if converted; RETICLE_* for the charge",
     "fire sound + charge UI"),
    ("pickups", "PICKUP_WEAPON / PICKUP_HEALTH", "one-shot at the player"),
    ("kill", "SCREAM", "one-shot at the victim"),
]

# Files deliberately NOT in this bank: an engine loop belongs to the vehicle
# bank, and a compressed source cannot be encoded without a decoder.
EXCLUDED = {
    "meldedv8.wav": "engine loop - the vehicle bank's sample, not a weapon sound",
    "gun-sniper-rifle-shot.mp3": "compressed source - needs an MP3 decoder or a WAV export",
}


def wav_info(path):
    """(channels, width, rate, frames, samples) for a PCM WAV."""
    with wave.open(str(path), "rb") as w:
        channels = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        frames = w.getnframes()

    samples, rate2 = vag.read_wav(str(path))
    return channels, width, rate, frames, samples, rate2


def level_and_encode(path, loop, max_seconds=None):
    """Load, trim, level and encode one source. Returns a dict or None."""
    channels, width, rate, frames, samples, read_rate = wav_info(path)

    if read_rate:
        rate = read_rate

    if max_seconds and len(samples) > int(max_seconds * rate):
        samples = samples[:int(max_seconds * rate)]

    rate, clamped = vag.fit_rate(rate)

    if clamped:
        samples = vag.resample(samples, read_rate, clamped)

    samples = vag.remove_dc(samples)
    before = vag.stats(samples)
    samples, _before, applied = vag.level(samples, peak=vag.DEFAULT_PEAK,
                                          rms=vag.DEFAULT_RMS)

    loop_info = None

    if loop:
        loop_info = vag.find_loop(samples, search_from=0.0, rate=rate)

        if loop_info:
            data, count, err = vag.encode_settled(
                samples, loop=True, loop_from=loop_info["start"],
                loop_end=loop_info["end"])
        else:
            data, count, err = vag.encode_settled(samples, loop=True)
    else:
        # a one-shot that ends mid-waveform clicks; a short fade lands it on zero
        seconds = len(samples) / float(rate) if rate else 0.0

        if seconds > 0.05:
            samples = vag.fade_out(samples, min(0.02, seconds / 4.0), rate)

        data, count, err = vag.encode_settled(samples, loop=False)

    after = vag.stats(vag.decode(data, len(samples)))

    return {
        "source": path,
        "channels": channels,
        "width": width,
        "rate": rate,
        "seconds": len(samples) / float(rate) if rate else 0.0,
        "samples": len(samples),
        "before": before,
        "applied": applied,
        "after": after,
        "loop": loop,
        "loop_info": loop_info,
        "data": data,
        "count": count,
        "err": err,
    }


def review():
    """Measure every source and every proposed loop. Writes nothing."""
    print("Caine's Crossfire sound review")
    print("sounds: %s" % SOUNDS)
    print()

    seen = {}
    missing = []

    for spec in SOUND_SPECS:
        path = SOUNDS / spec["file"]

        if path in seen:
            continue

        if not path.is_file():
            if not spec.get("optional"):
                missing.append(spec["file"])
            continue

        channels, width, rate, frames, samples, read_rate = wav_info(path)
        st = vag.stats(samples)
        secs = len(samples) / float(read_rate) if read_rate else 0.0

        print("%-26s %d ch, %d-bit, %5d Hz, %6.3fs" %
              (spec["file"], channels, width * 8, read_rate, secs))
        print("  peak %d (%.0f%%)  rms %.0f%%  crest %.1f  dc %+.1f  clipped %d%s" %
              (st["peak"], 100.0 * st["peak"] / 32767.0,
               100.0 * st["rms"] / 32767.0, st["crest"], st["dc"],
               st["clipped"], ", OVER" if st["over"] else ""))

        if spec["loop"]:
            info = vag.find_loop(samples, search_from=0.0, rate=read_rate)

            if info:
                print("  loop: start %.3fs end %.3fs (seam %d, %.2fs loop)" %
                      (info["start"] / float(read_rate),
                       info["end"] / float(read_rate), info["seam"],
                       info["seconds"]))
            else:
                print("  loop: NO candidate (too short for a block-aligned loop)")

        seen[path] = spec

    print()

    for name, reason in EXCLUDED.items():
        path = SOUNDS / name

        if path.is_file():
            print("%-26s EXCLUDED: %s" % (name, reason))

    for path in sorted(SOUNDS.glob("*")):
        if path.suffix.lower() not in (".wav", ".mp3"):
            continue
        if path in seen or path.name in EXCLUDED:
            continue
        if path.name == "weapons.sbk":
            continue
        print("%-26s UNASSIGNED (not in the bank order)" % path.name)

    if missing:
        print()
        print("MISSING required source(s): %s" % ", ".join(missing))

    print()
    print("Bank order (offset = index, id = index + 1):")

    for i, spec in enumerate(SOUND_SPECS):
        print("  %2d  %-16s %-26s %s%s" %
              (i, spec["id"], spec["file"], spec["role"],
               " [optional]" if spec.get("optional") else ""))

    print()
    print("Weapon mapping (proposed; the C defs carry the final values):")

    for weapon, sound, note in WEAPON_MAP:
        print("  %-8s %-48s %s" % (weapon, sound, note))


def convert_sniper():
    """Try to produce gun_sniper_rifle_shot.wav from the MP3. Returns a path."""
    for name in ("gun_sniper_rifle_shot.wav", "sniper_shot.wav"):
        path = SOUNDS / name

        if path.is_file():
            return path

    mp3 = SOUNDS / "gun-sniper-rifle-shot.mp3"

    if not mp3.is_file():
        return None

    out = SOUNDS / "gun_sniper_rifle_shot.wav"

    ffmpeg = shutil.which("ffmpeg")

    if ffmpeg:
        import subprocess
        rc = subprocess.call([ffmpeg, "-y", "-i", str(mp3), "-ac", "1",
                              "-ar", "44100", "-c:a", "pcm_s16le", str(out)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if rc == 0 and out.is_file():
            print("converted %s -> %s (ffmpeg)" % (mp3.name, out.name))
            return out

    try:
        import soundfile  # type: ignore
        data, rate = soundfile.read(str(mp3), dtype="int16", always_2d=True)
        mono = data.mean(axis=1).astype("int16")
        vag.write_wav(str(out), [int(v) for v in mono], int(rate))
        print("converted %s -> %s (soundfile)" % (mp3.name, out.name))
        return out
    except Exception as exc:  # noqa: BLE001 - the converter is best-effort
        print("NOTE: %s is not PCM and could not be converted (%s)." %
              (mp3.name, exc))
        print("      Export it as a mono 16-bit WAV named %s to include it." % out.name)

    return None


def build():
    built = []
    payload = bytearray()

    for index, spec in enumerate(SOUND_SPECS):
        path = SOUNDS / spec["file"]

        if not path.is_file() and spec["id"] == "SNIPER_SHOT":
            path = convert_sniper()

            if path is None:
                print("bank: omitting %s (slot %d) - no sniper source" %
                      (spec["id"], index))
                continue

        if not path.is_file():
            raise SystemExit("missing sound source: %s (%s)" % (path, spec["id"]))

        info = level_and_encode(path, spec["loop"], spec.get("max_seconds"))
        addr = len(payload)
        payload.extend(info["data"])
        built.append({"index": index, "spec": spec, "path": path,
                      "info": info, "addr": addr})

        extra = ""
        if info["loop_info"]:
            extra = " loop %.3f-%.3fs seam %d" % (
                info["loop_info"]["start"] / float(info["rate"]),
                info["loop_info"]["end"] / float(info["rate"]),
                info["loop_info"]["seam"])

        print("bank %2d %-16s %-26s %6d B %5d Hz %5.3fs%s" %
              (index, spec["id"], path.name, len(info["data"]), info["rate"],
               info["seconds"], extra))

    bank = bytearray(struct.pack("<i", len(built)))

    for item in built:
        bank += struct.pack("<IIII", item["addr"], len(item["info"]["data"]),
                            1 if item["spec"]["loop"] else 0,
                            item["info"]["rate"])

    bank += payload
    BANK.write_bytes(bytes(bank))

    lines = ["# Caine's Crossfire weapon bank - generated by tools/build_sound_bank.py",
             "# offset id file loop rate seconds role"]

    for item in built:
        info = item["info"]
        lines.append("%2d %-16s %-26s %d %5d %5.3f %s" %
                     (item["index"], item["spec"]["id"], item["path"].name,
                      1 if item["spec"]["loop"] else 0, info["rate"],
                      info["seconds"], item["spec"]["role"]))

    MANIFEST.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print()
    print("wrote %s: %d sample(s), %d bytes" %
          (BANK, len(built), len(bank)))
    print("wrote %s" % MANIFEST)


def check():
    if not BANK.is_file():
        raise SystemExit("no bank at %s (run --build)" % BANK)

    blob = BANK.read_bytes()

    if len(blob) < 4:
        raise SystemExit("bank is too short")

    count = struct.unpack_from("<i", blob, 0)[0]

    if count <= 0 or count > 64:
        raise SystemExit("bad sample count %d" % count)

    header = 4 + count * 16

    if header > len(blob):
        raise SystemExit("truncated header: %d samples need %d bytes" %
                         (count, header))

    entries = [struct.unpack_from("<IIII", blob, 4 + i * 16) for i in range(count)]
    payload = blob[header:]
    ok = True

    print("bank: %s" % BANK)
    print("  samples %d, header %d, payload %d, total %d" %
          (count, header, len(payload), len(blob)))

    for i, (addr, length, loop, rate) in enumerate(entries):
        bad = []

        if length <= 0 or length % vag.BLOCK_BYTES:
            bad.append("length")
        if addr + length > len(payload):
            bad.append("range")
        if rate < vag.MIN_RATE or rate > vag.MAX_RATE:
            bad.append("rate")

        data = payload[addr:addr + length]
        flags = [data[b] for b in range(1, len(data), vag.BLOCK_BYTES)]
        has_start = any(f & 0x04 for f in flags)
        has_end = bool(flags and (flags[-1] & 0x03))
        has_once = bool(flags and (flags[-1] & 0x01))

        if loop:
            if not has_start:
                bad.append("no loop start")
            if not has_end:
                bad.append("no loop end")
        else:
            if has_start:
                bad.append("unexpected loop start")
            if not has_once:
                bad.append("no one-shot end")

        if bad:
            ok = False

        spec = SOUND_SPECS[i]["id"] if i < len(SOUND_SPECS) else "?"
        print("  %2d %-16s addr %6d len %6d rate %5d loop %d %s%s" %
              (i, spec, addr, length, rate, loop,
               "OK" if not bad else "BAD: " + ", ".join(bad),
               "" if spec != "SNIPER_SHOT" or i < count else " (absent)"))

    if not ok:
        raise SystemExit("bank check FAILED")

    print("  check: OK")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--review", action="store_true",
                    help="measure the source WAVs (no writes)")
    ap.add_argument("--build", action="store_true",
                    help="write sounds/weapons.sbk + manifest")
    ap.add_argument("--check", action="store_true",
                    help="decode the bank back and verify it")
    ap.add_argument("--all", action="store_true",
                    help="--build then --check")
    args = ap.parse_args(argv)

    if args.review:
        review()
    if args.build or args.all:
        build()
    if args.check or args.all:
        check()
    if not (args.review or args.build or args.check or args.all):
        ap.print_help()
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
