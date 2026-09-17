// combatd2_internal.h — Combat D2: declarations shared BETWEEN this module's
// own source files.
//
// `combatd2.h` is the module's outward-facing header (its tunables and the few
// functions the weapon/AI/presentation files use). This one is the *internal*
// glue: the per-car state and hook handlers that the module entry in
// `combatd2.c` wires up, which now live in their own source files.
//
// File map (all flat siblings next to this header):
//
//   combatd2.c        core: config, per-car state, stats, bootstrap hooks,
//                     the module entry (which registers everything)
//   combatd2sim.c     the point-mass handling model (+ roll limit/recovery,
//                     wall restitution, physics params, the render lean)
//   combatd2respawn.c wreck age-out: destroyed-car respawn, traffic tumble,
//                     wreck mass accounting, the death bang
//   combatd2damage.c  damage scaling (scenery + car-vs-car)
//   combatd2menu.c    the pause menu
//   combatd2wreckfx.c the wreck explosion + kill credit (the "totaled" edge)
//   combatd2carfx.c   totaled-car presentation (flat black, no wheels, no
//                     engine, dropped body)
//   combatd2gearbox.c engine gearbox/rev-curve tuner
//   combatd2enginesnd.c engine rev + idle channel tuner
//   combatd2camerafx.c chase framing + speed FOV pull
//
// The module's subfolders each carry their own header, and are registered from
// the same entry:
//
//   factions/         the five teams - registry, roster, per-car assignment
//                     (factions/factions.h + factions.c; see FACTIONS.md)
//   ai/               the opponent AI (ai/ai.h; opponent.c is the brain)
//   weapons/          the weapon framework (weapons/core/weapon.h)
//   carhacks/         vehicle availability (carhacks/carhacks.h)

#ifndef CD2_INTERNAL_H
#define CD2_INTERNAL_H

#include "driver2.h"
#include "combatd2.h"
#include "jericho.h"

// ---------------------------------------------------------------------------
// Shared per-car state (owned by combatd2.c)
// ---------------------------------------------------------------------------
extern CD2_CAR gCd2Car[MAX_CARS];
extern int gCd2SceneryHits[MAX_CARS];	// scenery impacts per car this level
extern int gCd2TrafficLastHit[MAX_CARS];	// last scenery-hit count seen, per car

// ---------------------------------------------------------------------------
// Config (combatd2.c)
// ---------------------------------------------------------------------------
void cd2LoadConfig(void);
void cd2SaveConfig(void);
void cd2ApplyPreset(void);

// ---------------------------------------------------------------------------
// Car identity + stats (combatd2.c)
// ---------------------------------------------------------------------------
int cd2OwnsCar(CAR_DATA* cp);		// the player's car or an AI opponent
CD2_STATS cd2GetStats(CAR_DATA* cp);	// per-vehicle derived handling

// ---------------------------------------------------------------------------
// Hook handlers, registered by the module entry (combatd2.c)
// ---------------------------------------------------------------------------
int cd2OnCarPad(void* ud, void* args);			// combatd2sim.c
int cd2OnCarStep(void* ud, void* args);			// combatd2sim.c
int cd2OnCarTorque(void* ud, void* args);		// combatd2sim.c
int cd2OnGetWallRestitution(void* ud, void* args);	// combatd2sim.c
int cd2OnPhysicsParams(void* ud, void* args);		// combatd2sim.c
int cd2OnCarDraw(void* ud, void* args);			// combatd2sim.c
int cd2OnDebugTick(void* ud, void* args);		// combatd2sim.c
int cd2OnDamageScale(void* ud, void* args);		// combatd2damage.c
int cd2OnCarVsCar(void* ud, void* args);		// combatd2damage.c
int cd2OnGameStart(void* ud, void* args);		// combatd2respawn.c
int cd2OnFramePursuit(void* ud, void* args);		// combatd2.c (music hold)

// ---------------------------------------------------------------------------
// Wreck age-out (combatd2respawn.c), called from the sim's CAR_STEP
// ---------------------------------------------------------------------------
void cd2TrafficTumble(CAR_DATA* cp);
void cd2RespawnTick(CAR_DATA* cp);

// ---------------------------------------------------------------------------
// Deferred "Total Car" (combatd2sim.c): the pause menu QUEUES it and the next
// physics frame applies it, so the wreck + explosion never fire while the menu
// is up. The menu never touches the flag directly.
// ---------------------------------------------------------------------------
void cd2CarQueueTotal(void);

// The pause menu, registered by the entry so combatd2.c never sees cd2Menu
// (combatd2menu.c).
void cd2MenuRegister(JERICHO_CONTEXT* ctx);

#endif /* CD2_INTERNAL_H */
