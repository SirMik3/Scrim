#version 450
// ---- Scrim effect prelude (format 1) — prepended by tools/effectc ----
// See spec/effect-format.md. Do not edit per effect.

layout(set = 0, binding = 0, std140) uniform Host {
    vec4 uTimeFrame;   // x = time, y = frame, z = delta, w = wallpaper mip level count
    vec4 uRes;         // xy = pass resolution, zw = 1 / resolution
    vec4 uSizes;       // xy = screen size, zw = wallpaper size
    vec4 uParams[16];
};
layout(set = 0, binding = 1) uniform sampler2D uWallpaper;
layout(set = 0, binding = 2) uniform sampler2D uPrevious;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

#define uTime          uTimeFrame.x
#define uFrame         uTimeFrame.y
#define uDelta         uTimeFrame.z
#define uWallpaperLevels uTimeFrame.w
#define uResolution    uRes.xy
#define uTexel         uRes.zw
#define uScreenSize    uSizes.xy
#define uWallpaperSize uSizes.zw

float aspect() { return uScreenSize.x / uScreenSize.y; }

// Aspect-fill mapping from screen uv to wallpaper uv (matches macOS "Fill Screen").
vec2 wallpaperUV(vec2 uv) {
    float sa = uScreenSize.x / uScreenSize.y;
    float wa = uWallpaperSize.x / max(uWallpaperSize.y, 1.0);
    vec2 s = (wa > sa) ? vec2(sa / wa, 1.0) : vec2(1.0, wa / sa);
    return (uv - 0.5) * s + 0.5;
}

mat2 rot2(float a) { float c = cos(a), s = sin(a); return mat2(c, -s, s, c); }

// Hashes based on "Hash without Sine" by Dave Hoskins (MIT License).
float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
vec3 hash31(float p) {
    vec3 p3 = fract(vec3(p) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xxy + p3.yzz) * p3.zyx);
}
// Cubic B-spline sample of one mip level using 4 bilinear fetches. Unlike plain
// bilinear, it stays smooth when a small mip level is magnified many times.
vec4 textureBicubicLevel(sampler2D tex, vec2 uv, int level) {
    vec2 size = vec2(textureSize(tex, level));
    vec2 p = uv * size - 0.5;
    vec2 f = fract(p);
    p -= f;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = (-f3 + 3.0 * f2 - 3.0 * f + 1.0) / 6.0;
    vec2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
    vec2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 h0 = (p - 0.5 + w1 / g0) / size;      // texel p-1..p   -> one bilinear tap
    vec2 h1 = (p + 1.5 + w3 / g1) / size;      // texel p+1..p+2 -> one bilinear tap
    float l = float(level);
    return g0.y * (g0.x * textureLod(tex, vec2(h0.x, h0.y), l) + g1.x * textureLod(tex, vec2(h1.x, h0.y), l))
         + g1.y * (g0.x * textureLod(tex, vec2(h0.x, h1.y), l) + g1.x * textureLod(tex, vec2(h1.x, h1.y), l));
}

// Like textureLod, but large blurs look like soft focus instead of an upscaled
// low-res image. Below lod 1 it is plain trilinear (no extra cost). Use on uWallpaper.
vec4 textureLodSmooth(sampler2D tex, vec2 uv, float lod) {
    lod = max(lod, 0.0);
    if (lod < 1.0) return textureLod(tex, uv, lod);
    // Mip count comes from the host: GLSL ES has no textureQueryLevels. Only valid for uWallpaper.
    int maxLevel = max(int(uWallpaperLevels) - 1, 0);
    int i0 = min(int(lod), maxLevel);
    int i1 = min(i0 + 1, maxLevel);
    vec4 a = textureBicubicLevel(tex, uv, i0);
    float t = fract(lod);
    if (i0 == i1 || t < 0.01) return a;
    return mix(a, textureBicubicLevel(tex, uv, i1), t);
}
// ---- end prelude ----
