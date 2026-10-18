#!/usr/bin/env python3
"""Golden comparisons for the NPR master: numbers, not screenshots.

  python3 tools/sg_npr_golden.py                # every check
  python3 tools/sg_npr_golden.py identity       # just one, by name

There is no GPU here, and assets/personal (the Nahida model, its ILM and the ramp atlas) is gitignored,
so a pixel diff is not available. What is available is stronger for finding formula errors: both sides
of a comparison are evaluated as the same arithmetic on the same inputs, over a grid covering the
ranges the engine actually uses, and the largest absolute difference is reported.

Checks:

  identity             The NPR master's lighting branch against the two-tone Toon master branch it
                       replaced, with every new input at its default. All 111 shipped shaders were
                       regenerated when that branch grew; this is the proof that none of them changed
                       appearance. One differing float would mean an existing toon material renders
                       differently after the upgrade.
  identity-degenerate  The same over the exact thresholds and softnesses the shipped toon shaders use,
                       including softness 0, where a hard step makes a formula change obvious.

Standard library only, like the rest of tools/: no numpy to install before a check can run.
"""
import math
import random
import sys

TOLERANCE = 1e-12


# --- vector arithmetic (3-tuples), written the way the GLSL reads --------------------------------

def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, b):
    """Componentwise for two colours, scalar for a colour and a number: GLSL's vec3 * vec3 and vec3 * f."""
    if isinstance(b, (int, float)):
        return (a[0] * b, a[1] * b, a[2] * b)
    return (a[0] * b[0], a[1] * b[1], a[2] * b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def mix(a, b, t):
    if isinstance(a, tuple):
        return (mix(a[0], b[0], t), mix(a[1], b[1], t), mix(a[2], b[2], t))
    return a * (1.0 - t) + b * t


def clamp01(x):
    return 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)


def smoothstep(e0, e1, x):
    denom = e1 - e0
    t = clamp01((x - e0) / (denom if abs(denom) > 1e-12 else math.copysign(1e-12, denom or 1.0)))
    return t * t * (3.0 - 2.0 * t)


def normalise(v):
    length = math.sqrt(dot(v, v))
    return (v[0] / length, v[1] / length, v[2] / length) if length > 1e-12 else (0.0, 0.0, 0.0)


def hemisphere(normal, sky, ground):
    return mix(ground, sky, 0.5 + 0.5 * normal[1])


# --- the two lighting branches ---------------------------------------------------------------------
# Transcriptions of the generated main(): the branch that shipped before the NPR master and the branch
# that ships now. They keep the same variable names as editor/src/shader_graph/sg_codegen.cpp so the
# two can be lined up against it line by line.

def toon_branch_old(s, frame):
    """color = albedo * mix(shade, 1, ramp) * sunColor + albedo * hemi * 0.55 + emission

    Written with GLSL's left-to-right association, because a * b * c is (a * b) * c there and floating
    point multiplication is not associative: transcribing it as a * (b * c) would report a difference
    of one ULP that the shader does not actually have.
    """
    ndlt = dot(s["normal"], frame["sun_dir"])
    ramp = smoothstep(s["threshold"] - s["softness"], s["threshold"] + s["softness"], ndlt)
    albedo = s["base_color"]
    hemi = hemisphere(s["normal"], frame["sky"], frame["ground"])
    sun_term = mul(mul(albedo, mix(s["shade"], (1.0, 1.0, 1.0), ramp)), frame["sun_color"])
    ambient_term = mul(mul(albedo, hemi), 0.55)
    return add(add(sun_term, ambient_term), s["emission"])


