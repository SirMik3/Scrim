// Pass 2 (half resolution): falling rain behind the glass.
// Procedural streaks in three parallax layers. Each column of tall cells may
// hold one streak; nothing is simulated, so the cost is fixed per pixel.

float streakLayer(vec2 uv, float scale, float speed, float seed, float density) {
    const float cellH = 4.0;                       // cells are 4x taller than wide
    vec2 p = vec2(uv.x * scale, uv.y * scale / cellH);
    float col = floor(p.x);
    p.y += hash11(col * 3.7 + seed) * 17.0;        // stagger columns
    p.y -= uTime * speed * scale / cellH;          // fall downwards (+y)

    vec2 id = vec2(col, floor(p.y));
    vec2 f = fract(p) - 0.5;
    vec3 h = hash31(id.x * 41.3 + id.y * 289.1 + seed);

    float present = smoothstep(h.x, h.x + 0.08, density);   // exactly 0 when density is 0

    // Thin line, about one output pixel wide whatever the layer scale.
    float px = scale / uResolution.y;
    float x = f.x - (h.y - 0.5) * 0.7;
    float across = 1.0 - smoothstep(0.0, max(px * 0.9, 0.025), abs(x));

    // Faint tail at the top, bright head at the bottom.
    float t = (f.y + 0.5 - h.z * 0.4) / 0.55;
    float along = smoothstep(0.0, 1.0, t) * (1.0 - smoothstep(0.92, 1.0, t));

    return present * across * along;
}

void main() {
    vec3 c = texture(uPrevious, vUV).rgb;

    // Aspect-correct space centred on the screen, tilted by the wind.
    vec2 uv = (vUV - 0.5) * vec2(aspect(), 1.0);
    uv = rot2(p_wind) * uv;

    float speed = p_rainSpeed;
    float far  = streakLayer(uv, 70.0, 0.9 * speed, 11.0, p_rain * 0.8);
    float mid  = streakLayer(uv, 38.0, 1.3 * speed, 23.0, p_rain * 0.65);
    float near = streakLayer(uv, 20.0, 1.9 * speed, 37.0, p_rain * 0.45);

    float s = far * 0.35 + mid * 0.6 + near * 1.0;
    vec3 streakColor = vec3(0.82, 0.88, 0.95);
    // Screen blend keeps highlights from clipping harshly.
    c = 1.0 - (1.0 - c) * (1.0 - streakColor * clamp(s * p_streakGlow, 0.0, 1.0));

    fragColor = vec4(c, 1.0);
}
