# Patches for the mod repositories

These fixes belong in the mod repositories (`JERICHO-Mod-MP`, `JERICHO-Mod-carhacks`,
and `JERICHO-D1` for bake), not in this tree. Each one was made against the
commit `0.14` pins (mp `27beaa7`, carhacks `3f4e60b`, bake `a68bc81`), and each
applies cleanly there. Apply them in each repo, push, then bump the pins here with
`tools/update_mod_submodules.sh`.

| Repo | Patch | What it fixes |
|---|---|---|
| mp (`JERICHO/MODS/mp`) | `mp/0001-...patch` | `tools/pack_lan/make_lan_package_micheal.sh` stops with "no d1cars module in the repo" since the module became `bake`. The public packager, `sync_lan.bat` and the pack_lan README still describe the `d1cars` folder too. |
| carhacks (`JERICHO/MODS/carhacks`) | `carhacks/0001-...patch` | `bigrig_extract.py` and `slotmap.py` look in `MODS/d1cars/tools`, which no longer exists. Also fixes the `DRIVER1.md` doc links. |
| bake (`JERICHO/MODS/bake`) | `bake/0001-...patch` | `tools/car/README.md` sets `T=JERICHO/MODS/d1cars/tools`, so every command in it points at a missing folder. |

The dedicated server's `core_mods` bug, found on v0.13.4, is already fixed on
`0.14`'s mp pin. Its example config no longer names `d1cars`, and
`mp_lobby.c`/`dedi/core.py` deliberately treat `d1cars` as an old alias of `bake`.

To apply one (example: carhacks), from that repo's own clone:

```sh
git am /path/to/JERICHO/docs/patches/carhacks/0001-*.patch
```

Checks before export: carhacks' two tools start and their new paths resolve
(`pages1.decomp_asm` imports). The mp and bake changes are scripts, docs and
comments only.
