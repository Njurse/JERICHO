#ifndef NATTDYMATH_H
#define NATTDYMATH_H


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











#ifdef __cplusplus
}
#endif

#endif /* NATTDYMATH_H */