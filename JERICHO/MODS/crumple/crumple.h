// HEADER FOR VERTEX DEFORMATION SYSTEM (crumple.c)
//
// The crumple library replaces the old "move vertices toward the collision
// point" block inside denting.c with a proper impact-driven deformation:
//
//   1. An impact (collision point + normal + strength, in world space) is
//      recorded per car at collision time via crumple_recordImpact().
//   2. When the deferred denting pass runs, crumple_deform() consumes that
//      impact: it transforms the point/normal into the car's local space,
//      then for every vertex computes a signed projection of the
//      clean->collision offset onto the impact normal (always inward),
//      applies a radial falloff, clamps so a vertex can never travel past
//      the collision point, mixes in a small fraction of the artist-baked
//      damaged model for surface noise, and scales everything by a per-vertex
//      structural resistance derived from the model AABB (cabin = rigid,
//      nose/tail/roof = soft).
//   3. Hard impacts near a wheel accumulate a permanent per-wheel "bend"
//      (local-space offset) that the wheel raycast and the wheel draw both
//      consume, so crooked wheels scrub and fight the driver.
//
// Mass and health are folded in: a heavy car dents a light one more than the
// reverse, and a car that is already damaged crumples more easily.
//
// All math is the engine's 4096 fixed-point (ONE / FIXEDH / RSIN).
//
// Per-car state lives in global arrays inside crumple.c indexed by cp->id
// (same convention as gTempCarVertDump) so the core engine structs are
// untouched. See docs/crumple.md for the full design + tuning guide.

#ifndef CRUMPLE_H
#define CRUMPLE_H

#include "driver2.h"

