#!/usr/bin/env python3
"""Generates the anime style library: 50 starter shader graphs built from the node library.

  python3 tools/gen_anime_styles.py                                   # graph sources + the style doc
  python3 tools/compile_shader_graphs.py compile tools/shader_graphs assets   # -> assets/Shaders

The point of the library is variety. An anime game is not one look: a character face wants an SDF
shadow that never breaks under the nose, hair wants an anisotropic ring, a school uniform wants matte
cloth with vertex-colour AO, a flashback wants 90s TV dither, a boss wants hard ink and no ambient.
Each style here is a finished starting point - open it in the shader graph editor, tweak the exposed
properties, or pull it apart and rewire it. Everything a style's look depends on is exposed as a
material property, so a fine-tuned variant needs no graph editing at all.

Conventions every recipe follows:
  * Colour comes from b.albedo(): texture times tint, so a recolour or an untextured prop works.
  * Light comes from the scene: CelBands over N.L, then SunColor/AmbientColor. A style that ignores
    the scene's lights would not sit in a level next to one that respects them.
  * Masters are chosen for what they give back: ToonMaster for the engine's own two-tone branch,
    UnlitMaster when the style computes its whole look, PBRMaster when it should still catch real
    specular and sky light.
  * Character styles expose the outline trio (outlineColor, outlineWidth, outlineAlpha), which asks
    the engine for the inverted-hull pass; world and ink styles leave it off.

Writes tools/shader_graphs/Anime<Family>_<Style>.json plus research/anime_style_library.md, whose
property lists are read back from the compiled documents so they cannot drift from the shaders.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sg_build  # noqa: E402
import sg_mirror  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
GRAPH_DIR = os.path.join(ROOT, "tools", "shader_graphs")
DOC_PATH = os.path.join(ROOT, "research", "anime_style_library.md")


# --------------------------------------------------------------------------- A. Cel core
# The house looks: how many tones, where the terminator sits, how hard it is. Everything else in the
# library is a variation on one of these.

def cel_two_tone(b):
    """Flat base, one cool shadow, a hard terminator, an inverted-hull outline."""
    b.master("ToonMaster",
             base_color=b.albedo(),
             shade_color=b.color((0.62, 0.58, 0.80), "shadeColor"),
             threshold=b.float_(0.35, "threshold"),
             softness=b.float_(0.06, "softness"))
    b.outline()


def cel_three_tone(b):
    """Shadow, mid and light: the band count most TV anime actually paints in."""
    ramp = b.cel(b.half_lambert(), 3.0, 0.10, bands_ref="bands", softness_ref="softness")
    base = b.albedo()
    tone = b.gradient(b.color((0.30, 0.28, 0.42), "shadowColor"),
                      b.color((0.68, 0.64, 0.80), "midColor"), base, ramp)
    b.master("UnlitMaster", color=b.cel_lit(tone, ramp, ambient=0.50, floor_=0.30))
    b.outline()


def cel_four_tone_soft(b):
    """Four wide, soft bands: RealToon's shaded look, good on cloth and big shapes."""
    ramp = b.cel(b.half_lambert(wrap=0.15), 4.0, 0.30, bands_ref="bands", softness_ref="softness")
    base = b.albedo()
    tone = b.gradient(b.color((0.42, 0.38, 0.52), "shadowColor"),
                      b.color((0.74, 0.70, 0.84), "midColor"), base, ramp)
    b.master("UnlitMaster", color=b.cel_lit(tone, ramp, ambient=0.55, floor_=0.35))
    b.outline((0.16, 0.13, 0.18), 0.018, 1.0)


def cel_hard_flat(b):
    """Two tones, zero softness, low ambient: high-contrast broadcast cel with a thick outline."""
    ramp = b.cel(b.half_lambert(), 2.0, 0.0, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(b.albedo(), ramp, (0.30, 0.26, 0.34), "shadowColor")
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=1.0, ambient=0.30)))
    b.outline((0.06, 0.05, 0.07), 0.030, 1.0)


def cel_cool_shadow(b):
    """One colour drives both tones: the shadow is the base hue-rotated cool and darkened."""
    base = b.albedo()
    shifted = b.hue_rotate(base, b.float_(-18.0, "shadowHueShift"))
    shade = b.scale(shifted, b.float_(0.72, "shadowDarkness"))
    b.master("ToonMaster", base_color=base, shade_color=shade,
             threshold=b.float_(0.38, "threshold"), softness=b.float_(0.10, "softness"))
    b.outline()


def cel_warm_bounce(b):
    """Two tones plus warm bounce light in the shadow: sunlit exteriors, Ghibli-ish."""
    ramp = b.cel(b.half_lambert(), 2.0, 0.08, bands_ref="bands", softness_ref="softness")
    base = b.albedo()
    tone = b.shade_with(base, ramp, (0.58, 0.55, 0.70), "shadowColor")
    bounce = b.scale(b.color((1.00, 0.62, 0.38), "bounceColor"),
                     b.mul(b.one_minus(ramp), b.float_(0.35, "bounceStrength")))
    lit = b.mul(tone, b.light_flat(sun=1.0, ambient=0.45))
    b.master("UnlitMaster", color=b.add(lit, b.mul(base, bounce)))
    b.outline()


def cel_wrapped_soft(b):
    """Wrapped half-lambert: the terminator travels further around the form, so faces and limbs keep
    a wide lit area even when the sun is behind them (the Genshin body trick)."""
    ramp = b.cel(b.half_lambert(wrap=0.35), 2.0, 0.25, bands_ref="bands", softness_ref="softness")
    base = b.albedo()
    tone = b.shade_with(base, ramp, (0.66, 0.62, 0.76), "shadowColor")
    lit = b.mul(tone, b.light_flat(sun=1.0, ambient=0.60))
    b.master("UnlitMaster", color=b.add(lit, b.rim(power=4.5, power_ref="rimPower",
                                                   strength=0.18, strength_ref="rimStrength")))
    b.outline()


# --------------------------------------------------------------------------- B. Face
# Faces are drawn, not lit: an SDF map or a head-angle terminator, and no specular at all.

def face_sdf(b):
    """Face shadow from an SDF map, so the shadow never breaks across the nose or eye sockets.
    Assign the map in the material; unassigned, the face stays fully lit."""
    sdf = b.split3(b.texture("", "faceSdfMap"))[0]
    lit = b.face_sdf(sdf, 0.0, 0.06, bias_ref="shadowBias", softness_ref="shadowSoftness")
    tone = b.shade_with(b.albedo(), lit, (0.78, 0.66, 0.70), "shadowColor")
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=0.85, ambient=0.55)))
    b.outline((0.18, 0.13, 0.16), 0.012, 1.0)


def face_flat_terminator(b):
    """A mapless face: the head-forward angle slides a soft vertical terminator across the UVs, and
    the whole face dims as it turns away from the sun."""
    angle = b.split2(b.face_angle())          # Face Light Angle is a vec2: x left/right, y front/back
    uv = b.split2(b.uv())
    pos = b.add(0.5, b.mul(angle[0], b.float_(0.45, "terminatorTravel")))
    soft = b.float_(0.16, "terminatorSoftness")
    lit = b.smoothstep(b.sub(pos, soft), pos, uv[0])
    lit = b.mul(lit, b.remap(angle[1], -1.0, 1.0, 0.45, 1.0))
    tone = b.shade_with(b.albedo(), lit, (0.80, 0.70, 0.74), "shadowColor")
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=0.90, ambient=0.50)))
    b.outline((0.18, 0.13, 0.16), 0.012, 1.0)


def face_soft_gradient(b):
    """Airbrushed face: a vertical gradient (chin slightly cooler, forehead clean) and a soft cheek
    band. No hard shadow anywhere - the style used for close-ups and romance-genre leads."""
    uv = b.split2(b.uv())
    grad = b.gradient(b.color((0.86, 0.78, 0.80), "chinColor"),
                      b.color((1.00, 0.97, 0.96), "midColor"),
                      b.color((1.00, 1.00, 1.00), "topColor"), uv[1])
    band = b.mul(b.smoothstep(0.62, 0.40, uv[1]), b.smoothstep(0.02, 0.22, uv[1]))
    cheek = b.scale(b.color((1.00, 0.70, 0.68), "cheekColor"),
                    b.mul(band, b.float_(0.18, "cheekStrength")))
    color = b.add(b.mul(b.albedo(), grad), cheek)
    b.master("UnlitMaster", color=b.mul(color, b.light_flat(sun=0.90, ambient=0.60)))
    b.outline((0.22, 0.16, 0.18), 0.010, 1.0)


