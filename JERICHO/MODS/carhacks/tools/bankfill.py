#!/usr/bin/env python3
"""bankfill.py - complete a sparse car-data bank from its host city's own assets.

A *bank* is a car-data file the engine registers as a city (see the engine doc
city-registry.md): DATA1 holding LUMP_CAR_MODELS (28) + LUMP_TEXTUREINFO (34) +
LUMP_PALLET (25), then the permanent page data, sector-aligned, from pageBase on.

A retexture pack bakes only the models it repaints - the BetterCars banks in this
install carry 1-2 models of 13, with -1 for the rest - so a pick of any other
model from that bank falls back to the level's own car. This tool fills the blanks
by copying the missing models out of the host city's own LEVELS\\<CITY>.LEV, with
the page sets and palette rows those models need.

    python3 bankfill.py --report BCCHI.LEV --base CHICAGO.LEV
    python3 bankfill.py --report BetterCars/*.LEV --base-dir DRIVER2/LEVELS

Read-only with --report. See --help for the fill mode.

WHY THE MODELS ARE COPIED VERBATIM. A model's polys name their texture sets by
the set ids in LUMP_TEXTUREINFO (`texture_set`), and the engine resolves those
against the bank's own list. The bake that produced these banks numbered its sets
from the HOST city's lists, so a host model's ids are already meaningful in the
bank - the same id in both means the same page, and the bank's copy of that page
is the repainted one. Appending the host's entries for the sets the bank is
missing (with their original ids) therefore needs no poly rewrite at all, which is
what makes filling in place safe: every byte of a copied model record is the
host's own.

FORMAT FACTS (the engine's readers, for the next reader of this file):
  * 8-byte header, then an 8-int citylump table; table[0]/[1] = DATA1 offset/size
    and pageBase = (data1Off/2048 + data1Size/2048)*2048 (models.c:393).
  * DATA1's body is a container: [type][size][data] segments, 4-byte aligned.
  * LUMP_CAR_MODELS: +0 count, +4 = 13 x 3 int32 (clean/damaged/low) relative to
    +164, which is where the model data area starts. All three must be present and
    strictly increasing or the engine reads the model as ABSENT (models.c:824-846).
  * LUMP_TEXTUREINFO: [tpage_amount][texamount] [TP x (tpage_amount+1)] then per
    page [texamount][TEXINF x texamount], then [nperms][XYPAIR perms] in a FIXED
    16-entry region, then [nspecpages][XYPAIR specs]. An XYPAIR is {set id, byte
    size} and the sizes are what carve the page data after DATA1.
  * LUMP_PALLET: [total_cluts] then records of four int32 {palette, texnum,
    tpageindex, clut_number} until a lone -1; when clut_number is -1 an inline
    32-byte CLUT follows (cars.c:2511-2616).
  * each permanent/special page entry is [int npalettes][npalettes*32 bytes]
    followed by the page's own (compressed) stream, in the XYPAIR's byte size.

Cite check: the layouts above are mirrored from
src_rebuild/Game/C/models.c (JerLoadCarImportFromFile, GetCarImportModels),
src_rebuild/Game/C/texture.c (ParseImportedTextureInfoForCity, LoadPermanentTPages)
and src_rebuild/Game/C/cars.c (ProcessPalletLumpForRows). tools/doccheck.py keeps
the docs honest, not this comment - if a layout moves, fix it here too.
"""

import argparse
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from levgeom import POLY_SIZES, MODEL_FMT, MODEL_SIZE, SVECTOR_SIZE   # noqa: E402
from levpages import segments                                        # noqa: E402

LUMP_CAR_MODELS = 28
LUMP_PALLET = 25
LUMP_TEXTUREINFO = 34
LUMP_CAR_DATA = 35

CDSECTOR = 2048
MAX_CAR_MODELS = 13
MODELS_DATA_AREA = 4 + MAX_CAR_MODELS * 12      # +164
PERM_REGION = 16                                # fixed size of the perm list


