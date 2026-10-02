"""make_vram_issues_png.py - the VRAM layout, drawn from a REAL vram_dump.tga.

Every number and rectangle in the picture comes from the dump itself plus
tools/vrammap.py's claims table -- nothing here is illustrative:

  display buffers  x0..319, y0..511, double buffered   320 KiB
  texture area     x320..1023, y0..511                 704 KiB
    page slots     19 x 64x256                         608 KiB
    CLUT column    x960..1023, y256..511                32 KiB
    sky            x320..448, y0..256                   64 KiB
  THE ARENA        x0..1023, y512..1023               1024 KiB  <- JERICHO's alone
    arena pages    30 x 64x256 (rows 512 and 768)
    arena CLUTs    x960..1023, y512..1023             512 rows

The buffer is 2 MiB because it is 1024x1024: rows 512..1023 are addressable (the tpage
word carries page-Y bit 9 as bit 11) and no stock path computes a rectangle down there, so
the arena is where the import lives now.

The two things this figure used to be about are KEPT, because they are why the arena
exists - but they are marked as what they are: history.
  level font image rows 466..511   -> was written over by the import; now 0 differing
                                      texels vs a stock run (measured)
  import pin band  rows 480..511   -> was ENTIRELY inside the font; now deleted

Run from the checkout root:  python3 <this> [dump.tga] [out.png]
"""
import sys
import os
from PIL import Image, ImageDraw, ImageFont

# Args: [SRC] [OUT] [--stamp a|b|c|d|e] [--checkpoint TAG]
#   The stamp is "<date>|<level>|<cities>|<rows used>|<safe free>" — the run's
#   identity and its measured numbers, drawn INTO the figure so an archived image
#   can never be orphaned or mislabelled.
_positional = []
STAMP, CHECKPOINT = None, None
_argv = sys.argv[1:]
_i = 0
while _i < len(_argv):
    if _argv[_i] == "--stamp" and _i + 1 < len(_argv):
        STAMP = _argv[_i + 1]; _i += 2; continue
    if _argv[_i] == "--checkpoint" and _i + 1 < len(_argv):
        CHECKPOINT = _argv[_i + 1]; _i += 2; continue
    _positional.append(_argv[_i]); _i += 1

SRC = _positional[0] if len(_positional) > 0 else "src_rebuild/bin/Release_dev/vram_dump.tga"
OUT = _positional[1] if len(_positional) > 1 else \
    "JERICHO/MODS/carhacks/docs/vram-issues.png"

STAMP_DATE = STAMP_LEVEL = ""
STAMP_LINE = ""
if STAMP:
    _f = STAMP.split("|")
    if len(_f) < 5:
        sys.exit("--stamp wants <date>|<level>|<cities>|<rows used>|<safe free>")
    STAMP_DATE, STAMP_LEVEL = _f[0], _f[1]
    STAMP_LINE = ("checkpoint %s - %s, %s cities mixed - base-half column: %s CLUT rows "
                  "used, %s safe free" % (_f[0], _f[1], _f[2], _f[3], _f[4]))

HALF = 512                                     # JER_VRAM_HALF_Y
CLUT_X, CLUT_Y0, CLUT_Y1 = 960, 256, 511       # the base CLUT column, from vrammap.py
FONT_Y0, FONT_Y1 = 466, 511                    # the level font image
SAFE_LAST = 465                                # CD2_CLUT_SAFE_LAST
CDICON_Y0, CDICON_Y1 = 433, 464                # CD icon (spool)
PIN_Y0, PIN_Y1 = 480, 511                      # the import pin band (DELETED - history)

WHITE = (255, 255, 255)
INK = (24, 24, 28)
GREY = (150, 150, 158)
RED = (192, 40, 40)
BLUE = (40, 90, 190)
GREEN = (30, 140, 70)
AMBER = (200, 130, 20)
PURPLE = (120, 60, 160)
TEAL = (0, 130, 140)

font = None
for name in ("segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"):
    try:
        font = ImageFont.truetype(name, 13)
        big = ImageFont.truetype(name, 17)
        small = ImageFont.truetype(name, 11)
        break
    except OSError:
        continue
if font is None:
    font = big = small = ImageFont.load_default()

vram = Image.open(SRC).convert("RGB")
if vram.size != (1024, 1024):
    vram = vram.resize((1024, 1024), Image.NEAREST)

