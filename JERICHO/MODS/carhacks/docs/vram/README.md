# The CLUT column, checkpoint by checkpoint

Each row is one deliberate checkpoint from `tools/chk_vram_checkpoint.sh`,
with the image it produced in this folder. Row 1 is the **before** baseline.

| date | level | cities | CLUT rows used | safe free | largest free rect | commit | what changed |
|---|---|---|---|---|---|---|---|
| 2026-09-30 | LASVEGAS | 4 | 164 | 46 | (0,0) 0x0 = 0 KiB | 83a7256d | the flat 8-row reserve, and LASVEGAS' own pages needing 9 |
| 2026-09-30 | LASVEGAS | 4 | 169 | 41 | (0,0) 0x0 = 0 KiB | 5354f834 | the band reserves the level's own max rows (9 for LASVEGAS), not a flat 8; those clamps and the pin's font fallback are gone |
