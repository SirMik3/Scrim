// Rain on Glass — based on "Heartfelt" by Martijn Steinrucken (BigWIngs),
// https://www.shadertoy.com/view/ltffzl, via a modified Shadertoy variant.
// License: CC BY-NC-SA 3.0 — see NOTICE.txt in this folder.
// Ported to the Scrim effect format; see NOTICE.txt for the list of changes.

#define S(a, b, t) smoothstep(a, b, t)

vec3 N13(float p) {
    vec3 p3 = fract(vec3(p) * vec3(.1031, .11369, .13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
}

float N(float t) {
    return fract(sin(t * 12345.564) * 7658.76);
}

float Saw(float b, float t) {
    return S(0., b, t) * S(1., b, t);
}

vec4 DropLayer2(vec2 uv, float t) {
    vec2 UV = uv;
    uv.y += t * 0.75;
    vec2 a = vec2(6., 1.);
    vec2 grid = a * 2.;
    vec2 id = floor(uv * grid);

    float colShift = N(id.x);
    uv.y += colShift;

    id = floor(uv * grid);
    vec3 n = N13(id.x * 35.2 + id.y * 2376.1);
    vec2 st = fract(uv * grid) - vec2(.5, 0);

    float x = n.x - .5;
    float y = UV.y * 20.;
    float wiggle = sin(y + sin(y));
    x += wiggle * (.5 - abs(x)) * (n.z - .5);
    x *= .7;
    float ti = fract(t + n.z);
    y = (Saw(.85, ti) - .5) * .9 + .5;
    vec2 p = vec2(x, y);

    vec2 diff = st - p;
    float d = length(diff * a.yx);

    float mainDrop = S(.4, .0, d);

    float r = sqrt(S(1., y, st.y));
    float cd = abs(st.x - x);
    float trail = S(.23 * r, .15 * r * r, cd);
    float trailFront = S(-.02, .02, st.y - y);
    trail *= trailFront * r * r;

    y = UV.y;
    float trail2 = S(.2 * r, .0, cd);
    float droplets = max(0., (sin(y * (1. - y) * 120.) - st.y)) * trail2 * trailFront * n.z;
    y = fract(y * 10.) + (st.y - .5);
    float dd = length(st - vec2(x, y));
    droplets = S(.3, 0., dd);
    float m = mainDrop + droplets * r * trailFront;

    return vec4(diff.x * a.x, diff.y * a.y, m, trail);
}

float StaticDrops(vec2 uv, float t) {
    uv *= 40.;
    vec2 id = floor(uv);
    uv = fract(uv) - .5;
    vec3 n = N13(id.x * 107.45 + id.y * 3543.654);
    vec2 p = (n.xy - .5) * .7;
    float d = length(uv - p);

    float fade = Saw(.025, fract(t + n.z));
    float c = S(.3, 0., d) * fract(n.z * 10.) * fade;
    return c;
}

vec4 DropsWithOffset(vec2 uv, float t, float l0, float l1, float l2) {
    float s = StaticDrops(uv, t) * l0;
    vec4 m1 = DropLayer2(uv, t) * l1;
    vec4 m2 = DropLayer2(uv * 1.85, t) * l2;

    float c = s + m1.z + m2.z;
    c = S(.3, 1., c);

    vec2 offset = vec2(0.0);
    if (m1.z > m2.z) {
        offset = m1.xy;
    } else if (m2.z > 0.0) {
        offset = m2.xy;
    }
    float trail = max(m1.w * l1, m2.w * l2);

    return vec4(c, trail, offset.x, offset.y);
}

void main() {
    // The original is written for Shadertoy's y-up fragCoord; Scrim's vUV is y-down.
    vec2 fragCoord = vec2(vUV.x, 1.0 - vUV.y) * uResolution;
    vec2 uv = (fragCoord - .5 * uResolution) / uResolution.y;
    vec2 UV = fragCoord / uResolution;

    uv *= p_zoom;
    UV = (UV - .5) * p_zoom + .5;

    float t = uTime * p_speed;
    float rainAmount = p_rain;

    float staticDrops = S(-.5, 1., rainAmount) * 2.;
    float layer1 = S(.25, .75, rainAmount);
    float layer2 = S(.0, .5, rainAmount);

    vec4 dropData = DropsWithOffset(uv, t, staticDrops, layer1, layer2);
    float alpha = dropData.x;
    float trail = dropData.y;
    vec2 offsetToCenter = dropData.zw;

    float distFromCenter = length(offsetToCenter);
    float sizeDamping = mix(1.0, 0.4, alpha);
    float refractIndex = alpha * (1.0 - distFromCenter * 0.5) * sizeDamping;
    vec2 refractDir = normalize(offsetToCenter + vec2(0.51));
    float strength = refractIndex * (0.03 + trail * 0.04);
    vec2 refraction = -refractDir * strength;

    // Fogged → clearer along trails → clear inside drops (mip level = blur).
    float focus = mix(p_fog - trail * 2.0, p_clear, S(.1, .2, alpha));

    vec2 suv = UV + refraction;
    suv.y = 1.0 - suv.y;                       // back to Scrim's y-down uv
    // Smooth filtering: plain textureLod looks blocky when a small mip is stretched to a Retina screen.
    vec3 col = textureLodSmooth(uWallpaper, wallpaperUV(suv), focus).rgb;

    float brightness = 1.0 + alpha * refractIndex * 0.3;
    col *= brightness;
    col *= 1.0 - p_darken * 0.75;

    fragColor = vec4(col, 1.);
}