def npr_branch_new(s, frame):
    """The NPR branch. At its defaults - ramp white, occlusion/shadow/scene/blend/boost 1, ambient 0.55,
    wrap/mask mix/floor/normal shade 0 - this is the same expression as toon_branch_old, term for term
    and in the same order, so the two agree bit for bit."""
    ndlt = dot(s["normal"], frame["sun_dir"])
    wrapped = mix(ndlt, clamp01(ndlt) * 0.5 + 0.5, s["wrap"])
    ramp = smoothstep(s["threshold"] - s["softness"], s["threshold"] + s["softness"], wrapped)
    ramp = mix(ramp, clamp01(s["mask"]), s["mask_mix"])
    ramp_base = ramp
    ramp = mix(1.0, ramp, s["ramp_blend"])
    albedo = s["base_color"]
    hemi = hemisphere(s["normal"], frame["sky"], frame["ground"])
    normal_shade = mix(1.0, mix(0.92, 1.08, clamp01(wrapped)), s["normal_shade"])
    lit_side = (s["boost"] * normal_shade,) * 3
    tone = mul(albedo, mix(mul(s["shade"], s["ramp_color"]), lit_side, ramp))
    sun_term = mul(mul(mul(tone, frame["sun_color"]), s["shadow"]), s["scene"])
    ambient_term = mul(mul(mul(mul(albedo, hemi), s["ambient"]), s["occlusion"]), s["scene"])
    flat_term = mul(tone, 1.0 - s["scene"])
    color = add(add(sun_term, ambient_term), flat_term)
    floor_tone = mul(albedo, s["floor"] * (1.0 - ramp_base))
    color = mix(color, tuple(max(c, f) for c, f in zip(color, floor_tone)),
                1.0 if s["floor"] >= 0.0 else 0.0)
    return add(color, s["emission"])


DEFAULTS = {"ramp_color": (1.0, 1.0, 1.0), "occlusion": 1.0, "shadow": 1.0, "scene": 1.0, "wrap": 0.0,
            "mask": 0.0, "mask_mix": 0.0, "ramp_blend": 1.0, "floor": 0.0, "ambient": 0.55, "boost": 1.0,
            "normal_shade": 0.0}


def sample_surface(count, seed=20261018):
    """Random surfaces and lights over the ranges materials use: dark shades to over-bright suns."""
    rng = random.Random(seed)
    directions = []
    for _ in range(4096):
        v = (rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1))
        if dot(v, v) > 1e-6:
            directions.append(normalise(v))
    samples = []
    for i in range(count):
        surface = {
            "base_color": (rng.random(), rng.random(), rng.random()),
            "shade": (rng.random(), rng.random(), rng.random()),
            "emission": tuple(rng.random() * 0.4 for _ in range(3)),
            "threshold": rng.uniform(-0.5, 1.0),
            "softness": rng.uniform(0.0, 0.5),
            "normal": directions[rng.randrange(len(directions))],
        }
        surface.update(DEFAULTS)
        frame = {
            "sun_dir": directions[rng.randrange(len(directions))],
            "sun_color": tuple(rng.uniform(0, 3) for _ in range(3)),
            "sky": (rng.random(), rng.random(), rng.random()),
            "ground": tuple(rng.random() * 0.5 for _ in range(3)),
        }
        samples.append((surface, frame))
    return samples


def shipped_samples():
    """The thresholds and softnesses the shipped toon shaders actually use, against three light setups:
    overhead sun, sun below the horizon (the terminator case), and a diagonal."""
    thresholds = [-1.0, -0.05, 0.0, 0.28, 0.3, 0.35, 0.42, 0.5, 1.0, 1.5]
    softnesses = [0.0, 0.0001, 0.02, 0.05, 0.1, 0.2, 0.45, 0.5]
    lights = [((0.0, 1.0, 0.0), (0.0, -1.0, 0.0), (-0.5773502692, 0.5773502692, 0.5773502692))]
    shades = [((0.55, 0.5, 0.7), "Toon"), ((0.62, 0.58, 0.8), "AnimeCel_TwoTone"),
              ((0.92, 0.74, 0.72), "AnimeSkin_SoftPeach"), ((0.93, 0.86, 0.86), "AnimeFilm_KyoAniSoft")]
    samples = []
    for light_group in lights:
        for sun_dir in light_group:
            for threshold in thresholds:
                for softness in softnesses:
                    for shade, _name in shades:
                        surface = {"base_color": (0.85, 0.8, 0.85), "shade": shade,
                                   "emission": (0.0, 0.0, 0.0), "threshold": threshold,
                                   "softness": softness, "normal": (0.0, 1.0, 0.0)}
                        surface.update(DEFAULTS)
                        frame = {"sun_dir": sun_dir, "sun_color": (1.0, 0.98, 0.95),
                                 "sky": (0.5, 0.6, 0.8), "ground": (0.2, 0.18, 0.15)}
                        samples.append((surface, frame))
    return samples