def face_blush(b):
    """Toon face plus a mirrored radial blush mask built in UV space - no extra texture slot."""
    uv = b.split2(b.uv())
    dx = b.abs_(b.sub(uv[0], 0.5))
    dy = b.sub(uv[1], b.float_(0.38, "blushHeight"))
    dist = b.power(b.add(b.mul(dx, dx), b.mul(dy, dy)), 0.5)
    radius = b.float_(0.10, "blushRadius")
    mask = b.one_minus(b.smoothstep(radius, b.add(radius, b.float_(0.05, "blushSoftness")), dist))
    blush = b.scale(b.color((1.00, 0.45, 0.48), "blushColor"),
                    b.mul(mask, b.float_(0.55, "blushStrength")))
    b.master("ToonMaster", base_color=b.albedo(), shade_color=b.color((0.80, 0.70, 0.76), "shadeColor"),
             threshold=b.float_(0.40, "threshold"), softness=b.float_(0.20, "softness"),
             emission=blush)
    b.outline((0.20, 0.14, 0.17), 0.012, 1.0)


# --------------------------------------------------------------------------- C. Eye
# Eyes are jewellery: a matcap sheen, a hard catchlight, a dark limbal ring.

def eye_matcap(b):
    """Iris under a matcap sheen with a dark limbal ring and a hard catchlight. Unlit, so the eye
    reads the same from every angle - what anime eyes actually do."""
    base = b.albedo((1.0, 1.0, 1.0), "baseColor", "irisMap")
    sheen = b.scale(b.matcap("matcapMap"), b.float_(0.60, "matcapStrength"))
    lit = b.mul(base, b.add(b.float_(0.50, "eyeLight"), sheen))
    ring = b.scale(b.color((0.04, 0.04, 0.07), "limbalColor"),
                   b.mul(b.fresnel(2.5, "limbalPower"), b.float_(1.00, "limbalStrength")))
    catch = b.specular(color=(1.0, 1.0, 1.0), color_ref="catchlightColor", threshold=0.94,
                       threshold_ref="catchlightThreshold", softness=0.015,
                       softness_ref="catchlightSoftness", strength=1.20,
                       strength_ref="catchlightStrength")
    b.master("UnlitMaster", color=b.add(b.add(lit, ring), catch))


def eye_gloss(b):
    """The same eye kept in the PBR path: a wet cornea from the engine's specular plus a glowing
    catchlight, for scenes where the eye has to match physically lit skin."""
    b.master("PBRMaster",
             base_color=b.albedo((1.0, 1.0, 1.0), "baseColor", "irisMap"),
             metallic=0.0,
             smoothness=b.float_(0.92, "smoothness"),
             emission=b.specular(color=(1.0, 1.0, 1.0), color_ref="catchlightColor", threshold=0.90,
                                 threshold_ref="catchlightThreshold", strength=1.50,
                                 strength_ref="catchlightStrength"))


# --------------------------------------------------------------------------- D. Hair
# The anisotropic ring is the whole job; everything else is which two tones sit under it.

def hair_angel_ring(b):
    """Toon hair with the classic angel ring: a two-lobe anisotropic highlight as emission."""
    ring = b.hair_spec(shift=0.25, exponent=48.0, strength=1.0, shift_ref="ringShift",
                       exponent_ref="ringExponent", strength_ref="ringStrength")
    b.master("ToonMaster", base_color=b.albedo(), shade_color=b.color((0.55, 0.50, 0.68), "shadeColor"),
             threshold=b.float_(0.40, "threshold"), softness=b.float_(0.12, "softness"),
             emission=b.scale(b.color((1.00, 0.96, 0.88), "ringColor"), ring))
    b.outline()


def hair_two_lobe(b):
    """Kajiura hair: a tight white primary lobe and a wider tinted secondary one, over two tones."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 2.0, 0.12, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.50, 0.46, 0.62), "shadowColor")
    primary = b.hair_spec(shift=0.20, exponent=64.0, strength=1.0, shift_ref="primaryShift",
                          exponent_ref="primaryExponent", strength_ref="primaryStrength")
    secondary = b.hair_spec(shift=-0.35, exponent=24.0, strength=0.6, shift_ref="secondaryShift",
                            exponent_ref="secondaryExponent", strength_ref="secondaryStrength")
    glow = b.glow(b.scale(b.color((1.00, 1.00, 1.00), "primaryColor"), primary),
                  b.scale(b.color((0.90, 0.70, 0.80), "secondaryColor"), secondary),
                  b.rim(color=(0.85, 0.80, 1.00), color_ref="rimColor", power=3.0,
                        power_ref="rimPower", strength=0.30, strength_ref="rimStrength"))
    b.master("UnlitMaster", color=b.add(b.mul(tone, b.light_flat(sun=1.0, ambient=0.45)), glow))
    b.outline((0.14, 0.11, 0.17), 0.018, 1.0)


def hair_root_to_tip(b):
    """Dark roots to light tips: a world-height gradient under a soft cel ramp."""
    y = b.split3(b.world())[1]
    t = b.remap(y, b.float_(-0.20, "rootHeight"), b.float_(1.60, "tipHeight"), 0.0, 1.0)
    grad = b.gradient(b.color((0.35, 0.28, 0.32), "rootColor"),
                      b.color((0.75, 0.62, 0.60), "midColor"),
                      b.color((1.00, 0.95, 0.90), "tipColor"), t)
    ramp = b.cel(b.half_lambert(), 2.0, 0.18, bands_ref="bands", softness_ref="softness")
    base = b.mul(b.albedo(), grad)
    tone = b.shade_with(base, ramp, (0.55, 0.50, 0.62), "shadowColor")
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=1.0, ambient=0.50)))
    b.outline((0.14, 0.11, 0.16), 0.016, 1.0)


def hair_matcap_silk(b):
    """Silk: a matcap sheen over a soft two-tone, with a cool rim. Reads as straight, glossy hair."""
    base = b.albedo()
    sheen = b.scale(b.matcap("matcapMap"), b.float_(0.80, "matcapStrength"))
    ramp = b.cel(b.half_lambert(), 2.0, 0.30, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.60, 0.56, 0.70), "shadowColor")
    color = b.add(b.mul(tone, b.light_flat(sun=0.90, ambient=0.45)), sheen)
    color = b.add(color, b.rim(color=(0.90, 0.85, 1.00), color_ref="rimColor", power=3.5,
                               power_ref="rimPower", strength=0.35, strength_ref="rimStrength"))
    b.master("UnlitMaster", color=color)
    b.outline((0.14, 0.11, 0.16), 0.016, 1.0)


# --------------------------------------------------------------------------- E. Skin
# Wide soft bands, warm shadows, translucency at the backlight. No hard edges.

def skin_soft_peach(b):
    """Soft toon skin: a wide soft band, a warm shadow, a whisper of rim standing in for subsurface."""
    sss = b.scale(b.color((1.00, 0.55, 0.45), "sssColor"),
                  b.mul(b.fresnel(3.0, "sssPower"), b.float_(0.25, "sssStrength")))
    b.master("ToonMaster", base_color=b.albedo((1.0, 1.0, 1.0), "baseColor", "skinMap"),
             shade_color=b.color((0.92, 0.74, 0.72), "shadeColor"),
             threshold=b.float_(0.42, "threshold"), softness=b.float_(0.28, "softness"),
             emission=sss)
    b.outline((0.30, 0.20, 0.22), 0.010, 1.0)


def skin_subsurface_rim(b):
    """Backlit translucency: warm glow where the sun is behind the surface - ears, nose, fingers."""
    n = b.normal()
    ramp = b.cel(b.half_lambert(n), 2.0, 0.20, bands_ref="bands", softness_ref="softness")
    back = b.saturate(b.sub(0.0, b.dot(n, b.sun())))
    glow = b.scale(b.color((1.00, 0.42, 0.32), "sssColor"),
                   b.mul(back, b.float_(0.60, "sssStrength")))
    base = b.albedo((1.0, 1.0, 1.0), "baseColor", "skinMap")
    tone = b.shade_with(base, ramp, (0.88, 0.70, 0.70), "shadowColor")
    b.master("UnlitMaster", color=b.add(b.mul(tone, b.light_flat(sun=0.95, ambient=0.55)), glow))
    b.outline((0.30, 0.20, 0.22), 0.010, 1.0)


# --------------------------------------------------------------------------- F. Cloth
# Matte, folded, and shaded by the mesh's own vertex colours where the artist painted AO.

def cloth_matte_uniform(b):
    """School-uniform cloth: vertex-colour AO, a normal map for folds, matte toon light, faint rim."""
    base = b.mul(b.albedo((1.0, 1.0, 1.0), "baseColor", "clothMap"), b.vertex_color())
    normal = b.node("NormalMap")
    b.wire(b.texture("", "normalMap"), normal.id, 0)
    b.wire(b.float_(1.0, "normalStrength"), normal.id, 1)
    b.master("ToonMaster", base_color=base, shade_color=b.color((0.60, 0.58, 0.70), "shadeColor"),
             threshold=b.float_(0.38, "threshold"), softness=b.float_(0.10, "softness"),
             normal=normal,
             emission=b.rim(color=(0.80, 0.85, 1.00), color_ref="rimColor", power=4.0,
                            power_ref="rimPower", strength=0.20, strength_ref="rimStrength"))
    b.outline((0.12, 0.10, 0.14), 0.020, 1.0)


def cloth_satin_sheen(b):
    """Satin: kept in the PBR path so it catches real sky light, with cel-banded tint and a sheen rim."""
    base = b.albedo((1.0, 1.0, 1.0), "baseColor", "clothMap")
    ramp = b.cel(b.half_lambert(), 3.0, 0.25, bands_ref="bands", softness_ref="softness")
    banded = b.mul(base, b.gradient(b.color((0.55, 0.52, 0.62), "shadowColor"),
                                    b.color((0.85, 0.83, 0.90), "midColor"),
                                    b.color((1.00, 1.00, 1.00), "lightColor"), ramp))
    sheen = b.scale(b.color((1.00, 0.95, 0.90), "sheenColor"),
                    b.mul(b.fresnel(2.5, "sheenPower"), b.float_(0.35, "sheenStrength")))
    b.master("PBRMaster", base_color=banded, metallic=b.float_(0.0, "metallic"),
             smoothness=b.float_(0.55, "smoothness"), emission=sheen)
    b.outline((0.13, 0.11, 0.15), 0.014, 1.0)


# --------------------------------------------------------------------------- G. Ink and line
# Traditional-media looks: wash, screentone, hatching, contour lines, brush grain.

def ink_sumie(b):
    """Sumi-e wash: monochrome ink tones from the albedo's luma, mottled like wet paper."""
    luma = b.luminance(b.albedo())
    value = b.saturate(b.mul(luma, b.add(b.half_lambert(), 0.35)))
    tone = b.posterize(value, b.float_(4.0, "toneSteps"))
    ink = b.gradient(b.color((0.10, 0.10, 0.12), "inkColor"),
                     b.color((0.55, 0.55, 0.58), "midColor"),
                     b.color((0.96, 0.95, 0.92), "paperColor"), tone)
    mottle = b.mul(b.noise("ValueNoise", b.uv(), 6.0, "grainScale"), b.float_(0.35, "grainStrength"))
    ink = b.lerp(ink, b.mul(ink, b.color((0.85, 0.85, 0.82), "mottleColor")), mottle)
    b.master("UnlitMaster", color=ink)


