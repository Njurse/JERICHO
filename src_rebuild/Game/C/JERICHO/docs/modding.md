# Modding quickstart — from a folder to a published repo

Five commands, no hand-editing. The formats (`mods.json`, the package, `mod.toml`)
are in [`mod-repo.md`](mod-repo.md); this page is the workflow around them.

Everything below runs from a JERICHO install — the tools live in
`JERICHO/CORE/modstore/` and ship beside the game.

## 1. Give the mod a manifest

`JERICHO/MODS/<id>/mod.toml`, with `id` equal to the folder name (the game
installs to `MODS/<id>/`):

```toml
id = "mymod"
name = "My Mod"
version = "0.1.0"
author = "you"
description = "One sentence a player reads in the store."
```

`version` is the whole update mechanism: the store compares it to decide that an
update exists, so **bump it for every change**. A package rebuilt without a bump
is invisible to everyone who already has the old one.

## 2. Package it

```sh
sh JERICHO/CORE/modstore/make_mod_package.sh mymod --out out
```

```bat
JERICHO\CORE\modstore\make_mod_package.bat mymod --out out
```

Writes `out/mymod-0.1.0.zip` (rooted at `JERICHO/MODS/mymod/`, the shape the
installer accepts) and `out/mymod-0.1.0.entry.json` — the `mods.json` entry with
its `sha256` and `size` already computed. Build output (`obj/`, `lib/`, `*.pdb`)
is left out; `--keep-build` keeps it.

## 3. Build the whole index

```sh
sh JERICHO/CORE/modstore/gen_mods_json.sh --out out --repo-name "My mods"
```

Every mod in `MODS/` is packaged and listed in `out/mods.json`, with each version
taken from that mod's own `mod.toml`. Pass mod ids to publish just those.

## 4. Validate it

```sh
python3 JERICHO/CORE/modstore/check_mod_repo.py out/mods.json --previous old/mods.json
```

Catches the failures that would otherwise only appear on a player's machine: a
hash or size that does not match the file, a duplicate id, an unparsable version,
a missing package, a dangling `requires`, a version that goes *backwards*, and a
package rebuilt without a version bump. `--previous` is optional but is the check
that matters for a second release.

## 5. Serve it, then publish it

```sh
cd out && python3 -m http.server 8099
```

```ini
; JERICHO/CONFIG/modstore.ini
repo = http://127.0.0.1:8099/mods.json
```

Options → JERICHO → Mods → **Mod Repo** → fetch → install. That is the same path
a player takes, so if it installs here it installs for them.

## A repo of your own

```sh
sh JERICHO/CORE/modstore/new_mod_repo.sh ~/my-mods --name "My mods" --id mymods
```

Creates a **self-contained** repo: `MODS/` (your sources), `tools/` (copies of
the three tools above, so it works without a game install), `publish.sh`,
`mods.json`, and a GitHub workflow that publishes on a `v*` tag. Then:

```sh
cp -R JERICHO/MODS/mymod ~/my-mods/MODS/
cd ~/my-mods && ./publish.sh && git add . && git commit -m "first release"
```

Pushing a `v1.0.0` tag makes CI package, validate, and attach everything to a
release; the index players point at is

```
https://github.com/<owner>/<repo>/releases/latest/download/mods.json
```

## The rules

- **Never hand-edit a hash.** The store is right to refuse an install whose
  `sha256` does not match, so a stale hash is a repo that installs nothing.
- **Bump `version` for every change**, and never publish a lower one.
- **Keep old packages online** — someone on an older version asks for the older
  file.
- If what a mod needs changes, bump `min_jericho` too.
- **Bump `BAKE_VERSION`** (the Driver 1 bake tooling) when the bake output
  changes: that is what makes the game offer to re-bake instead of leaving
  players on cars baked by the previous tooling.
- For a release host, pass `--url <asset-url-prefix>` to `gen_mods_json.sh` so
  the index names absolute package URLs; leave it off and the packages are
  named relatively, which is what a plain folder of files wants.

## Checking a repo you did not write

```sh
python3 JERICHO/CORE/modstore/check_mod_repo.py <index-url-or-file> --allow-missing
```

`--allow-missing` treats a package that is not on this disk as a warning, which
is what you want when the zips live on a release host.
