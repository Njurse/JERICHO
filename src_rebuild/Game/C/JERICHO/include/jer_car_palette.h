#ifndef JER_CAR_PALETTE_H
#define JER_CAR_PALETTE_H

/* Per-INSTANCE car colour — the car-path companion to jer_ped_palette.h.
 *
 * A cover-all data addition that is INERT by default: nothing is tinted unless a
 * module (or the palette editor) calls jer_car_palette_set for a specific car. It
 * keys off the CAR_DATA slot (car_data[] index), which is stable for the length of
 * a session on every machine -- so the same value can travel in multiplayer and
 * mean the same thing on the owner and on every peer.
 *
 * NOTHING CALLS set/clear today. The LIVE car colour in a match is
 * `cp->ap.palette` -- an owner-authoritative INDEX carried in MP_CARSTATE and
 * arbitrated by JER_EVENT_CAR_PEER_DRAW (see JERICHO/MODS/mp/ and carhacks'
 * MP_ADAPTER.md). This RGB-per-slot table is the hook a future per-car tint (or
 * the palette editor) would use; it is kept because the slot is the agreed key.
 *
 * COLOUR. This header predates jer_colour.h and stores plain 8-bit RGB (0..255,
 * clamped). New code should build a JER_COLOUR (jer_colour.h) -- the canonical
 * type, which also owns the engine's two packings (a polygon word is B<<16|G<<8|R,
 * a CLUT entry is 5/5/5) -- and pass its channels here. That is where the two
 * colour paths would meet: jer_car_palette_set/get could take/return JER_COLOUR
 * directly, the way the ped path already does (jer_ped_palette_team_colour). Until
 * something needs it, the int API stands so no caller has to change.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Set the car's colour (0..255 per channel). Clamps. Idempotent. */
void jer_car_palette_set(int carId, int r, int g, int b);

/* Back to the car's stock paint. */
void jer_car_palette_clear(int carId);

/* 1 if the car has a colour; r/g/b receive it (or -1 each if off). */
int jer_car_palette_get(int carId, int* r, int* g, int* b);

/* Zero the whole table. Called once per level load, when car_data is cleared. */
void jer_car_palette_init(void);

#ifdef __cplusplus
}
#endif

#endif /* JER_CAR_PALETTE_H */
