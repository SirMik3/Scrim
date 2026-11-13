// Pass 3 (full resolution): water on the glass.
// Two layers of resting drops plus drops sliding down with a trail of droplets.
// Each drop is a small lens: it shows an inverted, sharper view of the scene.

struct Drop {
    vec2 n;     // position inside the drop, -1..1 from its centre (for shading)
    vec2 off;   // same vector in uv units (for refraction)
    float m;    // coverage 0..1
    float wet;  // wet trail left behind a sliding drop (clears the fog)
};

Drop noDrop() { return Drop(vec2(0.0), vec2(0.0), 0.0, 0.0); }

// Drops resting on the glass. They appear, linger and evaporate.
Drop restingDrops(vec2 uv, float scale, float seed) {
    vec2 p = uv * scale;
    vec2 id = floor(p);
    vec2 f = fract(p) - 0.5;

    float r1 = hash12(id + seed);
    float r2 = hash12(id + seed + 17.3);
    float present = step(r1, p_drops);

    float life = fract(uTime * 0.025 * (0.6 + r2) + r1 * 7.13);
    float grow = smoothstep(0.0, 0.06, life) * (1.0 - smoothstep(0.55, 1.0, life));

    float fullR = min(mix(0.1, 0.24, r2) * p_dropSize, 0.3);
    vec2 centre = (hash22(id + seed + 5.1) - 0.5) * (1.0 - 2.0 * fullR - 0.04);
    float R = fullR * grow * present;
    if (R < 1e-5) return noDrop();   // no drop in this cell right now

    vec2 d = f - centre;
    float dist = length(d * vec2(1.0, 0.92));
    float m = 1.0 - smoothstep(R * 0.72, R, dist);

    Drop o;
    o.n = d / max(R, 1e-4) * m;
    o.off = d / scale * m;
    o.m = m;
    o.wet = 0.0;
    return o;
}

// Drops sliding down in stick-slip jerks, leaving a wet trail and droplets.
Drop slidingDrops(vec2 uv, float scale, float seed) {
    const vec2 cell = vec2(1.0, 8.0);               // tall cells, in 1/scale units
    vec2 p = uv * scale / cell;
    float col = floor(p.x);
    p.y += hash11(col * 7.7 + seed) * 11.0;         // stagger columns
    vec2 id = vec2(col, floor(p.y));
    vec2 q = (fract(p) - 0.5) * cell;               // isotropic cell-local coords, +y down

    float r1 = hash12(id + seed);
    float r2 = hash12(id + seed + 3.7);
    float r3 = hash12(id + seed + 9.1);
    float present = step(r1, p_sliding);

    // One trip per period. tt is monotonic but slows down in places (stick-slip).
    float period = mix(4.0, 9.0, r2);
    float t = fract(uTime / period + r3 * 13.0);
    float w = 18.0 * (0.6 + r2);
    float tt = t + 0.85 * sin(t * w) / w;

    float R = min(0.24 * p_dropSize, 0.36);
    float halfH = cell.y * 0.5 - R - 0.1;
    float y = mix(-halfH, halfH, tt);
    float baseX = (r2 - 0.5) * (1.0 - 2.0 * R - 0.25);
    float fade = smoothstep(0.0, 0.05, t) * (1.0 - smoothstep(0.92, 1.0, t)) * present;
    if (fade < 1e-5) return noDrop();

    // The path wanders a little; trail and drop share it.
    #define PATH_X(yy) (baseX + 0.09 * sin((yy) * 1.3 + r3 * 6.2832))

    // Main drop, slightly wider at the bottom.
    vec2 d = q - vec2(PATH_X(y), y);
    float stretch = d.y > 0.0 ? 0.95 : 0.8;
    float dist = length(d * vec2(1.0, stretch));
    float Rf = R * fade;
    float m = 1.0 - smoothstep(Rf * 0.75, Rf, dist);

    Drop o;
    o.n = d / max(Rf, 1e-4) * m;
    o.off = d / scale * m;
    o.m = m;

    // Wet trail above the drop.
    float above = y - q.y;
    float trailLen = mix(1.5, 3.5, r3);
    float trailFade = smoothstep(0.0, 0.3, above) * (1.0 - smoothstep(0.0, trailLen, above)) * fade;
    float tx = abs(q.x - PATH_X(q.y));
    o.wet = (1.0 - smoothstep(R * 0.2, R * 0.7, tx)) * trailFade;

    // Little droplets left along the trail.
    const float spacing = 0.38;
    float sy = (floor(q.y / spacing) + 0.5) * spacing;
    float sAbove = y - sy;
    float sr = hash12(vec2(sy * 31.0, id.x + seed));
    float tr = 0.11 * p_dropSize * (0.35 + 0.65 * sr)
             * step(0.4, sAbove) * (1.0 - smoothstep(0.2, trailLen, sAbove)) * step(0.3, sr) * fade;
    vec2 td = q - vec2(PATH_X(sy) + (sr - 0.5) * 0.08, sy);
    float tm = tr > 1e-5 ? 1.0 - smoothstep(tr * 0.7, tr, length(td)) : 0.0;
    if (tm > o.m) {
        o.n = td / max(tr, 1e-4) * tm;
        o.off = td / scale * tm;
        o.m = tm;
    }
    #undef PATH_X
    return o;
}

vec3 mood(vec3 c) {
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 cool = lum * vec3(0.62, 0.74, 0.92) * 1.25;
    c = mix(c, cool, p_tint * 0.7);
    return c * (1.0 - p_darken * 0.65);
}

vec3 sharpView(vec2 uv) {
    return mood(texture(uWallpaper, wallpaperUV(uv)).rgb);
}

void main() {
    vec2 uv = vUV * vec2(aspect(), 1.0);           // aspect-correct, +y down

    Drop best = restingDrops(uv, 42.0, 1.0);
    Drop b = restingDrops(uv, 15.0, 61.0);
    if (b.m > best.m) best = b;
    Drop s = slidingDrops(uv, 22.0, 131.0);
    float wet = s.wet;
    if (s.m > best.m) best = s;

    vec3 base = texture(uPrevious, vUV).rgb;

    // Lens: sample on the opposite side of the drop centre (inverted view).
    float k = mix(0.3, 3.0, p_refraction);
    vec2 sUV = vUV - best.off * vec2(1.0 / aspect(), 1.0) * (1.0 + k);
    vec3 through = mix(texture(uPrevious, sUV).rgb, sharpView(sUV), p_clarity);

    vec3 c = base;
    // Wet trail: the fog is wiped away, the view is a little clearer.
    c = mix(c, mix(base, sharpView(vUV), p_clarity * 0.7), wet * 0.75 * (1.0 - best.m));
    c = mix(c, through, best.m);

    // Shading: darker rim and a soft highlight from the top-left.
    float rr = length(best.n);
    c *= 1.0 - smoothstep(0.55, 1.0, rr) * best.m * 0.35;
    float spec = pow(clamp(dot(best.n, normalize(vec2(-0.55, -0.8))), 0.0, 1.0), 5.0);
    c += spec * best.m * 0.18;

    fragColor = vec4(c, 1.0);
}