def compare(samples, label):
    worst = 0.0
    worst_at = None
    for surface, frame in samples:
        old = toon_branch_old(surface, frame)
        new = npr_branch_new(surface, frame)
        diff = max(abs(a - b) for a, b in zip(old, new))
        if diff > worst:
            worst, worst_at = diff, (surface, frame, old, new)
    if worst > TOLERANCE and worst_at:
        surface, frame, old, new = worst_at
        print(f"     worst case: threshold={surface['threshold']} softness={surface['softness']} "
              f"N={tuple(round(c, 4) for c in surface['normal'])} "
              f"L={tuple(round(c, 4) for c in frame['sun_dir'])}")
        print(f"       old {tuple(round(c, 10) for c in old)}")
        print(f"       new {tuple(round(c, 10) for c in new)}")
    return worst, len(samples)


def check_identity():
    return compare(sample_surface(20000), "random")


def check_identity_degenerate():
    return compare(shipped_samples(), "shipped")


def check_identity_scene_off():
    """Scene Light 0 must reduce the master to the ramp tone alone: no sun colour, no sky, no ground.
    This is the mode the Endfield and Nahida character shaders are authored in, where the ramp atlas
    already carries the light response."""
    samples = sample_surface(4000, seed=7)
    worst = 0.0
    for surface, frame in samples:
        surface = dict(surface, scene=0.0, ambient=0.55)
        ndlt = dot(surface["normal"], frame["sun_dir"])
        ramp = smoothstep(surface["threshold"] - surface["softness"],
                          surface["threshold"] + surface["softness"], ndlt)
        expected = add(mul(surface["base_color"], mix(surface["shade"], (1.0, 1.0, 1.0), ramp)),
                       surface["emission"])
        got = npr_branch_new(surface, frame)
        worst = max(worst, max(abs(a - b) for a, b in zip(expected, got)))
    return worst, len(samples)


