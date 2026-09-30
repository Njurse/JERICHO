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

SRC = sys.argv[1] if len(sys.argv) > 1 else "src_rebuild/bin/Release_dev/vram_dump.tga"
OUT = sys.argv[2] if len(sys.argv) > 2 else \
    "JERICHO/MODS/cainescrossfire/carhacks/vram-issues.png"

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

W, H = 1180, 720
sheet = Image.new("RGB", (W, H), WHITE)
d = ImageDraw.Draw(sheet)

d.text((20, 14), "PSX VRAM 1 MiB - and the CLUT column that has no room left",
       fill=INK, font=big)
d.text((20, 38), "measured from a live vram_dump.tga of a 3-city mashup run; "
                 "rectangles from tools/vrammap.py", fill=GREY, font=small)

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

# ---- what that means --------------------------------------------------------
y = ZY + 256 * ZC + 14
d.text((SX, y), "what is wrong, in the run that produced this dump:", fill=INK, font=font)
y += 19
for col, line in (
    (RED,   "level font image is rows 466..511, and the import's rows land in it"),
    (PURPLE, "the import pin band is rows 480..511 - ENTIRELY inside the font"),
    (AMBER, "VEGAS' own pages need 9 CLUT rows; the streamed-slot walk reserves 8"),
    (AMBER, "SendTPage orders cluts.h = npalettes/4 + 1 with no clamp (spool.c:495)"),
    (INK,   "with 3 imports the census read: 213 rows used, 0 safe free"),
    (INK,   "the palette upload in the crashing run wrote rows 428 -> 470, 5 past 465"),
    (GREEN, "the import's palette table is only 57 -> 38-42 rows after filtering,"),
    (GREEN, "but the cross-row closure pulls 142-152 of them back - the remap's job"),
):
    d.text((SX + 8, y), line, fill=col, font=small); y += 15

os.makedirs(os.path.dirname(OUT), exist_ok=True)
sheet.save(OUT)
print("wrote", OUT, sheet.size)
