#!/usr/bin/env python3
"""Builds the Endfield-style shaders and their materials for the Ukiyo engine.

Writes:
  assets/Shaders/EndfieldHybrid.ukshader   (from EndfieldHybrid.body.glsl)
  assets/Shaders/EndfieldDebug.ukshader    (from EndfieldDebug.body.glsl)
  assets/Materials/EndfieldHybrid.ukmat    (texture-less template; set textures per character)
  assets/Materials/EndfieldDebug.ukmat

The fragment is complete GLSL 4.50. This script writes the header: the frame block (set 0, binding 0),
the material block (set 1, binding 15, one vec4 per property slot), texture slots (set 1, bindings 0..),
the P_<name> accessors, and the engine's shadow lookup. The body files hold the shading code.

These are our own implementations of the feature set. Do not paste third-party shader source here.
Use is personal testing only; keep the output out of anything shipped or committed.

Run from anywhere:  python3 tools/endfield/build_endfield.py
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MAX_TEXTURE_SLOTS = 8    # GraphMaterial::kTextureSlots: bindings 0..7; binding 15 is the material UBO
MATERIAL_BINDING = 15

# Frame block: must match the engine's frame.glsl field order exactly (std140).
FRAME_BLOCK = """layout(set = 0, binding = 0, std140) uniform FrameBlock {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invViewProj;
    mat4 shadowMatrix[3];
    vec4 camPos;
    vec4 sunDir;
    vec4 sunColor;
    vec4 skyColor;
    vec4 groundColor;
    vec4 cascadeSplits;
    vec4 shadowParams;
    vec4 screenParams;
    vec4 pointPos[4];
    vec4 pointColor[4];
    vec4 toonA;
    vec4 toonB;
    vec4 toonC;
    vec4 toonD;
    vec4 toonE;
    vec4 toonF;
    vec4 toonG;
    vec4 toonH;
    vec4 toonI;
    vec4 waterArea;
    vec4 waterHalf;
    vec4 waterShallow;
    vec4 waterDeep;
    vec4 waterWaveD[4];
    vec4 waterWaveS[4];
} frame;
layout(set = 0, binding = 1) uniform sampler2D uShadowMap;
"""

# Cascaded shadow lookup (1 = lit). Same logic as the engine's shadow_sample.glsl: 2x2 atlas,
# 3x3 tent filter from four gathers.
SHADOW_FN = """
float shadowFactor(vec3 worldPos, vec3 N) {
    if (frame.shadowParams.x < 0.5) return 1.0;
    vec3 L = normalize(frame.sunDir.xyz);
    float viewDist = -(frame.view * vec4(worldPos, 1.0)).z;
    int cascade = -1;
    if (viewDist < frame.cascadeSplits.x) cascade = 0;
    else if (viewDist < frame.cascadeSplits.y) cascade = 1;
    else if (viewDist < frame.cascadeSplits.z) cascade = 2;
    if (cascade < 0) return 1.0;
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    vec3 p = worldPos + N * frame.shadowParams.z * (1.0 - ndl);
    vec4 lp = frame.shadowMatrix[cascade] * vec4(p, 1.0);
    vec3 sc = lp.xyz / lp.w;
    vec2 uv = sc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    vec2 quad = vec2(float(cascade % 2), float(cascade / 2)) * 0.5;
    uv = clamp(quad + uv * 0.5, quad + vec2(0.001), quad + vec2(0.499));
    float texel = frame.shadowParams.y;
    float z = sc.z - frame.shadowParams.w;
    vec4 g0 = textureGather(uShadowMap, uv + vec2(-0.5, -0.5) * texel);
    vec4 g1 = textureGather(uShadowMap, uv + vec2( 0.5, -0.5) * texel);
    vec4 g2 = textureGather(uShadowMap, uv + vec2(-0.5,  0.5) * texel);
    vec4 g3 = textureGather(uShadowMap, uv + vec2( 0.5,  0.5) * texel);
    float lit = dot(step(vec4(z), g0), vec4(1.0)) + dot(step(vec4(z), g1), vec4(1.0))
              + dot(step(vec4(z), g2), vec4(1.0)) + dot(step(vec4(z), g3), vec4(1.0));
    return lit / 16.0;
}
"""

# Packed texture slots (8). The channel layout is in pack_endfield.py; the body reads these channels.
PACKED = ["base", "normalStyle", "param", "maskFlow", "faceSdf", "emission", "highlightHair", "lookup"]

# Lookup atlas regions as (x, y, w, h) in texels of an ATLAS x ATLAS texture. Each lookup is sampled
# inside its region with a half-texel inset, at LOD 0.
ATLAS = 256
REGIONS = {
    "ramp": (0, 0, 256, 20),       # diffuse ramp, sampled at (rampU, row V). 20 rows: Nahida's five ILM rows
                                   # (V = (row + 0.5) / 5); Endfield ramps are one row replicated down.
    "spec": (0, 20, 256, 16),      # spec ramp, sampled at (N.H, param.a)
    "lut": (0, 36, 256, 16),       # colour LUT, sampled at (luma, lutRow)
    "matcap": (0, 52, 128, 128),   # matcap, sampled at view-space normal xy
}

# Properties: (name, kind, defaults). Each property takes one vec4 slot, in this order.
# Defaults match the Endfield shader's Properties block.
# Outline controls, slots 8..14 in every outline material (graph_outline(_map).vert reads o8..o14).
# All defaults are neutral: the distance range is off, the far width factor is 0, the offsets are 0.
OUTLINE_EXTRA = [
    ("outlineDistance", "vec2", ["0", "0"]),        # Kaze's _OutlineWidthParams.xy: width range in fov-corrected view depth
    ("outlineWidthFactors", "vec2", ["1", "1"]),    # Kaze's _OutlineWidthParams.zw: factors at the range's near and far end
    ("outlineScreenOffset", "vec2", ["0", "0"]),    # Kaze's _ScreenOffset (clip units)
    ("outlineZOffset", "float", ["0"]),             # Kaze's _OutlineZOffset (0.01 per step)
    ("outlineVertexAlpha", "float", ["0"]),         # 1 = also scale the width by COLOR_0.a (off: some assets store alpha 0)
    ("outlineSmoothNormal", "float", ["0"]),        # 1 = extrude along the smooth normal (Kaze's _UseSmoothNormal)
    ("outlineViewDirection", "float", ["0"]),       # 1 = Kaze's view-plane extrusion; 0 = the original world-normal extrusion
]

# Shared appended controls (neutral defaults). dayOverride -1 = follow the sun; lightDirMul scales the
# light direction before it is normalised; useBackUV switches the texture UV to TEXCOORD_1 on back faces.
SHARED_EXTRA = [
    ("dayOverride", "float", ["-1"]),
    ("lightDirMul", "vec3", ["1", "1", "1"]),
    ("useBackUV", "float", ["0"]),
]

HYBRID_BASE = [
    # Outline slots first: the loader requires outlineColor, outlineWidth and outlineAlpha at 0, 1 and 2.
    ("outlineColor", "vec3", ["0.08", "0.07", "0.06"]),
    ("outlineWidth", "float", ["0.0015"]),
    ("outlineAlpha", "float", ["0"]),
    ("baseColor", "vec3", ["1", "1", "1"]),
    ("shadowColor", "vec3", ["0.78", "0.72", "0.68"]),
    ("sdfShadowColor", "vec3", ["0.95", "0.72", "0.64"]),
    ("emissionColor", "vec3", ["1", "1", "1"]),
    ("rimColor", "vec3", ["1", "1", "1"]),
    ("wetDarkenColor", "vec3", ["0.58", "0.62", "0.68"]),
    ("faceForwardWS", "vec3", ["0", "0", "1"]),
    ("faceRightWS", "vec3", ["1", "0", "0"]),
    ("alpha", "float", ["1"]),
    ("alphaClip", "float", ["0.5"]),
    ("metallicScale", "float", ["0.25"]),
    ("smoothnessMin", "float", ["0.18"]),
    ("smoothnessMax", "float", ["0.75"]),
    ("smoothnessScale", "float", ["0.8"]),
    ("occlusionStrength", "float", ["0.2"]),
    ("normalStrength", "float", ["0"]),
    ("pbrBaseBlend", "float", ["1"]),
    ("rampBlend", "float", ["0.55"]),
    ("rampThreshold", "float", ["0.45"]),
    ("rampFeather", "float", ["0.08"]),
    ("styleRampStrength", "float", ["0"]),
    ("realtimeShadowStrength", "float", ["0.65"]),
    ("shadowFloor", "float", ["0.28"]),
    ("nprLightWrap", "float", ["0.75"]),
    ("nprNormalShadeStrength", "float", ["1"]),
    ("nprLitBoost", "float", ["1"]),
    ("lutStrength", "float", ["0.35"]),
    ("lutRow", "float", ["0.5"]),
    ("faceColorStabilize", "float", ["0"]),
    ("specRampStrength", "float", ["0.25"]),
    ("matCapStrength", "float", ["0.15"]),
    ("highlightStrength", "float", ["0"]),
    ("emissionStrength", "float", ["0"]),
    ("rimPower", "float", ["4"]),
    ("rimIntensity", "float", ["0.18"]),
    ("hairAnisoStrength", "float", ["0"]),
    ("hairAnisoPower", "float", ["80"]),
    ("hairSpecShift", "float", ["0"]),
    ("sdfShadowStrength", "float", ["0"]),
    ("sdfDirectionalRG", "float", ["1"]),
    ("sdfSwapRG", "float", ["0"]),
    ("sdfChannel", "float", ["0"]),
    ("sdfThreshold", "float", ["0.48"]),
    ("sdfSoftness", "float", ["0.08"]),
    ("sdfBackFade", "float", ["0.12"]),
    ("wetness", "float", ["0"]),
    ("wetClothMask", "float", ["0"]),
    ("wetGlossMask", "float", ["0"]),
    ("wetDarken", "float", ["0.35"]),
    ("wetSmoothnessBoost", "float", ["0.35"]),
    ("wetSpecBoost", "float", ["0.45"]),
    ("wetDropletVisibility", "float", ["0.2"]),
    ("wetDropletDensity", "float", ["0.35"]),
    ("wetNormalStrength", "float", ["0.2"]),
    ("wetDropletScale", "float", ["36"]),
    ("wetDropletSpeed", "float", ["1.4"]),
    ("wetStreakStrength", "float", ["0.12"]),
    ("wetPreviewTime", "float", ["0"]),
    ("splitLineOffsetPx", "float", ["0"]),
    ("splitLineDirX", "float", ["0"]),
    ("splitLineDirY", "float", ["0"]),
    ("debugMode", "float", ["0"]),
    ("debugExposure", "float", ["1"]),
    ("vertexAOStrength", "float", ["0"]),       # 0 = off; 1 = COLOR_0.r multiplies the occlusion
    ("customMatType", "float", ["-1"]),         # ILM ramp row type: -1 = from the ILM alpha, 0..1 = fixed
    ("shadowOffset", "float", ["0"]),           # ILM shadow threshold offset (NoL side)
    ("shadowSoftness", "float", ["1"]),         # ILM shadow edge width multiplier (1 = the original 0.2 wide)
    ("kazeShadow", "float", ["0"]),             # 1 = Kaze's shadow formula (half-Lambert x AO) instead of the ILM NoL curve
]
# Endfield hybrid: the base list with the outline controls at slots 8..11, then the shared controls.
HYBRID_PROPS = HYBRID_BASE[:8] + OUTLINE_EXTRA + HYBRID_BASE[8:] + SHARED_EXTRA

# Textures: the packed slots, in binding order.
HYBRID_TEXTURES = PACKED

# Nahida body and hair (BodyAndHair.shader): the Endfield set with the five-type outline (outlineMapAlpha0..4,
# slots 3..7, which the ILM alpha picks) and the five ramp rows (rampRow0..4, after the outline slots). Defaults
# are BodyAndHair's: rows 1, 4, 3, 5, 2; outline alphas 1 (every type draws an outline).
NAHIDA_RAMP_ROW_DEFAULTS = [1, 4, 3, 5, 2]
NAHIDA_PROPS = (
    HYBRID_BASE[:3]
    + [(f"outlineMapAlpha{i}", "float", ["1"]) for i in range(5)]
    + OUTLINE_EXTRA
    + [(f"rampRow{i}", "float", [str(v)]) for i, v in enumerate(NAHIDA_RAMP_ROW_DEFAULTS)]
    + HYBRID_BASE[3:]
    + SHARED_EXTRA
)
NAHIDA_TEXTURES = PACKED
NAHIDA_DEFINES = ["NAHIDA_ILM"]

# Nahida face (Face.shader). Its own body file. Defaults are Face.shader's; it declares no rim, so rim is 0.
# Outline: the plain single-colour outline (Face has one _OutlineColor), offset 0.0003 as in the source.
FACE_PROPS = [
    ("outlineColor", "vec3", ["0", "0", "0"]),
    ("outlineWidth", "float", ["0.0003"]),
    ("outlineAlpha", "float", ["1"]),
    ("baseTexFac", "float", ["1"]),
    ("toonTexFac", "float", ["1"]),
    ("ambientColor", "vec3", ["0.667", "0.667", "0.667"]),
    ("diffuseColor", "vec3", ["0.906", "0.906", "0.906"]),
    ("shadowColor", "vec3", ["0.737", "0.737", "0.737"]),
    ("faceRampRow", "float", ["5"]),
    ("forwardVector", "vec3", ["0", "0", "1"]),
    ("rightVector", "vec3", ["1", "0", "0"]),
    ("rimColor", "vec3", ["1", "1", "1"]),
    ("rimPower", "float", ["4"]),
    ("rimIntensity", "float", ["0"]),
    ("alpha", "float", ["1"]),
    ("alphaClip", "float", ["0.5"]),
    ("faceBlushColor", "vec3", ["1", "0.55", "0.6"]),
    ("faceBlushStrength", "float", ["0"]),
    ("faceShadowOffset", "float", ["0"]),
    ("faceShadowSoftness", "float", ["0"]),
] + SHARED_EXTRA
FACE_PROPS = FACE_PROPS[:8] + OUTLINE_EXTRA + FACE_PROPS[8:]
# Nahida's face material (the sample's Nahida_Face.mat) sets _OutlineZOffset 0.5. Same scale as Kaze (0.01 per step).
FACE_PROPS = [(n, k, ["0.5"] if n == "outlineZOffset" else d) for n, k, d in FACE_PROPS]
# NPR repo: _PBROcclusionStrength 0.12 (ILM blue is a highlight mask, not AO), so Nahida uses 0.12 not 0.2.
NAHIDA_PROPS = [(n, k, ["0.12"] if n == "occlusionStrength" else d) for n, k, d in NAHIDA_PROPS]
FACE_TEXTURES = PACKED

DEBUG_PROPS = [
    ("channel", "float", ["0"]),
    ("exposure", "float", ["1"]),
]
DEBUG_TEXTURES = ["main"]


def prop_accessor(name, kind, slot):
    comp = {"float": "x", "vec2": "xy", "vec3": "rgb"}[kind]
    return f"#define P_{name} p[{slot}].{comp}"


ATLAS_FN = """
const float ATLAS_SIZE = %d.0;
const vec4 REGION_RAMP = vec4(%d.0, %d.0, %d.0, %d.0);
const vec4 REGION_SPEC = vec4(%d.0, %d.0, %d.0, %d.0);
const vec4 REGION_LUT = vec4(%d.0, %d.0, %d.0, %d.0);
const vec4 REGION_MATCAP = vec4(%d.0, %d.0, %d.0, %d.0);
// Samples a lookup region of the atlas at (uv in 0..1), inset by half a texel, LOD 0.
vec3 atlasSample(vec4 region, vec2 uv) {
    vec2 px = region.xy + 0.5 + clamp(uv, 0.0, 1.0) * (region.zw - 1.0);
    return textureLod(tex_lookup, px / ATLAS_SIZE, 0.0).rgb;
}
""" % ((ATLAS,) + sum((REGIONS[k] for k in ("ramp", "spec", "lut", "matcap")), ()))


def build_header(props, textures, body_name, atlas=False, defines=()):
    assert len(props) <= 256, "too many properties for one material block"
    assert len(textures) <= MAX_TEXTURE_SLOTS, f"{body_name}: {len(textures)} textures > {MAX_TEXTURE_SLOTS}"
    names = [p[0] for p in props]
    assert len(set(names)) == len(names), "duplicate property names"
    tnames = textures
    assert len(set(tnames)) == len(tnames), "duplicate texture names"

    lines = ["#version 450"] + [f"#define {d} 1" for d in defines] + [
             "// Generated by tools/endfield/build_endfield.py from " + body_name + ". Edit the body file, then re-run.",
             "layout(location = 0) in vec2 v_uv;",
             "layout(location = 1) in vec3 v_normal;",
             "layout(location = 2) in vec3 v_world;",
             "layout(location = 3) in vec4 v_color;",
             "layout(location = 4) in vec2 v_uv1;",
             "layout(location = 0) out vec4 o_color;",
             FRAME_BLOCK,
             f"layout(set = 1, binding = {MATERIAL_BINDING}, std140) uniform UkMaterial {{",
             ]
    for i, (name, kind, _d) in enumerate(props):
        lines.append(f"    vec4 p{i}; // {name} ({kind})")
    lines.append("};")
    lines.append("")
    for i, (name, kind, _d) in enumerate(props):
        comp = {"float": "x", "vec2": "xy", "vec3": "rgb"}[kind]
        lines.append(f"#define P_{name} p{i}.{comp}")
    lines.append("")
    for i, tn in enumerate(tnames):
        lines.append(f"layout(set = 1, binding = {i}) uniform sampler2D tex_{tn};")
    if atlas:
        lines.append(ATLAS_FN)
    lines.append(SHADOW_FN)
    return "\n".join(lines) + "\n"


def build_fragment(props, textures, body_path, atlas=False, defines=()):
    body_name = os.path.basename(body_path)
    with open(body_path, "r", encoding="utf-8") as f:
        body = f.read()
    header = build_header(props, textures, body_name, atlas=atlas, defines=defines)
    return header + "\n" + body


def build_shader_asset(name, props, textures, body_path, note, outline=False, atlas=False,
                       outline_map=False, defines=()):
    fragment = build_fragment(props, textures, body_path, atlas=atlas, defines=defines)
    properties = []
    for i, (pname, kind, defaults) in enumerate(props):
        properties.append({"name": pname, "kind": kind, "slot": i, "defaults": defaults})
    tex = [{"name": t, "binding": i, "defaultPath": ""} for i, t in enumerate(textures)]
    doc = {
        "format": "ukiyo-shader-graph",
        "version": 2,
        "graph": {"nodes": [], "connections": []},
        "handWritten": True,
        "source": f"tools/endfield/{os.path.basename(body_path)}",
        "note": note,
        "compiled": {
            "ok": True,
            "fragment": fragment,
            "properties": properties,
            "textures": tex,
            "materialBinding": MATERIAL_BINDING,
            "blend": "none",
            "outline": outline,
            "outlineMap": outline_map,
        },
    }
    return doc


def build_material(name, shader_rel, props, overrides, note):
    values = {}
    for pname, kind, defaults in props:
        if pname in overrides:
            values[pname] = overrides[pname]
    return {
        "format": "ukiyo-material",
        "version": 1,
        "name": name,
        "shader": shader_rel,
        "note": note,
        "properties": values,
        "textures": {},
    }


def write_json(path, doc):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
        f.write("\n")


def main():
    shaders = os.path.join(ROOT, "assets", "Shaders")
    materials = os.path.join(ROOT, "assets", "Materials")
    note_hybrid = ("Personal test only. Own implementation of an Endfield-style hybrid look; no third-party "
                   "source. Not for shipping or committing with the assets.")
    note_nahida = ("Personal test only. Nahida body and hair (BodyAndHair's ILM ramp rows and five-type outline). "
                   "Own implementation of the source's logic.")
    note_nahida_face = "Personal test only. Nahida face (Face.shader's SDF shadow). Own implementation of the source's logic."
    note_debug = "Personal test only. Own implementation. Channel and exposure view for the Endfield textures."

    hybrid_doc = build_shader_asset("EndfieldHybrid", HYBRID_PROPS, HYBRID_TEXTURES,
                                    os.path.join(HERE, "EndfieldHybrid.body.glsl"), note_hybrid,
                                    outline=True, atlas=True)
    nahida_doc = build_shader_asset("NahidaHybrid", NAHIDA_PROPS, NAHIDA_TEXTURES,
                                    os.path.join(HERE, "EndfieldHybrid.body.glsl"), note_nahida,
                                    outline=True, atlas=True, outline_map=True, defines=NAHIDA_DEFINES)
    face_doc = build_shader_asset("NahidaFace", FACE_PROPS, FACE_TEXTURES,
                                  os.path.join(HERE, "NahidaFace.body.glsl"), note_nahida_face,
                                  outline=True, atlas=True)
    debug_doc = build_shader_asset("EndfieldDebug", DEBUG_PROPS, DEBUG_TEXTURES,
                                   os.path.join(HERE, "EndfieldDebug.body.glsl"), note_debug)
    write_json(os.path.join(shaders, "EndfieldHybrid.ukshader"), hybrid_doc)
    write_json(os.path.join(shaders, "EndfieldDebug.ukshader"), debug_doc)
    write_json(os.path.join(shaders, "NahidaHybrid.ukshader"), nahida_doc)
    write_json(os.path.join(shaders, "NahidaFace.ukshader"), face_doc)

    # Material templates. Textures are added per character. matCapStrength is 0 in the template:
    # an unbound matcap falls back to white and would otherwise add a flat tint.
    write_json(os.path.join(materials, "EndfieldHybrid.ukmat"),
               build_material("EndfieldHybrid", "Shaders/EndfieldHybrid.ukshader", HYBRID_PROPS,
                              {"normalStrength": 1, "matCapStrength": 0}, note_hybrid))
    write_json(os.path.join(materials, "NahidaHybrid.ukmat"),
               build_material("NahidaHybrid", "Shaders/NahidaHybrid.ukshader", NAHIDA_PROPS,
                              {"normalStrength": 1, "matCapStrength": 0}, note_nahida))
    write_json(os.path.join(materials, "NahidaFace.ukmat"),
               build_material("NahidaFace", "Shaders/NahidaFace.ukshader", FACE_PROPS,
                              {"normalStrength": 0, "matCapStrength": 0}, note_nahida_face))
    write_json(os.path.join(materials, "EndfieldDebug.ukmat"),
               build_material("EndfieldDebug", "Shaders/EndfieldDebug.ukshader", DEBUG_PROPS, {}, note_debug))
    print("wrote EndfieldHybrid (%d props, %d textures), NahidaHybrid (%d props), NahidaFace (%d props), EndfieldDebug" %
          (len(HYBRID_PROPS), len(HYBRID_TEXTURES), len(NAHIDA_PROPS), len(FACE_PROPS)))


if __name__ == "__main__":
    main()
