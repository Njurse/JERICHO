FIREBIRD — one car, rebuilt from art
====================================

A Driver 2 car-data bank for one car (model slot 1), rebuilt from the sources in
this folder. `bank.json` is the DECLARATION: it names the art, and the bake works
everything else out. `FIREBIRD.LEV` is the result — the loadable bank the game
reads.

One command:

    rebake.bat              bake, verify, and install into bin\Release_dev
    rebake.bat test 250     bake, then drive it headlessly for 250 frames
    rebake.bat help         every other mode, and the tools' own switches

What is what:

  bank.json              THE DECLARATION — sources, wheels, sounds, page bakes
  FIREBIRD.LEV/.LCF      the built bank + cosmetics (output; installed by rebake)
  FIREBIRD.json          the manifest the build writes back (output)
  firebird.obj           the car's geometry (author art)
  firebird_low.obj       low-detail geometry
  firebird.mtl           material names, which is how polys find their page
  textures/              page art for sets 1-3 and the wheel sprites
    firebird.png           set 1, the body
    page2.png, page3.png   sets 2 and 3, the wheel pages (backdrop keyed)
    wheelclean.png         the wheel sprite composited into set 2
    wheelfast.png          the fast wheel sprite, set 3
  sounds/                engine.wav, idle.wav, horn.wav (unused/ holds the raw takes)
  pages/                 the baked pages and their PNG previews (output)
  models/                the baked car blocks (output)
  palette.json/.png      the palette rows and a swatch view (output-ish)
  raw/, cosmos/, source/ the donor material this bank was started from
  work/                  scratch: extracted pages, temporary bakes (gitignored)

The rules the bake enforces, in one line each:

  - A page's image, `transparent` key, tolerance and palette come from
    `bank.json`; a page whose source changed is re-baked, otherwise reused.
  - A declared page or wheel sprite may not live under work/ or pages/ — those
    are what the bake writes, so a source there is a previous bake's output.
  - A wheel's island is CARVED from the declared sprite: the sprite's see-through
    texels become the page's transparent index, and whatever the page art had
    there is replaced.
  - Declare all three wheel variants; an undeclared one draws the host's wheel.
  - Sounds are 3 samples per car (engine, idle, horn-or-siren), levelled by
    loudness, loop points searched, and pitched for the game's own request.
    docs/SOUNDS.md has the numbers — bitrate, rate, duration, budgets.

Docs: JERICHO/MODS/bake/docs/SOUNDS.md (sounds), tools/car/bank-schema.md (the
full declaration schema).

Install as an importable city:

  1. Put this folder where cities.ini can reach it.
  2. Add its line under [cities] in JERICHO/CONFIG/cities.ini (city_N is index 11+N).
  3. Import it with  import = <resident slot>:<city index>:<model>
