FIREBIRD - one-car Driver 2 base bank
================================

Source: CHICAGO model 1
Destination model slot: 1
Texture sets: 1 65

This bank carries ONE car: clean, damaged and low are the donor's
three blocks. Every other model slot is -1.

Files:
  FIREBIRD.LEV        loadable car-data bank
  FIREBIRD.LCF        cosmetics (donor record at slot 1)
  FIREBIRD.json       manifest, accepted by exportd2 --rebuild-bank
  raw/models/   donor car blocks (clean/damaged/low)
  raw/pages/    page entries and PNG previews for sets 1 65
  raw/pallet.bin, raw/palette.png/.json, raw/cosmetics/

Install:
  1. Put this folder where cities.ini can reach it.
  2. Add the line from cities.ini under [cities] in
     JERICHO/CONFIG/cities.ini.
  3. Import it with import = <resident slot>:<city index>:<model>.
     city_N has city index 11 (11 + N).

Rebuild after editing the raw blocks:
  python exportd2.py --rebuild-bank FIREBIRD.json -o .