def ink_manga_halftone(b):
    """Manga screentone: flat base colour, halftone dots exactly where the shadow falls."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 2.0, 0.05, bands_ref="bands", softness_ref="softness")
    dots = b.halftone(b.one_minus(ramp), b.uv(), frequency=28.0, angle=45.0,
                      frequency_ref="dotFrequency", angle_ref="dotAngle")
    coverage = b.mul(dots, b.mul(b.one_minus(ramp), b.float_(1.0, "dotStrength")))
    color = b.lerp(base, b.mul(base, b.color((0.35, 0.33, 0.45), "shadowColor")), coverage)
    b.master("UnlitMaster", color=b.mul(color, b.light_flat(sun=1.0, ambient=0.35)))
    b.outline((0.05, 0.05, 0.06), 0.028, 1.0)


def ink_sketch_hatch(b):
    """Pencil sketch: single hatch in the shadow, cross-hatch in the deep shadow, albedo desaturated."""
    base = b.desaturate(b.albedo(), b.float_(0.55, "saturation"))
    ramp = b.cel(b.half_lambert(), 3.0, 0.08, bands_ref="bands", softness_ref="softness")
    shadow = b.one_minus(ramp)
    hatch = b.hatch(shadow, b.uv(), 38.0, 45.0, "hatchFrequency", "hatchAngle")
    cross = b.hatch(b.mul(shadow, shadow), b.uv(), 52.0, -45.0, "crossFrequency", "crossAngle")
    lines = b.saturate(b.add(b.mul(hatch, b.float_(0.70, "hatchStrength")),
                             b.mul(cross, b.float_(0.80, "crossStrength"))))
    color = b.lerp(base, b.color((0.12, 0.12, 0.14), "inkColor"), lines)
    b.master("UnlitMaster", color=b.mul(color, b.light_flat(sun=0.95, ambient=0.50)))
    b.outline((0.10, 0.10, 0.12), 0.020, 1.0)


def ink_contour_line(b):
    """Fake line art: flat colour darkened where the surface turns away, so contours read as drawn
    lines without any texture."""
    ramp = b.cel(b.half_lambert(), 2.0, 0.25, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(b.albedo(), ramp, (0.72, 0.70, 0.78), "shadowColor")
    line = b.saturate(b.mul(b.fresnel(2.0, "linePower"), b.float_(0.85, "lineStrength")))
    color = b.lerp(tone, b.color((0.08, 0.07, 0.10), "lineColor"), line)
    b.master("UnlitMaster", color=b.mul(color, b.light_flat(sun=1.0, ambient=0.40)))
    b.outline((0.06, 0.05, 0.08), 0.026, 1.0)


def ink_brush_grain(b):
    """Hand-drawn wobble: value noise jitters the cel terminator so edges breathe like brushwork,
    over a paper grain."""
    base = b.albedo()
    jitter = b.mul(b.sub(b.noise("ValueNoise", b.uv(), 9.0, "grainScale"), 0.5),
                   b.float_(0.22, "wobble"))
    ramp = b.cel(b.add(b.half_lambert(), jitter), 2.0, 0.03, bands_ref="bands",
                 softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.45, 0.42, 0.50), "shadowColor")
    paper = b.noise("ValueNoise", b.uv(), 3.0, "paperScale")
    tone = b.mul(tone, b.lerp(b.color((1.0, 1.0, 1.0)), b.color((0.93, 0.92, 0.88), "paperTint"), paper))
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=1.0, ambient=0.40)))
    b.outline((0.10, 0.09, 0.12), 0.022, 1.0)


# --------------------------------------------------------------------------- H. Painterly
# Traditional-media paints: wet edges, opaque poster colour, canvas tooth, chalk grain.

def paint_watercolor(b):
    """Watercolour: soft wide bands, low saturation, pigment that darkens at the edges, paper tooth."""
    base = b.desaturate(b.albedo(), b.float_(0.72, "saturation"))
    ramp = b.cel(b.half_lambert(wrap=0.20), 2.0, 0.45, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.80, 0.80, 0.84), "shadowColor")
    edge = b.saturate(b.mul(b.fresnel(2.2, "edgePower"), b.float_(0.45, "edgeStrength")))
    tone = b.lerp(tone, b.mul(tone, b.color((0.55, 0.58, 0.68), "pigmentColor")), edge)
    grain = b.noise("ValueNoise", b.uv(), 4.0, "grainScale")
    tone = b.lerp(tone, b.mul(tone, b.color((0.94, 0.93, 0.90), "paperTint")),
                  b.mul(grain, b.float_(0.50, "grainStrength")))
    b.master("UnlitMaster", color=b.saturate(b.mul(tone, b.light_flat(sun=0.95, ambient=0.65))))


def paint_gouache(b):
    """Gouache: opaque poster colour, value quantised into flat steps, faint brush grain."""
    base = b.desaturate(b.albedo(), b.float_(0.90, "saturation"))
    light = b.posterize(b.saturate(b.mul(b.luminance(base), b.add(b.half_lambert(), 0.30))),
                        b.float_(4.0, "toneSteps"))
    tone = b.mul(base, b.splat(b.remap(light, 0.0, 1.0, 0.40, 1.05)))
    grain = b.noise("ValueNoise", b.uv(), 26.0, "grainScale")
    tone = b.masked(tone, grain, b.float_(0.14, "grainStrength"))
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=1.0, ambient=0.45)))
    b.outline((0.18, 0.16, 0.18), 0.016, 1.0)


def paint_oil_impasto(b):
    """Oil: four bands, warm light against cool shadow, canvas tooth, a soft gloss where the paint is
    thick."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 4.0, 0.18, bands_ref="bands", softness_ref="softness")
    warm = b.hue_rotate(base, b.float_(12.0, "warmShift"))
    cool = b.hue_rotate(base, b.float_(-18.0, "coolShift"))
    tone = b.gradient(b.mul(cool, b.float_(0.45, "shadowDarkness")),
                      b.mul(warm, b.float_(0.75, "midDarkness")), base, ramp)
    canvas_uv = b.uv_transform(b.node("UV1"), (4.0, 4.0), (0.0, 0.0))
    canvas = b.mul(b.noise("ValueNoise", canvas_uv, 40.0, "canvasScale"),
                   b.luminance(b.texture("", "canvasMap", uv=canvas_uv)))
    tone = b.lerp(tone, b.mul(tone, b.color((0.92, 0.91, 0.88), "canvasTint")),
                  b.mul(canvas, b.float_(0.35, "canvasStrength")))
    gloss = b.specular(color=(1.00, 0.98, 0.92), color_ref="glossColor", threshold=0.86,
                       threshold_ref="glossThreshold", softness=0.06, softness_ref="glossSoftness",
                       strength=0.35, strength_ref="glossStrength")
    b.master("UnlitMaster", color=b.add(b.mul(tone, b.light_flat(sun=1.0, ambient=0.50)), gloss))


