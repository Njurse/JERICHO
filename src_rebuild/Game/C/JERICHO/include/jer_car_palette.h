#ifndef JER_CAR_PALETTE_H
#define JER_CAR_PALETTE_H

/* Per-INSTANCE car colour.
 *
 * A cover-all data addition that is inert by default: nothing is tinted unless a
 * module (or the editor) sets a colour for a specific car. It keys off the CAR_DATA
 * slot (car_data[] index), which is stable for the length of a session on every
 * machine -- so the same value can travel in multiplayer and mean the same thing
 * on the owner and on every peer.
 *
 * Colour order is plain 8-bit RGB here; the renderer does its own packing (the
 * engine's polygon words are B<<16|G<<8|R, and CLUT entries are 5/5/5).
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
