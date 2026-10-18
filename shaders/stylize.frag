// Stylize: one optional post pass after tonemap and before FXAA. Fifteen looks plus a shared
// distance-adaptive ink outline. Written for Ukiyo from the style descriptions in research/stylize.md;
// it does not reuse any third-party shader code.
//
// Inputs:
//   tex 1  LDR frame after tonemap (RGBA8, display sRGB)
//   tex 4  scene depth (D32F, 0..1, RH ZO projection)
//   pc.model[0].x = P22, pc.model[0].y = P32, so eye depth = P32 / (d + P22)
//   pc.p0  x style (1..15), y strength (0..1), z outline amount (0..2)
//   pc.p1  xy internal resolution in pixels, z time in seconds
#include "common.glsl"
TEX(0, 1) uniform sampler2D uSrc;
TEX(0, 4) uniform sampler2D uDepth;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float hash12(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p) {
    float s = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i) {
        s += a * vnoise(p);
        p = p * 2.03 + vec2(17.1, 9.3);
        a *= 0.5;
    }
    return s;
}

// Quantise v (0..1) to n flat steps. soft is the blend width at each step edge.
float quant(float v, float n, float soft) {
    float x = v * n;
    float q = floor(x);
    float f = x - q;
    return (q + smoothstep(0.5 - soft, 0.5 + soft, f)) / n;
}