def paint_pastel_chalk(b):
    """Pastel chalk: high key, lifted shadows, desaturated, dry grain over the whole surface."""
    base = b.desaturate(b.albedo(), b.float_(0.55, "saturation"))
    ramp = b.cel(b.half_lambert(wrap=0.25), 2.0, 0.40, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.88, 0.87, 0.92), "shadowColor")
    chalk = b.noise("WhiteNoise", b.uv(), 220.0, "grainScale")
    tone = b.lerp(tone, b.mul(tone, b.color((1.02, 1.01, 0.99), "chalkTint")),
                  b.mul(chalk, b.float_(0.18, "grainStrength")))
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=0.90, ambient=0.85)))
    b.outline((0.30, 0.28, 0.30), 0.010, 1.0)


def paint_poster_print(b):
    """Screen print: three flat tones, each hue-shifted like misregistered ink, over a halftone."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 3.0, 0.0, bands_ref="bands", softness_ref="softness")
    tone = b.gradient(b.hue_rotate(base, b.float_(-25.0, "shadowHueShift")),
                      b.hue_rotate(base, b.float_(-8.0, "midHueShift")), base, ramp)
    tone = b.mul(tone, b.gradient(b.color((0.35, 0.33, 0.45), "shadowColor"),
                                  b.color((0.75, 0.74, 0.80), "midColor"),
                                  b.color((1.00, 1.00, 1.00), "lightColor"), ramp))
    dots = b.halftone(b.mul(b.luminance(base), 0.60), b.uv(), 60.0, 15.0,
                      "dotFrequency", "dotAngle")
    tone = b.masked(tone, dots, b.float_(0.25, "dotStrength"))
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=1.0, ambient=0.40)))
    b.outline((0.08, 0.07, 0.09), 0.024, 1.0)


# --------------------------------------------------------------------------- I. Retro broadcast
# Limited palettes, ordered dither, scanlines and grain: the looks of old hardware and old TV.

def retro_pixel_8bit(b):
    """8-bit: blocky UVs, a quantised light ramp, ordered dither at the transitions, palette tint."""
    base = b.texture("", "baseMap", uv=b.pixelate(b.uv(), 96.0, "pixelBlocks"))
    dither = b.mul(b.sub(b.bayer(1.0, "ditherPixel"), 0.5), b.float_(0.15, "ditherStrength"))
    light = b.posterize(b.saturate(b.add(b.half_lambert(), dither)), b.float_(4.0, "lightSteps"))
    tone = b.mul(base, b.splat(b.remap(light, 0.0, 1.0, 0.25, 1.0)))
    palette = b.gradient(b.color((0.35, 0.30, 0.55), "shadowColor"),
                         b.color((0.75, 0.70, 0.80), "midColor"),
                         b.color((1.00, 1.00, 1.00), "lightColor"), light)
    color = b.mul(b.desaturate(tone, b.float_(0.85, "saturation")), palette)
    b.master("UnlitMaster", color=b.mul(color, b.light_flat(sun=1.0, ambient=0.30)))
    b.outline((0.05, 0.04, 0.08), 0.035, 1.0)


def retro_tv_90s(b):
    """90s TV transfer: two tones with a dithered terminator, scanlines, a little desaturation."""
    base = b.desaturate(b.albedo(), b.float_(0.85, "saturation"))
    dither = b.mul(b.sub(b.bayer(1.0, "ditherPixel"), 0.5), b.float_(0.10, "ditherStrength"))
    ramp = b.cel(b.add(b.half_lambert(), dither), 2.0, 0.02, bands_ref="bands",
                 softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.42, 0.40, 0.52), "shadowColor")
    lines = b.scanlines(3.0, 0.0, "scanlineHeight", "scanlineSpeed")
    tone = b.masked(tone, lines, b.float_(0.22, "scanlineStrength"))
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=1.0, ambient=0.40)))
    b.outline((0.09, 0.08, 0.11), 0.022, 1.0)


def retro_gameboy_mono(b):
    """Four green tones, dithered between: a handheld screen with no colour at all."""
    luma = b.luminance(b.albedo())
    value = b.posterize(b.saturate(b.mul(luma, b.add(b.half_lambert(), 0.30))),
                        b.float_(4.0, "toneSteps"))
    dither = b.mul(b.sub(b.bayer(1.0, "ditherPixel"), 0.5), b.float_(0.10, "ditherStrength"))
    value = b.saturate(b.add(value, dither))
    color = b.gradient(b.color((0.05, 0.12, 0.06), "darkColor"),
                       b.color((0.30, 0.48, 0.22), "midColor"),
                       b.color((0.62, 0.78, 0.45), "lightColor"), value)
    b.master("UnlitMaster", color=color)
    b.outline((0.03, 0.07, 0.04), 0.030, 1.0)


def retro_vhs(b):
    """VHS tape: animated grain, rolling scanlines, a slow hue wobble, washed colour."""
    base = b.hue_rotate(b.desaturate(b.albedo(), b.float_(0.70, "saturation")),
                        b.mul(b.sine(b.mul(b.time(), b.float_(1.5, "wobbleSpeed"))),
                              b.float_(6.0, "wobbleDegrees")))
    ramp = b.cel(b.half_lambert(), 2.0, 0.20, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.45, 0.42, 0.50), "shadowColor")
    grain_uv = b.uv_scroll(b.screen_position(), speed=(7.0, 3.0))
    tone = b.masked(tone, b.noise("WhiteNoise", grain_uv, 240.0, "grainScale"),
                    b.float_(0.28, "grainStrength"))
    tone = b.masked(tone, b.scanlines(2.0, 0.35, "scanlineHeight", "scanlineSpeed"),
                    b.float_(0.20, "scanlineStrength"))
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=1.0, ambient=0.40)))


def retro_dither_cel(b):
    """Ordered-dither cel: three tones whose transitions are a Bayer pattern instead of a gradient."""
    base = b.albedo()
    dither = b.mul(b.sub(b.bayer(1.0, "ditherPixel"), 0.5), b.float_(0.35, "ditherStrength"))
    ramp = b.cel(b.add(b.half_lambert(), dither), 3.0, 0.0, bands_ref="bands",
                 softness_ref="softness")
    tone = b.gradient(b.color((0.22, 0.20, 0.30), "shadowColor"),
                      b.color((0.60, 0.57, 0.68), "midColor"), base, ramp)
    b.master("UnlitMaster", color=b.cel_lit(tone, ramp, ambient=0.40, floor_=0.35))
    b.outline((0.08, 0.07, 0.10), 0.026, 1.0)


# --------------------------------------------------------------------------- J. Modern game looks
# What shipped anime games actually do: wrapped cel bodies, NPR metal, hand-painted fighters.

def game_genshin_body(b):
    """The open-world character kit: wrapped cel bands, a normal map, vertex-colour AO, rim light and
    a hard specular, all under an outline."""
    base = b.mul(b.albedo(), b.vertex_color())
    normal = b.node("NormalMap")
    b.wire(b.texture("", "normalMap"), normal.id, 0)
    b.wire(b.float_(1.0, "normalStrength"), normal.id, 1)
    ramp = b.cel(b.half_lambert(normal, wrap=0.25), 2.0, 0.08, bands_ref="bands",
                 softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.62, 0.58, 0.74), "shadowColor")
    glow = b.glow(
        b.rim(normal, color=(1.00, 0.95, 0.90), color_ref="rimColor", power=4.0,
              power_ref="rimPower", strength=0.35, strength_ref="rimStrength"),
        b.specular(normal, color=(1.0, 1.0, 1.0), color_ref="specColor", threshold=0.90,
                   threshold_ref="specThreshold", softness=0.02, softness_ref="specSoftness",
                   strength=0.60, strength_ref="specStrength"))
    b.master("UnlitMaster", color=b.add(b.mul(tone, b.light_flat(sun=1.0, ambient=0.50)), glow))
    b.outline((0.13, 0.10, 0.15), 0.020, 1.0)


def game_metal_trim(b):
    """NPR metal trim: real PBR reflections with a cel-banded base colour and a hard specular shape,
    so armour matches the character next to it."""
    base = b.albedo((0.85, 0.86, 0.90), "baseColor", "metalMap")
    ramp = b.cel(b.half_lambert(), 3.0, 0.06, bands_ref="bands", softness_ref="softness")
    banded = b.mul(base, b.gradient(b.color((0.25, 0.26, 0.32), "shadowColor"),
                                    b.color((0.70, 0.72, 0.78), "midColor"),
                                    b.color((1.15, 1.15, 1.20), "lightColor"), ramp))
    spec = b.specular(color=(1.00, 0.98, 0.90), color_ref="specColor", threshold=0.85,
                      threshold_ref="specThreshold", softness=0.03, softness_ref="specSoftness",
                      strength=1.20, strength_ref="specStrength")
    b.master("PBRMaster", base_color=banded, metallic=b.float_(0.90, "metallic"),
             smoothness=b.float_(0.62, "smoothness"), emission=spec)
    b.outline((0.07, 0.07, 0.09), 0.018, 1.0)


def game_guilty_gear(b):
    """Hand-painted fighter: three hard bands, a hue-shifted shadow, no ambient at all, a saturated
    rim and a very thick outline."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 3.0, 0.04, bands_ref="bands", softness_ref="softness")
    hue = b.hue_rotate(base, b.float_(-30.0, "shadowHueShift"))
    tone = b.gradient(b.mul(hue, b.float_(0.35, "shadowDarkness")),
                      b.mul(base, b.float_(0.72, "midDarkness")), base, ramp)
    glow = b.glow(b.rim(color=(1.00, 0.35, 0.45), color_ref="rimColor", power=2.5,
                        power_ref="rimPower", strength=0.80, strength_ref="rimStrength"))
    b.master("UnlitMaster", color=b.add(b.mul(tone, b.light_flat(sun=1.15, ambient=0.0)), glow))
    b.outline((0.03, 0.02, 0.04), 0.040, 1.0)


