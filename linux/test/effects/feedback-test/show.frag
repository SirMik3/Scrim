// Shows the self-test: dimmed wallpaper, magenta where the state check failed,
// and a green line at y = accumulated time * 0.06 (about 0.6 of the way down after 10 s).
void main() {
    vec4 s = texture(uPrevious, vUV);
    vec3 col = texture(uWallpaper, wallpaperUV(vUV)).rgb * 0.4;
    // uPrevious must line up with the screen too: its row tag is linear, so filtering keeps it exact.
    float misaligned = step(2.0 / uResolution.y, abs(s.a - vUV.y));
    col = mix(col, vec3(1.0, 0.0, 1.0), max(s.b, misaligned));
    float line = 1.0 - smoothstep(0.002, 0.006, abs(vUV.y - s.r));
    col = mix(col, vec3(0.1, 1.0, 0.3), line);
    fragColor = vec4(col, 1.0);
}
