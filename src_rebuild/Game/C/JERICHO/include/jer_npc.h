#ifndef JER_NPC_H
#define JER_NPC_H

/* jer_npc — the JERICHO pedestrian/NPC scaffolding library.
 *
 * Thin wrappers over the game's ped API so modules can script peds (the
 * future pedestrian-AI / police-bail-out systems) without digging into
 * pedest.c. The implementations live in the GAME (jer_npc.c) because only
 * the game sees the ped types; the core stays header-free and hands back
 * an opaque handle. A spawned NPC is a stock pedestrian: it walks with the
 * game's state machines, so the usual animation/behaviour hooks apply.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JerNpc
{
	void* ped;	/* LPPEDESTRIAN, opaque */
} JerNpc;

/* spawn a pedestrian at world (x, z) — the ground Y is snapped to the map.
 * Returns NULL on failure (the ped table is full). */
JerNpc* jer_npc_spawn(int x, int z);

/* spawn a pedestrian of a given model type: 0 = Tanner, 1 = the other player
 * model, 3 = civilian. Civilians are recycled by the ambient-pedestrian system
 * once they are far from the camera, so a ped a module wants to keep attached
 * to something (e.g. a crew member mounted on a car) should use 0 or 1. */
JerNpc* jer_npc_spawn_model(int pedModel, int x, int z);

/* freeze a ped where it stands: both state slots become no-ops, so it neither
 * walks, turns nor re-poses, and its current animation frame is held. Use it
 * before driving the transform yourself with jer_npc_set_world. */
void jer_npc_park(JerNpc* n);

/* hold an action pose: type = a PED_ACTION_* value (e.g. PED_ACTION_GETOUTCAR)
 * with the animation frozen at `frame` (0..15; 14 is the last "climbing out"
 * frame). Both the parsed `frame1` animation frame and the raw motion block
 * for `action` are set. Parks the ped as well, so the pose stays put. */
void jer_npc_set_action(JerNpc* n, int action, int frame);

/* place the ped outright: world x/z, raw engine Y (`position.vy`; the engine
 * is Y-DOWN, so ground level is -MapHeight and the ped origin sits 130 units
 * above it) and the whole-body yaw (0..4095). A parked ped holds this exactly,
 * so a module can hang it out of a car window from the car's transform. */
void jer_npc_set_world(JerNpc* n, int x, int y, int z, int yaw);

/* remove the ped from the world (safe on NULL / already-despawned) */
void jer_npc_despawn(JerNpc* n);

/* face a heading (0..4095) without moving */
void jer_npc_face(JerNpc* n, int heading);

/* set the movement speed (positive = forward, negative = backpedal; the
 * ped must already be in a run/walk state) */
void jer_npc_set_speed(JerNpc* n, int speed);

/* walk toward a world point at the given speed (turns + runs; the module
 * should poll or re-issue each frame while chasing) */
void jer_npc_move_to(JerNpc* n, int x, int z, int speed);

/* halt the ped (speed 0 + idle state) */
void jer_npc_stop(JerNpc* n);

/* query: the ped's current speed (for decision making) */
int jer_npc_speed(const JerNpc* n);

/* STUB for the future police AI: have the ped's driver (if any) bail out
 * of its car onto the street. Returns 0 when unimplemented/not possible. */
int jer_npc_leave_car(JerNpc* n);

#ifdef __cplusplus
}
#endif

#endif /* JER_NPC_H */