def game_borderlands_ink(b):
    """Graphic ink over lit colour: cel bands, hatching in the shadow, contour darkening, outline."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 2.0, 0.06, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.40, 0.38, 0.46), "shadowColor")
    hatch = b.hatch(b.one_minus(ramp), b.uv(), 45.0, 30.0, "hatchFrequency", "hatchAngle")
    tone = b.lerp(tone, b.mul(tone, b.color((0.55, 0.53, 0.60), "hatchColor")),
                  b.mul(hatch, b.float_(0.60, "hatchStrength")))
    line = b.saturate(b.mul(b.fresnel(2.5, "linePower"), b.float_(0.80, "lineStrength")))
    tone = b.lerp(tone, b.color((0.06, 0.05, 0.08), "lineColor"), line)
    glow = b.glow(b.specular(color=(1.0, 1.0, 1.0), color_ref="specColor", threshold=0.88,
                             threshold_ref="specThreshold", softness=0.04,
                             softness_ref="specSoftness", strength=0.40,
                             strength_ref="specStrength"))
    b.master("UnlitMaster", color=b.add(b.mul(tone, b.light_flat(sun=1.0, ambient=0.35)), glow))
    b.outline((0.04, 0.03, 0.05), 0.035, 1.0)


def game_spiderverse(b):
    """Comic film: posterised tones, a halftone over everything, and a chromatic fringe that splits
    warm and cool at the silhouette."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 3.0, 0.02, bands_ref="bands", softness_ref="softness")
    tone = b.gradient(b.color((0.18, 0.16, 0.30), "shadowColor"),
                      b.color((0.62, 0.40, 0.72), "midColor"), base, ramp)
    fringe = b.saturate(b.mul(b.fresnel(3.0, "fringePower"), b.float_(0.90, "fringeStrength")))
    tone = b.lerp(tone, b.hue_rotate(tone, b.float_(25.0, "warmFringeShift")), fringe)
    tone = b.lerp(tone, b.hue_rotate(tone, b.float_(-35.0, "coolFringeShift")), b.mul(fringe, 0.5))
    dots = b.halftone(b.mul(b.luminance(base), 0.80), b.uv(), 70.0, 20.0,
                      "dotFrequency", "dotAngle")
    tone = b.masked(tone, dots, b.float_(0.18, "dotStrength"))
    b.master("UnlitMaster", color=b.mul(tone, b.light_flat(sun=1.05, ambient=0.35)))
    b.outline((0.02, 0.02, 0.03), 0.030, 1.0)


# --------------------------------------------------------------------------- K. Studio film looks
# The lighting departments: soft high-key romance, pastel pastoral, hard pop action.

def film_shinkai_glow(b):
    """Golden-hour glow: a very soft band, a warm rim, and a light sweep travelling across the surface
    for bloom to catch."""
    uv = b.split2(b.uv())
    coord = b.add(b.mul(uv[0], 0.70), b.mul(uv[1], 0.70))
    phase = b.fract(b.sub(coord, b.mul(b.time(), b.float_(0.15, "sweepSpeed"))))
    distance = b.abs_(b.sub(phase, b.float_(0.50, "sweepCenter")))
    pulse = b.one_minus(b.smoothstep(0.0, b.float_(0.10, "sweepWidth"), distance))
    sweep = b.scale(b.color((1.00, 0.85, 0.60), "sweepColor"),
                    b.mul(pulse, b.float_(0.60, "sweepStrength")))
    rim = b.rim(color=(1.00, 0.80, 0.65), color_ref="rimColor", power=3.0, power_ref="rimPower",
                strength=0.70, strength_ref="rimStrength")
    b.master("ToonMaster", base_color=b.albedo(), shade_color=b.color((0.70, 0.62, 0.72), "shadeColor"),
             threshold=b.float_(0.32, "threshold"), softness=b.float_(0.35, "softness"),
             emission=b.glow(rim, sweep))
    b.outline((0.16, 0.13, 0.18), 0.014, 1.0)


