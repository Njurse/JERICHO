"""Name the function owning an RVA, using an MSVC linker .map file.

Usage: python3 tools/map_lookup.py <file.map> <rva-in-hex> [more-rvas...]

Reads the "Publics by Value" table, which lists
    <section>:<offset>  <symbol>  <virtual address>  ...
and picks the symbol with the greatest address <= the target, so a crash
address can be attributed without a debugger.
"""
import re
import sys

LINE = re.compile(r"^\s+([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{16})")


def load(path):
    syms = []
    for line in open(path, "r", errors="replace"):
        m = LINE.match(line)
        if not m:
            continue
        va = int(m.group(4), 16)          # full virtual address
        syms.append((va, m.group(3), int(m.group(1), 16), int(m.group(2), 16)))
    syms.sort()
    return syms


def lookup(syms, va):
    best = None
    for s in syms:
        if s[0] <= va:
            best = s
        else:
            break
    return best


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    syms = load(argv[1])

    # the map lists VAs; a crash gives an RVA, so rebase onto the image base
    base = 0x140000000
    print("%d symbols from %s (image base 0x%X)" % (len(syms), argv[1], base))

    for a in argv[2:]:
        rva = int(a, 16) & 0xFFFFFFFF
        va = base + rva
        hit = lookup(syms, va)
        if hit is None:
            print("  0x%X -> (before the first symbol)" % rva)
        else:
            print("  0x%X -> %s  (+0x%X, section %d, offset 0x%X)"
                  % (rva, hit[1], va - hit[0], hit[2], hit[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
