# World region streaming — and how to stream somewhere the engine has not been

The engine does not keep the world map resident. It streams it as a small moving
window of **regions**, and several things that look like bugs are just what that
window is. This matters every time something is placed at a point the player did
not drive to — a teleport, a camera cut, or (the case this doc was written for)
an **arena spawn point**.

## The window is a 2×2 barrel, not the whole map

Four regions are resident at once:

- `regions_unpacked[4]` and `loading_region[4]` (`spool.c:89-90`) are indexed by
  **barrel slot**, not by region number. Each holds the *region number* loaded
  into that slot (`-1` = none / mid-load).
- A region's slot is its **parity**:

  ```
  barrel = (region_x & 1) + (region_z & 1) * 2      (map.c:294, spool.c:1653)
  ```

  So `regions_unpacked[absoluteRegion]` is an out-of-bounds read. A readiness test
  written that way **never fails**, and the camera is drawn over geometry that was
  never streamed — the "skybox / nodraw" void. (This is the exact bug antfarm hit;
  see [[antfarm-region-streaming]].)

## The engine only pre-loads *neighbours*, and only near an edge

`ControlMap` (`map.c:348`) each frame:

1. computes the cell/region from **`MainPlayer.spoolXZ`** (`map.c:352-356`),
2. if `current_region == -1` (first pass only) unpacks that region into its barrel
   (`map.c:365-366`),
3. `CheckUnpackNewRegions` (`map.c:193`) — enqueues a **±1 neighbour** in x and/or
   z, and only when the current cell is within `force_load_boundary` (13, or 18)
   of a region edge (`map.c:217-286`). Offsets `(0,0)` never appear,
4. recomputes the cells from **`camera_position`** and calls `StartSpooling`
   (`map.c:374-377`).

Two consequences:

- **A region you hop into is never barrelled.** Standing mid-region, nothing is
  enqueued; and moving to a far region only pre-loads its *neighbours*, never the
  destination itself.
- **The streamer follows the spool, the renderer culls from the camera.** Unless
  they agree, a frame renders a region the streamer has already evicted.

`MapHeight` (`dr2roads.c:544`) returns **0** whenever `sdGetCell` is null — i.e.
whenever the queried cell is not resident. So "no ground here" and "this region
has not streamed yet" are the same answer, and a car placed at a non-resident
point drops into the void.

## Streaming there on purpose: `jer_map`

Because the engine will not load a region it did not drive to, an observer has to
do it — the same fix antfarm worked out for its far teleports, now shared:

```c
#include "jer_map.h"

jer_map_ready();                 /* is a level loaded? (header sizes are 0 in the FE) */
jer_map_region_of(x, z);         /* region holding a world point, or -1 */
jer_map_region_has_data(region); /* 0 = a void forever — re-pick, do not stream in */
jer_map_region_resident(region); /* region is the one unpacked into its parity barrel */
jer_map_force_region(region);    /* UnpackRegion into its barrel + synchronous flush */
jer_map_spool_to(x, z);          /* the full "stream there": region + texture AREA */
```

`jer_map_spool_to` is the one to call: it forces the region in
(`UnpackRegion(region, barrel)` + `StartSpooling()` + `UpdateSpool()` — synchronous
on PC) and then asks for the destination's texture **area** data
(`CheckLoadAreaData` + flush), because texture pages stream per area keyed off the
position, not with the region. Point `MainPlayer.spoolXZ` at the destination first,
and move the camera to match — otherwise the renderer culls the old spot.

Implementation: `src_rebuild/Game/C/jer_map.c`, SDK header
`src_rebuild/Game/C/JERICHO/include/jer_map.h`.

### The limit is four barrels

Forcing a region **evicts whatever shared its parity slot**. So:

- force the **destination**, **once** — never a batch of unrelated regions. Forcing
  region after region for a list of spawns evicts the ones just loaded, and
  `MapHeight` then answers 0 for ground that was fine (cainescrossfire's first
  attempt at this did exactly that and was reverted — see `arenas/arena.c`).
- a spawn that is **off the map** (no region data at all) still cannot be fixed;
  only a **non-resident** one can.

## Who uses it

- **antfarm** — moving its screensaver camera to a road far from the previous shot
  (its `AntFarmRegionUnpacked` / force-flush sequence is the origin of this helper).
- **Caine's Crossfire arenas** — placing the player at an authored `spawns[0]` that
  is far from the level's own start, so the ground around the spawn streams before
  the car needs it. See `JERICHO/MODS/cainescrossfire/ARENAS.md`.
