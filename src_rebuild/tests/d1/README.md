# d1test — headless Driver 1 validation harness

A **no-window, no-SDL, no-controller** console tool that exercises the real
`jer_d1` library (the same code the game links) against the extracted
Driver 1 (PS1) `.LEV` files. You can run it while the game — or any other
game — is open; it never touches the GPU, the audio device, or your
controller.

## Run

```
cd src_rebuild/bin/Release_dev        # where the DRIVER\ data folder lives
..\..\tests\d1\d1test.bat
```

The batch builds `d1test.exe` (MSVC, console) and runs it from the game
data folder. Expected result:

```
=== ALL PASS (0 failures) ===
```

## What it validates

| Test | What it proves |
|---|---|
| city tables | `jer_d1_level_file/name/folder/available` |
| LEV header | lump 37 + the 4 citylump offsets (identical to D2's container) |
| loadtime walk | D1 lump count = 11, types `[5,34,12,7,25,2,22x4,28]` (all 4 cities) |
| inmem walk | types `[1,26,8,9,16,17,20,10,24,33,21]` |
| map header | cells/cell-size/region-count/region-size per city |
| spoolinfo | parses byte-exact with the D2 layout; region offsets == map regions |
| region spool | roadm/roadh RLE decode -> 3600 cells/barrel, sane heights/types |
| cell objects | 16-byte D1 objects -> 8-byte packed (model index, yang, pos) |
| surface query | `jer_d1_find_surface` returns real street heights (Miami y≈28) |

## Files

- `d1test.c` — the harness (includes `jer_d1.c` so it tests the real code)
- `d1stubs.c` — minimal stubs for the engine symbols `jer_d1.c` touches
- `d1test.bat` — build + run

## Adding checks

Extend `main()` in `d1test.c`. Format ground truth lives in
`docs/driver1-cities.md`.
