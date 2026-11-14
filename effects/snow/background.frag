// Pass 1 (half resolution): a cold winter view. The wallpaper, softly blurred,
// cooled and darkened, with frost creeping in from the edges of the glass.

void main() {
    vec2 uv = wallpaperUV(vUV);
    // Frost: strongest in the corners, with an uneven, crystalline edge.
    vec2 c = (vUV - 0.5) * vec2(aspect(), 1.0);
    float edge = length(c) / length(vec2(aspect(), 1.0) * 0.5);   // 0 centre .. 1 corner
    vec2 q = vUV * vec2(aspect(), 1.0) * 9.0;
    float grain = 0.06 * (sin(q.x * 3.1 + sin(q.y * 2.3)) * sin(q.y * 2.7 + sin(q.x * 1.9)));
    float frost = smoothstep(0.55, 1.0, edge + grain) * p_frost;

    float lod = 1.0 + 4.0 * p_blur + 3.0 * frost;
    vec3 col = textureLodSmooth(uWallpaper, uv, lod).rgb;

    // Winter light: cooler, a little desaturated, darker overall.
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(col, lum * vec3(0.78, 0.86, 1.0) * 1.1, 0.45);
    col *= 1.0 - p_darken * 0.7;
    col = mix(col, vec3(0.86, 0.9, 0.96), frost * 0.55);

    fragColor = vec4(col, 1.0);
}
