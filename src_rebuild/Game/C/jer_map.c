/* jer_map.c — see JERICHO/include/jer_map.h.
 *
 * Engine-side (not in JERICHO/src) because it reaches into spool.c/map.c
 * internals — regions_unpacked[], UnpackRegion, the spool flush — exactly like
 * jer_hud.c needs the game's display buffer. The logic is the SAME fix the
 * antfarm module worked out for its far teleports (antfarm.c: the barrel is
 * (region_x & 1) + (region_z & 1) * 2, not the region number; the engine only
 * pre-loads neighbours, so a hop must force the destination in); it lives here
 * now so cainescrossfire's arena spawn and antfarm share one implementation.
 */

#include "driver2.h"

#include "map.h"	/* cell_header, MAP_CELL_SIZE/REGION_SIZE, regions_across/down,
				 * cells_across/down, units_across_halved/down */
#include "spool.h"	/* regions_unpacked, loading_region, spoolinfo_offsets,
				 * UnpackRegion, StartSpooling, UpdateSpool, CheckLoadAreaData */

#include "jer_map.h"

int jer_map_ready(void)
{
	return cell_header.cell_size > 0 &&
		cell_header.region_size > 0 &&
		cells_across > 0 && cells_down > 0 &&
		regions_across > 0 && regions_down > 0;
}

static int jer_map_total_regions(void)
{
	return regions_across * regions_down;
}

static int jer_map_barrel_of(int region)
{
	int rx = region % regions_across;
	int rz = region / regions_across;

	return (rx & 1) + (rz & 1) * 2;
}

int jer_map_region_of(int x, int z)
{
	int cellx, cellz, rx, rz;

	if (!jer_map_ready())
		return -1;

	cellx = (x + units_across_halved) / MAP_CELL_SIZE;
	cellz = (z + units_down_halved) / MAP_CELL_SIZE;

	rx = cellx / MAP_REGION_SIZE;
	rz = cellz / MAP_REGION_SIZE;

	if (rx < 0) rx = 0;
	if (rx >= regions_across) rx = regions_across - 1;
	if (rz < 0) rz = 0;
	if (rz >= regions_down) rz = regions_down - 1;

	return rx + rz * regions_across;
}

int jer_map_region_has_data(int region)
{
	if (!jer_map_ready() || spoolinfo_offsets == NULL)
		return 0;

	if (region < 0 || region >= jer_map_total_regions())
		return 0;

	return spoolinfo_offsets[region] != 0xffff;
}

int jer_map_region_resident(int region)
{
	if (!jer_map_ready())
		return 0;

	if (region < 0 || region >= jer_map_total_regions())
		return 0;

	return regions_unpacked[jer_map_barrel_of(region)] == region;
}

int jer_map_force_region(int region)
{
	int barrel;

	if (!jer_map_region_has_data(region))
		return 0;

	if (jer_map_region_resident(region))
		return 1;			/* already in its barrel */

	barrel = jer_map_barrel_of(region);

	/* Mid-load: UnpackRegion would bail, and the engine finishes it on its own
	 * next pass. Report "not forced" rather than pretend. */
	if (loading_region[barrel] != -1)
		return 0;

	UnpackRegion(region, barrel);

	/* Land it in THIS frame instead of waiting for the engine's next ControlMap
	 * pass. On PC the spool copies synchronously, so flushing the queue here
	 * makes the destination resident on return. */
	StartSpooling();
	UpdateSpool();

	printInfo("[jer_map] forced region %d into barrel %d\n", region, barrel);

	return 1;
}

int jer_map_spool_to(int x, int z)
{
	int region = jer_map_region_of(x, z);

	/* Nothing to stream to: no level, or the point is off the map (a region
	 * with no spool data would be a void forever). The caller keeps its spot. */
	if (region < 0 || !jer_map_region_has_data(region))
		return 0;

	if (!jer_map_region_resident(region) && !jer_map_force_region(region))
		return 0;

	/* The map geometry is in, but the TEXTURE pages stream per AREA keyed off
	 * the position, so ask for the destination's area explicitly and flush
	 * again — otherwise the region draws untextured until the camera reaches
	 * it. Same two-step antfarm uses. */
	{
		int cx = (x + units_across_halved) / MAP_CELL_SIZE;
		int cz = (z + units_down_halved) / MAP_CELL_SIZE;

		CheckLoadAreaData(cx, cz);
		StartSpooling();
		UpdateSpool();
	}

	return 1;
}