# The "what this means" block lives in the LEFT column, under the legend, so it can
# never run off the bottom of the sheet.
NOTES = (
    (TEAL,  "the arena: imported pages and CLUTs are rows 512..1023 - 30 pages, 512 CLUT rows"),
    (GREEN, "measured: world pages evicted 702 -> 0; imported sets lost 5 -> 0"),
    (GREEN, "measured: level font rows 466..511 - 0 differing texels vs a stock run"),
    (GREEN, "measured: 9 of 9 imported pages at y=512, x from 0; CLUTs y512..539"),
    (GREEN, "now 3 guest cities get their own civ_clut block: the 3rd used to wear the host's"),
    (RED,   "the pin band below is HISTORY: rows 480..511, ENTIRELY inside the font"),
    (RED,   "the import no longer takes a row from this column at all"),
    (AMBER, "what is left is page IDENTITY, not space - docs/VRAM.md section 7"),
)
BLOCK_Y = 646                         # BELOW the panels: nothing to collide with
BLOCK_H = 19 + 15 * len(NOTES)        # one title line, then one per entry

# The sheet grows if the block ever outgrows it, so text cannot be clipped again.
W, H = 1400, max(660, BLOCK_Y + BLOCK_H + 16)
sheet = Image.new("RGB", (W, H), WHITE)
d = ImageDraw.Draw(sheet)

d.text((20, 14), "PSX VRAM 2 MiB - and the arena that gave the CLUT column back",
       fill=INK, font=big)
d.text((20, 38), "measured from a live vram_dump.tga of a 3-city mashup run; "
                 "rectangles from tools/vrammap.py", fill=GREY, font=small)
if STAMP_LINE:
    d.text((20, 56), STAMP_LINE, fill=RED, font=small)

# ---- left: the whole 2 MiB, with the regions outlined ----------------------
SX, SY, SC = 20, 76, 0.25                     # 1024x1024 -> 256x256
thumb = vram.resize((int(1024 * SC), int(1024 * SC)), Image.NEAREST)
sheet.paste(thumb, (SX, SY))
d.rectangle([SX, SY, SX + 256, SY + 256], outline=INK)

def box(x0, y0, x1, y1, col, wd=2):
    d.rectangle([SX + x0 * SC, SY + y0 * SC, SX + x1 * SC - 1, SY + y1 * SC - 1],
                outline=col, width=wd)

box(0, 0, 320, 512, GREY)                     # display buffers
box(320, 0, 1024, 512, BLUE)                  # texture area (base half)
box(960, 256, 1024, 512, RED)                 # CLUT column
box(320, 0, 448, 256, GREEN)                  # sky
box(0, 512, 1024, 1024, TEAL)                 # THE ARENA
d.text((SX + 3, SY + 3), "display buffers 320 KiB", fill=GREY, font=small)
d.text((SX + 85, SY + 3), "texture area 704 KiB", fill=BLUE, font=small)
d.text((SX + 85, SY + 16), "sky 64 KiB", fill=GREEN, font=small)
d.text((SX + 3, SY + 132), "THE ARENA rows 512..1023 - 1024 KiB",
       fill=TEAL, font=small)
d.text((SX + 3, SY + 145), "imported pages + CLUTs live here", fill=TEAL, font=small)
d.text((SX + 96, SY + 88), "CLUT column", fill=RED, font=small)
d.line([SX + 160, SY + 94, SX + 240, SY + 96], fill=RED)

y = SY + 266
d.text((SX, y), "the 2 MiB, as measured:", fill=INK, font=font); y += 18
for line in (
    "base   x0..1023, y0..511    1 MiB  the game's, unchanged",
    "  display buffers x0..319    320 KiB  (double buffered)",
    "  texture area    x320..1023 704 KiB",
    "    page slots    19 x 64x256    608 KiB",
    "    sky           x320..448     64 KiB",
    "    CLUT column   x960..1023    32 KiB  <- 256 rows, the level's again",
    "ARENA  x0..1023, y512..1023  1 MiB  JERICHO's alone, nobody else computes it",
):
    d.text((SX + 8, y), line, fill=INK, font=small); y += 15

