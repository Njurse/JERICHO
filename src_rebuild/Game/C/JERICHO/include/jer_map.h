#ifndef JER_MAP_H
#define JER_MAP_H

/* jer_map — world region helper: where am I, and make the map stream there.
 *
 * The engine streams the world as a 2x2 window of REGIONS (a "barrel"), not as
 * a map it keeps whole. `regions_unpacked[4]` / `loading_region[4]` are indexed
 * by BARREL slot, and a region's slot is its parity:
 *
 *     barrel = (region_x & 1) + (region_z & 1) * 2
 *
 * So `regions_unpacked[absoluteRegion]` is an out-of-bounds read: a readiness
 * test written that way never fails and draws the camera over geometry that has
 * not streamed (the "skybox / nodraw" void).
 *
 * The engine only ever PRE-LOADS NEIGHBOURS (CheckUnpackNewRegions, map.c), and
 * only as you come near a region edge. The consequence that matters here: a
 * region you HOP into — a teleport, or a spawn point far from the level's own
 * start — is NEVER put into a barrel, so the ground there never loads and
 * MapHeight() answers 0. That is not a bug to route around; it is what the
 * streamer is.
 *
 * `jer_map_spool_to`/`jer_map_force_region` do the one thing an observer can:
 * unpack the destination region into its parity barrel and flush the spool
 * queue synchronously (on PC the copy is synchronous), so the destination is
 * resident by the end of the call rather than a few frames later. Point
 * `MainPlayer.spoolXZ` at the destination first (the streamer follows it) and
 * remember the renderer culls from `camera_position` (map.c:ControlMap ends by
 * recomputing the cells from the camera) — if the two disagree the shot renders
 * a region the streamer has already evicted.
 *
 * LIMIT — four barrels, one window. Forcing a region evicts whatever shared its
 * parity slot, so force the DESTINATION (once), never a batch of unrelated
 * regions: forcing region after region for a list of spawns evicts the ones
 * just loaded and MapHeight then answers 0 for ground that was fine. Keep what
 * you force inside the window you are actually playing in.
 *
 * The implementation lives in the GAME (jer_map.c) — it needs spool.c/map.c
 * internals — so modules just call these. Every entry point is a no-op / a
 * "not ready" answer until a level has loaded (the frontend runs with an empty
 * level header and zero region sizes, and every bit of map maths divides by
 * them; see jer_map_ready).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Is a level loaded? MAP_CELL_SIZE / MAP_REGION_SIZE are LEVEL-header fields
 * (cell_header.cell_size / region_size), ZERO until a level loads — and the
 * frontend runs with an empty header, so every region/cell division below is a
 * divide-by-zero there. Gate on this before any map maths. */
int jer_map_ready(void);

/* Region number holding world (x, z), or -1 when no level is loaded (or the map
 * has no regions). Clamped to the map. */
int jer_map_region_of(int x, int z);

/* Does region hold any spool data at all? 0 also for an out-of-range region, so
 * "no data here — re-pick" and "not a region" are the same answer. A region
 * with no data would be a void forever if you streamed into it. */
int jer_map_region_has_data(int region);

/* Is region the one currently unpacked into its parity barrel? The achievable
 * residency test — the engine holds FOUR regions, so "region + its 4 cardinal
 * neighbours" can never be satisfied. */
int jer_map_region_resident(int region);

/* Unpack region into its parity barrel and flush the spool queue (synchronous
 * on PC). Returns 1 when the region has data and was forced (or was already
 * resident), 0 when there is nothing to load (no level, no data, or it is
 * mid-load). Does NOT touch the texture AREA data.
 *
 * CAVEAT: this marks the region unpacked but does not run the engine's full
 * ControlMap bookkeeping, so the barrel's geometry/roadmap may stay unloaded and
 * MapHeight can still answer 0. Prefer jer_map_spool_to, which uses the engine's
 * own path. Kept as the low-level primitive. */
int jer_map_force_region(int region);

/* The full "stream there" call for a destination position. If the region at
 * (x, z) is not resident, make the engine load it the way it does at LEVEL
 * START: clear the engine's current region so ControlMap takes its first-pass
 * path and unpacks the region under MainPlayer.spoolXZ into the right 2x2 barrel
 * slot itself — then ask for the destination's texture AREA data
 * (CheckLoadAreaData) and flush again, so geometry and texture pages both land
 * in this frame.
 *
 * Point MainPlayer.spoolXZ at (x, z) BEFORE calling this, and move the camera to
 * match (see the header note). Returns 1 when the destination region has data
 * and is now resident; 0 when there is nothing to stream there (no level, or no
 * data at the point — the caller should keep its old spot). */
int jer_map_spool_to(int x, int z);

#ifdef __cplusplus
}
#endif

#endif /* JER_MAP_H */
