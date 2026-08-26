#ifndef JER_MATH_H
#define JER_MATH_H

/* jer_math — the JERICHO core math helpers.
 *
 * Small fixed-point friendly helpers shared by the JERICHO modules and the
 * game (cars.c, denting.c include this header). Only helpers that are
 * actually used live here; dead code was removed in the bloat pass.
 */

#ifdef __cplusplus
extern "C" {
#endif

    // Quartic ease-out: sharp whip into the cap, long settling tail.
    // Used by cars.c (suspension/impact response).
    static inline float interpolate_quartic_ease_out(float x, float min, float max, float margin) {
        if (x < min) {
            return min;
        }
        else if (x < min + margin) {
            float t = (x - min) / margin;
            return min + margin * t * t * t * t;
        }
        else if (x > max) {
            return max;
        }
        else if (x > max - margin) {
            float t = (max - x) / margin;
            return max - margin * t * t * t * t;
        }
        else {
            return x;
        }
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

#ifdef __cplusplus
}
#endif

#endif /* JER_MATH_H */
