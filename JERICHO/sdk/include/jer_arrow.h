#ifndef JER_ARROW_H
#define JER_ARROW_H

/* jer_arrow - the game's bouncing 3D target arrow, for modules.
 *
 * This is the stock mission arrow: Draw3DTarget(pos, 0x1) in targets.c, the red
 * arrow that bobs above a getaway car or a pickup point. A module registers an
 * arrow in a slot and the ENGINE draws it every frame, in the world pass right
 * after the cars (main.c, before JER_EVENT_DRAW_WORLD), until the slot is
 * cleared - the same set-and-forget shape as jer_hud_panel. A module needs no
 * draw hook of its own and must not call these from the frontend.
 *
 * Coordinates are the raw game frame, the same frame as CAR_DATA hd.where.t[]
 * (y is negative upward). y IS used: the arrow is a marker drawn a little above
 * a known point, not a spawn, so pass the point's real height. Passing a car's
 * hd.where.t[0..2] to jer_arrow_at draws the same arrow jer_arrow_on_car does.
 *
 * Every slot is dropped by the engine on the way out of a level and on the
 * frontend entry, so a stale car pointer can never be drawn in the next level.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_ARROW_MAX	8	/* arrows a module set can have up at once */

/* Bouncing arrow above the world point (x, y, z). Returns 1, or 0 for a bad slot.
 * (jer_arrow_on_car also lifts the arrow by the car's half-height plus a small gap,
 * so it clears the roof of a tall vehicle; jer_arrow_at does not.) */
int jer_arrow_at(int slot, int x, int y, int z);

/* Bouncing arrow that follows `car` (a CAR_DATA*) every frame. A NULL car, a
 * pointer outside car_data[] or a car that has left the world (control type
 * NONE) clears the slot. Returns 1 when the arrow is set, 0 otherwise. */
int jer_arrow_on_car(int slot, void* car);

/* Drop one slot / every slot. */
void jer_arrow_clear(int slot);
void jer_arrow_clear_all(void);

/* 1 while `slot` holds an arrow. */
int jer_arrow_active(int slot);

/* Engine-internal: draw the registered arrows for the current view. Called once
 * per view from DrawGame, right after DrawAllTheCars. Modules do not call this. */
void jer_arrow_draw(void);

#ifdef __cplusplus
}
#endif

#endif
