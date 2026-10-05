#!/usr/bin/env python3
"""Translate a car slot between the ORIGINAL game and the transplant.

A Driver 1 car's slot number is not the slot the engine finds it on. The
transplant lays each city's cars onto Driver 2's compliant models, in the order
the cars appear in the original level, because Driver 1's own numbers (0,1,2,5,6,7,8)
collide with the three models Driver 2 has no data for (5,6,7). So "Newcastle slot
1" is really model 1, and "Havana slot 5" is really model 4.

The mapping is not re-derived here: it is read from the sidecar cosmetics1.py
writes beside each .LCF (tools/cosmetics1.py), which is checked against the blob's
own model table when it is produced.

  python slotmap.py <car-city>              list what that city offers
  python slotmap.py <car-city> <slot>       the model that slot became
  python slotmap.py --model <car-city> <slot>   JUST the number, for a caller

A batch file cannot parse prose, so --model prints the model number alone (and
still exits 2 when the slot does not exist). Anything else and the caller ends up
counting words in a sentence, which is how the first version of this got the model
from the wrong token.

Exit 0 when the slot resolves, 2 when it does not - so a caller can refuse
instead of launching a car that will silently fall back to the level's own.

A Driver 2 source city has no sidecar: its car-data table is indexed by model
already, so the number given IS the model.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LCF_DIR = os.path.abspath(os.path.join(
    HERE, "..", "..", "..", "MODS", "d1cars", "tools", "out", "lcf"))

D2_CITIES = ("CHICAGO", "HAVANA", "VEGAS", "RIO")
# Driver 2's own model numbers - the ones its four cities ship and its frontend
# offers. A raw number for a D2 source city must be one of these.
D2_MODELS = (0, 1, 2, 3, 4, 8, 9, 10, 11, 12)


def sidecar(city):
    path = os.path.join(LCF_DIR, "%s.cosmetics.json" % city)
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)


def pairs(city):
    """[(original slot, transplanted model)] for a Driver 1 car-data city."""
    doc = sidecar(city)
    if not doc:
        return None
    return [(int(a), int(b)) for a, b in doc.get("slot_map", [])]


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2

    bare = False
    if argv[1] == "--model":
        bare = True
        argv = [argv[0]] + argv[2:]

    city = argv[1].upper()

    def say(text):
        """Human words normally; the bare number when a caller asked for it."""
        if not bare:
            print(text)


    if city in D2_CITIES:
        if len(argv) < 3:
            print("%s is a Driver 2 city: its car-data table is indexed by MODEL, so"
                  % city)
            print("the number you give is the model. It ships: %s"
                  % ", ".join(str(m) for m in D2_MODELS))
            return 0
        try:
            model = int(argv[2])
        except ValueError:
            print("%s: '%s' is not a number" % (city, argv[2]))
            return 2
        if model not in D2_MODELS:
            print("%s does not ship model %d (models 5, 6 and 7 exist in NO city)"
                  % (city, model))
            return 2
        say("%s model %d" % (city, model))
        if bare:
            print(model)
        return 0

    p = pairs(city)

    if p is None:
        print("no slot map for '%s' - is it a car-data city? Known: %s"
              % (city, ", ".join(sorted(D2_CITIES))))
        return 2

    if len(argv) < 3:
        print("%s offers %d car(s):" % (city, len(p)))
        for src, dst in p:
            print("   original slot %-3d -> model %d" % (src, dst))
        print("Give one of the ORIGINAL slot numbers.")
        return 0

    try:
        slot = int(argv[2])
    except ValueError:
        print("%s: '%s' is not a number" % (city, argv[2]))
        return 2

    for src, dst in p:
        if src == slot:
            say("%s original slot %d -> model %d" % (city, src, dst))
            if bare:
                print(dst)
            return 0

    print("%s has no car in original slot %d. It offers:" % (city, slot))
    for src, dst in p:
        print("   original slot %-3d -> model %d" % (src, dst))
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
