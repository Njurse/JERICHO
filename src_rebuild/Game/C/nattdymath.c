// Smooth step for a single float, will implement a proper lerp if theres not already one or i think its retarded
// expert deluxe C developer Nattdy
static inline float smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

// Soft clamp: identity in the middle, smoothly saturates at the caps.
float soft_clamp(float x, float min, float max, float margin) {
    if (x < min) {
        return min;
    }
    else if (x < min + margin) {
        float t = (x - min) / margin;          // 0..1
        return min + margin * smoothstep(t);
    }
    else if (x > max) {
        return max;
    }
    else if (x > max - margin) {
        float t = (x - (max - margin)) / margin; // 0..1
        return (max - margin) + margin * smoothstep(t);
    }
    else {
        return x;   // inside the linear region
    }
}