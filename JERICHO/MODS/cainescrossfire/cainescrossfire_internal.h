// cainescrossfire_internal.h — Combat D2: declarations shared BETWEEN this module's
// own source files.
//
// `cainescrossfire.h` is the module's outward-facing header (its tunables and the few
// functions the weapon/AI/presentation files use). This one is the *internal*
// glue: the per-car state and hook handlers that the module entry in
// `cainescrossfire.c` wires up, which now live in their own source files.
//
// File map (all flat siblings next to this header):
//
//   cainescrossfire.c        core: config, per-car state, stats, bootstrap hooks,
//                     the module entry (which registers everything)
//   cainescrossfiresim.c     the point-mass handling model (+ roll limit/recovery,
//                     wall restitution, physics params, the render lean)
//   cainescrossfirerespawn.c wreck age-out: destroyed-car respawn, traffic tumble,
//                     wreck mass accounting, the death bang
//   cainescrossfiredamage.c  damage scaling (scenery + car-vs-car)
//   cainescrossfiremenu.c    the pause menu
//   cainescrossfirewreckfx.c the wreck explosion + kill credit (the "totaled" edge)
//   cainescrossfirecarfx.c   totaled-car presentation (flat black, no wheels, no
//                     engine, dropped body)
//   cainescrossfiregearbox.c engine gearbox/rev-curve tuner
//   cainescrossfireenginesnd.c engine rev + idle channel tuner
//   cainescrossfirecamerafx.c chase framing + speed FOV pull
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
#include "cainescrossfire.h"
#include "jericho.h"

// ---------------------------------------------------------------------------
// Shared per-car state (owned by cainescrossfire.c)
// ---------------------------------------------------------------------------
extern CD2_CAR gCd2Car[MAX_CARS];
extern int gCd2SceneryHits[MAX_CARS];	// scenery impacts per car this level
extern int gCd2TrafficLastHit[MAX_CARS];	// last scenery-hit count seen, per car

// ---------------------------------------------------------------------------
// Config (cainescrossfire.c)
// ---------------------------------------------------------------------------
void cd2LoadConfig(void);
void cd2SaveConfig(void);
void cd2ApplyPreset(void);

// ---------------------------------------------------------------------------
// Car identity + stats (cainescrossfire.c)
// ---------------------------------------------------------------------------
int cd2OwnsCar(CAR_DATA* cp);		// the player's car or an AI opponent
CD2_STATS cd2GetStats(CAR_DATA* cp);	// per-vehicle derived handling

// ---------------------------------------------------------------------------
// Hook handlers, registered by the module entry (cainescrossfire.c)
// ---------------------------------------------------------------------------
int cd2OnCarPad(void* ud, void* args);			// cainescrossfiresim.c
int cd2OnCarStep(void* ud, void* args);			// cainescrossfiresim.c
int cd2OnCarTorque(void* ud, void* args);		// cainescrossfiresim.c
int cd2OnGetWallRestitution(void* ud, void* args);	// cainescrossfiresim.c
int cd2OnPhysicsParams(void* ud, void* args);		// cainescrossfiresim.c
int cd2OnCarDraw(void* ud, void* args);			// cainescrossfiresim.c
int cd2OnDebugTick(void* ud, void* args);		// cainescrossfiresim.c
int cd2OnDamageScale(void* ud, void* args);		// cainescrossfiredamage.c
int cd2OnCarVsCar(void* ud, void* args);		// cainescrossfiredamage.c
int cd2OnGameStart(void* ud, void* args);

/* handling.c. Set while a match is running so the player cannot leave his car by
 * choice - a bail mid-race leaves the grid a car short. Cleared again on shutdown. */
extern int gBlockPlayerExit;		// cainescrossfirerespawn.c
int cd2OnFramePursuit(void* ud, void* args);		// cainescrossfire.c (music hold)

// ---------------------------------------------------------------------------
// Wreck age-out (cainescrossfirerespawn.c), called from the sim's CAR_STEP
// ---------------------------------------------------------------------------
void cd2TrafficTumble(CAR_DATA* cp);
void cd2RespawnTick(CAR_DATA* cp);

// ---------------------------------------------------------------------------
// Deferred "Total Car" (cainescrossfiresim.c): the pause menu QUEUES it and the next
// physics frame applies it, so the wreck + explosion never fire while the menu
// is up. The menu never touches the flag directly.
// ---------------------------------------------------------------------------
void cd2CarQueueTotal(void);

// The pause menu, registered by the entry so cainescrossfire.c never sees cd2Menu
// (cainescrossfiremenu.c).
void cd2MenuRegister(JERICHO_CONTEXT* ctx);

#endif /* CD2_INTERNAL_H */