# ---------------------------------------------------------------------------
# reading a level / bank
# ---------------------------------------------------------------------------
def data1_of(blob):
    """(data1 off, data1 size, pageBase) of a bank or a level file."""
    table = struct.unpack_from("<8i", blob, 8)
    off, size = table[0], table[1]
    base = (off // CDSECTOR + size // CDSECTOR) * CDSECTOR
    return off, size, base


def lump_bodies(blob):
    """{(type, occurrence): (body offset, size)} for DATA1's container segments.

    Keyed by occurrence because a segment type can repeat (a level's DATA1 holds
    one of each of ours, but nothing guarantees it).
    """
    off, size, _base = data1_of(blob)
    out = {}
    seen = {}
    for typ, body, sz in segments(blob, off + 8):
        if body + sz > off + size:
            break
        n = seen.get(typ, 0)
        seen[typ] = n + 1
        out[(typ, n)] = (body, sz)
    return out


def lump(blob, typ):
    got = lump_bodies(blob).get((typ, 0))
    return got if got is not None else (0, 0)


def parse_models(blob):
    """{'count', 'offsets': [(clean, damaged, low)], 'size', 'body'} for a lump."""
    body, size = lump(blob, LUMP_CAR_MODELS)
    if body == 0:
        return {"count": 0, "offsets": [], "size": 0, "body": 0}
    count = struct.unpack_from("<i", blob, body)[0]
    offs = []
    for m in range(MAX_CAR_MODELS):
        offs.append(struct.unpack_from("<3i", blob, body + 4 + m * 12))
    return {"count": count, "offsets": offs, "size": size, "body": body}


def model_present(models, m):
    """The engine's own test (GetCarImportModels): all three, strictly increasing."""
    if m < 0 or m >= MAX_CAR_MODELS:
        return False
    c, d, l = models["offsets"][m]
    size = models["size"]
    return (c >= 0 and d > c and l > d and l < size)


def carried(models):
    return [m for m in range(MAX_CAR_MODELS) if model_present(models, m)]


def model_header(blob, models, m):
    """A model's MODEL header with absolute offsets, or None."""
    if not model_present(models, m):
        return None
    clean = models["offsets"][m][0]
    base = models["body"] + MODELS_DATA_AREA + clean
    if base + MODEL_SIZE > len(blob):
        return None
    fields = struct.unpack_from(MODEL_FMT, blob, base)
    return {
        "base": base,
        "shape_flags": fields[0], "instance_number": fields[2],
        "bounding_sphere": fields[5], "num_point_normals": fields[6],
        "num_vertices": fields[7], "num_polys": fields[8],
        "vertices": base + fields[9], "poly_block": base + fields[10],
    }


def model_set_ids(blob, models, m):
    """{set id: number of polys} that this model's polys name, or {}."""
    hdr = model_header(blob, models, m)
    if hdr is None:
        return {}
    q = hdr["poly_block"]
    out = {}
    for _ in range(hdr["num_polys"]):
        if q + 1 > len(blob):
            break
        pid = blob[q]
        low = pid & 0x1f
        size = POLY_SIZES[low] if low < len(POLY_SIZES) else 0
        if size == 0 or q + size > len(blob):
            break
        # FT3/FT4/GT3/GT4 carry (texture_set, texture_id) at +1/+2
        if low in (20, 21, 22, 23, 36, 37, 38, 39):
            out[blob[q + 1]] = out.get(blob[q + 1], 0) + 1
        q += size
    return out


def parse_texinfo(blob):
    """{'tpages', 'perms': [(set, size)], 'specs': [(set, size)], 'size'}."""
    body, size = lump(blob, LUMP_TEXTUREINFO)
    if body == 0:
        return {"tpages": 0, "perms": [], "specs": [], "size": 0, "body": 0}

    p = body
    tpage_amount, _texamount = struct.unpack_from("<ii", blob, p)
    p += 8
    p += (tpage_amount + 1) * 8                    # TP array
    for _ in range(tpage_amount):                  # per-page TEXINF lists
        (texn,) = struct.unpack_from("<i", blob, p)
        p += 4 + texn * 8

    (nperms,) = struct.unpack_from("<i", blob, p)
    p += 4
    perms = [struct.unpack_from("<ii", blob, p + i * 8) for i in range(nperms)]
    p += PERM_REGION * 8                           # FIXED region, not nperms
    (nspec, ) = struct.unpack_from("<i", blob, p)
    p += 4
    specs = [struct.unpack_from("<ii", blob, p + i * 8) for i in range(nspec)]

    return {"tpages": tpage_amount, "perms": perms, "specs": specs,
            "size": size, "body": body}


def parse_pallet(blob):
    """{'total_cluts', 'records', 'raw', 'term', 'size', 'body'}.

    `raw` holds each record's exact bytes (a record is four int32, or those plus an
    inline 32-byte CLUT when clut_number is -1), and `term` is the offset of the
    lone -1 terminator - the fill appends new records in front of it and copies the
    bytes rather than re-packing them, so nothing about an existing record changes.
    """
    body, size = lump(blob, LUMP_PALLET)
    if body == 0:
        return {"total_cluts": 0, "records": [], "raw": [], "term": 0,
                "size": 0, "body": 0}

    (total_cluts,) = struct.unpack_from("<i", blob, body)
    p = body + 4
    records, raw = [], []
    term = body + size
    while p + 4 <= body + size:
        (first,) = struct.unpack_from("<i", blob, p)
        if first == -1:
            term = p
            break
        palette, texnum, tpageindex, clut = struct.unpack_from("<4i", blob, p)
        start = p
        p += 16
        if clut == -1:
            p += 32                                # inline CLUT follows
        records.append((palette, texnum, tpageindex, clut))
        raw.append(blob[start:p])

    return {"total_cluts": total_cluts, "records": records, "raw": raw,
            "term": term, "size": size, "body": body}


def page_ranges(blob, texinfo):
    """{set id: (offset, size)} for every listed set, walking the page data.

    The engine carves these from pageBase in list order, each sector-aligned
    (texture.c LoadPermanentTPages), which is exactly how the bake laid them out.
    """
    _off, _size, base = data1_of(blob)
    out = {}
    cursor = base
    for setid, size in texinfo["perms"] + texinfo["specs"]:
        out[setid] = (cursor, size)
        cursor += (size + CDSECTOR - 1) // CDSECTOR * CDSECTOR
    return out


# ---------------------------------------------------------------------------
# what the host city's car list names
# ---------------------------------------------------------------------------
def host_models(femain_path, city=0):
    """The model numbers the stock frontend list offers for `city`.

    Read out of FEmain.c rather than copied here: the list is what makes a model
    selectable, so a stale copy would make 'fill everything a player can pick'
    quietly wrong. carNumLookup[0] is {1,2,3,4,0,8,9,10,11,12}; 0 means 'no car in
    that slot' (chkImportSlotModel in carhacks reads the same way), so it is
    dropped.

        int CarAvailability[CITY_COUNT][10] = { ... };
        char carNumLookup[CITY_COUNT][10] = { {1, 2, 3, 4, 0, 8, 9, 10, 11, 12}, ...
    """
    text = open(femain_path, "r", errors="replace").read()
    m = re.search(r"char\s+carNumLookup\s*\[[^]]*\]\[[^]]*\]\s*=\s*\{(.*?)\n\};",
                  text, re.S)
    if not m:
        raise SystemExit("bankfill: could not find carNumLookup in %s" % femain_path)

    rows = re.findall(r"\{([^{}]*)\}", m.group(1))
    if city >= len(rows):
        raise SystemExit("bankfill: %s has only %d carNumLookup rows"
                         % (femain_path, len(rows)))

    models = [int(x) for x in re.findall(r"-?\d+", rows[city])]
    return [x for x in models if 0 < x < MAX_CAR_MODELS]


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------
def describe(path, base_path, host):
    blob = open(path, "rb").read()
    base = open(base_path, "rb").read()

    models = parse_models(blob)
    texinfo = parse_texinfo(blob)
    pallet = parse_pallet(blob)
    pages = page_ranges(blob, texinfo)

    base_models = parse_models(base)
    base_texinfo = parse_texinfo(base)
    base_pallet = parse_pallet(base)
    base_pages = page_ranges(base, base_texinfo)

    mine = carried(models)
    missing = [m for m in host if m not in mine]

    print("=== %s   (base %s)" % (os.path.basename(path), os.path.basename(base_path)))
    print("  file           : %d bytes, DATA1 at %d, pageBase %d"
          % (len(blob), data1_of(blob)[0], data1_of(blob)[2]))
    print("  models carried : %-28s (%d of %d named)"
          % (" ".join(str(m) for m in mine) or "-", len(mine), MAX_CAR_MODELS))
    print("  host car list  : %-28s (%d)" % (" ".join(str(m) for m in host), len(host)))
    print("  MISSING here   : %-28s (%d)"
          % (" ".join(str(m) for m in missing) or "-", len(missing)))

    def sets_line(label, ti):
        if not ti["perms"] and not ti["specs"]:
            return "  %-14s : none" % label
        perm = " ".join("%d(%dB)" % (s, z) for s, z in ti["perms"]) or "-"
        spec = " ".join("%d(%dB)" % (s, z) for s, z in ti["specs"]) or "-"
        return "  %-14s : perm %s\n  %-14s   spec %s" % (label, perm, "", spec)

    print(sets_line("bank sets", texinfo))
    print(sets_line("base sets", base_texinfo))

    bank_sets = {s: z for s, z in texinfo["perms"] + texinfo["specs"]}
    base_sets = {s: z for s, z in base_texinfo["perms"] + base_texinfo["specs"]}

    # page bytes present for each listed set?
    bad_pages = [s for s, (off, size) in pages.items()
                 if off + size > len(blob) or size <= 8]

    need = {}                       # set -> [models that name it]
    convention = {}                 # set -> [models] (in NEITHER list: an id convention)
    for m in missing:
        for s in model_set_ids(base, base_models, m):
            need.setdefault(s, []).append(m)
            if s not in bank_sets and s not in base_sets:
                convention.setdefault(s, []).append(m)

    if need:
        print("  sets the missing models name:")
        for s in sorted(need):
            if s in bank_sets:
                off, size = pages.get(s, (0, 0))
                where = "bank(%dB)%s" % (bank_sets[s],
                                         ", REPAINTED" if s in base_sets else "")
                page = "page ok" if size > 8 and off + size <= len(blob) else "PAGE BYTES MISSING"
            elif s in base_sets:
                off, size = base_pages.get(s, (0, 0))
                where = "base(%dB) -> APPEND" % base_sets[s]
                page = "page ok" if size > 8 and off + size <= len(base) else "PAGE BYTES MISSING"
            else:
                # 253/254/255 and friends: the host city's own models name them and the
                # host city's list has no entry for them either, so this is an id
                # CONVENTION (an untextured / clut-only marker), not a gap. The copied
                # model keeps exactly what the host's model had.
                where = "id only (neither city lists it)"
                page = "no page needed"

            print("    set %-4d <- models %-14s : %s, %s"
                  % (s, " ".join(str(x) for x in need[s]), where, page))
    else:
        print("  sets the missing models name: -")

    pal_sets = sorted({r[2] for r in pallet["records"]})
    base_pal_sets = sorted({r[2] for r in base_pallet["records"]})
    # A filled model needs its sets' palette rows: the bank's own copy where the bank
    # has that tpageindex, else the host's. Only sets that are actually being appended
    # (i.e. the bank lacks them) can need copying.
    append_sets = [s for s in sorted(need) if s not in bank_sets and s in base_sets]
    missing_pal = [s for s in append_sets if s not in pal_sets and s in base_pal_sets]
    print("  palette rows   : bank tpageindex %s"
          % ((" ".join(str(s) for s in pal_sets) or "-")))
    print("  palette (base) : tpageindex %s" % ((" ".join(str(s) for s in base_pal_sets) or "-")))
    if append_sets:
        print("  sets to append : %s" % " ".join(str(s) for s in append_sets))
    if missing_pal:
        print("  palette rows to copy: %s"
              % " ".join(str(s) for s in missing_pal))

    if bad_pages:
        print("  PAGE DATA PROBLEM: sets listed without bytes: %s"
              % " ".join(str(s) for s in sorted(bad_pages)))
    if convention:
        print("  id conventions : %s (copied verbatim; no page in either city)"
              % " ".join(str(s) for s in sorted(convention)))

    perm_after = len(texinfo["perms"]) + len([s for s in append_sets
                                              if s not in dict(texinfo["perms"])])
    blocked = bool(bad_pages) or perm_after > PERM_REGION
    if perm_after > PERM_REGION:
        print("  WARNING: merged permanent list would be %d entries (max %d): the "
              "overflow goes to the special list" % (perm_after, PERM_REGION))

    verdict = "fillable" if not blocked else "NOT fillable as-is"
    print("  verdict        : %s (%d model(s) to copy from %s)"
          % (verdict, len(missing), os.path.basename(base_path)))
    print()

    return {"path": path, "missing": missing, "need": need, "convention": convention,
            "bad_pages": bad_pages, "missing_pal": missing_pal,
            "append_sets": append_sets, "blocked": blocked}


# ---------------------------------------------------------------------------
# filling a bank in place
# ---------------------------------------------------------------------------
def model_block_bytes(blob, models, m):
    """(clean, damaged, low) byte blocks of model `m`, as the lump stores them.

    A block's end is the next offset the lump uses at all - the next model's clean
    block, or another variant's - so the three blocks come out exactly as long as
    the host city wrote them and nothing of a neighbour's is dragged along."""
    if not model_present(models, m):
        return None

    area = models["body"] + MODELS_DATA_AREA
    c, d, l = models["offsets"][m]
    limit = models["size"] - MODELS_DATA_AREA

    later = [o for tri in models["offsets"] for o in tri if o > l]
    end = min(later) if later else limit
    if end <= l:
        end = limit
    return (blob[area + c:area + d], blob[area + d:area + l], blob[area + l:area + end])


def rebuild_models(blob, models, base, base_models, fill_models):
    """The LUMP_CAR_MODELS body with `fill_models` copied in from `base`."""
    area = models["body"] + MODELS_DATA_AREA
    data = bytearray(blob[area: models["body"] + models["size"]])
    offsets = [list(tri) for tri in models["offsets"]]

    for m in fill_models:
        blocks = model_block_bytes(base, base_models, m)
        if blocks is None:                         # cannot happen for a reported model
            continue
        start = len(data)
        clean = start
        damaged = clean + len(blocks[0])
        low = damaged + len(blocks[1])
        data += blocks[0] + blocks[1] + blocks[2]
        offsets[m] = [clean, damaged, low]

    body = bytearray()
    body += struct.pack("<i", models["count"])
    body += b"\x00" * (MAX_CAR_MODELS * 12)
    body += b"\x00" * (MODELS_DATA_AREA - len(body))
    body += data

    for i, (c, d, l) in enumerate(offsets):
        struct.pack_into("<3i", body, 4 + i * 12, c, d, l)
    return bytes(body)


def rebuild_texinfo(texinfo, appended):
    """LUMP_TEXTUREINFO with `appended` [(set, size)] added.

    The permanent list is the engine's pin list and lives in a FIXED 16-entry
    region (texture.c:1220 reaches the special list via &permlist[16]), so it takes
    what fits and the rest goes on the special list, which is unbounded. Both are
    honoured by the imported-car page walk; the report says which one was used.

    This assumes the bank has no per-page TEXINF tables (tpage_amount 0), which is
    true of every bake-made bank: they carry no name table. A bank with them is
    refused rather than silently rewritten."""
    if texinfo["tpages"] != 0:
        raise SystemExit("bankfill: this bank has %d per-page TEXINF tables; "
                         "rewriting them is not supported" % texinfo["tpages"])

    perms = list(texinfo["perms"])
    specs = list(texinfo["specs"])
    appended = list(appended)

    moved = []
    while appended and len(perms) < PERM_REGION:
        perms.append(appended.pop(0))
    if appended:
        moved = list(appended)
        specs.extend(appended)

    body = bytearray()
    body += struct.pack("<ii", 0, 0)               # tpage_amount, texamount
    body += b"\x00" * 8                            # tpage_position[tpage_amount+1]
    body += struct.pack("<i", len(perms))
    for setid, size in perms:
        body += struct.pack("<ii", setid, size)
    body += b"\x00" * (PERM_REGION - len(perms)) * 8
    body += struct.pack("<i", len(specs))
    for setid, size in specs:
        body += struct.pack("<ii", setid, size)
    return bytes(body), moved


def rebuild_pallet(blob, pallet, base, base_pallet, sets):
    """LUMP_PALLET with the host's records for `sets` appended.

    Records are copied byte for byte (including an inline CLUT when it has one), so
    the bank's own colours are untouched. `total_cluts` is bumped by what is added:
    it is a CLUT count for table sizing, not a record count (cars.c:2483)."""
    added = [raw for (rec, raw) in zip(base_pallet["records"], base_pallet["raw"])
             if rec[2] in sets]

    if not added:
        return bytes(blob[pallet["body"]: pallet["body"] + pallet["size"]]), 0

    head = bytearray(blob[pallet["body"]: pallet["term"]])
    struct.pack_into("<i", head, 0, pallet["total_cluts"] + len(added))

    body = bytearray(head)
    for raw in added:
        body += raw
    body += struct.pack("<i", -1)                  # the lone terminator
    return bytes(body), len(added)


def assemble(models_body, texinfo_body, pallet_body, page_blobs):
    """A whole bank file: header, 8-int table, DATA1 container, page data."""
    seg = bytearray()
    seg += struct.pack("<ii", LUMP_CAR_MODELS, len(models_body)) + models_body
    seg += b"\x00" * ((4 - len(seg) % 4) % 4)
    seg += struct.pack("<ii", LUMP_TEXTUREINFO, len(texinfo_body)) + texinfo_body
    seg += b"\x00" * ((4 - len(seg) % 4) % 4)
    seg += struct.pack("<ii", LUMP_PALLET, len(pallet_body)) + pallet_body
    seg += b"\x00" * ((4 - len(seg) % 4) % 4)

    data1 = bytearray(struct.pack("<ii", LUMP_CAR_DATA, len(seg) + 8) + seg)
    data1 += b"\x00" * ((CDSECTOR - len(data1) % CDSECTOR) % CDSECTOR)

    out = bytearray()
    out += struct.pack("<ii", 37, DATA1_OFF - 8)   # the header the bake writes
    out += struct.pack("<8I", DATA1_OFF, len(data1), 0, 0, 0, 0, 0, 0)
    out += b"\x00" * (DATA1_OFF - len(out))
    out += data1

    for blob in page_blobs:
        out += blob
        out += b"\x00" * ((CDSECTOR - len(out) % CDSECTOR) % CDSECTOR)
    return bytes(out)


DATA1_OFF = 2048


def fill_bank(bank_path, base_path, host, backup=True):
    """Copy the host city's models (and what they need) into the bank, in place."""
    blob = open(bank_path, "rb").read()
    base = open(base_path, "rb").read()

    models = parse_models(blob)
    texinfo = parse_texinfo(blob)
    pallet = parse_pallet(blob)
    pages = page_ranges(blob, texinfo)

    base_models = parse_models(base)
    base_texinfo = parse_texinfo(base)
    base_pallet = parse_pallet(base)
    base_pages = page_ranges(base, base_texinfo)

    carried_models = carried(models)
    fill_models = [m for m in host if m not in carried_models]
    if not fill_models:
        print("  %s already carries the host list - nothing to do"
              % os.path.basename(bank_path))
        return False

    bank_sets = {s: z for s, z in texinfo["perms"] + texinfo["specs"]}
    base_sets = {s: z for s, z in base_texinfo["perms"] + base_texinfo["specs"]}

    # Which of the copied models' sets the bank does not already ship. A set the
    # bank already lists stays THE BANK'S (that page is the repaint); a set only the
    # host lists is appended with the host's bytes; a set neither lists is an id
    # convention and needs nothing (the host's own models use it that way).
    needed = {}
    for m in fill_models:
        for setid in model_set_ids(base, base_models, m):
            needed.setdefault(setid, []).append(m)

    appended = [(s, base_sets[s]) for s in sorted(needed)
                if s not in bank_sets and s in base_sets]

    missing_page = [s for s, _z in appended
                    if not base_pages.get(s) or base_pages[s][1] <= 8]
    if missing_page:
        raise SystemExit("bankfill: %s: the host city lists set(s) %s with no page "
                         "bytes - refusing to write a broken bank"
                         % (os.path.basename(base_path),
                            " ".join(str(s) for s in missing_page)))

    # --- build the three lumps and the page data ---
    models_body = rebuild_models(blob, models, base, base_models, fill_models)
    texinfo_body, moved_to_spec = rebuild_texinfo(texinfo, appended)
    pallet_body, pal_added = rebuild_pallet(
        blob, pallet, base, base_pallet, {s for s, _z in appended})

    new_texinfo = parse_texinfo(assemble(models_body, texinfo_body, pallet_body, []))
    page_list = new_texinfo["perms"] + new_texinfo["specs"]
    page_blobs = []
    for setid, size in page_list:
        if setid in bank_sets:
            off, _z = pages[setid]
            page_blobs.append(blob[off:off + size])
        else:
            off, _z = base_pages[setid]
            page_blobs.append(base[off:off + size])

    out = assemble(models_body, texinfo_body, pallet_body, page_blobs)

    if backup:
        keep = bank_path + ".sparse.bak"
        if not os.path.exists(keep):
            open(keep, "wb").write(blob)
            print("  backup          : %s" % os.path.basename(keep))

    open(bank_path, "wb").write(out)

    print("  filled          : models %s copied from %s"
          % (" ".join(str(m) for m in fill_models), os.path.basename(base_path)))
    print("  sets appended   : %s%s"
          % (" ".join("%d(%dB)" % (s, z) for s, z in appended) or "-",
             "  (%d of them on the SPECIAL list: the permanent list is full)"
             % len(moved_to_spec) if moved_to_spec else ""))
    print("  palette records : +%d" % pal_added)
    print("  size            : %d -> %d bytes" % (len(blob), len(out)))
    return True


def verify_bank(bank_path, base_path, host):
    """The engine's own rules, asserted on the written file."""
    blob = open(bank_path, "rb").read()
    base = open(base_path, "rb").read()

    models = parse_models(blob)
    texinfo = parse_texinfo(blob)
    pallet = parse_pallet(blob)
    pages = page_ranges(blob, texinfo)
    base_models = parse_models(base)
    base_texinfo = parse_texinfo(base)
    base_pallet = parse_pallet(base)
    base_sets = {s: z for s, z in base_texinfo["perms"] + base_texinfo["specs"]}

    problems = []

    # (a) every host model is present and buildable
    for m in host:
        hdr = model_header(blob, models, m)
        if hdr is None:
            problems.append("model %d is not present/buildable" % m)
            continue
        if hdr["num_vertices"] <= 0 or hdr["num_polys"] <= 0:
            problems.append("model %d has no geometry (%d verts, %d polys)"
                            % (m, hdr["num_vertices"], hdr["num_polys"]))

    # (b) every set a present model names is either listed here with page bytes, or
    #     an id convention the host city does not list either
    listed = {s: z for s, z in texinfo["perms"] + texinfo["specs"]}
    for m in carried(models):
        for setid in model_set_ids(blob, models, m):
            if setid in listed:
                off, size = pages[setid]
                if size <= 8 or off + size > len(blob):
                    problems.append("model %d names set %d whose page bytes are missing"
                                    % (m, setid))
            elif setid in base_sets:
                problems.append("model %d names set %d, which the host has and this "
                                "bank does not list" % (m, setid))

    # (c) palette rows, for the sets THE HOST keeps rows for. A set the host lists
    #     without any palette record of its own is normal - Havana's set 0 is one, and
    #     the engine leaves such a row to the poly's own page CLUT rather than
    #     fabricating colours (cars.c "left to the poly's own page CLUT") - so only a
    #     row the host DOES have and this bank has lost is a fault.
    pal_sets = {rec[2] for rec in pallet["records"]}
    base_pal_sets = {rec[2] for rec in base_pallet["records"]}
    for setid in listed:
        if setid in base_pal_sets and setid not in pal_sets:
            problems.append("set %d is listed and the host has palette rows for it, but "
                            "this bank does not" % setid)

    # (d) the offset table is inside the lump and strictly increasing
    area = models["size"] - MODELS_DATA_AREA
    for m, tri in enumerate(models["offsets"]):
        if tri[0] < 0:
            continue
        if not (tri[0] < tri[1] < tri[2] < area):
            problems.append("model %d offsets %s are not strictly increasing inside "
                            "the %d-byte data area" % (m, tri, area))

    name = os.path.basename(bank_path)
    if problems:
        print("  %-14s: FAILED" % name)
        for p in problems:
            print("      %s" % p)
        return False

    print("  %-14s: ok - %d model(s), %d set(s), %d palette record(s); pageBase %d"
          % (name, len(carried(models)), len(listed), len(pallet["records"]),
             data1_of(blob)[2]))
    return True


def main(argv):
    ap = argparse.ArgumentParser(
        description="Report on (and, later, fill) sparse car-data banks.")
    ap.add_argument("banks", nargs="+", help="bank .LEV file(s), or a glob")
    ap.add_argument("--report", action="store_true",
                    help="print what each bank carries against its host city (the default)")
    ap.add_argument("--fill", action="store_true",
                    help="copy the host city's missing models into the bank(s), in place")
    ap.add_argument("--verify", action="store_true",
                    help="assert the engine's rules on the bank(s) and exit non-zero if broken")
    ap.add_argument("--no-backup", action="store_true",
                    help="do not keep <bank>.sparse.bak before the first in-place write")
    ap.add_argument("--base", help="host city .LEV for a single bank")
    ap.add_argument("--base-dir", help="folder of host city .LEV files")
    ap.add_argument("--city", type=int, default=0,
                    help="carNumLookup row to fill (default 0 = CHICAGO)")
    ap.add_argument("--femain",
                    default=os.path.join(HERE, "..", "..", "..", "..",
                                         "src_rebuild", "Game", "Frontend", "FEmain.c"))
    args = ap.parse_args(argv[1:])

    host = host_models(os.path.abspath(args.femain), args.city)

    paths = []
    for b in args.banks:
        paths.extend(sorted(glob.glob(b)) or [b])

    if args.base and len(paths) > 1:
        raise SystemExit("bankfill: --base takes one bank; use --base-dir for several")

    blocked = 0
    for path in paths:
        if args.base:
            base = args.base
        else:
            if not args.base_dir:
                raise SystemExit("bankfill: give --base or --base-dir")
            # BCCHI -> CHICAGO, BCHAV -> HAVANA, BCVGS -> VEGAS, BCRIO -> RIO
            stem = os.path.splitext(os.path.basename(path))[0]
            guess = {"BCCHI": "CHICAGO", "BCHAV": "HAVANA",
                     "BCVGS": "VEGAS", "BCRIO": "RIO"}.get(stem.upper())
            if guess is None:
                raise SystemExit("bankfill: do not know which city %s belongs to; "
                                 "pass --base" % stem)
            base = os.path.join(args.base_dir, guess + ".LEV")

        if not os.path.exists(base):
            raise SystemExit("bankfill: no base level at %s" % base)

        if args.verify:
            ok = verify_bank(path, base, host)
            if not ok:
                blocked += 1
            continue

        if args.fill:
            print("=== %s   (filling from %s)"
                  % (os.path.basename(path), os.path.basename(base)))
            fill_bank(path, base, host, backup=not args.no_backup)
            if not verify_bank(path, base, host):
                blocked += 1
            print()
            continue

        info = describe(path, base, host)
        if info["blocked"]:
            blocked += 1

    return 1 if blocked else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
