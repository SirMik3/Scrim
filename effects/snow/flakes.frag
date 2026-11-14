// Pass 2 (full resolution): snowflakes in five depth layers over the background.
// Near flakes are larger, faster and out of focus; far ones are small and sharp.
// Each flake lives in one grid cell per layer and sways within it, so a pixel
// only has to look at one cell per layer.

const int LAYERS = 5;

float flakes(vec2 uv, float depth, float seed) {
    float cells = mix(26.0, 5.0, depth);                 // cells per screen height
    float fall = mix(0.035, 0.16, depth) * p_speed;      // screen heights per second
    vec2 p = vec2(uv.x * aspect(), uv.y) * cells;
    p.y -= uTime * fall * cells;                         // +y is down: flakes move down
    p.x -= uTime * p_wind * fall * cells * 0.8;          // wind blows them sideways

    vec2 id = floor(p);
    vec2 f = fract(p) - 0.5;
    if (hash12(id + seed) > p_amount * mix(0.45, 1.0, depth)) return 0.0;   // far layers are sparser
    vec2 h = hash22(id + seed * 1.7);
    vec2 centre = (h - 0.5) * 0.45;
    centre.x += sin(uTime * (0.7 + h.x) + h.y * 6.2832) * 0.15;   // sway
    float r = mix(0.05, 0.1, h.y) * p_size;
    float soft = mix(0.25, 0.9, depth * depth);          // focus: sharp far away, bokeh up close
    float d = length(f - centre);
    return (1.0 - smoothstep(r * (1.0 - soft), r, d)) * mix(0.5, 0.85, depth);
}

void main() {
    vec3 col = texture(uPrevious, vUV).rgb;
    float snow = 0.0;
    for (int i = 0; i < LAYERS; i++) {
        float depth = float(i) / float(LAYERS - 1);
        snow = max(snow, flakes(vUV, depth, float(i) * 13.1));
    }
    col = mix(col, vec3(0.95, 0.97, 1.0), snow);
    fragColor = vec4(col, 1.0);
}
