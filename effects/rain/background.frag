// Pass 1 (half resolution): the view through fogged glass.
// Aspect-filled wallpaper, softened by a small blur, darkened and cooled.

vec3 mood(vec3 c) {
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 cool = lum * vec3(0.62, 0.74, 0.92) * 1.25;
    c = mix(c, cool, p_tint * 0.7);
    return c * (1.0 - p_darken * 0.75);
}

void main() {
    // 13-tap blur on two rings; radius grows with fog. Half-res + bilinear does the rest.
    float r = p_fog * 0.010;
    vec2 k = vec2(1.0 / aspect(), 1.0) * r;
    vec3 c = texture(uWallpaper, wallpaperUV(vUV)).rgb * 0.16;
    for (int i = 0; i < 6; i++) {
        float a = float(i) * 1.0471976;            // 60°
        vec2 d = vec2(cos(a), sin(a));
        c += texture(uWallpaper, wallpaperUV(vUV + d * k * 0.5)).rgb * 0.08;
        c += texture(uWallpaper, wallpaperUV(vUV + rot2(0.5236) * d * k)).rgb * 0.06;
    }

    // Fog: lift and flatten contrast slightly.
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(c, vec3(lum) * 0.85 + 0.08, p_fog * 0.22);

    c = mood(c);

    // Gentle vignette for depth.
    vec2 v = vUV - 0.5;
    c *= 1.0 - dot(v, v) * 0.45 * (0.3 + p_darken);

    fragColor = vec4(c, 1.0);
}
