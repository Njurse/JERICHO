# Mod repos — how to host one, and how to test the store against it

The mod store (Options → JERICHO → Mods → **Mod Repo**) installs, updates and
removes ONE mod at a time. It never touches the game exe: it downloads a package,
verifies it, and unpacks it into `JERICHO/MODS/<id>/`.

A **repo** is nothing more than:

1. one **`mods.json`** listing what you offer, and
2. the **package files** it points at (a zip or tar.gz per mod).

Host them anywhere that serves two static files — a release asset, GitHub
Pages, `raw.githubusercontent.com`, an S3 bucket, or a folder on this PC.

---

## The official repo

JERICHO's built-in feed is
`https://raw.githubusercontent.com/Njurse/JERICHO-Repo-Official/main/mods.json`.
It lists `mp`, `carhacks`, `crumple`, `d1cars` and `JerAI`, with the packages in
`packages/`. The sources live in their own repositories
(`Njurse/JERICHO-Mod-MP`, `Njurse/JERICHO-Mod-carhacks`,
`Njurse/Jericho-Mod-CRUMPLE`, `Njurse/JERICHO-D1`, and `JERICHO/MODS/JerAI` in the
main tree). See that repository's README for the publish steps.

## 1. The index: `mods.json`

```json
{
  "schema": 1,
  "id": "official",
  "name": "JERICHO Mods",
  "homepage": "https://github.com/Njurse/JERICHO-Repo-Official",

  "mods": [
    {
      "id": "d1cars",
      "name": "Driver 1 Cars",
      "version": "0.2.0",
      "author": "Jaret Ludvik",
      "description": "Driver 1 traffic car shapes as a fifth car-data source.",

      "file": "d1cars-0.2.0.zip",
      "sha256": "6b94e786c6c7c724581dbc4947b88590273185c4afc4e055e5152299fe204283",
      "size": 23456,

      "requires": ["carhacks"],
      "min_jericho": "0.10.9",
      "default_enabled": false,
      "bake_version": 2
    }
  ]
}
```

Only `id`, `version`, `file` and `sha256` are really required; everything else
makes the browser more useful.

| field | what it does |
|---|---|
| `id` | the folder it installs to (`JERICHO/MODS/<id>/`), and must match the package's own `mod.toml` — the store refuses a mismatch |
| `version` | compared to what is installed. **A repo only ever offers an update, never a downgrade** |
| `file` | a URL, or a name relative to `mods.json`'s own folder |
| `sha256` | **required**: the download is refused if it does not match |
| `size` | bytes; informational |
| `requires` | comma list of mod ids that must be installed first; the row is refused with `needs <id>` |
| `min_jericho` | the row reads `needs newer` and refuses on an older core |
| `bake_version` | for `d1cars`: a Driver 1 bake older than this prompts a re-bake |

## 2. The package

A zip (or `.tar.gz`) whose root is **either** `MODS/<id>/` **or**
`JERICHO/MODS/<id>/` — both are accepted. It must contain at least `mod.toml`;
its `id` has to equal the index entry's `id`.

```
d1cars-0.2.0.zip
└── JERICHO/MODS/d1cars/
    ├── mod.toml          id, name, version, author, description, runtime, dependencies
    └── d1cars.dll        the addon (runtime = "dll"), or a compiled-in mod's sources
```

Installs are **overwrite-only**: files the package does not carry are left
alone, so a package may be a partial update. Nothing is ever deleted by an
install; removal deletes the whole `MODS/<id>/` folder, and only for a mod the
store installed itself (it keeps a record per mod in
`JERICHO/CONFIG/modstore/installed/<id>.json`).

`mod.toml` also allows `version`, `author`, `dependencies`, `default-enabled`,
`runtime = "dll"` and `incompatible`. The manifest is authoritative: the game,
the inventory and the store all report the version written there, even for a
mod compiled into the exe.

## 3. Get the hash and size

The store will not install anything it cannot verify, so a published entry needs
the real hash of the exact file. Do not type it — one command packages the mod
and prints the whole entry:

```sh
sh JERICHO/CORE/modstore/make_mod_package.sh d1cars --out out
# -> out/d1cars-0.2.0.zip + out/d1cars-0.2.0.entry.json (sha256 + size computed)
```

or, to package every mod and rebuild the index in one go:

```sh
sh JERICHO/CORE/modstore/gen_mods_json.sh --out out
```

If you are writing an entry by hand for some other reason:

```sh
sha256sum   d1cars-0.2.0.zip          # Linux / macOS / git-bash
certutil -hashfile d1cars-0.2.0.zip SHA256   # Windows cmd
wc -c < d1cars-0.2.0.zip              # size in bytes
```

and then run the validator over the result — it recomputes both from the file:

```sh
python3 JERICHO/CORE/modstore/check_mod_repo.py out/mods.json --previous old/mods.json
```

The workflow view of all of this is [`modding.md`](modding.md).

## 4. Try it locally in five minutes

The repo already contains a working fixture and a generator — this is the
fastest way to exercise the whole path without hosting anything:

```sh
cd JERICHO/CORE/modstore/test
sh make_test_repo.sh          # builds repo/ : testmod-<ver>.zip + mods.json + mods-corrupt.json
sh verify.sh                  # 32 assertions across modstore.sh AND modstore.bat
```

`make_test_repo.sh` prints the exact `file:///` URLs. To browse and install it
*in the game*, serve that folder — the store speaks plain HTTP:

```sh
cd JERICHO/CORE/modstore/test/repo
python3 -m http.server 8731
```

then either add it to `JERICHO/CONFIG/modstore.ini` next to the game:

```ini
repo2 = http://127.0.0.1:8731/mods.json
repo2_name = Local
```

…restart, and it shows up in **Mod Repo** (left/right on the `Repo:` row cycles
repos). Or point the official slot at it for a single test run, which needs no
config edit:

```sh
JERICHO_TEST_MODSTORE_URL=http://127.0.0.1:8731/mods.json
JERICHO_TEST_MODSTORE=menu        # open the Mods table headlessly and report it
```

Other test levers, for a headless run with `-nointro`:

| `JERICHO_TEST_MODSTORE=` | what it does |
|---|---|
| `index` | fetch the repo index and log how many mods it found |
| `install[:id]` | fetch, then install a mod (the named one, or the first) |
| `remove:id` | remove a mod the store installed; the delete waits for the game to exit |
| `menu` | walk screen 41 → table menu → repo browser, then toggle and reorder a row |

## 5. The rules a repo should respect

- **Never publish a lower version than you already shipped.** The store will not
  downgrade; a row that is older than what is installed reads `newer here` and
  refuses.
- **Bump `version` for every change.** That comparison is the only way an update
  is noticed.
- **Bump `BAKE_VERSION`** in `BAKE_D1_CARS.*` when a change alters what the bake
  writes — that is what prompts players to re-bake rather than run on stale data.
- **Ship `sha256`, always.** An entry without it cannot be installed.
- A package that is also shipped by a release is fine: the store updates it, and
  if a later core update overwrites the folder, the row reads `replaced` and
  offers the store's version again.