CHECKS = [
    ("identity", "NPR master at its defaults == the two-tone Toon master it replaced", check_identity),
def nahida_hand_written(base_map, normal_style, param, mask_flow, emission_map, p, frame):
    """The hand-written EndfieldHybrid.body.glsl (NahidaHybrid.ukshader), constrained to the features
    the Step 1 graph supports: PBR, shadows, face SDF, matcap, wetness, hair, and debug views off."""
    albedo = mul(base_map[:3], p["baseColor"])
    normal_strength = p["normalStrength"]
    nt = normal_style[:3]
    nt = (nt[0] * 2.0 - 1.0, nt[1] * 2.0 - 1.0, nt[2] * 2.0 - 1.0)
    nt = normalise(mix((0.0, 0.0, 1.0), nt, normal_strength))
    # We mock TBN as identity for this test, so nt is the world normal
    N = nt
    L = normalise(mul(frame["sun_dir"], p["lightDirMul"]))
    V = normalise((0.0, 0.0, -1.0)) # fixed view for test
    H = normalise(add(L, V))
    NoL = clamp01(dot(N, L))
    NoV = clamp01(dot(N, V))
    NoH = clamp01(dot(N, H))
    halfLambert = NoL * 0.5 + 0.5
    lightVisible = 1.0

    # LUT grade
    luma = dot(albedo, (0.2126, 0.7152, 0.0722))
    lut = (1.0, 1.0, 1.0) # Mock atlasSample(REGION_LUT, ...)
    lutBase = mix(mix(albedo, mul(albedo, lut), p["lutStrength"]), albedo, p["faceColorStabilize"])

    # NPR part
    rampU = smoothstep(p["rampThreshold"] - p["rampFeather"], p["rampThreshold"] + p["rampFeather"], halfLambert)
    rampU = clamp01(mix(rampU, normal_style[3], p["styleRampStrength"]))
    
    # ilmDay, ilmC, legacyU, kazeU...
    ilmDay = 1.0 # mock
    ilmEdge = max(0.1 * p["shadowSoftness"], 1e-4)
    ilmC = 0.3 + p["shadowOffset"]
    legacyU = clamp01(smoothstep(ilmC - ilmEdge, ilmC + ilmEdge, (NoL * 0.5 + 0.5)**2.0))
    kazeAo = clamp01(param[1] * 1.0) # v_color.r = 1.0
    kazeS = clamp01(2.0 * (dot(N, L) * 0.5 + 0.5) * kazeAo)
    kazeS = mix(kazeS, 1.0, 1.0 if kazeAo >= 0.9 else 0.0)
    kzMax = 0.5 + p["shadowOffset"]
    kazeU = clamp01(smoothstep(kzMax - ilmEdge, kzMax, kazeS))
    ilmU = kazeU if p["kazeShadow"] > 0.5 else legacyU
    
    rampColor = (1.0, 1.0, 1.0) # Mock atlasSample(REGION_RAMP, ...)
    shadowTint = p["shadowColor"]
    if p["kazeShadow"] > 0.5:
        rampColor = mix(rampColor, (1.0, 1.0, 1.0), 1.0 if kazeS >= kzMax else 0.0)
        shadowTint = mix(p["shadowColor"], (1.0, 1.0, 1.0), smoothstep(0.9, 1.0, ilmU))

    wrapped = mix(NoL, halfLambert, p["nprLightWrap"])
    normalShade = mix(1.0, mix(0.92, 1.08, clamp01(wrapped)), p["nprNormalShadeStrength"])
    nprLit = mul(mul(lutBase, normalShade), p["nprLitBoost"])
    rampTone = mul(mul(lutBase, rampColor), shadowTint)

    rampShadow = 1.0 - rampU
    stableRampShadow = mix(rampShadow, 0.0, p["faceColorStabilize"])
    stylizedShadow = clamp01(stableRampShadow)
    
    # pbrBaseBlend 0, so litBase is nprLit
    litBase = nprLit
    hybrid = mix(litBase, rampTone, stylizedShadow * p["rampBlend"])
    hybrid = (max(hybrid[0], lutBase[0] * p["shadowFloor"] * stylizedShadow),
              max(hybrid[1], lutBase[1] * p["shadowFloor"] * stylizedShadow),
              max(hybrid[2], lutBase[2] * p["shadowFloor"] * stylizedShadow))
              
    pureNpr = mix(nprLit, rampTone, stylizedShadow * p["rampBlend"])
    pureNpr = (max(pureNpr[0], lutBase[0] * p["shadowFloor"] * stylizedShadow),
               max(pureNpr[1], lutBase[1] * p["shadowFloor"] * stylizedShadow),
               max(pureNpr[2], lutBase[2] * p["shadowFloor"] * stylizedShadow))

    mask = mask_flow[:2]
    flow = mask_flow[2:]
    # Specular
    specRamp = (1.0, 1.0, 1.0) # mock
    hybrid = add(hybrid, mul(specRamp, mask[0] * p["specRampStrength"] * (NoH**32.0) * clamp01(NoL + 0.2)))
    
    # Emission and Rim
    emissionMask = clamp01(emission_map[3] + emission_map[0] + flow[0] * 0.35)
    stylizedAdd = mul(emission_map[:3], p["emissionColor"] * emissionMask * p["emissionStrength"])
    rim = (1.0 - NoV)**p["rimPower"] * p["rimIntensity"]
    stylizedAdd = add(stylizedAdd, mul(rim, p["rimColor"]))
    
    return add(hybrid, stylizedAdd)

    ("scene-off", "Scene Light 0 == the ramp tone alone, with no sun or sky term",
     check_identity_scene_off),
    ("identity-degenerate", "same, over the shipped toon shaders' own thresholds and softnesses",
     check_identity_degenerate),
]


def main(argv):
    only = argv[1] if len(argv) > 1 else None
    if only and only not in {name for name, _, _ in CHECKS}:
        print(f"no check named {only!r}; available: " + ", ".join(name for name, _, _ in CHECKS),
              file=sys.stderr)
        return 2
    failures = 0
    ran = 0
    for name, description, run in CHECKS:
        if only and only != name:
            continue
        ran += 1
        diff, count = run()
        ok = diff <= TOLERANCE
        failures += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'} {name}: {description}")
        print(f"     {count} samples, max |difference| = {diff:.3e} (tolerance {TOLERANCE:.0e})")
    print(f"{ran - failures} of {ran} golden checks pass")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
