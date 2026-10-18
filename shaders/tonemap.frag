// HDR composite: look, bloom add, exposure, tonemap, display-space colour grade, vignette,
// optional grain, sRGB encode. Output alpha holds perceptual luma for FXAA.
//
// pc.p1.w packs four settings as w = op + 16 * look + 64 * logContrast + 128 * linearSat:
//   op 0..15 is the tonemap operator (below); look 0..3: 0 cel (crisp), 1 painterly (soft film),
//   2 cinematic (comic-print: CMYK misregistration and halftone).
//   logContrast 1 puts contrast around ACEScc mid grey in LogC (Kaze's Genshin post) instead of
//   the display-space pivot at 0.5. linearSat 1 makes saturation a luma lerp in linear light
//   (Kaze's), instead of the display-space vibrance.
// Operators: 0 ACES fit, 1 PBR Neutral, 2 AgX, 3 Custom Anime, 4 Linear clamp (debug),
// 5 Reinhard extended (luminance, white point 4), 6 Hejl-Burgess-Dawson, 7 Hable (Uncharted 2),
// 8 Lottes, 9 Uchimura (GT), 10 Genshin rational (Kaze's UnityGenshinPostProcessing).
// The output is dithered with a 64x64 true blue-noise tile (void-and-cluster, bound at Post
// binding 6), one 8-bit step peak to peak, so skies and fog do not band.
//
// PBR Neutral keeps base hues; ACES pushes saturated blues and cyans toward purple and
// white, which breaks anime colour accents.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uTex0;   // HDR scene
TEX(0, 2) uniform sampler2D uTex1;   // bloom (half resolution)
TEX(0, 6) uniform sampler2D uBlueNoise;   // 64x64 blue-noise dither tile (R channel)
TEX(0, 3) uniform sampler2D uLut;    // colour grading strip: N*N x N (N slices of N x N; red across, green down, blue by slice). N = 16 for PNG, any N for .cube
TEX(0, 5) uniform sampler2D uExpo;   // auto-exposure multiplier, 1x1, log2 in 8 bits: m = 2^(v*4-2)
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 acesFilm(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}


// AgX (Troy Sobotka's display transform, the common polynomial approximation). Inset and outset
// matrices move colour into a space where the sigmoid rolls off highlights without hue skew.
vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 agx(vec3 val) {
    const mat3 inset = mat3(0.856627153315983, 0.137318972929847, 0.11189821299995,
                            0.0951212405381588, 0.761241990602591, 0.0767994186031903,
                            0.0482516061458583, 0.101439036467562, 0.811302368396859);
    const mat3 outset = mat3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826,
                             -0.11060664309660323, 1.157823702216272, -0.11060664309660294,
                             -0.016493938717834573, -0.016493938717834257, 1.2519364065950405);
    const float minEv = -12.47393, maxEv = 4.026069;
    val = inset * val;
    val = max(val, vec3(1e-10));
    val = clamp(log2(val), minEv, maxEv);
    val = (val - minEv) / (maxEv - minEv);
    val = agxContrast(val);
    return clamp(outset * val, 0.0, 1.0);
}

// Custom Anime: a hue-preserving filmic shoulder. The brightest channel is compressed toward 1
// above a knee and the other channels scale with it, so a saturated blue stays blue when it
// is bright. A small toe lifts near-black so shadows keep their colour instead of crushing.
vec3 animeCurve(vec3 c, float knee, float toe) {
    float m = max(c.r, max(c.g, c.b));
    if (m <= 0.0) return c;
    float out_ = m <= knee ? m : knee + (1.0 - knee) * (1.0 - exp(-(m - knee) / (1.0 - knee)));
    out_ += toe * (1.0 - smoothstep(0.0, 0.06, out_));
    return c * (out_ / m);
}

// Colour grading LUT lookup. The strip holds a 16-cube: slice b occupies columns b*16 .. b*16+15.
// Each slice is sampled at its two neighbouring blue positions and blended linearly.
vec3 gradingLut(vec3 c) {
    float size = float(textureSize(uLut, 0).y);
    // Blue picks the slice from the raw value; red and green pick the texel inside it.
    float slice = clamp(c.b, 0.0, 1.0) * (size - 1.0);
    float s0 = floor(slice);
    float f = slice - s0;
    vec2 uv = clamp(c.rg, 0.0, 1.0) * ((size - 1.0) / size) + 0.5 / size;
    vec3 a = texture(uLut, vec2((s0 + uv.x) / size, uv.y)).rgb;
    vec3 b = texture(uLut, vec2((s0 + 1.0 + uv.x) / size, uv.y)).rgb;
    return mix(a, b, f);
}

