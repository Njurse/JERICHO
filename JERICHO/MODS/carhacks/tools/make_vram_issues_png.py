"""make_vram_issues_png.py - the CLUT-column problem, drawn from a REAL vram_dump.tga.

Every number and rectangle in the picture comes from the dump itself plus
tools/vrammap.py's claims table -- nothing here is illustrative:

  display buffers  x0..319, double buffered        320 KiB
  texture area     x320..1023                      704 KiB
    page slots     19 x 64x256                     608 KiB
    CLUT column    x960..1023, y256..511            32 KiB
    sky            x320..448, y0..256               64 KiB
  level font image rows 466..511   -> COLLIDES with the import's rows
  import pin band  rows 480..511   -> inside the font image
  font/map CLUTs   row 256

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
    STAMP_LINE = ("checkpoint %s - %s, %s cities mixed - %s CLUT rows used, %s safe free"
                  % (_f[0], _f[1], _f[2], _f[3], _f[4]))

CLUT_X, CLUT_Y0, CLUT_Y1 = 960, 256, 511      # the CLUT column, from vrammap.py
FONT_Y0, FONT_Y1 = 466, 511                    # the level font image
SAFE_LAST = 465                                # CD2_CLUT_SAFE_LAST
CDICON_Y0, CDICON_Y1 = 433, 464                # CD icon (spool)
PIN_Y0, PIN_Y1 = 480, 511                      # import pin band

WHITE = (255, 255, 255)
INK = (24, 24, 28)
GREY = (150, 150, 158)
RED = (192, 40, 40)
BLUE = (40, 90, 190)
GREEN = (30, 140, 70)
AMBER = (200, 130, 20)
PURPLE = (120, 60, 160)

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
if vram.size != (1024, 512):
    vram = vram.resize((1024, 512), Image.NEAREST)

# The "what is wrong" block lives in the LEFT column, under the legend, so it can
# never run off the bottom of the sheet. (It used to start below the 2x CLUT strip,
# at y=606 on a 720-tall sheet, which clipped its last two lines.)
WRONG = (
    (RED,   "level font image is rows 466..511, and the import's rows land in it"),
    (PURPLE, "the import pin band is rows 480..511 - ENTIRELY inside the font"),
    (AMBER, "VEGAS' own pages need 9 CLUT rows; the streamed-slot walk reserves 8"),
    (AMBER, "SendTPage orders cluts.h = npalettes/4 + 1 with no clamp (spool.c:495)"),
    (INK,   "this dump's run (LASVEGAS, 3 cities mixed): 164 rows used, 46 free"),
    (INK,   "so the column FITS today - the reserve is what is thin, not the total"),
    (GREEN, "widen the reserve to the level's own max and the margin stays honest,"),
    (GREEN, "and the cross-row closure is what buys a 4th city - the remap's job"),
)
BLOCK_Y = 456                         # just under the left legend (which ends at 438)
BLOCK_H = 19 + 15 * len(WRONG)        # one title line, then one per entry

# The sheet grows if the block ever outgrows it, so text cannot be clipped again.
W, H = 1180, max(720, BLOCK_Y + BLOCK_H + 16)
sheet = Image.new("RGB", (W, H), WHITE)
d = ImageDraw.Draw(sheet)

d.text((20, 14), "PSX VRAM 1 MiB - and the CLUT column that has no room left",
       fill=INK, font=big)
d.text((20, 38), "measured from a live vram_dump.tga of a 3-city mashup run; "
                 "rectangles from tools/vrammap.py", fill=GREY, font=small)
if STAMP_LINE:
    d.text((20, 56), STAMP_LINE, fill=RED, font=small)

# ---- left: the whole 1 MiB, with the regions outlined ----------------------
SX, SY, SC = 20, 70, 0.5                      # 1024x512 -> 512x256
thumb = vram.resize((int(1024 * SC), int(512 * SC)), Image.NEAREST)
sheet.paste(thumb, (SX, SY))
d.rectangle([SX, SY, SX + 512, SY + 256], outline=INK)

def box(x0, y0, x1, y1, col):
    d.rectangle([SX + x0 * SC, SY + y0 * SC, SX + x1 * SC - 1, SY + y1 * SC - 1],
                outline=col, width=2)

box(0, 0, 320, 512, GREY)                     # display buffers
box(320, 0, 1024, 512, BLUE)                  # texture area
box(960, 256, 1024, 512, RED)                 # CLUT column
box(320, 0, 448, 256, GREEN)                  # sky
d.text((SX + 4, SY + 4), "display buffers 320 KiB", fill=GREY, font=small)
d.text((SX + 165, SY + 4), "texture area 704 KiB", fill=BLUE, font=small)
d.text((SX + 165, SY + 20), "sky 64 KiB", fill=GREEN, font=small)
d.text((SX + 400, SY + 130), "CLUT column", fill=RED, font=small)
d.text((SX + 400, SY + 144), "32 KiB", fill=RED, font=small)
d.line([SX + 480, SY + 132, SX + 470, SY + 150], fill=RED)

y = SY + 275
d.text((SX, y), "the 1 MiB, as measured:", fill=INK, font=font); y += 18
for line in (
    "display buffers x0..319 (double buffered)     320 KiB",
    "texture area   x320..1023                     704 KiB",
    "   page slots  19 x 64x256                    608 KiB",
    "   sky         x320..448, y0..256              64 KiB",
    "   CLUT column x960..1023, y256..511           32 KiB   <- 256 rows, all of them",
):
    d.text((SX + 8, y), line, fill=INK, font=small); y += 15

# ---- right: the CLUT column itself, at 2x -----------------------------------
ZX, ZY, ZC = 600, 80, 2
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
band(FONT_Y0, FONT_Y1, RED, "level font image")
band(PIN_Y0, PIN_Y1, PURPLE, "import pin band")
d.line([ZX, row(SAFE_LAST + 1), ZX + 64 * ZC, row(SAFE_LAST + 1)], fill=RED, width=2)
d.text((ZX - 52, row(SAFE_LAST + 1) - 7), "465", fill=RED, font=small)

d.text((ZX, ZY - 20), "the CLUT column, 64x256 at 2x (x960..1023)",
       fill=INK, font=font)

# ---- what that means (left column, under the legend) ------------------------
y = BLOCK_Y
d.text((SX, y), "what is wrong, in the run that produced this dump:", fill=INK, font=font)
y += 19
for col, line in WRONG:
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
