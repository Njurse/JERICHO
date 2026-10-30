FIREBIRD - one car, rebuilt from art
====================================

A Driver 2 car-data bank for one car (model slot 1), rebuilt from the sources in
this folder. `bank.json` is the DECLARATION: it names the art, and the bake works
everything else out. `FIREBIRD.LEV` is the result - the loadable bank the game
reads. The bake never writes bank.json.

One command:

    rebake.bat              bake, verify, install into bin\Release_dev, then
                            0-3 = Take a Ride (Chicago/Havana/Vegas/Rio), 4 = exit
    rebake.bat help         every other mode, and the tools' own switches
    play_chicago.bat        drive the installed bank in Chicago

What is what:

  bank.json              THE DECLARATION - sources, wheels, sounds, page bakes,
                         and "donor_dir"/"build_dir" (the layout below)
  FIREBIRD.LEV/.LCF      the built bank + cosmetics (installed here and in the
                         game mirror by rebake; cities.ini points here)
  cities.ini             the [cities] line for this bank
  art/                   author art the declaration names
    firebird.obj/.mtl      the car's geometry; material names find the page
    firebird_low.obj/.mtl  low-detail geometry
    textures/              firebird.png (set 1, body), page2/page3.png (wheel
                           pages, backdrop keyed), wheelclean/wheelfast.png
  sounds/                engine.wav, idle.wav, horn.wav - nothing else
  donor/                 what the bank was started from (CHICAGO model 1):
                         FIREBIRD.json donor manifest, source.LCF (HANDLING
                         lives here: model 1 at offset 288), cosmetics.json,
                         donor LEV/LCF, raw lumps, donor palette
  build/                 everything the bake writes: pages/, models/, the
                         rebuilt palette, FIREBIRD.json, FIREBIRD.report.json
                         (checks, warnings, sizes), rebake.log. Gitignored -
                         delete it any time for a clean bake.
  authoring/             takes, Audacity projects, blend, videos, tim2view,
                         original source objs/TIMs. The bake never reads it.
  work/                  scratch (gitignored)

The rules the bake enforces, in one line each:

  - A page's image, `transparent` key, tolerance and palette come from
    `bank.json`; a page whose source changed is re-baked, otherwise reused.
  - A declared page or wheel sprite may not live under work/, pages/ or build/
    - those are what the bake writes.
  - A wheel's island is CARVED from the declared sprite: the sprite's see-through
    texels become the page's transparent index, and whatever the page art had
    there is replaced.
  - Declare all three wheel variants; an undeclared one draws the host's wheel.
  - Sounds are 3 samples per car (engine, idle, horn-or-siren), levelled by
    loudness, loop points searched, and pitched for the game's own request.
    docs/SOUNDS.md has the numbers - bitrate, rate, duration, budgets.

Colours / paints: texture "1" in bank.json is the body page (16 colours max -
the bake quantises it). "protect" keeps the headlight white and taillight red
from being merged into the body. "paints" adds colour variants: column 0 is the
art as painted (blue), then yellow, red, green, grey (columns 1-4). Only the
body blues are recoloured; lights, glass/trim darks and transparency stay put.
Change a hex value and rebake. Full rules: bank-schema.md, "Paints".

Handling: there is no handling block in bank.json yet. The bake copies the
236-byte record for model 1 from donor/source.LCF unchanged; patch that (keep a
backup) and rebake, then check with cosmetics1.py --dump FIREBIRD.LCF.

Docs: JERICHO/MODS/bake/docs/SOUNDS.md (sounds), tools/car/bank-schema.md (the
full declaration schema and the folder layout).

Install as an importable city:

  1. Put this folder where cities.ini can reach it.
  2. Add its line under [cities] in JERICHO/CONFIG/cities.ini (city_N is index 11+N).
  3. Import it with  import = <resident slot>:<city index>:<model>

PREVIEW (bake\tools\car\preview_car.py)
  After a good rebake, a small window spins the clean OBJ slowly and loops:
  idle ~2s -> gears 1-4 (clean wheel, fast wheel once up to speed) -> coast
  to idle on the damaged wheel. It closes when you answer, on Esc, or after 12s.
  End menu: 0-3 Take a Ride (Chicago/Havana/Vegas/Rio), 4 preview (stays open
  until Esc, then back to the menu), 5 or the 10s timeout exits.
  Engine pitch slides continuously through 4 gears with their own ratios. Needs pygame-ce + PyOpenGL (+ numpy for engine pitch); without
  them it prints a note and the bake carries on. set NOPREVIEW=1 to skip.
  By hand: python ..\..\..\bake\tools\car\preview_car.py bank.json
           [--mute] [--flip-y] [--shot DIR]   (--shot saves PNG frames)
  Shows authoring art, not the 16-colour baked pages; paint columns are not
  shown yet.
