#ifndef JER_MATH_H
#define JER_MATH_H

/* jer_math — the JERICHO core math helpers.
 *
 * Originated as Nattdy's nattdymath (soft clamps, interpolations, radial
 * collision test) and now lives inside the JERICHO SDK as a core element
 * so both the game and every module share one home for small math helpers.
 * The game's vanilla files include it too (cars.c, denting.c).
 */

// ALERT: MESSY HO SHIT IN CODE I NEED TO WRITE AN EXPONENT FUNCTION

// This is probably unnecessary AI silliness
// I'm rusty on headers but the math not so much
// Expect to see polish as this library grows
#ifdef __cplusplus
extern "C" {
#endif

    // Maybe this already exists under another name but
    // There'll be other interpolation methods I want here anyway
    static inline float soft_clamp(float x, float min, float max) {
        if (min == 0.0f && max == 0.0f) {
            min = -1.0f;
            max = 1.0f;
        }

        if (x <= min) return min;
        if (x >= max) return max;

        float t = (x - min) / (max - min);  // 0 → 1
        return min + (max - min) * (t * t * (3.0f - 2.0f * t));
    }
    // Quadratic ease‑out soft clamp.
    // Moves quickly when entering the margarine zone, then slows to a stop at the cap.
    static inline float interpolate_quad_ease_out(float x, float min, float max, float margin) {
        if (x < min) {
            return min;
        }
        else if (x < min + margin) {
            // Approaching min from above – t goes 1 (fast) → 0 (stop)
            float t = (x - min) / margin;
            return min + margin * t * t;
        }
        else if (x > max) {
            return max;
        }
        else if (x > max - margin) {
            // Approaching max from below – t goes 1 (fast) → 0 (stop)
            float t = (max - x) / margin;
            return max - margin * t * t;
        }
        else {
            return x; // Linear / unchanged in the middle
        }
    }
    // Quartic ease-out: sharp whip into the cap, long settling tail.
    // Inspired by the "snap and hang" of the Bouncing Yaris meme.
    static inline float interpolate_quartic_ease_out(float x, float min, float max, float margin) {
        if (x < min) {
            return min;
        }
        else if (x < min + margin) {
            float t = (x - min) / margin;  // 1 (entry) -> 0 (cap)
            return min + margin * t * t * t * t * t * t * t * t * t * t * t * t * t * t;
        }
        else if (x > max) {
            return max;
        }
        else if (x > max - margin) {
            float t = (max - x) / margin;  // 1 (entry) -> 0 (cap)
            return max - margin * t * t * t * t * t * t * t * t * t * t * t * t * t * t;
        }
        else {
            return x;
        }
    }



    // Some fancier stuff like for car crumple - we need to evaluate a distance from the impact to check if a point is close enough
    // Then later we can add calculations along normals to further improve fidelity
    static inline bool CheckRadialCollision(VECTOR* centerA, float radiusA, VECTOR* centerB, float radiusB) {
        float dx = centerB->vx - centerA->vx;
        float dz = centerB->vz - centerA->vz;
        float distSq = dx * dx + dz * dz;

        float threshold = radiusA + radiusB;
        float thresholdSq = threshold * threshold;

        // If squared distance is less than squared threshold, we have a hit!
        return distSq < thresholdSq;
    }











    // ------------------------------------------------------------------
    // Int helpers shared by the JERICHO modules (camera free-look, weapon
    // aim, etc.). Fixed-point friendly: no floats.
    // ------------------------------------------------------------------

    static inline int jer_clamp_int(int v, int lo, int hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    // Exponential approach: value moves 1/div of the way to target each call.
    // Used for the camera settle-back and pitch smoothing (div ~ 2..8).
    static inline int jer_lerp_int(int from, int to, int div) {
        if (div <= 0)
            return to;
        return from + (to - from) / div;
    }

    // Signed angular difference in PSX angle units (0..4095 = full circle),
    // result in -2048..2047, matching the game's DIFF_ANGLES(A, B).
    static inline int jer_angle_diff(int a, int b) {
        return ((((b) - (a)) + 2048) & 4095) - 2048;
    }

    // Linear interpolation between two ints; t is 0..4096 (0 = from, 4096 = to).
    // Use for value tweens that need an explicit progress.
    static inline int jer_lerp(int from, int to, int t) {
        if (t <= 0) return from;
        if (t >= 4096) return to;
        return from + ((to - from) * t >> 12);
    }

    // Smooth an angle toward a target on the shortest path: moves 1/div of
    // the remaining signed difference per call. div ~ 2..8; larger = slower.
    // Handles the 0..4095 wraparound (unlike jer_lerp_int on angles).
    static inline int jer_lerp_angle(int from, int to, int div) {
        int diff;

        if (div <= 0)
            return to & 4095;

        diff = jer_angle_diff(from, to);
        return (from + diff / div) & 4095;
    }

    // Smooth step on an int over [0..range]; returns 0..range.
    static inline int jer_smooth_step(int t, int range) {
        int x = jer_clamp_int(t, 0, range);

        // x * x * (3*range - 2*x) / range^2 in int math (range > 0)
        if (range <= 0)
            return 0;

        return (x * x * (3 * range - 2 * x)) / (range * range);
    }

#ifdef __cplusplus
}
#endif

#endif /* JER_MATH_H */