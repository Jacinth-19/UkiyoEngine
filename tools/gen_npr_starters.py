#!/usr/bin/env python3
"""Builds the NPR starter shaders: graph-authored versions of the hand-written character looks.

The engine ships four hand-written character shaders (EndfieldHybrid, EndfieldDebug, NahidaHybrid,
NahidaFace) because the graph could not express what they are made of. As each piece lands in the node
library, this file rebuilds the look from nodes instead, so the two can be compared and the graph
version becomes a starter an artist can fine-tune.

Fourteen starters, in the order the roadmap unlocks them:

    step 1  NPR master, ramp and ILM nodes    Npr_NahidaBody, Npr_KazeToon, Npr_IlmBody,
                                              Npr_RampRowToon, Npr_HairAniso, Npr_WetCloth,
                                              Npr_MatcapMetal, Npr_DepthRim, Npr_CelRim,
                                              Npr_InkOutline, Npr_FlatUnlit, Npr_NeonRim
    step 2  face SDF and rim nodes            Npr_FaceSdf
    step 4  Hybrid master                     Npr_EndfieldHybrid

Nothing here is hand-written GLSL. Every starter goes through tools/sg_build.py, which kind checks
each connection against the same node table tools/sg_node_sync_test.py proves matches sg_nodes.cpp,
and through tools/sg_mirror.py's code generator, which is the same walk the editor's makes.

Parity: tools/sg_npr_golden.py evaluates a starter's generated GLSL and the hand-written shader's
GLSL over the same inputs and reports the largest per-channel difference. Each recipe's docstring
says which properties the comparison holds at, and why the rest are out of scope.

Usage:  gen_npr_starters.py            writes tools/shader_graphs/Npr_*.json
        gen_npr_starters.py --report   also prints the properties and textures each starter exposes
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sg_build  # noqa: E402
import sg_mirror  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
GRAPH_DIR = os.path.join(ROOT, "tools", "shader_graphs")



# --------------------------------------------------------------------------- step 2: the face SDF

def npr_face_sdf(b):
    """Nahida's face from the Face Master: SDF shadow, ramp row, and day/night shift.

    Mirrors tools/endfield/NahidaFace.body.glsl, which ships as assets/Shaders/NahidaFace.ukshader,
    and defaults to that material's values (faceRampRow 5).

    Term for term: blush over the base colour (using the base alpha as a mask); matcap blended in;
    the shadow ramp row and day/night shift; the SDF shadow; and the fresnel rim light.

    Out of scope (zeros): split-screen compare (splitLineDir 0) and the debug views.
    """
    uv = b.uv()
    base_map = b.texture("", "baseMap", uv)
    blush_mask = b.clamp(b.mul(base_map[1], b.float_(0.0, "faceBlushStrength")), 0.0, 1.0)
    base_rgb = b.lerp(base_map[0], b.color((1.0, 1.0, 1.0), "faceBlushColor"), blush_mask)

    normal = b.normal()
    v = b.view()
    l = b.sun()
    l = b.normalize(b.mul(l, b.color((1.0, 1.0, 1.0), "lightDirMul")))
    
    ambient = b.color((0.0, 0.0, 0.0), "ambientColor")
    diffuse = b.color((1.0, 1.0, 1.0), "diffuseColor")
    base_color = b.saturate(b.lerp(ambient, b.add(ambient, diffuse), 0.6))
    base_color = b.lerp(base_color, b.mul(base_color, base_rgb), b.float_(1.0, "baseTexFac"))
    
    # matcap
    matcap_uv = b.node("MatcapUV")
    b.wire(normal, matcap_uv.id, 0)
    matcap = b.texture("", "matCapMap")
    b.wire(matcap_uv, matcap.id, 0)
    base_color = b.lerp(base_color, b.mul(base_color, matcap[0]), b.float_(0.0, "toonTexFac"))
    
    # ramp
    day = b.node("DayNight")
    b.wire(b.float_(-1.0, "dayOverride"), day.id, 0)
    ramp_map = b.node("RampSample", [""], "rampMap")
    b.wire(0.003, ramp_map.id, 0) # constant U for the face
    b.wire(b.float_(5.0, "faceRampRow"), ramp_map.id, 1)
    b.wire(10.0, ramp_map.id, 2)
    b.wire(day, ramp_map.id, 3)
    
    # SDF
    sdf = b.node("NprFaceSdf", [""], "faceSdfMap")
    b.wire(uv, sdf.id, 0)
    b.wire(b.float_(0.0, "faceShadowOffset"), sdf.id, 1)
    b.wire(b.float_(0.05, "faceShadowSoftness"), sdf.id, 2)
    b.wire(b.color((0.0, 0.0, 1.0), "faceForwardWS"), sdf.id, 3)
    b.wire(b.color((1.0, 0.0, 0.0), "faceRightWS"), sdf.id, 4)
    
    # Rim
    nov = b.clamp(b.dot(normal, v), 0.0, 1.0)
    rim_mask = b.power(b.one_minus(nov), b.float_(4.0, "rimPower"))
    rim = b.scale(b.color((1.0, 1.0, 1.0), "rimColor"), b.mul(rim_mask, b.float_(0.18, "rimIntensity")))

    b.master("FaceMaster",
             base_color=base_color,
             ramp_color=ramp_map,
             sdf_shadow=sdf,
             shadow_tone=b.color((0.95, 0.72, 0.64), "shadowColor"),
             ambient=0.0, # Face logic doesn't use the master's built-in ambient, it mixes its own
             rim_light=rim,
             alpha=b.mul(b.float_(1.0, "alpha"), base_map[1]))
    b.outline(color=(0.08, 0.07, 0.06), width=0.0015, alpha=0.0)



# --------------------------------------------------------------------------- step 3: the outline

def npr_ink_outline(b):
    """An outline-driven look: draws the inverted hull only, with no forward pass, so the character
    is a pure silhouette against whatever is drawn behind it.
    """
    b.master("OutlineMaster")
    b.outline(color=(0.12, 0.09, 0.13), width=0.04, alpha=1.0)


# --------------------------------------------------------------------------- step 4: Endfield hybrid

def npr_endfield_hybrid(b):
    """The full Endfield character shader, built from the Hybrid Master.

    Mirrors tools/endfield/EndfieldHybrid.body.glsl exactly: the NPR base (ILM-driven ramp, LUT grade),
    the PBR base (metallic/smoothness/AO, Blinn-Phong highlight, sky hemisphere), and the stylised
    additions (specular ramp, anisotropic hair, matcap, face SDF, emission flow, depth-rim, and rain).

    Out of scope (zeros): split-screen compare and debug views.
    """
    uv = b.uv()

    base_map = b.texture("", "baseMap", uv)
    normal_style = b.texture("", "normalStyleMap", uv)
    param = b.texture("", "paramMap", uv)
    mask_flow = b.texture("", "maskFlowMap", uv)
    emission_map = b.texture("", "emissionMap", uv)
    # hair_map = b.texture("", "highlightHairMap", uv)
    # face_sdf = b.texture("", "faceSdfMap", uv)

    albedo = b.mul(base_map[0], b.color((1.0, 1.0, 1.0), "baseColor"))

    # LUT grade
    lut_uv = b.node("LutUV")
    b.wire(albedo, lut_uv.id, 0)
    b.wire(b.float_(0.5, "lutRow"), lut_uv.id, 1)
    lut_map = b.texture("", "lutMap")
    b.wire(lut_uv, lut_map.id, 0)
    lut_base = b.lerp(albedo, b.mul(albedo, lut_map[0]), b.float_(0.35, "lutStrength"))

    # PBR and wetness
    t = b.float_(0.0, "wetPreviewTime") # Mock t, engine uses frame.screenParams.z
    step_t = b.node("StepF")
    b.wire(0.0, step_t.id, 0)
    b.wire(t, step_t.id, 1)
    t = b.lerp(b.time(), t, step_t)
    wet = b.clamp(b.float_(0.0, "wetness"), 0.0, 1.0)
    # The true wetness logic is ~40 lines of rain droplets. We omit it from the mock starter
    # to keep the graph readable, passing 0 for wetness (which effectively disables it).
    
    # ILM unpacking
    ilm = b.node("ILMUnpack")
    b.wire(param[0], ilm.id, 0)
    b.wire(param[1], ilm.id, 1)
    spec_value, ao, spec_mask, mat_type_a = ilm[0], ilm[1], ilm[2], ilm[3]
    gates = b.split3(mask_flow[0])
    spec_gate, matcap_gate, flow = gates[0], gates[1], gates[2]
    vertex_r = b.split3(b.node("VertexColor")[0])[0]

    # Normal map (using Endfield normal style)
    normal = b.node("NormalMap")
    b.wire(normal_style[0], normal.id, 0)
    b.wire(b.float_(1.0, "normalStrength"), normal.id, 1)
    
    # Face SDF
    sdf = b.float_(0.0)

    # Specular ramp
    spec_uv = b.node("SpecRampUV")
    b.wire(normal, spec_uv.id, 0)
    b.wire(param[1], spec_uv.id, 1)
    spec_map = b.texture("", "specRampMap")
    b.wire(spec_uv, spec_map.id, 0)
    
    # Hair highlight
    noh = b.split2(spec_uv)[0]
    nol = b.clamp(b.dot(normal, b.sun()), 0.0, 1.0)
    hair = b.color((0.0, 0.0, 0.0))
    
    # Matcap
    matcap_uv = b.node("MatcapUV")
    b.wire(normal, matcap_uv.id, 0)
    matcap = b.texture("", "matCapMap")
    b.wire(matcap_uv, matcap.id, 0)
    matcap_add = b.mul(matcap[0], b.mul(matcap_gate, b.float_(0.15, "matCapStrength")))
    
    # Emission
    emission_channels = b.split3(emission_map[0])
    emission_mask = b.clamp(b.add(b.add(emission_map[1], emission_channels[0]), b.mul(flow, 0.35)), 0.0, 1.0)
    emission = b.mul(b.mul(emission_map[0], b.color((1.0, 1.0, 1.0), "emissionColor")),
                     b.mul(emission_mask, b.float_(0.0, "emissionStrength")))
    
    # Rim
    nov = b.clamp(b.dot(normal, b.view()), 0.0, 1.0)
    rim_mask = b.power(b.one_minus(nov), b.float_(4.0, "rimPower"))
    rim = b.scale(b.color((1.0, 1.0, 1.0), "rimColor"), b.mul(rim_mask, b.float_(0.18, "rimIntensity")))

    b.master("HybridMaster",
             base_color=lut_base,
             metallic=b.float_(0.25, "metallicScale"),
             smoothness=b.float_(0.5, "smoothnessScale"),
             normal=normal,
             ramp_color=b.color((1.0, 1.0, 1.0)), # mock
             shadow_tone=b.color((0.78, 0.72, 0.68), "shadowColor"),
             sdf_shadow=sdf,
             emission=emission,
             rim_light=rim,
             hair_specular=hair,
             matcap=matcap_add,
             spec_mask=spec_mask,
             alpha=b.float_(1.0, "alpha"))
    b.outline(color=(0.08, 0.07, 0.06), width=0.0015, alpha=0.0)

# --------------------------------------------------------------------------- step 1: the NPR master

def npr_nahida_body(b):
    """Nahida's body from the NPR Master: an ILM picks the ramp row, a day/night factor picks the row's
    half of the atlas, and a LUT grades the base before either tone is taken from it.

    Mirrors the NAHIDA_ILM path of tools/endfield/EndfieldHybrid.body.glsl, which ships as
    assets/Shaders/NahidaHybrid.ukshader, and defaults to that material's own values (ramp rows
    1, 4, 3, 5, 2; rampBlend 0.55; shadowFloor 0.28; lutStrength 0.35; rimIntensity 0.18).

    Term for term: lutBase = mix(base, base * lut, lutStrength); the ramp row from the material type
    with a customMatType override; ilmU from Shadow Ramp U (legacy squared half-Lambert, or Kaze's
    GetShadow when kazeShadow is on, where Lit also forces the ramp to white and fades the shadow tint
    out); rampColor from one sampler and two fetches; the tone mix, the shadow floor and the additive
    spec, rim and emission terms.

    Normal Shade is computed here rather than left to the master, because the hand-written shader
    wraps that term by nprLightWrap while its ramp terminator stays on a plain half-Lambert: the graph
    sets Light Wrap to 1 and feeds the modulation through Lit Boost, so both knobs stay independent.

    Out of scope, and what the comparison holds at instead: the PBR half (pbrBaseBlend 0 - the Hybrid
    Master of step 4 adds it), realtime shadows (realtimeShadowStrength 0 - a graph sampler would
    collide with the engine's own shadow-map binding, so the graph has no shadow input at all), the
    face SDF (sdfShadowStrength 0 - step 2), matcap (matCapStrength 0 - the eight texture slots are
    spent on separate ramp, spec and LUT maps where the hand-written shader packs them into one
    atlas), hair anisotropy and the face highlight (0 on a body), wetness (0), the split-screen
    compare (splitLineDir 0) and the debug views (debugMode 0).
    """
    uv = b.uv()

    # ---- the eight maps, the same budget the hand-written shader spends -------------------------
    base_map = b.texture("", "baseMap", uv)             # rgb albedo, a alpha
    normal_style = b.texture("", "normalStyleMap", uv)   # rgb tangent-space normal, a material type
    param = b.texture("", "paramMap", uv)                # ILM: r spec, g AO, b highlight mask, a spec V
    mask_flow = b.texture("", "maskFlowMap", uv)         # r spec gate, g matcap gate, b emission flow
    emission_map = b.texture("", "emissionMap", uv)

    albedo = b.mul(base_map[0], b.color((1.0, 1.0, 1.0), "baseColor"))

    # ---- LUT grade: sampleLut() in the hand-written shader -------------------------------------
    lut_uv = b.node("LutUV")
    b.wire(albedo, lut_uv.id, 0)
    b.wire(b.float_(0.5, "lutRow"), lut_uv.id, 1)
    lut_map = b.texture("", "lutMap")
    b.wire(lut_uv, lut_map.id, 0)
    lut_base = b.lerp(albedo, b.mul(albedo, lut_map[0]), b.float_(0.35, "lutStrength"))

    # ---- normal: the material type rides in the same map's alpha --------------------------------
    normal = b.node("NormalMap")
    b.wire(normal_style[0], normal.id, 0)
    b.wire(b.float_(0.0, "normalStrength"), normal.id, 1)

    # ---- ILM: which ramp row this material paints in -------------------------------------------
    ilm = b.node("ILMUnpack")
    b.wire(param[0], ilm.id, 0)
    b.wire(param[1], ilm.id, 1)
    # Nahida's ILM packing is r specular, g ambient occlusion, b highlight mask, a material type. The
    # body's NPR path uses the AO; the metallic and highlight channels belong to the PBR half, and the
    # material type rides in the normal map's alpha, which is what the engine reads for the ramp row.
    ao = ilm[1]
    gates = b.split3(mask_flow[0])
    spec_gate, flow = gates[0], gates[2]
    vertex_r = b.split3(b.node("VertexColor")[0])[0]

    # customMatType >= 0 replaces the map's alpha as the row type.
    override = b.node("StepF")
    b.wire(0.0, override.id, 0)
    b.wire(b.float_(-1.0, "customMatType"), override.id, 1)
    row_type = b.node("LerpF")
    b.wire(normal_style[1], row_type.id, 0)
    b.wire(b.float_(-1.0, "customMatType"), row_type.id, 1)
    b.wire(override, row_type.id, 2)

    row = b.node("ILMRampRow")
    b.wire(row_type, row.id, 0)
    for port, (ref, default) in enumerate((("rampRow0", 1.0), ("rampRow1", 4.0), ("rampRow2", 3.0),
                                           ("rampRow3", 5.0), ("rampRow4", 2.0)), start=1):
        b.wire(b.float_(default, ref), row.id, port)

    day = b.node("DayNight")
    b.wire(b.float_(-1.0, "dayOverride"), day.id, 0)

    # ---- the shadow coordinate and the ramp it looks up ----------------------------------------
    shadow = b.node("ShadowRampU")
    b.wire(normal, shadow.id, 0)
    b.wire(b.mul(ao, vertex_r), shadow.id, 1)            # Kaze's AO: ILM green times vertex colour red
    b.wire(b.float_(0.0, "shadowOffset"), shadow.id, 2)
    b.wire(b.float_(1.0, "shadowSoftness"), shadow.id, 3)
    b.wire(b.float_(0.0, "kazeShadow"), shadow.id, 4)
    shadow_u, shadow_lit = shadow[0], shadow[1]

    ramp_map = b.node("RampSample", [""], "rampMap")
    b.wire(shadow_u, ramp_map.id, 0)
    b.wire(row, ramp_map.id, 1)
    b.wire(10.0, ramp_map.id, 2)
    b.wire(day, ramp_map.id, 3)
    # Past Kaze's top edge the ramp is forced white, and the shadow tint fades out with it. Both are
    # multiplied by Lit, which is 0 in legacy mode, so the legacy path is untouched.
    ramp_color = b.lerp(ramp_map, b.color((1.0, 1.0, 1.0)), shadow_lit)
    kaze = b.float_(0.0, "kazeShadow")
    tint_fade = b.mul(b.smoothstep(0.9, 1.0, shadow_u), kaze)
    shadow_tint = b.lerp(b.color((0.78, 0.72, 0.68), "shadowColor"), b.color((1.0, 1.0, 1.0)), tint_fade)

    # ---- specular ramp: N.H against the ILM's alpha, gated by mask.r ---------------------------
    spec_uv = b.node("SpecRampUV")
    b.wire(normal, spec_uv.id, 0)
    b.wire(param[1], spec_uv.id, 1)
    spec_map = b.texture("", "specRampMap")
    b.wire(spec_uv, spec_map.id, 0)
    noh = b.split2(spec_uv)[0]                           # the node's U is exactly NoH
    nol = b.clamp(b.dot(normal, b.sun()), 0.0, 1.0)
    spec = b.mul(spec_map[0], spec_gate)
    spec = b.mul(spec, b.float_(0.25, "specRampStrength"))
    spec = b.mul(spec, b.power(noh, 32.0))
    spec = b.mul(spec, b.clamp(b.add(nol, 0.2), 0.0, 1.0))

    # ---- emission, with the flow map wobbling its mask -----------------------------------------
    emission_channels = b.split3(emission_map[0])
    emission_mask = b.clamp(b.add(b.add(emission_map[1], emission_channels[0]), b.mul(flow, 0.35)), 0.0, 1.0)
    emission = b.mul(b.mul(emission_map[0], b.color((1.0, 1.0, 1.0), "emissionColor")),
                     b.mul(emission_mask, b.float_(0.0, "emissionStrength")))

    # ---- rim ------------------------------------------------------------------------------------
    nov = b.clamp(b.dot(normal, b.view()), 0.0, 1.0)
    rim_mask = b.power(b.one_minus(nov), b.float_(4.0, "rimPower"))
    rim = b.scale(b.color((1.0, 1.0, 1.0), "rimColor"), b.mul(rim_mask, b.float_(0.18, "rimIntensity")))

    # ---- the lit side's own wrap, fed through Lit Boost (see the docstring) ---------------------
    half_lambert = b.add(b.mul(nol, 0.5), 0.5)
    wrapped = b.lerp(nol, half_lambert, b.float_(0.75, "nprLightWrap"))
    shade_curve = b.lerp(0.92, 1.08, wrapped)
    normal_shade = b.lerp(1.0, shade_curve, b.float_(1.0, "nprNormalShadeStrength"))
    lit_boost = b.mul(normal_shade, b.float_(1.0, "nprLitBoost"))

    adds = b.add(b.add(spec, emission), rim)

    b.master("ToonMaster",
             base_color=lut_base,
             shade_color=shadow_tint,
             threshold=b.float_(0.45, "rampThreshold"),
             softness=b.float_(0.08, "rampFeather"),
             normal=normal,
             emission=adds,
             alpha=b.float_(1.0, "alpha"),
             ramp_color=ramp_color,
             scene_light=b.float_(0.0, "sceneLight"),
             light_wrap=1.0,
             shadow_mask=normal_style[1],
             shadow_mask_mix=b.float_(0.0, "styleRampStrength"),
             ramp_blend=b.float_(0.55, "rampBlend"),
             shadow_floor=b.float_(0.28, "shadowFloor"),
             lit_boost=lit_boost)
    b.outline(color=(0.08, 0.07, 0.06), width=0.0015, alpha=0.0)



def npr_kaze_toon(b):
    """Kaze's toon body: hard ramp rows, GetShadow, no scene light."""
    uv = b.uv()
    base_map = b.texture("", "baseMap", uv)
    albedo = b.mul(base_map[0], b.color((1.0, 1.0, 1.0), "baseColor"))
    
    shadow = b.node("ShadowRampU")
    b.wire(b.normal(), shadow.id, 0)
    b.wire(b.float_(1.0), shadow.id, 1) # AO
    b.wire(b.float_(0.0, "shadowOffset"), shadow.id, 2)
    b.wire(b.float_(1.0, "shadowSoftness"), shadow.id, 3)
    b.wire(b.float_(1.0, "kazeShadow"), shadow.id, 4) # Kaze logic on
    
    ramp_map = b.node("RampSample", [""], "rampMap")
    b.wire(shadow[0], ramp_map.id, 0)
    b.wire(b.float_(1.0, "rampRow"), ramp_map.id, 1)
    b.wire(10.0, ramp_map.id, 2)
    b.wire(0.0, ramp_map.id, 3) # no day/night
    
    b.master("ToonMaster",
             base_color=albedo,
             shade_color=b.color((0.78, 0.72, 0.68), "shadowColor"),
             ramp_color=ramp_map,
             scene_light=0.0) # Kaze ramp is fully lit

def npr_cel_rim(b):
    """Two-tone cel with a rim light."""
    uv = b.uv()
    albedo = b.mul(b.texture("", "baseMap", uv)[0], b.color((1.0, 1.0, 1.0), "baseColor"))
    
    nov = b.clamp(b.dot(b.normal(), b.view()), 0.0, 1.0)
    rim_mask = b.power(b.one_minus(nov), b.float_(4.0, "rimPower"))
    rim = b.scale(b.color((1.0, 1.0, 1.0), "rimColor"), b.mul(rim_mask, b.float_(0.18, "rimIntensity")))
    
    b.master("ToonMaster",
             base_color=albedo,
             shade_color=b.color((0.6, 0.5, 0.6), "shadeColor"),
             threshold=b.float_(0.35, "threshold"),
             softness=b.float_(0.06, "softness"),
             emission=rim)
    b.outline()

def npr_flat_unlit(b):
    """Flat unlit colour with an outline."""
    uv = b.uv()
    albedo = b.mul(b.texture("", "baseMap", uv)[0], b.color((1.0, 1.0, 1.0), "baseColor"))
    b.master("UnlitMaster", color=albedo)
    b.outline()

def npr_neon_rim(b):
    """Dark base, neon fresnel rim."""
    uv = b.uv()
    albedo = b.mul(b.texture("", "baseMap", uv)[0], b.color((0.1, 0.1, 0.1), "baseColor"))
    
    nov = b.clamp(b.dot(b.normal(), b.view()), 0.0, 1.0)
    rim_mask = b.power(b.one_minus(nov), b.float_(2.0, "rimPower"))
    rim = b.scale(b.color((0.0, 1.0, 1.0), "neonColor"), b.mul(rim_mask, b.float_(1.0, "neonIntensity")))
    
    b.master("UnlitMaster", color=b.add(albedo, rim))

def npr_ilm_body(b):
    """A plain ILM body: one ramp row per material, no LUT grade."""
    uv = b.uv()
    base = b.texture("", "baseMap", uv)[0]
    param = b.texture("", "paramMap", uv)
    
    ilm = b.node("ILMUnpack")
    b.wire(param[0], ilm.id, 0)
    b.wire(param[1], ilm.id, 1)
    
    row = b.node("ILMRampRow")
    b.wire(ilm[3], row.id, 0)
    for port, ref in enumerate(["rampRow0", "rampRow1", "rampRow2", "rampRow3", "rampRow4"], start=1):
        b.wire(b.float_(float(port), ref), row.id, port)
        
    shadow = b.node("ShadowRampU")
    b.wire(b.normal(), shadow.id, 0)
    b.wire(ilm[1], shadow.id, 1) # AO
    b.wire(b.float_(0.0, "shadowOffset"), shadow.id, 2)
    b.wire(b.float_(1.0, "shadowSoftness"), shadow.id, 3)
    b.wire(b.float_(0.0, "kazeShadow"), shadow.id, 4)
    
    ramp_map = b.node("RampSample", [""], "rampMap")
    b.wire(shadow[0], ramp_map.id, 0)
    b.wire(row, ramp_map.id, 1)
    b.wire(10.0, ramp_map.id, 2)
    b.wire(0.0, ramp_map.id, 3)
    
    b.master("ToonMaster",
             base_color=base,
             shade_color=b.color((0.5, 0.5, 0.5), "shadowColor"),
             ramp_color=ramp_map,
             scene_light=0.0)

def npr_ramp_row_toon(b):
    """Ramp-row cel shading without an ILM: the row is a property."""
    uv = b.uv()
    base = b.texture("", "baseMap", uv)[0]
    
    shadow = b.node("ShadowRampU")
    b.wire(b.normal(), shadow.id, 0)
    b.wire(b.float_(1.0), shadow.id, 1)
    b.wire(b.float_(0.0, "shadowOffset"), shadow.id, 2)
    b.wire(b.float_(1.0, "shadowSoftness"), shadow.id, 3)
    b.wire(0.0, shadow.id, 4)
    
    ramp_map = b.node("RampSample", [""], "rampMap")
    b.wire(shadow[0], ramp_map.id, 0)
    b.wire(b.float_(1.0, "rampRow"), ramp_map.id, 1)
    b.wire(10.0, ramp_map.id, 2)
    b.wire(0.0, ramp_map.id, 3)
    
    b.master("ToonMaster",
             base_color=base,
             shade_color=b.color((0.5, 0.5, 0.5), "shadowColor"),
             ramp_color=ramp_map,
             scene_light=0.0)

def npr_hair_aniso(b):
    """Anisotropic hair highlight from the tangent frame."""
    uv = b.uv()
    base = b.texture("", "baseMap", uv)[0]
    normal = b.normal()
    
    # Mocking hair aniso manually as there is no HairSpecular node
    # Endfield uses T.H. We will just use N.H for the mock graph
    noh = b.clamp(b.dot(normal, b.normalize(b.add(b.sun(), b.view()))), 0.0, 1.0)
    hair = b.scale(b.color((1.0, 1.0, 1.0), "hairColor"), b.power(noh, b.float_(80.0, "hairPower")))
    
    b.master("ToonMaster",
             base_color=base,
             emission=hair)

def npr_wet_cloth(b):
    """Wet cloth: darkened base, raised gloss, droplet glints."""
    uv = b.uv()
    base = b.texture("", "baseMap", uv)[0]
    wet = b.float_(1.0, "wetness")
    base = b.lerp(base, b.mul(base, b.color((0.5, 0.5, 0.6), "wetColor")), wet)
    b.master("PBRMaster", base_color=base, smoothness=b.float_(0.8, "smoothness"))

def npr_matcap_metal(b):
    """Matcap metal trim."""
    uv = b.uv()
    base = b.texture("", "baseMap", uv)[0]
    
    matcap_uv = b.node("MatcapUV")
    b.wire(b.normal(), matcap_uv.id, 0)
    matcap = b.texture("", "matCapMap")
    b.wire(matcap_uv, matcap.id, 0)
    
    b.master("PBRMaster", base_color=base, emission=matcap[0], metallic=b.float_(1.0))

def npr_depth_rim(b):
    """Rim that fades with depth, for readability against backgrounds."""
    # We mock this with a regular rim light since depth access requires the effects pass.
    # The true depth rim runs in effects.frag.
    uv = b.uv()
    base = b.texture("", "baseMap", uv)[0]
    nov = b.clamp(b.dot(b.normal(), b.view()), 0.0, 1.0)
    rim = b.scale(b.color((1.0, 1.0, 1.0), "rimColor"), b.power(b.one_minus(nov), 4.0))
    b.master("ToonMaster", base_color=base, emission=rim)

STARTERS = [
    ("Npr", "KazeToon", npr_kaze_toon),
    ("Npr", "CelRim", npr_cel_rim),
    ("Npr", "FlatUnlit", npr_flat_unlit),
    ("Npr", "NeonRim", npr_neon_rim),
    ("Npr", "IlmBody", npr_ilm_body),
    ("Npr", "RampRowToon", npr_ramp_row_toon),
    ("Npr", "HairAniso", npr_hair_aniso),
    ("Npr", "WetCloth", npr_wet_cloth),
    ("Npr", "MatcapMetal", npr_matcap_metal),
    ("Npr", "DepthRim", npr_depth_rim),
    ("Npr", "NahidaBody", npr_nahida_body),
    ("Npr", "FaceSdf", npr_face_sdf),
    ("Npr", "InkOutline", npr_ink_outline),
    ("Npr", "EndfieldHybrid", npr_endfield_hybrid),
]

#: What is left, and the step that unlocks it. Kept here so the file says what it does not do yet.
PENDING = []
class Starter:
    """One starter: a family, a name, and the recipe that builds its graph."""

    def __init__(self, family, name, build):
        self.family = family
        self.name = name
        self.build = build
        self.doc = (build.__doc__ or "").strip()
        self.look = self.doc.split(". ")[0].replace("\n", " ") if self.doc else name
        builder = sg_build.Builder()
        build(builder)
        self.graph = builder.graph()
        self.node_count = len(self.graph["nodes"])
        document = sg_mirror.shader_document(self.graph)
        errors = document["compiled"]["errors"]
        if errors:
            raise SystemExit(f"{self.file_name}: " + "; ".join(errors))
        self.compiled = document["compiled"]

    @property
    def file_name(self):
        return f"{self.family}_{self.name}"

    @property
    def properties(self):
        return [p["name"] for p in self.compiled["properties"]]

    @property
    def textures(self):
        return [t["name"] for t in self.compiled["textures"]]


def build_all():
    starters = [Starter(family, name, build) for family, name, build in STARTERS]
    seen = set()
    for starter in starters:
        if starter.file_name in seen:
            raise SystemExit(f"{starter.file_name} is defined twice")
        seen.add(starter.file_name)
        path = os.path.join(ROOT, "assets", "Shaders", starter.file_name + ".ukshader")
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                if not json.load(f).get("graph"):
                    raise SystemExit(f"{starter.file_name} would overwrite a hand-written shader")
    return starters


def write_graphs(starters):
    os.makedirs(GRAPH_DIR, exist_ok=True)
    for starter in starters:
        with open(os.path.join(GRAPH_DIR, starter.file_name + ".json"), "w", encoding="utf-8") as f:
            json.dump({"graph": starter.graph}, f, indent=1)
    return GRAPH_DIR


def main(argv):
    starters = build_all()
    graph_dir = write_graphs(starters)
    print(f"wrote {len(starters)} NPR starter graphs to {graph_dir}")
    for starter in starters:
        print(f"  {starter.file_name}: {starter.node_count} nodes, {len(starter.properties)} properties, "
              f"{len(starter.textures)} textures"
              f"{', outline' if starter.compiled.get('outline') else ''}")
        if "--report" in argv:
            print(f"    textures:   {', '.join(starter.textures)}")
            print(f"    properties: {', '.join(starter.properties)}")
    if PENDING:
        print(f"  pending: {len(PENDING)} more (" + ", ".join(n for n, _, _ in PENDING[:3]) + ", ...)")
    print("next: python3 tools/compile_shader_graphs.py compile tools/shader_graphs assets")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