//-----------------------------------------------------------------------------
// Tuning. All deformation / wheel scalars live here so the behaviour can be
// tweaked in one place. Every field is 4096-fixed-point unless noted.
//-----------------------------------------------------------------------------
typedef struct CRUMPLE_PARAMS
{
	// -- impact strength ---------------------------------------------------
	// howHard value that maps to a "full scale" deformation (see impactNormalize).
	// howHard / impactNormalize is clamped to 0..4096 before use.
	int impactNormalize;
	// howHard below this produces no deformation at all.
	int impactMinHowHard;
	// Maximum distance (world units) a vertex may be pushed by one impact.
	int maxDisplacement;
	// Maximum TOTAL displacement per vertex across all recorded impacts
	// (compounding guard — prevents a vertex inverting through the body).
	int compoundCap;
	// Per-vertex zone damage multiplier: 4096 + tempDamage * damageBoostScale / 4096.
	// tempDamage is the accumulated zone damage (0..~6142) from denting.c.
	// NOTE: keep at 0 — a per-zone boost steps at zone boundaries (visible
	// seams) and makes the dent shape depend on which panel was hit last.
	int damageBoostScale;

	// -- falloff -----------------------------------------------------------
	// Radial falloff radius (world units) around the collision point. Large
	// enough that a hit contaminates the surrounding body structure (hood,
	// fenders, doors) and not just the zone that was struck.
	int falloffRadius;
	// Oblique impacts (poles, edges — normal NOT aligned with a car face)
	// concentrate into a deeper, tighter dent; face-perpendicular hits
	// (walls, car sides) spread wide. This is the depth side of that split
	// (radius scales 0.5x..1.0x by normal alignment, hardcoded below).
	int contactDeepen;
	// 0 = linear falloff, 1 = squared (smoother edge). Extra curve headroom.
	int falloffSmooth;

	// -- structure ---------------------------------------------------------
	// Per-vertex resistance (0..4096) at the cabin centre (rigid) and at the
	// AABB extremes (soft). resistance = cabin - (cabin - nose) * u, where u
	// eases 0->1 across the region OUTSIDE the cabin (see cabinSize).
	int cabinResistance;
	int noseResistance;
	// How strongly resistance limits movement: 4096 = rigid parts barely move.
	int resistanceInfluence;
	// Cabin extent: t threshold (0..4096) that marks the rigid midsection.
	// t is the normalized distance from the AABB centre; 2048 = the central
	// ~half of the box in each axis stays rigid.
	int cabinSize;

	// -- global scale ------------------------------------------------------
	// Master deformation multiplier (4096 = 1.0x). Bump it to exaggerate
	// every crumple calculation for faster testing (applied to body
	// displacement and wheel kicks).
	int globalScale;

	// -- mass / health -----------------------------------------------------
	// Asymmetry clamps for (otherMass / victimMass) in 4096 fixed point.
	int massFactorMin;
	int massFactorMax;
	// Impact strength multiplier for hits against static/world geometry.
	int worldImpactFactor;
	// Damaged cars crumple more easily: softness += totalDamage/16 * healthInfluence.
	int healthInfluence;

	// -- surface noise -----------------------------------------------------
	// Fraction (0..4096) of the artist-baked damaged-model delta mixed in
	// per vertex for natural surface noise.
	int damageModelMix;

	// -- wheels ------------------------------------------------------------
	// Distance (world units, car-local) from a wheel at which an impact
	// counts as "near the wheel".
	int wheelProximity;
	// howHard threshold before a wheel starts bending.
	int wheelMinHowHard;
	// howHard value that applies a full wheelBendStep kick.
	int wheelNormalize;
	// Local-space offset (world units) added per full-strength impact.
	int wheelBendStep;
	// Max cumulative wheel-damage level (compounds across hits, see §6 —
	// the applied bend is level * direction-of-latest-kick).
	int wheelBendMaxLevel;
	// Maximum bend magnitude per component (world units).
	int wheelBendMaxX;
	int wheelBendMaxY;
	int wheelBendMaxZ;

	// -- impact curve ------------------------------------------------------
	// howHard response curve: 1 = linear (howHard/impactNormalize), 2 =
	// squared — hard/high-speed impacts deform disproportionately more.
	int impactCurve;

	// -- wheel physics -----------------------------------------------------
	// Lateral scrub/drag coefficient per unit of lateral+toe bend (0..4096).
	// A misaligned contact patch resists sideways motion; the applied force
	// is (bendMag * wheelScrubForce) against the wheel's lateral velocity.
	int wheelScrubForce;
	// Frames between wheel-bend kicks. Stops sustained contact (scraping a
	// wall) from grinding a wheel to maximum bend in a fraction of a second.
	int wheelBendCooldown;

	// -- wheel draw --------------------------------------------------------
	// Camber/toe angle scale (4096-fixed rotation units per world unit of
	// lateral / longitudinal bend). Positive = one direction; flip the sign
	// to reverse the lean. Sized so a MAX bend (wheelBendMax*) reads as real
	// degrees: e.g. 19370 -> 48 units * 19370 >> 12 = 227 = 20 deg.
	int wheelCamberScale;
	int wheelToeScale;
	// Steering-deviation scale: per-wheel yaw offset (4096-fixed rotation
	// units per unit of LATERAL bend) added to the wheel's normal steering
	// transform in the draw AND to its heading in the physics — a T-boned
	// wheel breaks from the steering and drags the car. 38827 -> 48 units *
	// 38827 >> 12 = 455 = 40 deg. Applies to the FRONT wheels.
	int wheelSteerScale;
	// Same for the REAR wheels, scaled way down: rears may visibly twist off
	// rotation, but they must not steer the car or distort the drive nearly
	// as much as the fronts. 3883 -> ~4 deg at max bend.
	int wheelSteerScaleRear;

	// -- debug / repair ----------------------------------------------------
	// howHard used by the pause-menu "simulate impact" (d-pad) feature.
	int simHowHard;
	// Per-frame howHard while a d-pad direction is HELD (grinding) — small
	// so the car isn't crushed flat instantly.
	int simTickHowHard;
	// Random deviation for simulated impacts: position jitter (world units)
	// and normal tilt (4096-fixed perturbation, ~2048 = up to ~7deg) so hits
	// land slightly off-centre / off-perpendicular for a more organic crush.
	int simJitter;
	int simAngleJitter;
	// Zone damage removed per frame while repairing (4095 max -> ~1s at 128).
	int repairRate;
} CRUMPLE_PARAMS;

// Central tuning surface; defaults live in crumple.c.
extern CRUMPLE_PARAMS gCrumpleParams;

//-----------------------------------------------------------------------------
// JERICHO module internals — this package (mods/crumple) is a compiled-in
// JERICHO module: the engine never calls crumple directly, it fires events
// (JER_EVENT_*) and this module's handlers implement the behavior. See
// crumple.c -> jer_module_crumple_entry() for the hook registration.
//-----------------------------------------------------------------------------

#endif /* CRUMPLE_H */