# ---- middle: the base CLUT column at 2x -------------------------------------
ZX, ZY, ZC = 470, 90, 2
strip = vram.crop((CLUT_X, CLUT_Y0, CLUT_X + 64, CLUT_Y1 + 1))
sheet.paste(strip.resize((64 * ZC, 256 * ZC), Image.NEAREST), (ZX, ZY))
d.rectangle([ZX, ZY, ZX + 64 * ZC, ZY + 256 * ZC], outline=INK, width=2)

def row(y):                                    # VRAM row -> pixel
    return ZY + (y - CLUT_Y0) * ZC

def band(y0, y1, col, label, lx=None):
    d.rectangle([ZX, row(y0), ZX + 64 * ZC, row(y1 + 1) - 1], outline=col, width=2)
    d.text((lx if lx else ZX + 64 * ZC + 8, (row(y0) + row(y1)) // 2 - 7),
           label, fill=col, font=small)

band(CLUT_Y0, CDICON_Y0 - 1, GREEN, "the level's own pages")
band(CDICON_Y0, CDICON_Y1, AMBER, "CD icon (spool)")
band(FONT_Y0, FONT_Y1, RED, "level font image 466..511 - untouched now")
band(PIN_Y0, PIN_Y1, PURPLE, "the import's OLD pin band 480..511 (HISTORY)")
d.line([ZX, row(SAFE_LAST + 1), ZX + 64 * ZC, row(SAFE_LAST + 1)], fill=RED, width=2)
d.text((ZX - 52, row(SAFE_LAST + 1) - 7), "465", fill=RED, font=small)

d.text((ZX, ZY - 20), "the base CLUT column, 64x256 at 2x (x960..1023)",
       fill=INK, font=font)
d.text((ZX, ZY + 256 * ZC + 8),
       "The import used to be pinned into rows 480..511 - inside the font.",
       fill=RED, font=small)
d.text((ZX, ZY + 256 * ZC + 23),
       "It now takes nothing from this column: the level keeps all of it.",
       fill=GREEN, font=small)

# ---- right: the arena's own CLUT column, 1x ---------------------------------
AX, AY, AC = 980, 90, 1
astrip = vram.crop((CLUT_X, HALF, CLUT_X + 64, 1024))
sheet.paste(astrip.resize((64 * AC, 512 * AC), Image.NEAREST), (AX, AY))
d.rectangle([AX, AY, AX + 64 * AC, AY + 512 * AC], outline=INK, width=2)
# The 3-city mix's own arena CLUT rows, measured from the pinned rects in the log.
d.rectangle([AX, AY, AX + 64 * AC, AY + 28 * AC], outline=TEAL, width=2)
d.text((AX + 64 * AC + 10, AY + 4),
       "imported CLUT rows - y512..539", fill=TEAL, font=small)
d.text((AX + 64 * AC + 10, AY + 19),
       "(28 of 512; the rest is free)", fill=TEAL, font=small)
d.text((AX + 64 * AC + 10, AY + 512 - 30),
       "512 rows, mirrored at the same x,", fill=INK, font=small)
d.text((AX + 64 * AC + 10, AY + 512 - 15),
       "so IncrementClutNum walks it unchanged", fill=INK, font=small)

d.text((AX, AY - 20), "the ARENA's CLUT column (x960..1023, y512..1023)",
       fill=INK, font=font)

# ---- what that means (left column, under the legend) ------------------------
y = BLOCK_Y
d.text((SX, y), "what changed, and what is still open:", fill=INK, font=font)
y += 19
for col, line in NOTES:
    d.text((SX + 8, y), line, fill=col, font=small); y += 15
assert y <= H, "the sheet is too short for the block: %d > %d" % (y, H)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
sheet.save(OUT)
print("wrote", OUT, sheet.size)

# --checkpoint archives this figure as a dated breadcrumb of the CLUT work, so the
# trail is a series you can read back: date, the run it was, and its own numbers.
if CHECKPOINT:
    if not (STAMP_DATE and STAMP_LEVEL):
        sys.exit("--checkpoint needs --stamp <date>|<level>|<cities>|<rows>|<free>")
    _arch = os.path.join(os.path.dirname(OUT), "vram",
                         "%s-%s-%s.png" % (STAMP_DATE, STAMP_LEVEL.lower(), CHECKPOINT))
    os.makedirs(os.path.dirname(_arch), exist_ok=True)
    sheet.save(_arch)
    print("archived", _arch)
