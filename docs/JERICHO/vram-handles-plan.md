# Plan: import page handles (end VRAM page-number conflicts)

Problem: car AND ped imports take page numbers from one global pool, 110..127 (18), shared
with the host's own sets; numbers are baked into polys at build time. Exhaustion => refused
cars/peds, host pages, fallback wheel CLUTs.

Facts (2026-11-24):
- `texture_pages[128]` / `texture_cluts[128][32]` (texture.h). Host sets live in 0..~70.
- Cars: polys bake an index (`CAR_BAKE_TPAGE`), resolved at draw via `CAR_TPAGE_OF`
  -> `texture_pages[idx]`. Wheels read `texture_pages[set]` (cars.c ~2324/2345).
- Peds: model polys carry `u_char texture_set` (engine/mdl.h) -> resolved through
  `texture_pages[set]` at draw. A byte, so a handle must stay <= 255.

Approach: an import-only handle space 128..255, never touched by host/frontend.
1. Grow `texture_pages`/`texture_cluts` to 256; host code keeps using 0..127. Audit every
   `< 128` / `[128]` bound in texture.c, models.c, cars.c (sIndexCity, sNoIndexLogged,
   CarSetRemap, pin arrays) and give them `JER_TPAGE_SLOTS`.
2. One allocator `JerPageHandleAlloc(owner, city, set)` / `Release(owner)`, refcounted by
   owner (car slot bitmask, ped import id). Replaces FindFreeSetIndex + sIndexCity + the
   remap/reservation lists. Every import set always gets a handle; never "keeps the host page".
3. Cars: CarImportDstSetCore returns a handle; pin writes `texture_pages[handle]`. No draw change
   (CAR_TPAGE_OF already indirects). Wheels: same table, so they follow automatically.
4. Peds: JerPedImportBaked* / area pages rewrite `texture_set` to a handle from the same
   allocator; release on JerPedReleasePins / JerPedImportReset.
5. Released handle -> point `texture_pages[h]` at the checker page (already done for D3) so a
   stale reference is visible, not someone else's art.
6. Physical limit stays: lower-pool pages/CLUT rows. When those run out, refuse BEFORE building
   (car: CarImportSlotMissingSets precheck; ped: probe) with a console line.
7. Bake side: car bake and jmpped_bake write set numbers only as local ids (0..N); add a
   selftest that a bank's sets/CLUT rows fit the engine budget (pages per car, rows per page)
   and fail the bake with a clear message otherwise. Ped bake: same check + `jmpped_selftest`.
8. Report: JERICHO-INDEX line becomes handles used/free per owner kind; leak check unchanged.
9. Tests: extend JERICHO/test/test_car_release.c with alloc/release/refcount across 3 cities +
   peds; Python selftests for both bakes.

Order: 1 -> 2 -> 3 (cars verify) -> 4 (peds) -> 5,8 -> 6 -> 7,9. Each step its own commit.