def film_ghibli_pastel(b):
    """Pastel pastoral: desaturated, lifted warm shadow, very soft band, faint grain, thin outline."""
    base = b.desaturate(b.albedo(), b.float_(0.78, "saturation"))
    base = b.masked(base, b.noise("ValueNoise", b.uv(), 5.0, "grainScale"),
                    b.float_(0.10, "grainStrength"))
    b.master("ToonMaster", base_color=base, shade_color=b.color((0.88, 0.85, 0.80), "shadeColor"),
             threshold=b.float_(0.30, "threshold"), softness=b.float_(0.40, "softness"),
             emission=b.rim(color=(1.00, 0.95, 0.85), color_ref="rimColor", power=4.0,
                            power_ref="rimPower", strength=0.25, strength_ref="rimStrength"))
    b.outline((0.24, 0.21, 0.20), 0.012, 1.0)


def film_kyoani_soft(b):
    """High-key soft: bright warm shadow, wide softness, a subtle rim, the thinnest outline in the
    library. Everyday school-life lighting."""
    base = b.albedo((1.0, 1.0, 1.0), "baseColor", "skinMap")
    b.master("ToonMaster", base_color=base, shade_color=b.color((0.93, 0.86, 0.86), "shadeColor"),
             threshold=b.float_(0.28, "threshold"), softness=b.float_(0.45, "softness"),
             emission=b.rim(color=(1.00, 0.92, 0.90), color_ref="rimColor", power=5.0,
                            power_ref="rimPower", strength=0.18, strength_ref="rimStrength"))
    b.outline((0.28, 0.22, 0.24), 0.008, 1.0)


def film_trigger_pop(b):
    """Pop action: two bold saturated tones, a hard white rim, almost no ambient, a thick outline."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 2.0, 0.0, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.32, 0.26, 0.42), "shadowColor")
    glow = b.glow(b.rim(color=(1.0, 1.0, 1.0), color_ref="rimColor", power=2.0,
                        power_ref="rimPower", strength=0.90, strength_ref="rimStrength"))
    b.master("UnlitMaster", color=b.add(b.mul(tone, b.light_flat(sun=1.10, ambient=0.15)), glow))
    b.outline((0.02, 0.02, 0.03), 0.045, 1.0)


# --------------------------------------------------------------------------- L. Neon and stage
# Emissive looks: night city, idol stage, anything that has to glow in the bloom pass.

def neon_cyber_cel(b):
    """Cyberpunk night: near-black shadow, a neon rim, a grid over the surface, scanlines."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 2.0, 0.05, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.16, 0.14, 0.26), "shadowColor")
    grid = b.saturate(b.add(b.hatch(b.fract(b.mul(b.split2(b.uv())[0], b.float_(8.0, "gridScale"))),
                                    b.uv(), 8.0, 0.0, "gridFrequency", "gridAngle"),
                            b.hatch(b.fract(b.mul(b.split2(b.uv())[1], b.float_(8.0, "gridScale"))),
                                    b.uv(), 8.0, 90.0, "gridFrequency", "gridAngle")))
    glow = b.glow(
        b.scale(b.color((0.20, 0.90, 1.00), "gridColor"), b.mul(grid, b.float_(0.50, "gridStrength"))),
        b.rim(color=(1.00, 0.20, 0.70), color_ref="rimColor", power=2.5, power_ref="rimPower",
              strength=1.20, strength_ref="rimStrength"))
    color = b.masked(b.mul(tone, b.light_flat(sun=0.80, ambient=0.25)),
                     b.scanlines(4.0, 0.10, "scanlineHeight", "scanlineSpeed"),
                     b.float_(0.18, "scanlineStrength"))
    b.master("UnlitMaster", color=b.add(color, glow))


def neon_idol_stage(b):
    """Idol stage: glossy cel, a matcap sparkle, an animated sweep and a strong rim - all of it in
    the bloom pass."""
    base = b.albedo()
    ramp = b.cel(b.half_lambert(), 3.0, 0.12, bands_ref="bands", softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.48, 0.44, 0.62), "shadowColor")
    sparkle = b.scale(b.matcap("matcapMap"), b.float_(0.70, "matcapStrength"))
    phase = b.fract(b.sub(b.mul(b.split2(b.uv())[1], 1.20),
                          b.mul(b.time(), b.float_(0.35, "sweepSpeed"))))
    pulse = b.one_minus(b.smoothstep(0.0, b.float_(0.12, "sweepWidth"), b.abs_(b.sub(phase, 0.5))))
    sweep = b.scale(b.color((1.00, 0.90, 1.00), "sweepColor"),
                    b.mul(pulse, b.float_(0.80, "sweepStrength")))
    glow = b.glow(sparkle, sweep,
                  b.rim(color=(0.90, 0.70, 1.00), color_ref="rimColor", power=3.0,
                        power_ref="rimPower", strength=0.90, strength_ref="rimStrength"))
    b.master("UnlitMaster", color=b.add(b.mul(tone, b.light_flat(sun=0.90, ambient=0.35)), glow))
    b.outline((0.10, 0.08, 0.14), 0.016, 1.0)


# --------------------------------------------------------------------------- M. Anime world
# The environment side: painted skies, cel foliage, stylised water. No outlines - hull outlines on
# world geometry read as silhouette errors.

def world_sky_gradient(b):
    """Painted sky: a three-stop height gradient, a sun glow that follows the scene's sun direction,
    and slow cloud noise above the horizon."""
    height = b.saturate(b.remap(b.split3(b.world())[1], b.float_(-20.0, "horizonHeight"),
                                b.float_(120.0, "zenithHeight"), 0.0, 1.0))
    sky = b.gradient(b.color((1.00, 0.78, 0.62), "horizonColor"),
                     b.color((0.55, 0.72, 0.95), "midColor"),
                     b.color((0.16, 0.32, 0.68), "zenithColor"), height)
    glow = b.power(b.saturate(b.dot(b.view(), b.sun())), b.float_(24.0, "sunGlowPower"))
    sky = b.add(sky, b.scale(b.color((1.00, 0.85, 0.60), "sunGlowColor"),
                             b.mul(glow, b.float_(0.80, "sunGlowStrength"))))
    clouds = b.noise("ValueNoise", b.uv_scroll(b.uv(), speed=(0.005, 0.002), tiling=(3.0, 3.0)),
                     3.0, "cloudScale")
    mask = b.mul(b.smoothstep(0.05, 0.45, height), b.mul(clouds, b.float_(0.80, "cloudStrength")))
    sky = b.lerp(sky, b.color((1.00, 0.99, 0.97), "cloudColor"), b.saturate(mask))
    b.master("UnlitMaster", color=sky)


def world_foliage_cel(b):
    """Cel foliage: wrapped bands so leaves stay lit, translucency where the sun is behind them,
    vertex-colour AO, and a root-to-tip tint."""
    base = b.mul(b.albedo((1.0, 1.0, 1.0), "baseColor", "leafMap"), b.vertex_color())
    height = b.saturate(b.remap(b.split3(b.world())[1], b.float_(0.0, "rootHeight"),
                                b.float_(2.0, "tipHeight"), 0.0, 1.0))
    base = b.mul(base, b.gradient(b.color((0.55, 0.62, 0.42), "rootColor"),
                                  b.color((0.85, 0.92, 0.70), "midColor"),
                                  b.color((1.05, 1.10, 0.90), "tipColor"), height))
    n = b.normal()
    ramp = b.cel(b.half_lambert(n, wrap=0.40), 2.0, 0.15, bands_ref="bands",
                 softness_ref="softness")
    tone = b.shade_with(base, ramp, (0.45, 0.55, 0.40), "shadowColor")
    back = b.saturate(b.sub(0.0, b.dot(n, b.sun())))
    glow = b.glow(b.scale(b.color((0.75, 1.00, 0.35), "translucencyColor"),
                          b.mul(back, b.float_(0.45, "translucencyStrength"))))
    b.master("UnlitMaster", color=b.add(b.mul(tone, b.light_flat(sun=1.0, ambient=0.55)), glow))


