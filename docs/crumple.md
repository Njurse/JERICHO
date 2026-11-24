# Crumple — impact-driven car damage

This document is canonical in the mod's own folder.

→ [JERICHO/MODS/crumple/crumple.md](../JERICHO/MODS/crumple/crumple.md)

## Notes: imported-car damage UVs and page-index pool (2026-11-23)
- `denting.c JerDentSyncSlot` keys on the bank's damage block pointer, not just (city, model); `DentCarDirectional` re-syncs.
- `texture.c CarImportForgetCity` now returns the city's remap/index claims (pool 110..127). Symptom of the leak: `carimport: ... no free import index` then `slot N ... NOT READY` and `wheel ... via texture_cluts fallback` in the log.
- Not compiled here (PsyCross submodule empty); rebuild and re-test: swap between 3+ cities' cars, check log for `no free import index` and `JERICHO-CLUT COLLISION`.
- Level-load VRAM report now also prints `JERICHO-INDEX:` (page numbers held/free/leaked) and warns in the console if a city with no cars still owns any.
- Tidy: `CarRemapDropAt` replaces three copies of the remap+reservation removal; the dead `remapFrom/remapTo` pin code is gone.
- denting.c now includes models.h (CarImportCityHeld / GetCarImportDenting were undeclared; found by a g++ -fsyntax-only pass: `g++ -x c++ -fpermissive -std=gnu++17 -I. -I.. -I../../PsyCross/include -I../../PsyCross/include/psx ...` from Game/C works as a quick compile check).
