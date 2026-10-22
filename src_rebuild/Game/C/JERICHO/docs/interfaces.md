# The mod interface

What a mod may rely on, and what breaks it. If this page and the code disagree,
the code is right and this page is a bug — `tools/check_mod_interface.py`
enforces the parts that can be checked.

## The interface version

`JERICHO_SDK_VERSION` (`JERICHO/include/jericho.h`) is the version of the contract
between the game and a mod. A mod declares the one it was **built against**:

```toml
# mod.toml
interface = 2
```

It is an **exact** match, not a minimum, and it is checked twice:

| where | catches | refusal |
|---|---|---|
| load (loadable addon) | an addon DLL built against another SDK | `needs SDK vN but host is vM — DISABLED` |
| activation (any mod) | a **bundled** mod from another build — the case the load check cannot see, because a bundled mod compiles against the header beside it | `was built for JERICHO interface vN; this build is vM` |
| the store | an index entry built for another interface, before it is downloaded | row shows `needs newer` with both numbers |

Bump it when a mod could break — see the rule in `jericho.h`: event ids added,
removed or renumbered; a `JER_ARGS_*` struct whose fields change order or size; a
contract header's ids or structs changing; `JERICHO_CONTEXT` gaining, losing or
reordering a member; `jer_register_module` changing signature. Do not bump for a
comment, a doc, or an engine-internal hook no module can see. A bump is not free:
every mod must be rebuilt and every published package re-released with the new
`interface`.

## Custom event ids

Mods talk to each other through `jer_fire` with ids from
`JER_EVENT_MODULE_CUSTOM` upwards, so two mods must never pick the same number.
The space is **blocked**, ten ids per owner, and a new mod takes a block (a tenth
of the space is unclaimed for exactly that):

| ids | owner | in use |
|---|---|---|
| +0 .. +9 | fixtures and tests | `+1` — `example` |
| +10 .. +19 | `sandbox` | `+10` |
| +20 .. +29 | `cainescrossfire` | — |
| +30 .. +39 | `carhacks` | — |
| +40 .. +49 | **core contracts** | `+40..+46` — the live car query, the per-car crush multiplier |
| +50 .. +59 | `mp` | — |
| +60 .. +69 | `d1cars` | — |
| +70 .. +79 | `JerAI` | — |
| +80 .. +89 | `antfarm` | — |
| +90 .. +99 | unclaimed — ask before using | — |

Ids are declared next to the mod that owns them, or in a contract header when the
id is part of a contract. `check_mod_interface.py` fails on a duplicate.

## Contracts

A contract is a **core** header of custom events: ids and argument structs, asked
and answered through `jer_fire`, where "nobody answered" is a normal result. That
is what lets the mods be built, released and loaded separately — no mod includes
another mod's header and no mod links to another mod's symbols.

| header | asked by | answered by |
|---|---|---|
| `jer_carquery.h` | `mp`, `sandbox` | `carhacks` |
| `jer_crumple.h` | `mp` | `crumple` |

A new contract belongs in `Game/C/JERICHO/include/` (already on every mod's
include path), mirrored into `JERICHO/sdk/include/`, with its ids inside the core
contract block above.

## Manifest keys a mod may rely on

Read by `jerParseModToml` (`jer_loader.c`); unknown keys are ignored, so a new key
never breaks an old build.

| key | meaning |
|---|---|
| `id` | must equal the folder name; it is the install path and the entry symbol |
| `name`, `version`, `author`, `description` | shown by the manager and the store |
| `interface` | the SDK version this mod was built against (above) |
| `dependencies` | mods that must also be active (`["a","b"]` or `"a,b"`). `requires` is accepted as a synonym — that is what the repo index calls it |
| `incompatible` | mods this one must not run beside |
| `default-enabled` | `true`/`1`/`enabled` only; anything else is OFF |
| `runtime` | `"dll"` = loadable addon; absent = deep mod, compiled in |

## What a repo index may rely on

`mods.json` per repo, read by the store (`jer_modstore.c`) and by
`check_mod_repo.py`: `schema`, `id`, `name`, `mods[]`, and per mod `id`, `name`,
`version`, `author`, `description`, `file`, `sha256`, `size`, `requires`,
`interface`, `min_jericho`, `default_enabled`, `bake_version`. See
[mod-repo.md](mod-repo.md).

## See also

- [events.md](events.md) — the engine's own event catalogue and argument structs
- [modding.md](modding.md) — writing, packaging and publishing a mod
- [module-activation.md](module-activation.md) — how a module gets enabled