def world_water_sheen(b):
    """Stylised water: scrolling ripples that perturb the normal, a height gradient from deep to
    shallow, a broken specular, fresnel-driven alpha."""
    waves = b.noise("ValueNoise", b.uv_scroll(b.uv(), speed=(0.03, 0.05), tiling=(6.0, 6.0)),
                    6.0, "waveScale")
    parts = b.split3(b.normal())
    offset = b.mul(b.sub(waves, 0.5), b.float_(0.35, "waveStrength"))
    ripple = b.node("Combine")
    b.wire(b.add(parts[0], offset), ripple.id, 0)
    b.wire(parts[1], ripple.id, 1)
    b.wire(b.add(parts[2], offset), ripple.id, 2)
    ripple = b.normalize(ripple)                      # a bent normal is no longer unit length
    fres = b.fresnel(3.5, "fresnelPower", ripple)
    depth = b.clamp(b.remap(b.split3(b.world())[1], b.float_(-2.0, "deepHeight"),
                            b.float_(1.0, "shallowHeight"), 0.0, 1.0), 0.0, 1.0)
    color = b.gradient(b.color((0.02, 0.10, 0.18), "deepColor"),
                       b.color((0.10, 0.35, 0.45), "midColor"),
                       b.color((0.45, 0.75, 0.80), "shallowColor"), depth)
    spec = b.specular(ripple, color=(1.0, 1.0, 1.0), color_ref="specColor", threshold=0.85,
                      threshold_ref="specThreshold", softness=0.08, softness_ref="specSoftness",
                      strength=0.80, strength_ref="specStrength")
    rim = b.scale(b.color((0.80, 0.95, 1.00), "rimColor"), b.mul(fres, b.float_(0.50, "rimStrength")))
    color = b.add(b.mul(color, b.light_flat(sun=1.0, ambient=0.70)), b.add(spec, rim))
    alpha = b.lerp(b.float_(0.55, "deepAlpha"), b.float_(0.95, "edgeAlpha"), fres)
    b.master("UnlitMaster", color=color, alpha=alpha)


# --------------------------------------------------------------------------- registry
FAMILIES = [
    ("Cel", "Cel core", "How many tones there are, where the terminator sits, and how hard it is."),
    ("Face", "Face", "Faces are drawn, not lit: an SDF map or a head-angle terminator, no specular."),
    ("Eye", "Eye", "Eyes are jewellery: matcap sheen, hard catchlight, dark limbal ring."),
    ("Hair", "Hair", "The anisotropic ring is the whole job; the rest is which tones sit under it."),
    ("Skin", "Skin", "Wide soft bands, warm shadows, translucency at the backlight."),
    ("Cloth", "Cloth", "Matte, folded, shaded by vertex-colour AO where the artist painted it."),
    ("Ink", "Ink and line", "Traditional media: wash, screentone, hatching, contour lines, brush grain."),
    ("Paint", "Painterly", "Wet edges, opaque poster colour, canvas tooth, chalk grain."),
    ("Retro", "Retro broadcast", "Limited palettes, ordered dither, scanlines and grain."),
    ("Game", "Modern game looks", "What shipped anime games do: wrapped cel bodies, NPR metal, painted fighters."),
    ("Film", "Studio film looks", "Lighting departments: golden hour, pastoral pastel, high key, pop action."),
    ("Neon", "Neon and stage", "Emissive looks that live in the bloom pass."),
    ("World", "Anime world", "Environment side: painted skies, cel foliage, stylised water."),
]

STYLES = [
    ("Cel", "TwoTone", cel_two_tone),
    ("Cel", "ThreeTone", cel_three_tone),
    ("Cel", "FourToneSoft", cel_four_tone_soft),
    ("Cel", "HardFlat", cel_hard_flat),
    ("Cel", "CoolShadow", cel_cool_shadow),
    ("Cel", "WarmBounce", cel_warm_bounce),
    ("Cel", "WrappedSoft", cel_wrapped_soft),
    ("Face", "SDFShadow", face_sdf),
    ("Face", "FlatTerminator", face_flat_terminator),
    ("Face", "SoftGradient", face_soft_gradient),
    ("Face", "BlushCheeks", face_blush),
    ("Eye", "MatcapIris", eye_matcap),
    ("Eye", "GlossHighlight", eye_gloss),
    ("Hair", "AngelRing", hair_angel_ring),
    ("Hair", "TwoLobeGloss", hair_two_lobe),
    ("Hair", "RootToTip", hair_root_to_tip),
    ("Hair", "MatcapSilk", hair_matcap_silk),
    ("Skin", "SoftPeach", skin_soft_peach),
    ("Skin", "SubsurfaceRim", skin_subsurface_rim),
    ("Cloth", "MatteUniform", cloth_matte_uniform),
    ("Cloth", "SatinSheen", cloth_satin_sheen),
    ("Ink", "SumieWash", ink_sumie),
    ("Ink", "MangaHalftone", ink_manga_halftone),
    ("Ink", "SketchHatch", ink_sketch_hatch),
    ("Ink", "ContourLine", ink_contour_line),
    ("Ink", "BrushGrain", ink_brush_grain),
    ("Paint", "Watercolor", paint_watercolor),
    ("Paint", "GouacheFlat", paint_gouache),
    ("Paint", "OilImpasto", paint_oil_impasto),
    ("Paint", "PastelChalk", paint_pastel_chalk),
    ("Paint", "PosterPrint", paint_poster_print),
    ("Retro", "Pixel8Bit", retro_pixel_8bit),
    ("Retro", "TV90sDither", retro_tv_90s),
    ("Retro", "GameboyMono", retro_gameboy_mono),
    ("Retro", "VHSGrain", retro_vhs),
    ("Retro", "OrderedDitherCel", retro_dither_cel),
    ("Game", "GenshinBody", game_genshin_body),
    ("Game", "MetalTrim", game_metal_trim),
    ("Game", "GuiltyGearPaint", game_guilty_gear),
    ("Game", "BorderlandsInk", game_borderlands_ink),
    ("Game", "SpiderVersePop", game_spiderverse),
    ("Film", "ShinkaiGlow", film_shinkai_glow),
    ("Film", "GhibliPastel", film_ghibli_pastel),
    ("Film", "KyoAniSoft", film_kyoani_soft),
    ("Film", "TriggerPop", film_trigger_pop),
    ("Neon", "CyberCel", neon_cyber_cel),
    ("Neon", "IdolStage", neon_idol_stage),
    ("World", "SkyGradient", world_sky_gradient),
    ("World", "FoliageCel", world_foliage_cel),
    ("World", "WaterSheen", world_water_sheen),
]

STYLE_COUNT = 50


class Style:
    """One starter: a family, a name, and the recipe that builds its graph.

    The look and the detail both come from the recipe's docstring, so the documentation cannot drift
    away from the code that generates the shader.
    """

    def __init__(self, family, name, build):
        self.family = family
        self.name = name
        self.build = build
        doc = one_line(build.__doc__)
        self.look = first_sentence(doc)
        self.detail = doc
        self.master = ""
        self.compiled = None
        self.node_count = 0
        self.graph = None

    @property
    def file_name(self):
        return f"Anime{self.family}_{self.name}"

    @property
    def properties(self):
        return self.compiled["properties"]

    @property
    def textures(self):
        return self.compiled["textures"]


def one_line(text):
    """Docstring to one line: the recipes wrap their prose, markdown tables and lists cannot."""
    return " ".join((text or "").split())


def first_sentence(text):
    text = one_line(text).rstrip(".")
    cut = text.find(". ")
    if cut > 0:
        text = text[:cut]
    return text + "."


def property_line(style):
    """`name` kind (defaults), in slot order - what the material inspector shows for this starter."""
    parts = []
    for prop in style.properties:
        defaults = ", ".join(prop["defaults"])
        parts.append(f"`{prop['name']}` {prop['kind']} ({defaults})" if prop["defaults"]
                     else f"`{prop['name']}` {prop['kind']}")
    return "; ".join(parts)


def facts_line(style):
    facts = [f"{style.master} master", f"{style.node_count} nodes",
             "outline pass" if style.compiled.get("outline") else "no outline",
             "alpha blended" if style.compiled["blend"] == "alpha" else "opaque"]
    if style.textures:
        facts.append("maps: " + ", ".join(f"`{t['name']}`" for t in style.textures))
    return ", ".join(facts) + "."


