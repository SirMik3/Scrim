// Pass 1 (half resolution, persistent): how fogged each spot of the glass is.
//   r = fog, 0 (clear) .. 1 (fully fogged). It builds up again over time, and
//       drops sliding down wipe it away, leaving clear trails that slowly fog over.
//   g = where the drops are right now (for highlights in glass.frag).
//
// Needs rgba16f: at 30 fps the fog grows by about 0.002 per step, which is less
// than half of one 8-bit step, so in rgba8 it would never grow at all.

const int MAX_DROPS = 32;

// Position of drop `i` in its cycle `cycle`, at progress u (0..1) down the glass,
// in aspect-corrected units (x * aspect, y). Drops stick and slip: they pause,
// then run, several times on the way down, wobbling a little sideways.
vec2 dropPos(float i, float cycle, float u) {
    vec3 h = hash31(i * 13.17 + cycle * 7.31);
    float jumps = floor(mix(4.0, 9.0, h.z));
    float s = u * jumps;
    float run = (floor(s) + smoothstep(0.0, 1.0, fract(s))) / jumps;
    float y = mix(-0.08, 1.08, run);
    float x = (0.03 + 0.94 * h.x) * aspect() + 0.006 * sin(y * 29.0 + h.y * 6.2832);
    return vec2(x, y);
}

float segmentDistance(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
    return length(p - a - ab * t);
}

void main() {
    if (uStateReset > 0.5) {
        fragColor = vec4(1.0, 0.0, 0.0, 1.0);   // start with fully fogged glass
        return;
    }
    float fog = texelFetch(uFeedback, ivec2(vUV * uResolution), 0).r;
    fog = min(fog + uStepDelta / p_refog, 1.0);

    vec2 q = vec2(vUV.x * aspect(), vUV.y);
    float tNow = uTime, tPrev = uTime - uStepDelta;
    float heads = 0.0;
    for (int k = 0; k < MAX_DROPS; k++) {
        float i = float(k);
        if (hash11(i * 3.71 + 0.5) > p_drops) continue;
        float period = mix(6.0, 14.0, hash11(i * 1.37 + 2.0)) / p_speed;
        float phase = hash11(i * 5.13 + 9.0) * period;
        float cNow = floor((tNow + phase) / period), cPrev = floor((tPrev + phase) / period);
        vec2 pNow = dropPos(i, cNow, fract((tNow + phase) / period));
        // Wipe the whole path since the last step, so trails are continuous at any frame rate.
        vec2 pPrev = cPrev == cNow ? dropPos(i, cPrev, fract((tPrev + phase) / period)) : pNow;
        float r = mix(0.005, 0.011, hash11(i * 9.7 + cNow)) * p_size;

        float wipe = 1.0 - smoothstep(r * 0.55, r, segmentDistance(q, pPrev, pNow));
        fog *= 1.0 - wipe;
        heads = max(heads, 1.0 - smoothstep(r * 0.6, r * 1.15, length(q - pNow)));
    }
    fragColor = vec4(fog, heads, 0.0, 1.0);
}