// Blue-noise dither value for this pixel, in 0..1. The tile repeats every 64 pixels.
float blueNoise(vec2 p) {
    return texelFetch(uBlueNoise, ivec2(p) % 64, 0).r;
}

// Reinhard extended on luminance: the white point W maps to 1; hue and ratios are kept.
vec3 reinhardExtended(vec3 x) {
    const float W = 4.0;
    float L = dot(x, vec3(0.2126, 0.7152, 0.0722));
    float Ln = L * (1.0 + L / (W * W)) / (1.0 + L);
    return L > 1e-6 ? x * (Ln / L) : x;
}

// Hejl-Burgess-Dawson. The curve outputs display-encoded values, so the result is linearised
// (pow 2.2) here and re-encoded with the common sRGB step.
vec3 hejlBurgessDawson(vec3 x) {
    x = max(vec3(0.0), x - 0.004);
    vec3 y = (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
    return pow(clamp(y, 0.0, 1.0), vec3(2.2));
}

// Hable / Uncharted 2 filmic curve, normalised so that the white point W maps to 1.
vec3 hableCurve(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
vec3 hableFilmic(vec3 x) {
    const float W = 11.2;
    return hableCurve(x * 2.0) / hableCurve(vec3(W));
}

// Lottes (2016) HDR curve, per channel. Constants chosen so that mid grey 0.18 maps to 0.267
// and the HDR maximum 8.0 maps to 1.0.
vec3 lottesCurve(vec3 x) {
    const float a = 1.6, d = 0.977, hdrMax = 8.0, midIn = 0.18, midOut = 0.267;
    float ad = a * d;
    float b = (-pow(midIn, a) + pow(hdrMax, a) * midOut) / ((pow(hdrMax, ad) - pow(midIn, ad)) * midOut);
    float c = (pow(hdrMax, ad) * pow(midIn, a) - pow(hdrMax, a) * pow(midIn, ad) * midOut)
            / ((pow(hdrMax, ad) - pow(midIn, ad)) * midOut);
    return pow(x, vec3(a)) / (pow(x, vec3(ad)) * b + c);
}

// Uchimura (GT) parametric curve, per channel: toe, linear section, shoulder.
// Defaults: max brightness P 1, contrast a 1, linear start m 0.22, linear length l 0.4, toe c 1.33.
float uchimuraChannel(float x) {
    const float P = 1.0, a = 1.0, m = 0.22, l = 0.4, c = 1.33, b = 0.0;
    float l0 = (P - m) * l / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = a * P / (P - S1);
    float CP = -C2 / P;
    float t = clamp(x / m, 0.0, 1.0);
    float w0 = 1.0 - t * t * (3.0 - 2.0 * t);
    float w2 = step(m + l0, x);
    float w1 = 1.0 - w0 - w2;
    float T = m * pow(max(x / m, 0.0), c) + b;
    float L = m + a * (x - m);
    float S = P - (P - S1) * exp(CP * (x - S0));
    return T * w0 + L * w1 + S * w2;
}
vec3 uchimuraGT(vec3 x) {
    return vec3(uchimuraChannel(x.r), uchimuraChannel(x.g), uchimuraChannel(x.b));
}

// Genshin rational curve (Kaze's UnityGenshinPostProcessing ColorGrading pass). Linear in, linear out.
vec3 genshinCurve(vec3 x) {
    vec3 c0 = (1.36 * x + 0.047) * x;
    vec3 c1 = (0.93 * x + 0.56) * x + 0.14;
    return clamp(c0 / c1, 0.0, 1.0);
}

// ARRI LogC (EI 800) and ACEScc mid grey, as Unity's ColorUtils.hlsl, for the log-space contrast
// that Kaze's ColorGrading pass uses. Inputs are non-negative, so log and pow are safe.
const float ACESCC_MIDGRAY = 0.4135884;
vec3 linearToLogC(vec3 x) {
    const float a = 5.555556, b = 0.052272, c = 0.247190, d = 0.385537, e = 5.367655, f = 0.092809, cut = 0.010591;
    x = max(x, vec3(0.0));
    vec3 hi = c * log(a * x + b) / log(10.0) + d;
    vec3 lo = e * x + f;
    return mix(lo, hi, step(cut, x));
}
vec3 logCToLinear(vec3 y) {
    const float a = 5.555556, b = 0.052272, c = 0.247190, d = 0.385537, e = 5.367655, f = 0.092809, cut = 0.010591;
    vec3 hi = (pow(vec3(10.0), (y - d) / c) - b) / a;
    vec3 lo = (y - f) / e;
    return mix(lo, hi, step(e * cut + f, y));
}

// Khronos PBR Neutral tonemapper (Khronos Group reference).
vec3 pbrNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

vec3 linearToSRGB(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
vec2 frame_seed() { return vec2(pc.p1.z * 37.0, pc.p1.z * 91.0); }
float hash12(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}
// Smooth value noise for paper texture (cells are in pixels).
float valueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + vec2(1, 0));
    float c = hash12(i + vec2(0, 1)), d = hash12(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    vec3 hdr = texture(uTex0, vUV).rgb;
    vec3 bloom = texture(uTex1, vUV).rgb;
    float exposure = pc.p0.x;
    float bloomStrength = pc.p0.y;
    float saturation = pc.p0.z;
    float contrast = pc.p0.w;
    float vignetteAmt = pc.p1.x;
    float grainAmt = pc.p1.y;
    int modeBits = int(pc.p1.w + 0.5);
    int op = modeBits % 16;                  // operator, see the header
    int look = (modeBits / 16) % 4;          // 0 cel, 1 painterly, 2 cinematic
    bool logContrast = (modeBits / 64) % 2 == 1;
    bool linearSat = modeBits / 128 == 1;

    vec2 texel = 1.0 / vec2(textureSize(uTex0, 0));
    vec2 px = gl_FragCoord.xy;

    // Chromatic aberration (grade model[2].y, 0..1) splits red and blue towards the edges.
    // Sharpen (grade model[2].z, 0..1) is an unsharp mask on the HDR frame. Both are off at 0.
    float caAmt = pc.model[2].y;
    float sharpenAmt = pc.model[2].z;
    if (caAmt > 0.0) {
        vec2 caOff = (vUV - 0.5) * caAmt * 0.006;
        hdr.r = texture(uTex0, vUV + caOff).r;
        hdr.b = texture(uTex0, vUV - caOff).b;
    }
    // Auto exposure (grade model[2].w = 1 when on): the 1x1 state from expo_reduce.frag.
    if (pc.model[2].w > 0.5) exposure *= exp2(texture(uExpo, vec2(0.5)).r * 4.0 - 2.0);
    if (sharpenAmt > 0.0) {
        vec3 nb = (texture(uTex0, vUV + vec2(texel.x, 0.0)).rgb + texture(uTex0, vUV - vec2(texel.x, 0.0)).rgb
                 + texture(uTex0, vUV + vec2(0.0, texel.y)).rgb + texture(uTex0, vUV - vec2(0.0, texel.y)).rgb) * 0.25;
        hdr = max(hdr + (hdr - nb) * sharpenAmt, vec3(0.0));
    }

    // Look: cinematic splits the colour channels slightly (print misregistration), painterly
    // softens the image toward a film-like focus.
    if (look == 2) {
        vec2 off = vec2(1.4 * texel.x, 0.0);
        hdr.r = texture(uTex0, vUV + off).r;
        hdr.b = texture(uTex0, vUV - off).b;
    } else if (look == 1) {
        vec3 blur = (texture(uTex0, vUV + vec2(1.5, 0.0) * texel).rgb
                   + texture(uTex0, vUV - vec2(1.5, 0.0) * texel).rgb
                   + texture(uTex0, vUV + vec2(0.0, 1.5) * texel).rgb
                   + texture(uTex0, vUV - vec2(0.0, 1.5) * texel).rgb) * 0.25;
        hdr = mix(hdr, blur, 0.28);
    }

    vec3 c = (hdr + bloom * bloomStrength) * exposure;
    vec3 cPos = max(c, vec3(0.0));
    if (op == 1) c = pbrNeutral(cPos);
    else if (op == 2) c = agx(cPos);
    else if (op == 3) c = animeCurve(cPos, pc.model[3].y, pc.model[3].z);
    else if (op == 4) c = clamp(cPos, 0.0, 1.0);
    else if (op == 5) c = reinhardExtended(cPos);
    else if (op == 6) c = hejlBurgessDawson(cPos);
    else if (op == 7) c = hableFilmic(cPos);
    else if (op == 8) c = lottesCurve(cPos);
    else if (op == 9) c = uchimuraGT(cPos);
    else if (op == 10) c = genshinCurve(cPos);
    else c = acesFilm(c);
    // Linear-light contrast and saturation, when selected. Kaze's order: contrast, then saturation.
    const vec3 lumW = vec3(0.2126, 0.7152, 0.0722);
    float contrastAmt = contrast + (look == 2 ? 0.03 : 0.0);
    c = clamp(c, 0.0, 1.0);
    if (logContrast) {
        c = logCToLinear(mix(vec3(ACESCC_MIDGRAY), linearToLogC(c), contrastAmt));
    }
    if (linearSat) {
        c = mix(vec3(dot(clamp(c, 0.0, 1.0), lumW)), clamp(c, 0.0, 1.0), saturation);
    }
    vec3 s = clamp(linearToSRGB(clamp(c, 0.0, 1.0)), 0.0, 1.0);

    // Display-space grade, the way anime colour is graded.
    float l = dot(s, lumW);
    if (!linearSat) {
        float mx = max(s.r, max(s.g, s.b));
        float mn = min(s.r, min(s.g, s.b));
        float chroma = mx > 1e-4 ? (mx - mn) / mx : 0.0;
        // Vibrance: lifts muted colours more than already vivid ones (protects skin and cyan).
        float satAmt = saturation * (1.0 + 0.35 * (1.0 - chroma));
        s = mix(vec3(l), s, satAmt);
    }
    if (!logContrast) s = (s - 0.5) * contrastAmt + 0.5;
    // Cool shadows, warm highlights: the colour split that reads as anime lighting.
    s += (1.0 - smoothstep(0.0, 0.45, l)) * vec3(-0.012, -0.004, 0.022);
    s += smoothstep(0.55, 1.0, l) * vec3(0.010, 0.004, -0.012);
    if (look == 1) {
        // Painterly: warm paper tint, low-frequency paper texture.
        float paper = valueNoise(px / 14.0) * 0.7 + valueNoise(px / 5.0) * 0.3;
        s *= 1.0 - 0.025 * paper;
        s += vec3(0.012, 0.006, -0.010);
    }
    s = clamp(s, 0.0, 1.0);

    if (look == 2) {
        // Halftone: dots on a 45-degree screen. Darker areas get larger dots, applied only
        // in shadows and midtones, as in comic-print shading.
        float cell = max(4.0, textureSize(uTex0, 0).y / 120.0);
        mat2 rot = mat2(0.7071, -0.7071, 0.7071, 0.7071);
        vec2 q = rot * px / cell;
        float dist = length(fract(q) - 0.5);
        float radius = sqrt(clamp(1.0 - l, 0.0, 1.0)) * 0.5;
        float dot_ = smoothstep(radius + 0.04, radius - 0.04, dist);
        // Only on the character: HDR alpha is 1 for character pixels and 0 for sky and grid.
        float charMask = clamp(texture(uTex0, vUV).a, 0.0, 1.0);
        float amount = 0.55 * smoothstep(0.9, 0.35, l) * charMask;
        s *= 1.0 - amount * dot_ * 0.75;
    }

    // Artist grade (display space): white balance, then shadow and highlight tints, then an
    // optional LUT blend. The values come from pc.model, which this pass does not otherwise use.
    //   pc.model[0].rgb  shadow tint        (added, weighted toward black)
    //   pc.model[1].rgb  highlight tint     (added, weighted toward white)
    //   pc.model[2].x    white balance      (-1 cool .. +1 warm)
    //   pc.model[3].x    LUT amount         (0 = off)
    //   pc.model[3].y    Custom Anime knee  (shoulder start, 0.3 .. 0.95)
    //   pc.model[3].z    Custom Anime toe   (shadow lift, 0 .. 0.05)
    //   pc.model[3].w    dither             (1 = on, 0 = off)
    float wb = pc.model[2].x;
    s *= vec3(1.0 + 0.06 * wb, 1.0, 1.0 - 0.06 * wb);
    s += pc.model[0].rgb * (1.0 - smoothstep(0.0, 0.45, l));
    s += pc.model[1].rgb * smoothstep(0.55, 1.0, l);
    s = clamp(s, 0.0, 1.0);
    float lutAmt = clamp(pc.model[3].x, 0.0, 1.0);
    if (lutAmt > 0.0) s = mix(s, gradingLut(s), lutAmt);

    vec2 q2 = vUV - 0.5;
    s *= clamp(1.0 - vignetteAmt * dot(q2, q2) * 2.2, 0.0, 1.0);

    s += (hash12(gl_FragCoord.xy + frame_seed()) - 0.5) * grainAmt;
    // Dither at the 8-bit output step so gradients do not band.
    s += (blueNoise(gl_FragCoord.xy) - 0.5) / 255.0 * pc.model[3].w;
    s = clamp(s, 0.0, 1.0);
    outColor = vec4(s, dot(s, vec3(0.299, 0.587, 0.114)));
}