def build_all():
    """Builds and compiles every style. Raises on a duplicate name, a generator error or a graph the
    code generator rejects - the same errors the editor would report on load."""
    styles = [Style(family, name, build) for family, name, build in STYLES]
    seen = set()
    for style in styles:
        if style.file_name in seen:
            raise SystemExit(f"{style.file_name} is defined twice")
        seen.add(style.file_name)
        builder = sg_build.Builder()
        style.build(builder)
        graph = builder.graph()
        style.node_count = len(graph["nodes"])
        document = sg_mirror.shader_document(graph)
        errors = document["compiled"]["errors"]
        if errors:
            raise SystemExit(f"{style.file_name}: " + "; ".join(errors))
        style.compiled = document["compiled"]
        style.graph = graph
        masters = [n["internal-data"]["model-name"] for n in graph["nodes"]
                   if n["internal-data"]["model-name"].endswith("Master")]
        if len(masters) != 1:
            raise SystemExit(f"{style.file_name}: expected one master node, found {masters}")
        style.master = masters[0][:-6]
        if not style.properties:
            raise SystemExit(f"{style.file_name}: exposes no material properties - a starter has to be "
                             "tunable without editing the graph")
    return styles


def write_graphs(styles):
    os.makedirs(GRAPH_DIR, exist_ok=True)
    for style in styles:
        path = os.path.join(GRAPH_DIR, style.file_name + ".json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"graph": style.graph}, f, indent=1)
    return GRAPH_DIR


def write_doc(styles):
    """research/anime_style_library.md: an index to scan, then one entry per style with the properties
    it exposes and their defaults."""
    by_family = {}
    for style in styles:
        by_family.setdefault(style.family, []).append(style)
    nodes_used = sorted({n["internal-data"]["model-name"] for s in styles for n in s.graph["nodes"]})
    unused = [spec.id for spec in sg_mirror.NODES if spec.id not in nodes_used]

    out = []
    out.append("# Anime style library\n")
    out.append(f"{len(styles)} starter shader graphs, every one built from the shader graph's own node "
               "library - no hand-written GLSL. The point is variety: an anime game is not one look. A "
               "face wants an SDF shadow that never breaks under the nose, hair wants an anisotropic "
               "ring, a uniform wants matte cloth with painted AO, a flashback wants 90s TV dither, a "
               "boss wants hard ink and no ambient at all.\n")
    out.append("Each style is a finished look *and* a starting point. Everything its look depends on is "
               "exposed as a material property with the default that makes the reference image, so a "
               "fine-tuned variant needs no graph editing: assign the shader, move the sliders.\n")
    out.append("Sources live in `tools/shader_graphs/Anime*.json`. "
               "`python3 tools/compile_shader_graphs.py compile tools/shader_graphs assets` - or "
               "`ukiyo_shader_tool compile tools/shader_graphs assets` with Qt - writes "
               "`assets/Shaders/Anime*.ukshader`, which the material inspector lists as soon as it "
               "exists. Both files are generated by `tools/gen_anime_styles.py`: edit the recipe, not "
               "the JSON or the shader.\n")

    out.append("## Using a starter\n")
    out.append("1. Assign `assets/Shaders/Anime<Family>_<Style>.ukshader` to a material, or open it in "
               "the shader graph editor to see how the look is wired.\n"
               "2. Set properties in the material inspector. Unassigned texture slots sample white, so "
               "every style is usable before any art exists.\n"
               "3. Duplicate the shader when a property is not enough, then rewire: the recipes are "
               "small on purpose (10-50 nodes) so a change stays readable.\n")

    out.append("## Conventions every recipe follows\n")
    out.append("* **Colour** comes from `albedo()`: a texture times a tint, so recolours and untextured "
               "props work with the same shader.\n"
               "* **Light** comes from the scene: `Cel Bands` over N.L, then `Sun Color` and `Ambient "
               "Color` (hemisphere). A style that ignored the scene's lights would not sit in a level "
               "next to one that respects them.\n"
               "* **Masters** are chosen for what they give back: `Toon` for the engine's own two-tone "
               "branch, `Unlit` when the style computes its whole look itself, `PBR` when it should "
               "still catch real specular and sky light.\n"
               "* **Outlines** are the three exposed values `outlineColor`, `outlineWidth`, "
               "`outlineAlpha`, which ask the engine for the inverted-hull pass. Character styles "
               "expose them; ink, world and retro styles mostly do not.\n"
               "* **Glow** means additive: into `Emission` on a lit master, straight into `Color` on "
               "`Unlit`. Rim lights, specular shapes and light sweeps are all built that way.\n")

    out.append("## Index\n")
    out.append("| # | Shader | Family | Master | Outline | Look |")
    out.append("| --- | --- | --- | --- | --- | --- |")
    for i, style in enumerate(styles, start=1):
        out.append(f"| {i} | `{style.file_name}` | {style.family} | {style.master} | "
                   f"{'yes' if style.compiled.get('outline') else 'no'} | {style.look} |")
    out.append("")

    out.append("## The styles\n")
    for family, title, blurb in FAMILIES:
        group = by_family.get(family, [])
        out.append(f"### {title} ({len(group)})\n")
        out.append(blurb + "\n")
        for style in group:
            out.append(f"#### `{style.file_name}`\n")
            out.append(f"*{facts_line(style)}*\n")
            out.append(style.detail + "\n")
            out.append(f"Properties: {property_line(style)}\n")
    out.append("## Node coverage\n")
    out.append(f"The {len(styles)} styles exercise {len(nodes_used)} of the {len(sg_mirror.NODES)} node "
               "types in `editor/src/shader_graph/sg_nodes.cpp`"
               + (f". They do not use {', '.join(f'`{n}`' for n in unused)}" if unused else "") + ".\n")
    out.append("That gap is deliberate. `Complex Lit` and `Baked Lit` are for clear-coat paint and "
               "lightmapped statics, which the engine's own built-in shaders already cover; `Head "
               "Forward` is for hand-authored face rigs (`assets/Shaders/NahidaFace.ukshader` reads the "
               "same frame field); `Subtract V3` is a convenience next to `Add V3` with a negated "
               "operand. All four are still compiled and type checked by `tools/sg_coverage_check.py`, "
               "which builds one graph per node.\n")
    out.append("Checks that keep the library honest:\n")
    out.append("* `python3 tools/gen_anime_styles.py` - refuses to write a graph the code generator "
               "rejects, a style with no master, or a style that exposes nothing to tune.\n"
               "* `Builder.wire()` in `tools/sg_build.py` - every connection is kind checked against the "
               "node table, the same check QtNodes makes in the editor, so a recipe cannot wire a float "
               "into a colour input.\n"
               "* `python3 tools/compile_shader_graphs.py verify assets` - every shipped `.ukshader` is "
               "byte identical to a fresh compile of its own graph.\n"
               "* `python3 tools/sg_glsl_check.py` - type checks the generated GLSL of every shipped "
               "shader.\n"
               "* `python3 tools/sg_coverage_check.py` - compiles one graph per node type and reports "
               "which ones no shipped shader uses.\n")
    os.makedirs(os.path.dirname(DOC_PATH), exist_ok=True)
    with open(DOC_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    return DOC_PATH


def main(argv):
    if len(STYLES) != STYLE_COUNT:
        raise SystemExit(f"expected {STYLE_COUNT} styles, found {len(STYLES)}")
    styles = build_all()
    graph_dir = write_graphs(styles)
    doc = write_doc(styles)
    outlined = sum(1 for s in styles if s.compiled.get("outline"))
    transparent = sum(1 for s in styles if s.compiled["blend"] == "alpha")
    print(f"wrote {len(styles)} anime styles to {graph_dir}")
    print(f"  masters: " + ", ".join(f"{m}={sum(1 for s in styles if s.master == m)}"
                                     for m in sorted({s.master for s in styles})))
    print(f"  {outlined} ask for the outline pass, {transparent} are alpha blended, "
          f"average {sum(s.node_count for s in styles) / len(styles):.1f} nodes")
    print(f"style doc: {doc}")
    print(f"next: python3 tools/compile_shader_graphs.py compile tools/shader_graphs assets")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