// 4x4 Bayer threshold in 0..1, from pixel coordinates.
float bayer4(vec2 fp) {
    const float b[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 q = ivec2(mod(fp, 4.0));
    return (b[q.x + q.y * 4] + 0.5) / 16.0;
}

// Diagonal hatch: 1 on ink lines, 0 elsewhere. spacing in pixels, width is the ink fraction of a period.
float hatch(vec2 p, float angle, float spacing, float width) {
    vec2 dir = vec2(-sin(angle), cos(angle));
    float s = dot(p, dir) / spacing;
    return 1.0 - smoothstep(width - 0.1, width, fract(s));
}

// Halftone coverage for one channel: 1 inside a dot. Dot area follows the ink (1 - value).
float dots(vec2 fp, float ang, float cell, float v) {
    mat2 R = mat2(cos(ang), -sin(ang), sin(ang), cos(ang));
    vec2 cc = fract(R * fp / cell) - 0.5;
    float r = sqrt(1.0 - v) * 0.56;
    return smoothstep(r + 0.08, r - 0.08, length(cc));
}

float linDepth(vec2 uv) {
    float d = texture(uDepth, uv).r;
    return pc.model[0].y / (d + pc.model[0].x);
}

// Ink from depth and colour edges. Lines are 0.75 px near and up to 2.5 px far, so they neither bloat up
// close nor vanish far away. Edges fade out between 20 and 40 m so distant foliage stops filling with lines.
float inkEdge(vec2 uv, vec2 p, float d0, float widthScale) {
    float width = widthScale * mix(0.75, 2.5, smoothstep(2.0, 30.0, d0));
    vec2 o = p * width;
    const vec2 offs[4] = vec2[4](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
    float dd = 0.0;
    float lmax = 0.0;
    float l0 = lum(texture(uSrc, uv).rgb);
    for (int i = 0; i < 4; ++i) {
        vec2 q = uv + offs[i] * o;
        dd = max(dd, abs(linDepth(q) - d0) / max(d0, 1e-3));
        lmax = max(lmax, abs(lum(texture(uSrc, q).rgb) - l0));
    }
    float depthEdge = smoothstep(0.02, 0.06, dd);
    float colEdge = smoothstep(0.10, 0.25, lmax);
    float fade = 1.0 - smoothstep(20.0, 40.0, d0);
    return max(depthEdge, 0.5 * colEdge) * fade;
}

// How strongly each style takes the ink outline (before the global outline amount).
float styleInk(int k) {
    if (k == 1) return 1.0;
    if (k == 2) return 0.5;
    if (k == 3) return 0.8;
    if (k == 4) return 0.6;
    if (k == 5) return 0.3;
    if (k == 6) return 1.0;
    if (k == 7) return 0.0;
    if (k == 8) return 0.3;
    if (k == 9) return 1.6;
    if (k == 10) return 0.2;
    if (k == 11) return 1.0;
    if (k == 12) return 0.0;
    if (k == 13) return 0.4;
    if (k == 14) return 0.8;
    if (k == 15) return 0.3;
    return 0.0;
}

// 1. Genshin Anime Cel: four stepped tones, indigo-tinted shadows, cyan rim on depth edges.
vec3 styleCel(vec3 c, float l, float edgeRaw) {
    float k = quant(l, 4.0, 0.04);
    vec3 shadeTint = vec3(0.45, 0.36, 0.72);     // #37285C, lifted so it can multiply the shadows
    vec3 res = c * mix(shadeTint, vec3(1.0), smoothstep(0.15, 0.55, k));
    res *= 0.85 + 0.15 * k;
    res += vec3(0.66, 0.95, 1.0) * edgeRaw * 0.35 * smoothstep(0.4, 0.9, l);
    return res;
}

// 2. Ghibli Soft Pastel: four soft tones, desaturated, lifted shadows, warm sunrise highlights.
vec3 styleGhibli(vec3 c, float l) {
    float k = quant(l, 4.0, 0.15);
    vec3 res = mix(vec3(l), c, 0.75) * mix(vec3(0.88, 0.90, 0.98), vec3(1.02, 1.0, 0.96), k);
    res = mix(res, vec3(0.42, 0.40, 0.46), 0.25 * (1.0 - k));
    res += vec3(0.07, 0.04, -0.02) * smoothstep(0.6, 1.0, l);
    return res;
}

// 3. RealToon Pro 3D Anime: three-step ramp, richer colour, a hard specular band on the top tone.
vec3 styleRealToon(vec3 c, float l) {
    float k = quant(l, 3.0, 0.06);
    vec3 res = c * mix(0.5, 1.0, k);
    res = mix(vec3(lum(res)), res, 1.15);
    res = mix(res, vec3(1.0), smoothstep(0.85, 0.95, l) * 0.5);
    return res;
}

// 4. Comic Pop Art Halftone: 45 degree screen, dot size follows darkness.
vec3 styleComic(vec3 c, float l, vec2 fp) {
    float m = dots(fp, 0.7854, 6.0, l);
    vec3 paper = mix(vec3(l), c, 0.5) * 1.05;
    return mix(paper, vec3(0.08, 0.07, 0.12), m * 0.9);
}

// 5. Watercolor Paper Bleed: wet-edge warp, pigment pooling at edges, paper tooth, washed colour.
// Flat wash: the mean of the lowest-variance 3x3 quadrant (Kuwahara-style). Keeps paint regions flat and edges crisp.
vec3 flatWash(vec2 uv, vec2 p) {
    vec3 best = vec3(0.0);
    float bestVar = 1e9;
    const vec2 centres[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    for (int qd = 0; qd < 4; ++qd) {
        vec3 mean = vec3(0.0);
        vec3 m2 = vec3(0.0);
        for (int j = 0; j <= 2; ++j) {
            for (int i = 0; i <= 2; ++i) {
                vec3 s = texture(uSrc, uv + vec2(float(i), float(j)) * centres[qd] * p * 1.5).rgb;
                mean += s;
                m2 += s * s;
            }
        }
        mean /= 9.0;
        float v = dot(m2 / 9.0 - mean * mean, vec3(1.0));
        if (v < bestVar) { bestVar = v; best = mean; }
    }
    return best;
}

vec3 styleWatercolor(vec2 uv, vec2 p, vec2 fp) {
    // Wet-in-wet: a low-frequency warp, then a flat wash.
    vec2 q = fp / 60.0;
    vec2 warp = (vec2(fbm(q), fbm(q + vec2(5.2, 1.3))) - 0.5) * 28.0 * p;   // up to about 14 px
    vec3 c = flatWash(uv + warp, p);
    // Pigment pools at the edge of each wash: darken where the flat colour changes.
    // Four raw samples are enough for the edge direction; full washes here cost 36 fetches each.
    float lr = lum(texture(uSrc, uv + warp + vec2(p.x * 2.0, 0.0)).rgb);
    float ll = lum(texture(uSrc, uv + warp - vec2(p.x * 2.0, 0.0)).rgb);
    float lu = lum(texture(uSrc, uv + warp + vec2(0.0, p.y * 2.0)).rgb);
    float ld = lum(texture(uSrc, uv + warp - vec2(0.0, p.y * 2.0)).rgb);
    float g = length(vec2(lr - ll, lu - ld));
    float pool = smoothstep(0.03, 0.14, g);
    c *= 1.0 - 0.38 * pool;
    // Wet blooms: slow tonal and hue drift across the sheet.
    float blot = fbm(fp / 90.0);
    c *= 0.92 + 0.16 * blot;
    c *= mix(vec3(1.0), vec3(1.06, 0.98, 0.94), smoothstep(0.45, 0.7, blot));
    // Granulation: pigment settles in the paper tooth.
    float gran = 0.6 * vnoise(fp / 1.6) + 0.4 * vnoise(fp / 4.5);
    c *= 0.90 + 0.18 * gran;
    // Paper shows through the light washes.
    float l = lum(c);
    c = mix(c, vec3(0.98, 0.96, 0.91), smoothstep(0.62, 0.9, l) * 0.55);
    c = mix(vec3(l), c, 0.85);
    return c;
}

vec3 styleSketch(float l, vec2 fp, float t) {
    float dark = 1.0 - l;
    float tq = floor(t * 12.0);
    vec2 jit = (vec2(hash12(vec2(tq, 1.0)), hash12(vec2(tq, 2.0))) - 0.5) * 1.5;
    float ink = 0.0;
    if (dark > 0.35) ink = max(ink, hatch(fp + jit, 0.7854, 4.0, 0.35));
    if (dark > 0.5) ink = max(ink, hatch(fp + jit, -0.7854, 4.0, 0.35));
    if (dark > 0.7) ink = max(ink, hatch(fp + jit, 0.0, 4.0, 0.30));
    vec3 paper = vec3(0.96, 0.95, 0.92) * (0.85 + 0.15 * l);
    return mix(paper, vec3(0.10, 0.10, 0.12), ink * 0.8);
}

// 7. Retro 8-Bit Arcade: 4x4 pixel blocks, 16 levels with Bayer dither, an 8-colour palette, scanlines.
vec3 styleRetro(vec2 uv, vec2 p, vec2 fp) {
    vec2 blk = floor(uv / (4.0 * p)) * 4.0 * p + 2.0 * p;
    vec3 c = texture(uSrc, blk).rgb;
    float l = lum(c);
    float th = (bayer4(fp) - 0.5) / 16.0;
    float lq = clamp(floor((l + th) * 16.0 + 0.5) / 16.0, 0.0, 1.0);
    vec3 q = clamp(c * (lq / max(l, 1e-3)), 0.0, 1.0);
    const vec3 pal[8] = vec3[8](vec3(0.06, 0.05, 0.10), vec3(0.20, 0.16, 0.35), vec3(0.45, 0.20, 0.36),
                                vec3(0.85, 0.30, 0.25), vec3(0.95, 0.65, 0.25), vec3(0.95, 0.92, 0.50),
                                vec3(0.30, 0.70, 0.45), vec3(0.85, 0.95, 0.95));
    vec3 best = pal[0];
    float bd = 1e9;
    for (int i = 0; i < 8; ++i) {
        vec3 dv = q - pal[i];
        float dd = dot(dv, dv);
        if (dd < bd) { bd = dd; best = pal[i]; }
    }
    vec3 res = mix(q, best, 0.6);
    res *= 0.86 + 0.14 * smoothstep(0.0, 0.5, fract(fp.y / 2.0));
    return res;
}

// 8. Arcane Painterly Realism: four-quadrant Kuwahara filter (brush-flat areas, soft edges), warm light and cool shade.
vec3 stylePainterly(vec2 uv, vec2 p, float l) {
    vec3 best = vec3(0.0);
    float bestVar = 1e9;
    const vec2 centres[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    for (int qd = 0; qd < 4; ++qd) {
        vec3 mean = vec3(0.0);
        vec3 m2 = vec3(0.0);
        float n = 0.0;
        for (int j = 0; j <= 2; ++j) {
            for (int i = 0; i <= 2; ++i) {
                vec2 o = vec2(float(i), float(j)) * centres[qd];
                vec3 s = texture(uSrc, uv + o * 2.0 * p).rgb;
                mean += s;
                m2 += s * s;
                n += 1.0;
            }
        }
        mean /= n;
        vec3 var3 = m2 / n - mean * mean;
        float v = dot(var3, vec3(1.0));
        if (v < bestVar) { bestVar = v; best = mean; }
    }
    best += mix(vec3(-0.02, 0.01, 0.05), vec3(0.06, 0.03, -0.03), smoothstep(0.3, 0.8, l));
    return best;
}

// 9. Borderlands Graphic PBR: colours kept, a little richer, with diagonal ink shadow hatching.
vec3 styleBorderlands(vec3 c, float l, vec2 fp) {
    vec3 res = mix(vec3(l), c, 1.1) * 1.03;
    float sh = 1.0 - smoothstep(0.25, 0.45, l);
    float ink = max(hatch(fp, 0.7854, 5.0, 0.22), hatch(fp, -0.7854, 5.0, 0.22));
    return mix(res, res * 0.35, ink * sh);
}

// 10. Pixar 3D Cinematic: vibrance, a soft glow in the highlights, warm mid-tones, film rim light.
vec3 stylePixar(vec3 c, float l, vec2 uv, vec2 p, float edgeRaw) {
    vec3 blur = vec3(0.0);
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            blur += texture(uSrc, uv + vec2(float(i), float(j)) * 3.0 * p).rgb;
        }
    }
    blur /= 9.0;
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float sat = (mx - mn) / max(mx, 1e-3);
    vec3 res = mix(vec3(l), c, 1.0 + 0.35 * (1.0 - sat));
    res += blur * 0.12 * smoothstep(0.55, 1.0, l);
    res += vec3(0.035, 0.015, 0.0) * smoothstep(0.2, 0.6, l) * (1.0 - smoothstep(0.6, 0.9, l));
    res += vec3(1.0, 0.8, 0.55) * edgeRaw * 0.12 * smoothstep(0.2, 0.6, l);
    return res;
}

// 11. Spider-Verse Graphic Novel: red and blue fringe that grows towards the frame edge,
// per-channel halftone screens at three angles, and action hatching in the dark.
vec3 styleSpider(vec2 uv, vec2 fp) {
    vec2 dir = uv - 0.5;
    vec2 off = dir * 0.01;
    vec3 c = vec3(texture(uSrc, uv + off).r, texture(uSrc, uv).g, texture(uSrc, uv - off).b);
    vec3 res = vec3(c.r * (1.0 - 0.25 * dots(fp, 0.2618, 6.0, c.r)),
                    c.g * (1.0 - 0.25 * dots(fp, 1.3090, 6.0, c.g)),
                    c.b * (1.0 - 0.25 * dots(fp, 0.7854, 6.0, c.b)));
    float l = lum(c);
    float act = hatch(fp, -0.9, 4.0, 0.25) * (1.0 - smoothstep(0.2, 0.4, l));
    return mix(res, res * 0.45, act);
}

// 12. Cyberpunk Neon Cel: three-step high-contrast cel, deep violet shadows, neon rim, faint tech grid.
vec3 styleCyber(vec2 uv, vec3 c, float l, vec2 fp, float edgeRaw, vec2 p) {
    // Split tone: violet shadows, magenta mids, cyan highlights, in four hard bands.
    float k = quant(smoothstep(0.05, 0.95, l), 4.0, 0.04);
    vec3 shadowC = vec3(0.06, 0.00, 0.16);
    vec3 midC = vec3(0.95, 0.12, 0.60);
    vec3 highC = vec3(0.45, 0.98, 1.00);
    vec3 ramp = mix(shadowC, midC, smoothstep(0.0, 0.5, k));
    ramp = mix(ramp, highC, smoothstep(0.5, 1.0, k));
    vec3 res = mix(ramp, c, 0.18);
    // Colour split on edges: red leans one way, blue the other.
    float sh = edgeRaw * 2.0;
    res.r = mix(res.r, texture(uSrc, uv + vec2(sh * p.x, 0.0)).r * 0.5 + res.r * 0.5, 0.6);
    res.b = mix(res.b, texture(uSrc, uv - vec2(sh * p.x, 0.0)).b * 0.5 + res.b * 0.5, 0.6);
    // Neon outlines, shifting from hot pink on the left to cyan on the right.
    vec3 neon = mix(vec3(1.0, 0.15, 0.75), vec3(0.2, 0.95, 1.0), smoothstep(0.35, 0.65, uv.x));
    res += neon * edgeRaw * 1.1;
    // Street-grid lines and scanlines.
    float gx = step(0.96, fract(fp.x / 24.0));
    float gy = step(0.96, fract(fp.y / 24.0));
    res += max(gx, gy) * vec3(0.0, 0.25, 0.30) * 0.7;
    res *= 0.90 + 0.10 * step(0.5, fract(fp.y / 3.0));
    // Glow on the brightest areas.
    res += highC * smoothstep(0.78, 1.0, l) * 0.35;
    return res;
}

vec3 styleHorror(vec3 c, float l, vec2 uv, vec2 fp, float d, float t) {
    vec3 res = mix(vec3(l), c, 0.35);
    res *= mix(vec3(0.75, 0.90, 1.0), vec3(1.0), smoothstep(0.1, 0.6, l));
    float fog = (1.0 - exp(-d * 0.03)) * (0.5 + 0.5 * fbm(uv * 6.0 + vec2(t * 0.05, 0.0)));
    res = mix(res, vec3(0.22, 0.27, 0.28) * (0.5 + l), clamp(fog, 0.0, 0.7));
    res += (hash12(fp + fract(t) * 97.0) - 0.5) * 0.08;
    float vig = smoothstep(0.9, 0.35, length(uv - 0.5) * 1.4);
    return res * mix(0.7, 1.0, vig);
}

// 14. Vector Pop Graphic: sixteen flat tones on luminance with soft edges, then a saturation pop.
vec3 styleVector(vec3 c, float l) {
    float lq = quant(l, 16.0, 0.02);
    vec3 res = clamp(c * (lq / max(l, 1e-3)), 0.0, 1.0);
    return clamp(mix(vec3(lum(res)), res, 1.2), 0.0, 1.0);
}

// 15. Impasto Heavy Oil Paint: canvas height from stretched noise, lit for relief, glossy highlights.
vec3 styleImpasto(vec3 c, float l, vec2 fp, vec2 uv, vec2 p, float d) {
    // Relief follows the paint: strong colour change or a near surface. Sky and far ground stay smooth.
    float busy = smoothstep(0.02, 0.10,
        abs(lum(texture(uSrc, uv + vec2(p.x, 0.0)).rgb) - lum(texture(uSrc, uv - vec2(p.x, 0.0)).rgb)) +
        abs(lum(texture(uSrc, uv + vec2(0.0, p.y)).rgb) - lum(texture(uSrc, uv - vec2(0.0, p.y)).rgb)));
    float near = 1.0 - smoothstep(60.0, 120.0, d);
    float relief = max(busy, 0.6) * near;
    vec2 sp = vec2(fp.x * 0.15, fp.y * 0.05);       // strokes run along x
    float h0 = fbm(sp);
    float hx = fbm(sp + vec2(0.15, 0.0));
    float hy = fbm(sp + vec2(0.0, 0.15));
    vec3 n = normalize(vec3(-(hx - h0) * 3.0 * relief, -(hy - h0) * 3.0 * relief, 1.0));
    vec3 lightDir = normalize(vec3(-0.4, 0.6, 0.7));
    float ndl = max(dot(n, lightDir), 0.0);
    vec3 res = c * mix(1.0, 0.7 + 0.5 * ndl, relief);
    float spec = pow(max(dot(reflect(-lightDir, n), vec3(0.0, 0.0, 1.0)), 0.0), 24.0);
    res += vec3(spec) * 0.12 * smoothstep(0.5, 0.9, l) * relief;
    // Canvas tooth, only where there is paint.
    res *= 1.0 + (0.96 + 0.08 * hash12(floor(fp)) - 1.0) * relief;
    return res;
}

void main() {
    int style = int(pc.p0.x + 0.5);
    float strength = pc.p0.y;
    float outlineAmt = pc.p0.z;
    vec2 p = 1.0 / pc.p1.xy;
    float t = pc.p1.z;
    vec2 uv = vUV;
    vec2 fp = gl_FragCoord.xy;

    vec3 src = texture(uSrc, uv).rgb;
    if (style < 1 || style > 15) {
        outColor = vec4(src, 1.0);
        return;
    }
    float l = lum(src);
    float d = linDepth(uv);
    float edgeRaw = inkEdge(uv, p, d, style == 9 ? 1.8 : 1.0);
    float edge = edgeRaw * outlineAmt * styleInk(style);

    vec3 res;
    if (style == 1) res = styleCel(src, l, edgeRaw);
    else if (style == 2) res = styleGhibli(src, l);
    else if (style == 3) res = styleRealToon(src, l);
    else if (style == 4) res = styleComic(src, l, fp);
    else if (style == 5) res = styleWatercolor(uv, p, fp);
    else if (style == 6) res = styleSketch(l, fp, t);
    else if (style == 7) res = styleRetro(uv, p, fp);
    else if (style == 8) res = stylePainterly(uv, p, l);
    else if (style == 9) res = styleBorderlands(src, l, fp);
    else if (style == 10) res = stylePixar(src, l, uv, p, edgeRaw);
    else if (style == 11) res = styleSpider(uv, fp);
    else if (style == 12) res = styleCyber(uv, src, l, fp, edgeRaw, p);
    else if (style == 13) res = styleHorror(src, l, uv, fp, d, t);
    else if (style == 14) res = styleVector(src, l);
    else res = styleImpasto(src, l, fp, uv, p, d);

    res = clamp(res, 0.0, 1.0);
    res = mix(res, vec3(0.08, 0.06, 0.10), clamp(edge * 0.9, 0.0, 1.0));
    outColor = vec4(mix(src, res, strength), 1.0);
}
