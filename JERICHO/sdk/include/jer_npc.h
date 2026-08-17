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
