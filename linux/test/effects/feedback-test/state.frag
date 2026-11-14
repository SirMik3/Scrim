// Persistent test state, one texel per pixel of this pass:
//   r = accumulated uStepDelta * 0.06, wrapped to 0..1 (drives the bar in show.frag)
//   g = -vUV.x  (negative: proves rgba16f storage; also tags the texel's column)
//   b = sticky error flag
//   a = vUV.y   (tags the texel's row)
// Every step checks that uFeedback returned this texel's own tags. A host that mirrors,
// shifts or clamps the state sets the flag, and show.frag paints it magenta.
void main() {
    if (uStateReset > 0.5) {
        fragColor = vec4(0.0, -vUV.x, 0.0, vUV.y);
        return;
    }
    vec4 prev = texelFetch(uFeedback, ivec2(vUV * uResolution), 0);
    vec2 tol = 0.5 * uTexel;
    float bad = step(tol.x, abs(prev.g + vUV.x)) + step(tol.y, abs(prev.a - vUV.y));
    fragColor = vec4(fract(prev.r + uStepDelta * 0.06), -vUV.x, max(prev.b, min(bad, 1.0)), vUV.y);
}
