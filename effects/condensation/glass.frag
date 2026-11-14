// Pass 2 (full resolution): the wallpaper seen through the glass. Fogged areas are
// blurred and lifted, clear trails are sharp, and drop heads catch a little light.

void main() {
    vec4 s = texture(uPrevious, vUV);
    float fog = s.r * p_fog;
    vec2 uv = wallpaperUV(vUV);

    vec3 sharp = texture(uWallpaper, uv).rgb;
    vec3 blurred = textureLodSmooth(uWallpaper, uv, 1.0 + 5.0 * p_fog).rgb;
    vec3 c = mix(sharp, blurred, smoothstep(0.0, 0.6, fog));

    // Fog scatters light: lift and cool it slightly.
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(c, vec3(lum) * vec3(0.92, 0.97, 1.05) + 0.07, fog * 0.35);

    // Drop heads: a lens that flips what's behind it, plus a soft highlight on top.
    float head = s.g;
    vec2 e = 1.5 / vec2(textureSize(uPrevious, 0));
    vec2 slope = vec2(texture(uPrevious, vUV + vec2(e.x, 0.0)).g - texture(uPrevious, vUV - vec2(e.x, 0.0)).g,
                      texture(uPrevious, vUV + vec2(0.0, e.y)).g - texture(uPrevious, vUV - vec2(0.0, e.y)).g);
    vec3 lens = texture(uWallpaper, wallpaperUV(vUV + slope * 0.02)).rgb;
    c = mix(c, lens * 1.05, head * 0.85);
    c += max(-slope.y, 0.0) * head * 0.25;

    c *= 1.0 - p_darken * 0.7;
    fragColor = vec4(c, 1.0);
}
