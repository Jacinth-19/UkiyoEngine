#!/usr/bin/env python3
"""A Python mirror of the editor's shader-graph code generator.

Mirrors, line for line where it matters:
  editor/src/shader_graph/sg_nodes.cpp    -> NODES (the node library)
  editor/src/shader_graph/sg_codegen.cpp  -> generate(), shader_document()

Why a mirror exists: the C++ generator lives in ukiyo_shader_tool, which needs Qt and QtNodes. This
module produces byte-identical .ukshader documents without them, so the starter shader library can be
regenerated anywhere Python runs. `tools/compile_shader_graphs.py verify` re-compiles every shipped
graph and fails on any drift, and `tools/sg_node_sync_test.py` diffs NODES against sg_nodes.cpp, so
the two tables cannot quietly diverge.

The document format is Qt's QJsonDocument::toJson(Indented): four-space indent, keys in ascending
order, and an empty array written as "[\\n]" at the key's own indent. json.dumps does not do this, so
the writer here is hand rolled.
"""

# ---------------------------------------------------------------- node library

FLOAT = "float"
VEC2 = "vec2"
VEC3 = "vec3"


class Port:
    """One input or output port. `default` is the GLSL used when an input is not connected."""

    __slots__ = ("name", "kind", "default")

    def __init__(self, name, kind, default=""):
        self.name = name
        self.kind = kind
        self.default = default

    def __repr__(self):
        return f"Port({self.name!r}, {self.kind!r}, {self.default!r})"


class NodeSpec:
    """A node type. Fields match the C++ NodeSpec of the same name."""

    __slots__ = ("id", "category", "caption", "inputs", "outputs", "param", "out_expr", "decl",
                 "helper", "master", "surface_fields")

    def __init__(self, id, category, caption, inputs=(), outputs=(), param="none", out_expr=(),
                 decl="", helper="", master=False, surface_fields=()):
        self.id = id
        self.category = category
        self.caption = caption
        self.inputs = list(inputs)
        self.outputs = list(outputs)
        self.param = param            # "none", "float", "vec2", "color", "texture"
        self.out_expr = list(out_expr)
        self.decl = decl
        self.helper = helper
        self.master = master
        self.surface_fields = list(surface_fields)

    def __repr__(self):
        return f"NodeSpec({self.id!r})"


def _p(name, kind, default=""):
    return Port(name, kind, default)


def build_nodes():
    """The node library, in the same order as sg_nodes.cpp's buildSpecs()."""
    n = []

    # ---- Inputs ----
    n.append(NodeSpec("Float", "Input", "Float", outputs=[_p("Out", FLOAT)], param="float",
                      out_expr=["%P"]))
    n.append(NodeSpec("Vector2", "Input", "Vector 2", outputs=[_p("Out", VEC2)], param="vec2",
                      out_expr=["%P"]))
    n.append(NodeSpec("Color", "Input", "Color", outputs=[_p("Out", VEC3)], param="color",
                      out_expr=["%P"]))
    n.append(NodeSpec("UV", "Input", "UV", outputs=[_p("Out", VEC2)], out_expr=["uv"]))
    n.append(NodeSpec("UV1", "Input", "UV (Channel 1)", outputs=[_p("Out", VEC2)], out_expr=["v_uv1"]))
    n.append(NodeSpec("Time", "Input", "Time", outputs=[_p("Out", FLOAT)],
                      out_expr=["frame.screenParams.z"]))
    n.append(NodeSpec("Normal", "Input", "Normal (World)", outputs=[_p("Out", VEC3)],
                      out_expr=["normalize(normal)"]))
    n.append(NodeSpec("WorldPosition", "Input", "Position (World)", outputs=[_p("Out", VEC3)],
                      out_expr=["v_world"]))
    n.append(NodeSpec("ViewDirection", "Input", "View Direction", outputs=[_p("Out", VEC3)],
                      out_expr=["normalize(frame.camPos.xyz - v_world)"]))
    n.append(NodeSpec("SunDirection", "Input", "Sun Direction", outputs=[_p("Out", VEC3)],
                      out_expr=["normalize(frame.sunDir.xyz)"]))
    n.append(NodeSpec("SunColor", "Input", "Sun Color", outputs=[_p("Out", VEC3)],
                      out_expr=["frame.sunColor.rgb"]))
    n.append(NodeSpec("AmbientColor", "Input", "Ambient (Hemisphere)", outputs=[_p("Out", VEC3)],
                      out_expr=["mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * normalize(normal).y)"]))
    n.append(NodeSpec("ScreenPosition", "Input", "Screen Position", outputs=[_p("Out", VEC2)],
                      out_expr=["gl_FragCoord.xy / max(frame.screenParams.xy, vec2(1.0))"]))
    n.append(NodeSpec("VertexColor", "Input", "Vertex Color",
                      outputs=[_p("RGB", VEC3), _p("A", FLOAT)],
                      out_expr=["v_color.rgb", "v_color.a"]))
    n.append(NodeSpec("HeadForward", "Input", "Head Forward (Face)", outputs=[_p("Out", VEC2)],
                      out_expr=["normalize(frame.toonD.xy + vec2(1e-5, 0.0))"]))

    # ---- Float math ----
    n.append(NodeSpec("AddF", "Math", "Add",
                      inputs=[_p("A", FLOAT, "0.0"), _p("B", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["%0 + %1"]))
    n.append(NodeSpec("MultiplyF", "Math", "Multiply",
                      inputs=[_p("A", FLOAT, "1.0"), _p("B", FLOAT, "1.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["%0 * %1"]))
    n.append(NodeSpec("PowerF", "Math", "Power",
                      inputs=[_p("Base", FLOAT, "1.0"), _p("Exponent", FLOAT, "1.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["pow(%0, %1)"]))
    n.append(NodeSpec("OneMinusF", "Math", "One Minus", inputs=[_p("In", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["1.0 - %0"]))
    n.append(NodeSpec("SaturateF", "Math", "Saturate", inputs=[_p("In", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["clamp(%0, 0.0, 1.0)"]))
    n.append(NodeSpec("SmoothstepF", "Math", "Smoothstep",
                      inputs=[_p("Edge0", FLOAT, "0.0"), _p("Edge1", FLOAT, "1.0"), _p("X", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["smoothstep(%0, %1, %2)"]))
    n.append(NodeSpec("LerpF", "Math", "Lerp",
                      inputs=[_p("A", FLOAT, "0.0"), _p("B", FLOAT, "1.0"), _p("T", FLOAT, "0.5")],
                      outputs=[_p("Out", FLOAT)], out_expr=["mix(%0, %1, %2)"]))
    n.append(NodeSpec("SubtractF", "Math", "Subtract",
                      inputs=[_p("A", FLOAT, "0.0"), _p("B", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["%0 - %1"]))
    n.append(NodeSpec("AbsF", "Math", "Absolute",
                      inputs=[_p("In", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["abs(%0)"]))
    n.append(NodeSpec("StepF", "Math", "Step",
                      inputs=[_p("Edge", FLOAT, "0.5"), _p("X", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["step(%0, %1)"]))
    n.append(NodeSpec("ClampF", "Math", "Clamp",
                      inputs=[_p("Min", FLOAT, "0.0"), _p("Max", FLOAT, "1.0"), _p("X", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["clamp(%2, %0, %1)"]))
    n.append(NodeSpec("Remap", "Math", "Remap",
                      inputs=[_p("In", FLOAT, "0.0"), _p("In Min", FLOAT, "0.0"), _p("In Max", FLOAT, "1.0"),
                              _p("Out Min", FLOAT, "0.0"), _p("Out Max", FLOAT, "1.0")],
                      outputs=[_p("Out", FLOAT)],
                      out_expr=["mix(%3, %4, clamp((%0 - %1) / max(abs(%2 - %1), 1e-5), 0.0, 1.0))"]))
    n.append(NodeSpec("SineF", "Math", "Sine", inputs=[_p("In", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["sin(%0)"]))
    n.append(NodeSpec("FractF", "Math", "Fract", inputs=[_p("In", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["fract(%0)"]))
    n.append(NodeSpec("Posterize", "Math", "Posterize",
                      inputs=[_p("In", FLOAT, "0.0"), _p("Steps", FLOAT, "4.0")],
                      outputs=[_p("Out", FLOAT)],
                      out_expr=["floor(clamp(%0, 0.0, 1.0) * max(%1, 1.0) + 0.5) / max(%1, 1.0)"]))

    # ---- Vector math ----
    n.append(NodeSpec("AddV3", "Vector", "Add (Vector 3)",
                      inputs=[_p("A", VEC3, "vec3(0.0)"), _p("B", VEC3, "vec3(0.0)")],
                      outputs=[_p("Out", VEC3)], out_expr=["%0 + %1"]))
    n.append(NodeSpec("MultiplyV3", "Vector", "Multiply (Vector 3)",
                      inputs=[_p("A", VEC3, "vec3(1.0)"), _p("B", VEC3, "vec3(1.0)")],
                      outputs=[_p("Out", VEC3)], out_expr=["%0 * %1"]))
    n.append(NodeSpec("LerpV3", "Vector", "Lerp (Vector 3)",
                      inputs=[_p("A", VEC3, "vec3(0.0)"), _p("B", VEC3, "vec3(1.0)"), _p("T", FLOAT, "0.5")],
                      outputs=[_p("Out", VEC3)], out_expr=["mix(%0, %1, %2)"]))
    n.append(NodeSpec("Combine", "Vector", "Combine RGB",
                      inputs=[_p("R", FLOAT, "0.0"), _p("G", FLOAT, "0.0"), _p("B", FLOAT, "0.0")],
                      outputs=[_p("RGB", VEC3)], out_expr=["vec3(%0, %1, %2)"]))
    n.append(NodeSpec("Split", "Vector", "Split RGB",
                      inputs=[_p("RGB", VEC3, "vec3(0.0)")],
                      outputs=[_p("R", FLOAT), _p("G", FLOAT), _p("B", FLOAT)],
                      out_expr=["%0.x", "%0.y", "%0.z"]))
    n.append(NodeSpec("SubtractV3", "Vector", "Subtract (Vector 3)",
                      inputs=[_p("A", VEC3, "vec3(0.0)"), _p("B", VEC3, "vec3(0.0)")],
                      outputs=[_p("Out", VEC3)], out_expr=["%0 - %1"]))
    n.append(NodeSpec("DotV3", "Vector", "Dot Product (Vector 3)",
                      inputs=[_p("A", VEC3, "vec3(0.0)"), _p("B", VEC3, "vec3(0.0)")],
                      outputs=[_p("Out", FLOAT)], out_expr=["dot(%0, %1)"]))
    n.append(NodeSpec("ScaleV3", "Vector", "Scale (Vector 3 by Float)",
                      inputs=[_p("A", VEC3, "vec3(1.0)"), _p("B", FLOAT, "1.0")],
                      outputs=[_p("Out", VEC3)], out_expr=["%0 * %1"]))
    n.append(NodeSpec("NormalizeV3", "Vector", "Normalize (Vector 3)",
                      inputs=[_p("In", VEC3, "vec3(0.0, 0.0, 1.0)")],
                      outputs=[_p("Out", VEC3)], out_expr=["normalize(%0)"]))
    n.append(NodeSpec("SaturateV3", "Vector", "Saturate (Vector 3)",
                      inputs=[_p("In", VEC3, "vec3(0.0)")],
                      outputs=[_p("Out", VEC3)], out_expr=["clamp(%0, vec3(0.0), vec3(1.0))"]))
    n.append(NodeSpec("Combine2", "Vector", "Combine XY",
                      inputs=[_p("X", FLOAT, "0.0"), _p("Y", FLOAT, "0.0")],
                      outputs=[_p("Out", VEC2)], out_expr=["vec2(%0, %1)"]))
    n.append(NodeSpec("Split2", "Vector", "Split XY",
                      inputs=[_p("XY", VEC2, "vec2(0.0)")],
                      outputs=[_p("X", FLOAT), _p("Y", FLOAT)],
                      out_expr=["%0.x", "%0.y"]))
    n.append(NodeSpec("Luminance", "Vector", "Luminance (Vector 3)",
                      inputs=[_p("In", VEC3, "vec3(0.0)")],
                      outputs=[_p("Out", FLOAT)],
                      out_expr=["dot(%0, vec3(0.2126, 0.7152, 0.0722))"]))
    n.append(NodeSpec("Desaturate", "Vector", "Desaturate (Vector 3)",
                      inputs=[_p("In", VEC3, "vec3(0.0)"), _p("Saturation", FLOAT, "1.0")],
                      outputs=[_p("Out", VEC3)],
                      out_expr=["mix(vec3(dot(%0, vec3(0.2126, 0.7152, 0.0722))), %0, %1)"]))
    n.append(NodeSpec("HueRotate", "Vector", "Hue Rotate (Vector 3)",
                      inputs=[_p("In", VEC3, "vec3(1.0)"), _p("Degrees", FLOAT, "0.0")],
                      outputs=[_p("Out", VEC3)], out_expr=["ukiyo_hue_rotate(%0, %1)"],
                      helper=(
                          "vec3 ukiyo_hue_rotate(vec3 c, float degrees) {\n"
                          "    float a = radians(degrees);\n"
                          "    float co = cos(a);\n"
                          "    float si = sin(a);\n"
                          "    vec3 k = vec3(0.57735027);   // the grey axis, normalised\n"
                          "    // Rodrigues' rotation around the grey axis: luma is kept, the hue turns.\n"
                          "    return c * co + cross(k, c) * si + k * dot(k, c) * (1.0 - co);\n"
                          "}")))

    # ---- Texture ----
    n.append(NodeSpec("SampleTexture2D", "Texture", "Sample Texture 2D", param="texture",
                      inputs=[_p("UV", VEC2, "uv")],
                      outputs=[_p("RGB", VEC3), _p("A", FLOAT)],
                      out_expr=["texture(tex%I, %0).rgb", "texture(tex%I, %0).a"],
                      decl="layout(set = 1, binding = %B) uniform sampler2D tex%I;"))

    # Normal map: tangent frame from screen-space derivatives, so it needs no vertex tangents.
    n.append(NodeSpec("NormalMap", "Texture", "Normal Map",
                      inputs=[_p("Map", VEC3, "vec3(0.5, 0.5, 1.0)"), _p("Strength", FLOAT, "1.0")],
                      outputs=[_p("Normal", VEC3)],
                      out_expr=["ukiyo_normal_map(%0, %1, normalize(normal), uv, v_world)"],
                      helper=(
                          "vec3 ukiyo_normal_map(vec3 mapSample, float strength, vec3 N, vec2 uv, vec3 p) {\n"
                          "    vec3 dp1 = dFdx(p);\n"
                          "    vec3 dp2 = dFdy(p);\n"
                          "    vec2 duv1 = dFdx(uv);\n"
                          "    vec2 duv2 = dFdy(uv);\n"
                          "    vec3 dp2perp = cross(dp2, N);\n"
                          "    vec3 dp1perp = cross(N, dp1);\n"
                          "    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;\n"
                          "    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;\n"
                          "    float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));\n"
                          "    mat3 TBN = mat3(T * invmax, B * invmax, N);\n"
                          "    vec3 m = mix(vec3(0.0, 0.0, 1.0), normalize(mapSample * 2.0 - 1.0), strength);\n"
                          "    return normalize(TBN * m);\n"
                          "}")))

    # ---- Procedural ----
    n.append(NodeSpec("WhiteNoise", "Procedural", "White Noise",
                      inputs=[_p("UV", VEC2, "uv"), _p("Scale", FLOAT, "64.0")],
                      outputs=[_p("Out", FLOAT)],
                      out_expr=["fract(sin(dot(%0 * %1, vec2(12.9898, 78.233))) * 43758.5453)"]))
    n.append(NodeSpec("ValueNoise", "Procedural", "Value Noise",
                      inputs=[_p("UV", VEC2, "uv"), _p("Scale", FLOAT, "8.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["ukiyo_value_noise(%0 * %1)"],
                      helper=(
                          "float ukiyo_value_noise(vec2 p) {\n"
                          "    vec2 i = floor(p);\n"
                          "    vec2 f = fract(p);\n"
                          "    vec2 u = f * f * (3.0 - 2.0 * f);\n"
                          "    float a = fract(sin(dot(i, vec2(12.9898, 78.233))) * 43758.5453);\n"
                          "    float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(12.9898, 78.233))) * 43758.5453);\n"
                          "    float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(12.9898, 78.233))) * 43758.5453);\n"
                          "    float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(12.9898, 78.233))) * 43758.5453);\n"
                          "    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);\n"
                          "}")))
    n.append(NodeSpec("BayerDither", "Procedural", "Ordered Dither (Bayer 4x4)",
                      inputs=[_p("Pixel Size", FLOAT, "1.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["ukiyo_bayer4(%0)"],
                      helper=(
                          "float ukiyo_bayer4(float pixelSize) {\n"
                          "    const int bayer[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);\n"
                          "    vec2 p = floor(gl_FragCoord.xy / max(pixelSize, 1.0));\n"
                          "    int i = int(mod(p.y, 4.0)) * 4 + int(mod(p.x, 4.0));\n"
                          "    return (float(bayer[i]) + 0.5) / 16.0;\n"
                          "}")))
    n.append(NodeSpec("Halftone", "Procedural", "Halftone Dots",
                      inputs=[_p("UV", VEC2, "uv"), _p("Frequency", FLOAT, "24.0"),
                              _p("Angle", FLOAT, "45.0"), _p("Value", FLOAT, "0.5")],
                      outputs=[_p("Out", FLOAT)], out_expr=["ukiyo_halftone(%0, %1, %2, %3)"],
                      helper=(
                          "float ukiyo_halftone(vec2 uv, float frequency, float angleDegrees, float value) {\n"
                          "    float a = radians(angleDegrees);\n"
                          "    mat2 rot = mat2(cos(a), -sin(a), sin(a), cos(a));\n"
                          "    vec2 p = rot * (uv * max(frequency, 1.0));\n"
                          "    float d = length(fract(p) - 0.5);\n"
                          "    float w = fwidth(d) + 1e-5;\n"
                          "    float r = sqrt(clamp(value, 0.0, 1.0)) * 0.7071;\n"
                          "    return 1.0 - smoothstep(r - w, r + w, d);\n"
                          "}")))
    n.append(NodeSpec("HatchLines", "Procedural", "Hatch Lines",
                      inputs=[_p("UV", VEC2, "uv"), _p("Frequency", FLOAT, "40.0"),
                              _p("Angle", FLOAT, "45.0"), _p("Value", FLOAT, "0.35")],
                      outputs=[_p("Out", FLOAT)], out_expr=["ukiyo_hatch(%0, %1, %2, %3)"],
                      helper=(
                          "float ukiyo_hatch(vec2 uv, float frequency, float angleDegrees, float value) {\n"
                          "    float a = radians(angleDegrees);\n"
                          "    float t = (uv.x * cos(a) - uv.y * sin(a)) * max(frequency, 1.0);\n"
                          "    float f = abs(fract(t) - 0.5) * 2.0;\n"
                          "    float w = clamp(value, 0.0, 1.0);\n"
                          "    float aa = fwidth(t) + 1e-4;\n"
                          "    return 1.0 - smoothstep(w - aa, w + aa, f);\n"
                          "}")))
    n.append(NodeSpec("Scanlines", "Procedural", "Scanlines",
                      inputs=[_p("Line Height", FLOAT, "3.0"), _p("Speed", FLOAT, "0.0")],
                      outputs=[_p("Out", FLOAT)], out_expr=["ukiyo_scanlines(%0, %1)"],
                      helper=(
                          "float ukiyo_scanlines(float lineHeight, float speed) {\n"
                          "    float y = gl_FragCoord.y / max(lineHeight, 1.0) + frame.screenParams.z * speed;\n"
                          "    return 0.5 + 0.5 * cos(y * 6.2831853);\n"
                          "}")))
    n.append(NodeSpec("PixelateUV", "Procedural", "Pixelate UV",
                      inputs=[_p("UV", VEC2, "uv"), _p("Blocks", FLOAT, "64.0")],
                      outputs=[_p("Out", VEC2)],
                      out_expr=["(floor(%0 * max(%1, 1.0)) + 0.5) / max(%1, 1.0)"]))
    n.append(NodeSpec("TilingAndOffset", "Procedural", "Tiling And Offset (UV)",
                      inputs=[_p("UV", VEC2, "uv"), _p("Tiling", VEC2, "vec2(1.0)"),
                              _p("Offset", VEC2, "vec2(0.0)")],
                      outputs=[_p("Out", VEC2)], out_expr=["%0 * %1 + %2"]))

    # ---- Stylize ----
    n.append(NodeSpec("Fresnel", "Stylize", "Fresnel (Rim Mask)",
                      inputs=[_p("Normal", VEC3, "normal"), _p("Power", FLOAT, "4.0")],
                      outputs=[_p("Out", FLOAT)],
                      out_expr=["pow(1.0 - clamp(dot(normalize(%0), normalize(frame.camPos.xyz - v_world)),"
                                " 0.0, 1.0), max(%1, 0.0001))"]))
    n.append(NodeSpec("CelBands", "Stylize", "Cel Bands (Ramp)",
                      inputs=[_p("Light", FLOAT, "0.5"), _p("Bands", FLOAT, "2.0"), _p("Softness", FLOAT, "0.1")],
                      outputs=[_p("Out", FLOAT)], out_expr=["ukiyo_cel_bands(%0, %1, %2)"],
                      helper=(
                          "float ukiyo_cel_bands(float light, float bands, float softness) {\n"
                          "    float n = max(bands, 2.0);\n"
                          "    float t = clamp(light, 0.0, 1.0) * (n - 1.0);\n"
                          "    float i = floor(t);\n"
                          "    float f = t - i;\n"
                          "    float s = clamp(softness, 0.0, 1.0);\n"
                          "    float e = s > 0.0001 ? smoothstep(0.5 - 0.5 * s, 0.5 + 0.5 * s, f) : step(0.5, f);\n"
                          "    return min(i + e, n - 1.0) / (n - 1.0);\n"
                          "}")))
    n.append(NodeSpec("SpecBand", "Stylize", "Specular Band",
                      inputs=[_p("Normal", VEC3, "normal"), _p("Threshold", FLOAT, "0.9"),
                              _p("Softness", FLOAT, "0.02")],
                      outputs=[_p("Out", FLOAT)],
                      out_expr=["ukiyo_spec_band(normalize(%0), %1, %2)"],
                      helper=(
                          "float ukiyo_spec_band(vec3 N, float threshold, float softness) {\n"
                          "    vec3 V = normalize(frame.camPos.xyz - v_world);\n"
                          "    vec3 H = normalize(normalize(frame.sunDir.xyz) + V);\n"
                          "    float nh = max(dot(N, H), 0.0);\n"
                          "    float s = max(softness, 1e-4);\n"
                          "    return smoothstep(threshold - s, threshold + s, nh);\n"
                          "}")))
    n.append(NodeSpec("HairSpecular", "Stylize", "Hair Specular (Anisotropic)",
                      inputs=[_p("Normal", VEC3, "normal"), _p("Shift", FLOAT, "0.3"),
                              _p("Exponent", FLOAT, "40.0"), _p("Strength", FLOAT, "1.0")],
                      outputs=[_p("Out", FLOAT)],
                      out_expr=["ukiyo_hair_spec(normalize(%0), %1, %2, %3)"],
                      helper=(
                          "float ukiyo_hair_spec(vec3 N, float shift, float exponent, float strength) {\n"
                          "    vec3 V = normalize(frame.camPos.xyz - v_world);\n"
                          "    vec3 H = normalize(normalize(frame.sunDir.xyz) + V);\n"
                          "    vec3 up = vec3(0.0, 1.0, 0.0);\n"
                          "    vec3 t1 = normalize(N + shift * up);\n"
                          "    vec3 t2 = normalize(N - shift * 0.5 * up);\n"
                          "    float e = max(exponent, 1.0);\n"
                          "    float s1 = pow(max(dot(t1, H), 0.0), e);\n"
                          "    float s2 = pow(max(dot(t2, H), 0.0), e * 0.5);\n"
                          "    return (s1 + 0.5 * s2) * strength;\n"
                          "}")))
    n.append(NodeSpec("MatcapUV", "Stylize", "Matcap UV",
                      inputs=[_p("Normal", VEC3, "normal")], outputs=[_p("Out", VEC2)],
                      out_expr=["ukiyo_matcap_uv(normalize(%0))"],
                      helper=(
                          "vec2 ukiyo_matcap_uv(vec3 N) {\n"
                          "    vec3 vn = (frame.view * vec4(N, 0.0)).xyz;\n"
                          "    return vn.xy * 0.5 + 0.5;\n"
                          "}")))
    n.append(NodeSpec("FaceLightAngle", "Stylize", "Face Light Angle", outputs=[_p("Out", VEC2)],
                      out_expr=["ukiyo_face_light_angle()"],
                      helper=(
                          "vec2 ukiyo_face_light_angle() {\n"
                          "    vec2 Lh = frame.sunDir.xz;\n"
                          "    float lh = length(Lh);\n"
                          "    if (lh < 1e-4) return vec2(0.0);\n"
                          "    Lh /= lh;\n"
                          "    vec2 f = normalize(frame.toonD.xy + vec2(1e-5, 0.0));\n"
                          "    vec2 r = vec2(f.y, -f.x);\n"
                          "    return vec2(dot(r, Lh), dot(f, Lh));\n"
                          "}")))
    n.append(NodeSpec("FaceSDFTerm", "Stylize", "Face Shadow (SDF)",
                      inputs=[_p("SDF", FLOAT, "0.5"), _p("Bias", FLOAT, "0.0"), _p("Softness", FLOAT, "0.05")],
                      outputs=[_p("Out", FLOAT)], out_expr=["ukiyo_face_sdf(%0, %1, %2)"],
                      helper=(
                          "float ukiyo_face_sdf(float sdf, float bias, float softness) {\n"
                          "    vec2 Lh = frame.sunDir.xz;\n"
                          "    float lh = length(Lh);\n"
                          "    if (lh < 1e-4) return 1.0;\n"
                          "    Lh /= lh;\n"
                          "    vec2 f = normalize(frame.toonD.xy + vec2(1e-5, 0.0));\n"
                          "    vec2 r = vec2(f.y, -f.x);\n"
                          "    float side = dot(r, Lh);\n"
                          "    float t = clamp(0.5 - 0.5 * side + bias, 0.0, 1.0);\n"
                          "    float s = max(softness, 1e-4);\n"
                          "    return smoothstep(t - s, t + s, sdf);\n"
                          "}")))
    n.append(NodeSpec("Gradient3", "Stylize", "Gradient (3 Stops)",
                      inputs=[_p("A", VEC3, "vec3(0.0)"), _p("B", VEC3, "vec3(0.5)"),
                              _p("C", VEC3, "vec3(1.0)"), _p("T", FLOAT, "0.5")],
                      outputs=[_p("Out", VEC3)],
                      out_expr=["mix(mix(%0, %1, clamp(%3 * 2.0, 0.0, 1.0)), mix(%1, %2, clamp(%3 * 2.0 - 1.0,"
                                " 0.0, 1.0)), clamp(%3 * 2.0 - 1.0, 0.0, 1.0))"]))

    # ---- Masters (one per graph) ----
    # ---- NPR ----
    # The ramp and ILM blocks the hand-written character shaders are built from, as nodes: the same
    # formulas as tools/endfield/EndfieldHybrid.body.glsl, so a graph can reproduce one and be compared
    # against it term for term.
    n.append(NodeSpec("ILMUnpack", "NPR", "ILM Unpack",
                      inputs=[_p("RGB", VEC3, "vec3(0.5)"), _p("A", FLOAT, "0.0")],
                      outputs=[_p("Spec (R)", FLOAT), _p("AO (G)", FLOAT), _p("Mask (B)", FLOAT),
                               _p("Type (A)", FLOAT)],
                      out_expr=["%0.r", "%0.g", "%0.b", "%1"]))
    n.append(NodeSpec("ILMRampRow", "NPR", "ILM Ramp Row (5 Types)",
                      inputs=[_p("Type", FLOAT, "0.0"), _p("Row 0", FLOAT, "1.0"), _p("Row 1", FLOAT, "2.0"),
                              _p("Row 2", FLOAT, "3.0"), _p("Row 3", FLOAT, "4.0"), _p("Row 4", FLOAT, "5.0")],
                      outputs=[_p("Row", FLOAT)],
                      out_expr=["mix(mix(mix(mix(%5, %4, step(%0, 0.85)), %3, step(%0, 0.6)), %2, step(%0, 0.4)),"
                                " %1, step(%0, 0.15))"]))
    n.append(NodeSpec("DayNight", "NPR", "Day Night Factor",
                      inputs=[_p("Override", FLOAT, "-1.0")],
                      outputs=[_p("Out", FLOAT)],
                      out_expr=["ukiyo_day_night(%0)"],
                      helper=(
                          "float ukiyo_day_night(float dayOverride) {\n"
                          "    if (dayOverride < 0.0) return (normalize(frame.sunDir.xyz).y + 1.0) * 0.5;\n"
                          "    return clamp(dayOverride, 0.0, 1.0);\n"
                          "}")))
    n.append(NodeSpec("RampUV", "NPR", "Ramp UV (Row)",
                      inputs=[_p("Lit", FLOAT, "0.5"), _p("Row", FLOAT, "1.0"), _p("Rows", FLOAT, "10.0"),
                              _p("Night", FLOAT, "0.0")],
                      outputs=[_p("Out", VEC2)],
                      out_expr=["vec2(clamp(%0, 0.003, 0.997), (%1 - 0.5) / max(%2, 1.0)"
                                " + 0.5 * clamp(%3, 0.0, 1.0))"]))
    # The ramp lookup itself: one sampler, two fetches, day and night mixed as colours.
    n.append(NodeSpec("RampSample", "NPR", "Ramp Sample (Day/Night)", param="texture",
                      inputs=[_p("U", FLOAT, "0.5"), _p("Row", FLOAT, "1.0"), _p("Rows", FLOAT, "10.0"),
                              _p("Day", FLOAT, "1.0")],
                      outputs=[_p("RGB", VEC3)],
                      out_expr=["ukiyo_ramp_sample(tex%I, %0, %1, %2, %3)"],
                      decl="layout(set = 1, binding = %B) uniform sampler2D tex%I;",
                      helper=(
                          "vec3 ukiyo_ramp_sample(sampler2D rampTex, float u, float row, float rows, float day) {\n"
                          "    float v = (row - 0.5) / max(rows, 1.0);\n"
                          "    float cu = clamp(u, 0.003, 0.997);\n"
                          "    return mix(texture(rampTex, vec2(cu, v + 0.5)).rgb, texture(rampTex, vec2(cu, v)).rgb,\n"
                          "             clamp(day, 0.0, 1.0));\n"
                          "}")))
    n.append(NodeSpec("ShadowRampU", "NPR", "Shadow Ramp U (ILM / Kaze)",
                      inputs=[_p("Normal", VEC3, "normal"), _p("AO", FLOAT, "1.0"), _p("Offset", FLOAT, "0.0"),
                              _p("Softness", FLOAT, "1.0"), _p("Kaze", FLOAT, "0.0")],
                      outputs=[_p("U", FLOAT), _p("Lit", FLOAT)],
                      out_expr=["ukiyo_shadow_ramp_u(%0, %1, %2, %3, %4).x",
                                "ukiyo_shadow_ramp_u(%0, %1, %2, %3, %4).y"],
                      helper=(
                          "vec2 ukiyo_shadow_ramp_u(vec3 N, float ao, float offset, float softness,"
                          " float kaze) {\n"
                          "    float ndl = dot(normalize(N), normalize(frame.sunDir.xyz));\n"
                          "    float edge = max(0.1 * softness, 1e-4);\n"
                          "    float legacy = clamp(smoothstep(0.3 + offset - edge, 0.3 + offset + edge,\n"
                          "                                    pow(clamp(ndl, 0.0, 1.0) * 0.5 + 0.5, 2.0)),"
                          " 0.003, 0.997);\n"
                          "    if (kaze < 0.5) return vec2(legacy, 0.0);\n"
                          "    float kazeAo = clamp(ao, 0.0, 1.0);\n"
                          "    float s = clamp(2.0 * (ndl * 0.5 + 0.5) * kazeAo, 0.0, 1.0);\n"
                          "    s = mix(s, 1.0, step(0.9, kazeAo));\n"
                          "    float top = 0.5 + offset;\n"
                          "    return vec2(clamp(smoothstep(top - edge, top, s), 0.003, 0.997),"
                          " step(top, s));\n"
                          "}")))
    n.append(NodeSpec("LutUV", "NPR", "LUT UV (Grade)",
                      inputs=[_p("Color", VEC3, "vec3(0.5)"), _p("Row", FLOAT, "0.5")],
                      outputs=[_p("Out", VEC2)],
                      out_expr=["vec2(clamp(dot(%0, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0), %1)"]))
    n.append(NodeSpec("NprFaceSdf", "NPR", "Face SDF Shadow (Kaze)", param="texture",
                      inputs=[_p("UV", VEC2, "uv"), _p("Threshold", FLOAT, "0.48"), _p("Softness", FLOAT, "0.08"),
                              _p("Forward (Material)", VEC3, "vec3(0.0, 0.0, 1.0)"),
                              _p("Right (Material)", VEC3, "vec3(1.0, 0.0, 0.0)")],
                      outputs=[_p("Shadow", FLOAT)],
                      out_expr=["ukiyo_npr_face_sdf(tex%I, %0, %1, %2, %3, %4)"],
                      decl="layout(set = 1, binding = %B) uniform sampler2D tex%I;",
                      helper=(
                          "float ukiyo_npr_face_sdf(sampler2D sdfTex, vec2 uv, float threshold, float softness,"
                          " vec3 matFwd, vec3 matRight) {\n"
                          "    float sdfR = texture(sdfTex, uv).r;\n"
                          "    float sdfL = texture(sdfTex, vec2(1.0 - uv.x, uv.y)).r;\n"
                          "    float fromFrame = step(0.5, frame.toonD.z);\n"
                          "    vec3 fwdRaw = mix(matFwd, vec3(frame.toonD.x, 0.0, frame.toonD.y), fromFrame);\n"
                          "    vec3 fwd = normalize(mix(vec3(0.0, 0.0, 1.0), fwdRaw, step(1e-10, dot(vec2(fwdRaw.x, fwdRaw.z), vec2(fwdRaw.x, fwdRaw.z)))));\n"
                          "    vec3 right = mix(normalize(matRight), normalize(cross(vec3(0.0, 1.0, 0.0), fwd)), fromFrame);\n"
                          "    vec3 up = cross(fwd, right);\n"
                          "    float upLen2 = dot(up, up);\n"
                          "    if (upLen2 < 1e-10) return 0.0;\n"
                          "    vec3 L = normalize(frame.sunDir.xyz);\n"
                          "    vec3 lightOnHead = L - dot(L, up) / upLen2 * up;\n"
                          "    float lightLen = length(lightOnHead);\n"
                          "    if (lightLen < 1e-5) return 0.0;\n"
                          "    vec3 ld = lightOnHead / lightLen;\n"
                          "    float angle01 = acos(clamp(dot(ld, right), -1.0, 1.0)) / 3.14159265;\n"
                          "    float exposeRight = step(angle01, 0.5);\n"
                          "    float valueR = pow(clamp(1.0 - angle01 * 2.0, 0.0, 1.0), 3.0);\n"
                          "    float valueL = pow(clamp(angle01 * 2.0 - 1.0, 0.0, 1.0), 3.0);\n"
                          "    float angleValue = mix(valueL, valueR, exposeRight);\n"
                          "    float sdfValue = mix(sdfR, sdfL, exposeRight);\n"
                          "    float bias = threshold - 0.5;\n"
                          "    float lit = smoothstep(angleValue - softness, angleValue + softness, sdfValue + bias);\n"
                          "    float front = smoothstep(-0.12, 0.12, dot(ld, fwd));\n"
                          "    return 1.0 - mix(1.0, lit, front);\n"
                          "}")))
    n.append(NodeSpec("SpecRampUV", "NPR", "Spec Ramp UV (N.H)",
                      inputs=[_p("Normal", VEC3, "normal"), _p("Style", FLOAT, "0.0")],
                      outputs=[_p("Out", VEC2)],
                      out_expr=["ukiyo_spec_ramp_uv(%0, %1)"],
                      helper=(
                          "vec2 ukiyo_spec_ramp_uv(vec3 N, float style) {\n"
                          "    vec3 V = normalize(frame.camPos.xyz - v_world);\n"
                          "    vec3 H = normalize(normalize(frame.sunDir.xyz) + V);\n"
                          "    return vec2(clamp(dot(normalize(N), H), 0.0, 1.0), style);\n"
                          "}")))

    n.append(NodeSpec("HybridMaster", "Master", "Hybrid Master", master=True,
                      inputs=[_p("Base Color", VEC3, "vec3(0.5)"), _p("Metallic", FLOAT, "0.0"),
                              _p("Smoothness", FLOAT, "0.5"), _p("Normal", VEC3, "normal"),
                              _p("Baked Light", VEC3, "vec3(1.0)"),
                              _p("Clear Coat", FLOAT, "0.0"), _p("Clear Coat Smoothness", FLOAT, "0.5"),
                              _p("Ramp Color", VEC3, "vec3(1.0)"), _p("Occlusion", FLOAT, "1.0"),
                              _p("Shadow", FLOAT, "1.0"), _p("Scene Light", FLOAT, "1.0"),
                              _p("Light Wrap", FLOAT, "0.0"),
                              _p("Shadow Mask", FLOAT, "0.0"), _p("Shadow Mask Mix", FLOAT, "0.0"),
                              _p("Ramp Blend", FLOAT, "1.0"),
                              _p("Shadow Floor", FLOAT, "0.0"), _p("Ambient", FLOAT, "0.55"),
                              _p("Lit Boost", FLOAT, "1.0"), _p("Normal Shade", FLOAT, "0.0"),
                              _p("SDF Shadow", FLOAT, "0.0"), _p("Shadow Tone", VEC3, "vec3(0.5)"),
                              _p("Rim Light", VEC3, "vec3(0.0)"), _p("Emission", VEC3, "vec3(0.0)"),
                              _p("Alpha", FLOAT, "1.0"),
                              _p("Spec Mask", FLOAT, "1.0"), _p("Hair Specular", VEC3, "vec3(0.0)"),
                              _p("Matcap", VEC3, "vec3(0.0)"), _p("Wetness", FLOAT, "0.0")],
                      surface_fields=["baseColor", "metallic", "smoothness", "normal", "bakedLight",
                                      "clearCoat", "clearCoatSmoothness", "toonRamp", "toonOcclusion",
                                      "toonShadow", "toonScene", "toonWrap", "toonMask", "toonMaskMix",
                                      "toonRampBlend", "toonFloor", "toonAmbient", "toonBoost",
                                      "toonNormalShade", "sdfShadow", "toonShade", "toonRim", "emission",
                                      "alpha", "specMask", "hairSpecular", "matcap", "wetness"]))
    n.append(NodeSpec("OutlineMaster", "Master", "Outline Master", master=True,
                      inputs=[], surface_fields=[]))
    n.append(NodeSpec("FaceMaster", "Master", "Face Master", master=True,
                      inputs=[_p("Base Color", VEC3, "vec3(0.5)"), _p("Ramp Color", VEC3, "vec3(1.0)"),
                              _p("SDF Shadow", FLOAT, "0.0"), _p("Shadow Tone", VEC3, "vec3(0.5)"),
                              _p("Ambient", FLOAT, "0.55"), _p("Rim Light", VEC3, "vec3(0.0)"),
                              _p("Emission", VEC3, "vec3(0.0)"), _p("Alpha", FLOAT, "1.0")],
                      surface_fields=["baseColor", "toonRamp", "toonShadow", "toonShade", "toonAmbient",
                                      "toonRim", "emission", "alpha"]))
    n.append(NodeSpec("PBRMaster", "Master", "PBR Master", master=True,
                      inputs=[_p("Base Color", VEC3, "vec3(0.5)"), _p("Metallic", FLOAT, "0.0"),
                              _p("Smoothness", FLOAT, "0.5"), _p("Normal", VEC3, "normal"),
                              _p("Emission", VEC3, "vec3(0.0)"), _p("Alpha", FLOAT, "1.0")],
                      surface_fields=["baseColor", "metallic", "smoothness", "normal", "emission", "alpha"]))
    n.append(NodeSpec("ComplexLitMaster", "Master", "Complex Lit Master", master=True,
                      inputs=[_p("Base Color", VEC3, "vec3(0.5)"), _p("Metallic", FLOAT, "0.0"),
                              _p("Smoothness", FLOAT, "0.5"), _p("Normal", VEC3, "normal"),
                              _p("Emission", VEC3, "vec3(0.0)"), _p("Alpha", FLOAT, "1.0"),
                              _p("Clear Coat", FLOAT, "0.0"), _p("Clear Coat Smoothness", FLOAT, "0.5")],
                      surface_fields=["baseColor", "metallic", "smoothness", "normal", "emission", "alpha",
                                      "clearCoat", "clearCoatSmoothness"]))
    n.append(NodeSpec("BakedLitMaster", "Master", "Baked Lit Master", master=True,
                      inputs=[_p("Base Color", VEC3, "vec3(0.5)"), _p("Baked Light", VEC3, "vec3(1.0)"),
                              _p("Emission", VEC3, "vec3(0.0)"), _p("Alpha", FLOAT, "1.0")],
                      surface_fields=["baseColor", "bakedLight", "emission", "alpha"]))
    n.append(NodeSpec("ToonMaster", "Master", "NPR Master (Toon)", master=True,
                      inputs=[_p("Base Color", VEC3, "vec3(0.5)"), _p("Shade Color", VEC3, "vec3(0.6, 0.5, 0.6)"),
                              _p("Threshold", FLOAT, "0.0"), _p("Softness", FLOAT, "0.05"),
                              _p("Normal", VEC3, "normal"), _p("Emission", VEC3, "vec3(0.0)"),
                              _p("Alpha", FLOAT, "1.0"),
                              _p("Ramp Color", VEC3, "vec3(1.0)"), _p("Occlusion", FLOAT, "1.0"),
                              _p("Shadow", FLOAT, "1.0"), _p("Scene Light", FLOAT, "1.0"),
                              _p("Light Wrap", FLOAT, "0.0"),
                              _p("Shadow Mask", FLOAT, "0.0"), _p("Shadow Mask Mix", FLOAT, "0.0"),
                              _p("Ramp Blend", FLOAT, "1.0"),
                              _p("Shadow Floor", FLOAT, "0.0"), _p("Ambient", FLOAT, "0.55"),
                              _p("Lit Boost", FLOAT, "1.0"), _p("Normal Shade", FLOAT, "0.0")],
                      surface_fields=["baseColor", "toonShade", "toonThreshold", "toonSoftness", "normal",
                                      "emission", "alpha", "toonRamp", "toonOcclusion", "toonShadow",
                                      "toonScene", "toonWrap", "toonMask", "toonMaskMix", "toonRampBlend",
                                      "toonFloor", "toonAmbient", "toonBoost", "toonNormalShade"]))
    n.append(NodeSpec("UnlitMaster", "Master", "Unlit Master", master=True,
                      inputs=[_p("Color", VEC3, "vec3(1.0)"), _p("Alpha", FLOAT, "1.0")],
                      surface_fields=["baseColor", "alpha"]))
    return n


NODES = build_nodes()
NODE_BY_ID = {s.id: s for s in NODES}

PARAM_COUNT = {"none": 0, "float": 1, "vec2": 2, "color": 3, "texture": 1}


def default_params(param):
    if param == "float":
        return ["1"]
    if param == "vec2":
        return ["0", "0"]
    if param == "color":
        return ["1", "1", "1"]
    if param == "texture":
        return [""]
    return []


def find_spec(node_id):
    return NODE_BY_ID.get(node_id)


# ---------------------------------------------------------------- GLSL literals


def glsl_float(value):
    """QString::number(v, 'g', 9) plus the '.0' GLSL needs on a whole number."""
    v = float(value)
    if v != v or v in (float("inf"), float("-inf")):
        return "nan" if v != v else ("inf" if v > 0 else "-inf")
    s = f"{v:.9g}"
    if "." not in s and "e" not in s and "E" not in s and "inf" not in s and "nan" not in s:
        s += ".0"
    return s


def param_expression(param, params):
    """ShaderNodeModel::expression(): the GLSL for a node's parameter value."""
    if param in ("none", "texture"):
        return ""
    if param == "float":
        return glsl_float(params[0] if params else 0.0)
    if param == "vec2":
        return "vec2(%s, %s)" % (glsl_float(params[0] if len(params) > 0 else 0.0),
                                 glsl_float(params[1] if len(params) > 1 else 0.0))
    if param == "color":
        return "vec3(%s, %s, %s)" % (glsl_float(params[0] if len(params) > 0 else 0.0),
                                     glsl_float(params[1] if len(params) > 1 else 0.0),
                                     glsl_float(params[2] if len(params) > 2 else 0.0))
    return ""


def property_kind_name(param):
    return {"float": "float", "vec2": "vec2", "color": "vec3"}.get(param, "none")


# ---------------------------------------------------------------- the graph


class Graph:
    """The subset of QtNodes::DataFlowGraphModel the generator reads."""

    def __init__(self, graph_json):
        self.nodes = {}          # id -> {"model", "params", "reference", "position", "label"}
        self.order = []          # node ids, in document order
        self.in_connections = {}  # (node id, port index) -> [(out node id, out port index), ...]
        for node in graph_json.get("nodes", []):
            nid = int(node["id"])
            internal = node.get("internal-data", {})
            params = [str(p) for p in internal.get("params", [])]
            self.nodes[nid] = {
                "model": internal.get("model-name", ""),
                "params": params,
                "reference": internal.get("reference", "") or "",
                "position": node.get("position", {"x": 0, "y": 0}),
                "label": node.get("label", ""),
                "labelVisible": bool(node.get("labelVisible", True)),
            }
            self.order.append(nid)
        for conn in graph_json.get("connections", []):
            key = (int(conn["inNodeId"]), int(conn["inPortIndex"]))
            self.in_connections.setdefault(key, []).append(
                (int(conn["outNodeId"]), int(conn["outPortIndex"])))

    def model(self, nid):
        node = self.nodes[nid]
        return find_spec(node["model"]), node

    def connection(self, nid, port):
        conns = self.in_connections.get((nid, port), [])
        return conns[0] if conns else None


class CodegenResult:
    def __init__(self):
        self.surface_function = ""
        self.fragment_shader = ""
        self.properties = []      # [{"name","kind","slot","defaults"}]
        self.textures = []        # [{"name","binding","defaultPath"}]
        self.errors = []
        self.transparent = False
        self.outline = False

    @property
    def ok(self):
        return not self.errors


MATERIAL_BLOCK_BINDING = 15
MAX_GRAPH_TEXTURES = 8

# The frame block the generator declares: the leading part of the engine's FrameBlock
# (shaders/frame.glsl), field for field, so the std140 offsets match. The toon rows are the scene's
# toon parameters (hair highlight, face head-forward, light sweep, eye terms), so a graph style can
# react to the same global sliders the engine's own toon shaders use.
FRAME_BLOCK_LINES = [
    "layout(set = 0, binding = 0, std140) uniform FrameBlock {",
    "    mat4 view;",
    "    mat4 proj;",
    "    mat4 viewProj;",
    "    mat4 invViewProj;",
    "    mat4 shadowMatrix[3];",
    "    vec4 camPos;",
    "    vec4 sunDir;",
    "    vec4 sunColor;",
    "    vec4 skyColor;",
    "    vec4 groundColor;",
    "    vec4 cascadeSplits;",
    "    vec4 shadowParams;",
    "    vec4 screenParams;   // z = time (s)",
    "    vec4 pointPos[4];",
    "    vec4 pointColor[4];",
    "    vec4 toonA;          // x hair highlight, y hair shift, z hair exponent, w self shadow on",
    "    vec4 toonB;          // x self threshold, y self hardness, z gloss, w gloss power",
    "    vec4 toonC;          // x cel band on, y normal map strength, z outline noise, w reflection",
    "    vec4 toonD;          // xy head forward (x, z) for face shadow, z face shadow on, w smear",
    "    vec4 toonE;          // x soft light, y light sweep strength, z sweep speed",
    "    vec4 toonF;          // camera dither fade: x dithered-out distance, y visible distance",
    "    vec4 toonG;          // x stylised PBR, y eye flatten, z eye parallax, w eye matcap",
    "    vec4 toonH;          // x eye highlight map strength",
    "    vec4 toonI;          // occlusion target: xyz world point, w radius",
    "} frame;",
]

# Extra fragment varyings. graph.vert writes both (COLOR_0 and TEXCOORD_1), so a graph can read
# painted masks and a second UV set. Declared after v_world so existing locations do not move.
VARYING_LINES = [
    "layout(location = 3) in vec4 v_color;   // COLOR_0, white when the mesh has none",
    "layout(location = 4) in vec2 v_uv1;     // TEXCOORD_1, a copy of v_uv when the mesh has none",
]

SURFACE_STRUCT_LINES = [
    "struct SurfaceData {",
    "    vec3 baseColor;",
    "    float metallic;",
    "    float smoothness;",
    "    vec3 normal;",
    "    vec3 emission;",
    "    vec3 bakedLight;   // baked light multiplier; 1 when no bake is connected",
    "    float alpha;",
    "    float clearCoat;            // 0 = none (clear-coat layer strength)",
    "    float clearCoatSmoothness;  // clear-coat highlight sharpness",
    "    vec3 toonShade;     // Toon Master: shade colour in the shadow band",
    "    float toonThreshold;// Toon Master: N.L where the band starts",
    "    float toonSoftness; // Toon Master: width of the band edge",
    "    vec3 toonRamp;      // NPR Master: ramp atlas colour, multiplied into the shade",
    "    float toonOcclusion;// NPR Master: ambient occlusion (ILM green, vertex colour red)",
    "    float toonShadow;   // NPR Master: 1 where the sun is visible",
    "    float toonScene;    // NPR Master: 0 = no sun or sky term, the ramp carries the light",
    "    float toonRampBlend;// NPR Master: how strongly the ramp tone replaces the lit tone",
    "    float toonWrap;     // NPR Master: 0 = N.L terminator, 1 = half-Lambert",
    "    float toonMask;     // NPR Master: replaces the ramp (painted style ramp, ILM mask)",
    "    float toonMaskMix;  // NPR Master: how far toonMask replaces the ramp",
    "    float toonFloor;    // NPR Master: albedo floor kept in the shadow",
    "    float toonAmbient;  // NPR Master: hemisphere ambient strength",
    "    float toonBoost;    // NPR Master: multiplies the lit term",
    "    float toonNormalShade; // NPR Master: 0.92..1.08 lit modulation by the wrap",
    "    vec3 toonRim;       // Face Master: additive rim light",
    "    float toon;         // 1 = NPR (Toon) Master lighting",
    "    float sdfShadow;    // Hybrid/Face: 0 = lit, 1 = shadowed",
    "    float specMask;     // Hybrid: specular mask",
    "    vec3 hairSpecular;  // Hybrid: anisotropic hair",
    "    vec3 matcap;        // Hybrid: matcap reflection",
    "    float wetness;      // Hybrid: rain coverage",
    "    float hybrid;       // 1 = Hybrid Master lighting",
    "    float face;         // 1 = Face Master lighting",
    "    float lit;   // 1 = shaded by the frame's sun and sky, 0 = unlit",
    "};",
]

SURFACE_DEFAULTS = [
    "    SurfaceData s;",
    "    s.baseColor = vec3(1.0);",
    "    s.metallic = 0.0;",
    "    s.smoothness = 0.5;",
    "    s.normal = normal;",
    "    s.emission = vec3(0.0);",
    "    s.bakedLight = vec3(1.0);",
    "    s.alpha = 1.0;",
    "    s.clearCoat = 0.0;",
    "    s.clearCoatSmoothness = 0.5;",
    "    s.toonShade = vec3(0.6, 0.5, 0.6);",
    "    s.toonThreshold = 0.0;",
    "    s.toonSoftness = 0.05;",
    "    s.toonRamp = vec3(1.0);",
    "    s.toonOcclusion = 1.0;",
    "    s.toonShadow = 1.0;",
    "    s.toonScene = 1.0;",
    "    s.toonRampBlend = 1.0;",
    "    s.toonWrap = 0.0;",
    "    s.toonMask = 0.0;",
    "    s.toonMaskMix = 0.0;",
    "    s.toonFloor = 0.0;",
    "    s.toonAmbient = 0.55;",
    "    s.toonBoost = 1.0;",
    "    s.toonNormalShade = 0.0;",
    "    s.toonRim = vec3(0.0);",
    "    s.toon = 0.0;",
    "    s.sdfShadow = 0.0;",
    "    s.specMask = 1.0;",
    "    s.hairSpecular = vec3(0.0);",
    "    s.matcap = vec3(0.0);",
    "    s.wetness = 0.0;",
    "    s.hybrid = 0.0;",
    "    s.face = 0.0;",
    "    s.lit = 1.0;",
]

MAIN_LINES = [
    "void main() {",
    "    SurfaceData s = ukiyo_surface(v_uv, normalize(v_normal));",
    "    vec3 color = s.baseColor * s.bakedLight + s.emission;",
    "    if (s.toon > 0.5) {",
    "        // NPR Master: ramp-toned sun light plus hemisphere ambient.",
    "        vec3 Nt = normalize(s.normal);",
    "        float ndlt = dot(Nt, normalize(frame.sunDir.xyz));",
    "        float wrapped = mix(ndlt, clamp(ndlt, 0.0, 1.0) * 0.5 + 0.5, s.toonWrap);",
    "        float ramp = smoothstep(s.toonThreshold - s.toonSoftness, s.toonThreshold + s.toonSoftness, wrapped);",
    "        ramp = mix(ramp, clamp(s.toonMask, 0.0, 1.0), s.toonMaskMix);",
    "        float rampBase = ramp;",
    "        ramp = mix(1.0, ramp, s.toonRampBlend);",
    "        vec3 albedo = s.baseColor * s.bakedLight;",
    "        vec3 hemiT = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * Nt.y);",
    "        float normalShade = mix(1.0, mix(0.92, 1.08, clamp(wrapped, 0.0, 1.0)), s.toonNormalShade);",
    "        vec3 tone = albedo * mix(s.toonShade * s.toonRamp, vec3(s.toonBoost * normalShade), ramp);",
    "        color = tone * frame.sunColor.rgb * s.toonShadow * s.toonScene",
    "              + albedo * hemiT * s.toonAmbient * s.toonOcclusion * s.toonScene",
    "              + tone * (1.0 - s.toonScene);",
    "        vec3 floorTone = albedo * s.toonFloor * (1.0 - rampBase);",
    "        color = mix(color, max(color, floorTone), step(0.0, s.toonFloor));",
    "        color += s.emission;",
    "    } else if (s.hybrid > 0.5) {",
    "        // Hybrid Master: PBR and NPR blended, plus matcap, hair, rim and wetness.",
    "        vec3 N = normalize(s.normal);",
    "        vec3 L = normalize(frame.sunDir.xyz);",
    "        vec3 V = normalize(frame.camPos.xyz - v_world);",
    "        vec3 H = normalize(L + V);",
    "        float ndl = max(dot(N, L), 0.0);",
    "        float ndv = max(dot(N, V), 0.0);",
    "        float ndh = max(dot(N, H), 0.0);",
    "        vec3 albedo = s.baseColor * s.bakedLight;",
    "        vec3 hemi = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * N.y);",
    "        float shin = exp2(1.0 + 10.0 * s.smoothness);",
    "        float spec = pow(ndh, shin) * (shin + 8.0) / 25.13274;",
    "        vec3 F0 = mix(vec3(0.04), albedo, s.metallic);",
    "        vec3 pbr = albedo * (1.0 - s.metallic) * (frame.sunColor.rgb * ndl + hemi) * s.toonOcclusion",
    "                 + F0 * frame.sunColor.rgb * spec * step(0.0, ndl) * s.specMask;",
    "        float wrapped = mix(dot(N, L), clamp(dot(N, L), 0.0, 1.0) * 0.5 + 0.5, s.toonWrap);",
    "        float ramp = smoothstep(s.toonThreshold - s.toonSoftness, s.toonThreshold + s.toonSoftness, wrapped);",
    "        ramp = mix(ramp, clamp(s.toonMask, 0.0, 1.0), s.toonMaskMix);",
    "        float rampBase = ramp;",
    "        ramp = mix(1.0, ramp, s.toonRampBlend);",
    "        float normalShade = mix(1.0, mix(0.92, 1.08, clamp(wrapped, 0.0, 1.0)), s.toonNormalShade);",
    "        vec3 nprLit = albedo * s.toonBoost * normalShade;",
    "        vec3 rampTone = albedo * mix(s.toonShade * s.toonRamp, vec3(1.0), ramp);",
    "        float shadow = clamp(max(1.0 - s.toonShadow, s.sdfShadow), 0.0, 1.0);",
    "        vec3 litBase = mix(nprLit, pbr, s.toonScene);",
    "        color = mix(litBase, rampTone, shadow * s.toonRampBlend);",
    "        color = max(color, albedo * s.toonFloor * shadow);",
    "        color += s.hairSpecular * s.toonRamp;",
    "        color += s.matcap;",
    "        color += s.toonRim + s.emission;",
    "    } else if (s.face > 0.5) {",
    "        // Face Master: SDF shadow over the ramp, plus hemisphere ambient.",
    "        vec3 albedo = s.baseColor * s.bakedLight;",
    "        vec3 hemiT = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * s.normal.y);",
    "        vec3 tone = albedo * mix(vec3(1.0), s.toonRamp * s.toonShade, clamp(s.toonShadow, 0.0, 1.0));",
    "        color = tone * frame.sunColor.rgb",
    "              + albedo * hemiT * s.toonAmbient",
    "              + s.toonRim + s.emission;",
    "    } else if (s.lit > 0.5) {",
    "        vec3 N = normalize(s.normal);",
    "        float ndl = max(dot(N, normalize(frame.sunDir.xyz)), 0.0);",
    "        vec3 hemi = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * N.y);",
    "        // Blinn-Phong highlight: metallic tints F0, smoothness sets the exponent.",
    "        vec3 V = normalize(frame.camPos.xyz - v_world);",
    "        vec3 H = normalize(normalize(frame.sunDir.xyz) + V);",
    "        float shin = exp2(1.0 + 10.0 * s.smoothness);",
    "        float spec = pow(max(dot(N, H), 0.0), shin) * (shin + 8.0) / 25.13274;",
    "        vec3 F0 = mix(vec3(0.04), s.baseColor, s.metallic);",
    "        vec3 diffuse = s.baseColor * (1.0 - s.metallic) * (frame.sunColor.rgb * ndl + hemi);",
    "        color = diffuse + F0 * frame.sunColor.rgb * spec * step(0.0, ndl) + s.emission;",
    "        // Clear coat: a sharper white highlight over the base; it also dims the base by its Fresnel.",
    "        float ccExp = exp2(1.0 + 10.0 * s.clearCoatSmoothness);",
    "        float ccSpec = pow(max(dot(N, H), 0.0), ccExp) * (ccExp + 8.0) / 25.13274;",
    "        color = color * (1.0 - 0.04 * s.clearCoat) + frame.sunColor.rgb * ccSpec * s.clearCoat * step(0.0, ndl);",
    "    } else if (s.toon < 0.5 && s.face < 0.5 && s.lit < 0.5) {",
    "        discard; // Outline Master",
    "    }",
    "    o_color = vec4(color, s.alpha);",
    "}",
]


def expand(template, inputs, param, nid, slot):
    """Replaces the placeholders in one output expression (sg_codegen.cpp's expand())."""
    out = template.replace("%P", param).replace("%I", nid).replace("%B", str(slot))
    for k in range(len(inputs) - 1, -1, -1):
        out = out.replace("%" + str(k), inputs[k])
    return out


class _Gen:
    def __init__(self, graph):
        self.graph = graph
        self.body = []
        self.decls = []
        self.resolved = {}
        self.ids = {}
        self.visiting = set()
        self.tex_slot = 0
        self.errors = []
        self.props = []
        self.textures = []

    # sg_codegen.cpp's Gen::propertyExpr(): the first use of a name allocates its slot.
    def property_expr(self, name, param, defaults):
        for p in self.props:
            if p["name"] != name:
                continue
            if p["kind"] != param:
                self.errors.append(f"Exposed name '{name}' is used for two different types.")
            suffix = {FLOAT: ".x", VEC2: ".xy"}.get(property_kind_name(param), ".xyz")
            return f"p{p['slot']}{suffix}"
        slot = len(self.props)
        self.props.append({"name": name, "kind": param, "slot": slot, "defaults": list(defaults)})
        suffix = {FLOAT: ".x", VEC2: ".xy"}.get(property_kind_name(param), ".xyz")
        return f"p{slot}{suffix}"

    def id_for(self, nid):
        if nid not in self.ids:
            self.ids[nid] = f"n{len(self.ids) + 1}"
        return self.ids[nid]

    def input_expr(self, nid, port, spec_port):
        conn = self.graph.connection(nid, port)
        if conn is None:
            return spec_port.default
        out_id, out_port = conn
        outs = self.resolve(out_id)
        if out_port >= len(outs):
            self.errors.append(f"Input '{spec_port.name}' is connected to a missing output.")
            return spec_port.default
        return outs[out_port]

    def resolve(self, nid):
        if nid in self.resolved:
            return self.resolved[nid]
        if nid in self.visiting:
            self.errors.append("The graph has a loop.")
            return []
        self.visiting.add(nid)

        spec, node = self.graph.model(nid)
        if spec is None:
            self.errors.append("A node has no shader model.")
            self.visiting.discard(nid)
            return []

        inputs = [self.input_expr(nid, i, p) for i, p in enumerate(spec.inputs)]
        short_id = self.id_for(nid)
        out_vars = []
        if not spec.master:
            slot = 0
            if spec.decl:
                if self.tex_slot >= MAX_GRAPH_TEXTURES:
                    self.errors.append(f"Too many textures: a graph supports {MAX_GRAPH_TEXTURES} sampler nodes.")
                    self.visiting.discard(nid)
                    self.resolved[nid] = []
                    return []
                slot = self.tex_slot
                self.tex_slot += 1
                self.decls.append(expand(spec.decl, [], "", short_id, slot))
                if spec.param == "texture" and node["reference"]:
                    ref = node["reference"]
                    taken = any(t["name"] == ref for t in self.textures)
                    taken = taken or any(p["name"] == ref for p in self.props)
                    if taken:
                        self.errors.append(f"Exposed name '{ref}' is used twice.")
                    else:
                        path = node["params"][0] if node["params"] else ""
                        self.textures.append({"name": ref, "binding": slot, "defaultPath": path})
            # Helpers are shared: each one is emitted once however many nodes use it.
            if spec.helper and spec.helper not in self.decls:
                self.decls.append(spec.helper)
            param = param_expression(spec.param, node["params"])
            if node["reference"] and spec.param in ("float", "vec2", "color"):
                param = self.property_expr(node["reference"], spec.param, node["params"])
            for k in range(min(len(spec.outputs), len(spec.out_expr))):
                var = f"{short_id}_{k}"
                expr = expand(spec.out_expr[k], inputs, param, short_id, slot)
                self.body.append(f"    {spec.outputs[k].kind} {var} = {expr};")
                out_vars.append(var)
        self.visiting.discard(nid)
        self.resolved[nid] = out_vars
        return out_vars


def emit_master(gen, master_id, spec, surface):
    """Master nodes write their inputs into the SurfaceData fields."""
    for i, field in enumerate(spec.surface_fields):
        if i >= len(spec.inputs):
            break
        expr = gen.input_expr(master_id, i, spec.inputs[i])
        surface.append(f"    s.{field} = {expr};")


def generate(graph):
    """sg_codegen.cpp's generate()."""
    out = CodegenResult()
    gen = _Gen(graph)

    masters = [nid for nid in graph.order
               if graph.model(nid)[0] is not None and graph.model(nid)[0].master]
    if not masters:
        out.errors.append("Add a master node (PBR Master or Unlit Master).")
        return out
    if len(masters) > 1:
        out.errors.append("A graph can have only one master node.")
        return out

    master_id = masters[0]
    master_spec = graph.model(master_id)[0]

    # A wired Alpha input makes the material transparent. The exception is an exposed Float at 1 or
    # more (the default Lit and Unlit alpha), which is opaque.
    for i, field in enumerate(master_spec.surface_fields):
        if field != "alpha":
            continue
        conn = graph.connection(master_id, i)
        out.transparent = conn is not None
        if conn is None:
            continue
        src_spec, src_node = graph.model(conn[0])
        if src_spec and src_spec.id == "Float" and src_node["reference"] and src_node["params"]:
            try:
                if float(src_node["params"][0]) >= 1.0:
                    out.transparent = False
            except ValueError:
                pass

    # An outline is a material contract rather than a surface field: shaders/graph_outline.vert reads
    # slots 0, 1 and 2 of the same block. A graph asks for the pass by exposing three values with exactly
    # those names; they are allocated before the master is walked so they land in those slots whatever
    # order the rest of the graph uses. They do not have to feed the master.
    outline_names = ["outlineColor", "outlineWidth", "outlineAlpha"]
    outline_kinds = ["color", "float", "float"]
    outline_defaults = [None, None, None]
    for nid in graph.order:
        spec, node = graph.model(nid)
        if spec is None or not node["reference"]:
            continue
        for k, name in enumerate(outline_names):
            if outline_defaults[k] is not None or node["reference"] != name or spec.param != outline_kinds[k]:
                continue
            outline_defaults[k] = node["params"]
    if all(d is not None for d in outline_defaults):
        for k, name in enumerate(outline_names):
            gen.property_expr(name, outline_kinds[k], outline_defaults[k])
        out.outline = True

    surface_lines = []
    emit_master(gen, master_id, master_spec, surface_lines)
    out.errors.extend(gen.errors)

    fn = list(SURFACE_STRUCT_LINES)
    fn.append("")
    fn.append("SurfaceData ukiyo_surface(vec2 uv, vec3 normal) {")
    fn.extend(SURFACE_DEFAULTS)
    if master_spec.id in ("UnlitMaster", "BakedLitMaster"):
        surface_lines.append("    s.lit = 0.0;")
    if master_spec.id == "ToonMaster":
        surface_lines.append("    s.toon = 1.0;")
    if master_spec.id == "FaceMaster":
        surface_lines.append("    s.face = 1.0;")
    if master_spec.id == "HybridMaster":
        surface_lines.append("    s.hybrid = 1.0;")
    if master_spec.id == "OutlineMaster":
        surface_lines.append("    s.lit = 0.0;")
        surface_lines.append("    s.alpha = 0.0;")
    fn.extend(gen.body)
    fn.extend(surface_lines)
    fn.append("    return s;")
    fn.append("}")
    out.surface_function = "\n".join(fn) + "\n"

    frag = ["#version 450",
            "// Generated by the Ukiyo Shader Graph. Do not edit by hand.",
            "layout(location = 0) in vec2 v_uv;",
            "layout(location = 1) in vec3 v_normal;",
            "layout(location = 2) in vec3 v_world;"]
    frag.extend(VARYING_LINES)
    frag.append("layout(location = 0) out vec4 o_color;")
    frag.extend(FRAME_BLOCK_LINES)
    if gen.props:
        frag.append(f"layout(set = 1, binding = {MATERIAL_BLOCK_BINDING}, std140) uniform UkMaterial {{")
        for p in gen.props:
            frag.append(f"    vec4 p{p['slot']}; // {p['name']} ({property_kind_name(p['kind'])})")
        frag.append("};")
    out.properties = gen.props
    out.textures = gen.textures
    frag.extend(gen.decls)
    frag.append("")
    frag.extend(fn)
    frag.append("")
    frag.extend(MAIN_LINES)
    out.fragment_shader = "\n".join(frag) + "\n"

    seen = set()
    deduped = []
    for e in out.errors:
        if e not in seen:
            seen.add(e)
            deduped.append(e)
    out.errors = deduped
    return out


def shader_document(graph_json, groups=None):
    """sg_codegen.cpp's shaderDocument(): the .ukshader document."""
    graph = Graph(graph_json)
    r = generate(graph)
    compiled = {
        "ok": r.ok,
        "errors": list(r.errors),
        "fragment": r.fragment_shader,
        "surface": r.surface_function,
        "properties": [{"name": p["name"], "kind": property_kind_name(p["kind"]), "slot": p["slot"],
                        "defaults": list(p["defaults"])} for p in r.properties],
        "textures": [{"name": t["name"], "binding": t["binding"], "defaultPath": t["defaultPath"]}
                     for t in r.textures],
        "materialBinding": MATERIAL_BLOCK_BINDING,
        "blend": "alpha" if r.transparent else "none",
    }
    if r.outline:
        compiled["outline"] = True
    root = {
        "format": "ukiyo-shader-graph",
        "version": 2,
        "graph": normalise_graph(graph_json),
        "compiled": compiled,
    }
    if groups is not None:
        root["groups"] = groups
    return root


def normalise_graph(graph_json):
    """Nodes in ascending id order and connections in document order, as the editor writes them."""
    nodes = []
    for node in sorted(graph_json.get("nodes", []), key=lambda n: int(n["id"])):
        internal = node.get("internal-data", {})
        pos = node.get("position", {"x": 0, "y": 0})
        nodes.append({
            "id": int(node["id"]),
            "internal-data": {
                "model-name": internal.get("model-name", ""),
                "params": [str(p) for p in internal.get("params", [])],
                "reference": internal.get("reference", "") or "",
            },
            "label": node.get("label", ""),
            "labelVisible": bool(node.get("labelVisible", True)),
            "position": {"x": _number(pos.get("x", 0)), "y": _number(pos.get("y", 0))},
        })
    conns = [{"inNodeId": int(c["inNodeId"]), "inPortIndex": int(c["inPortIndex"]),
              "outNodeId": int(c["outNodeId"]), "outPortIndex": int(c["outPortIndex"])}
             for c in graph_json.get("connections", [])]
    return {"connections": conns, "nodes": nodes}


def _number(v):
    f = float(v)
    return int(f) if f == int(f) else f


# ---------------------------------------------------------------- Qt JSON writer


def qt_json(value, indent=0):
    """QJsonDocument::toJson(Indented): 4 spaces, sorted keys, '[\\n]' for an empty array."""
    pad = "    " * indent
    inner = "    " * (indent + 1)
    if isinstance(value, dict):
        if not value:
            return "{\n" + pad + "}"
        parts = []
        for key in sorted(value.keys()):
            parts.append(f'{inner}"{_escape(str(key))}": {qt_json(value[key], indent + 1)}')
        return "{\n" + ",\n".join(parts) + "\n" + pad + "}"
    if isinstance(value, (list, tuple)):
        if not value:
            return "[\n" + pad + "]"
        parts = [inner + qt_json(v, indent + 1) for v in value]
        return "[\n" + ",\n".join(parts) + "\n" + pad + "]"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return str(int(value)) if float(value) == int(value) else repr(float(value))
    if value is None:
        return "null"
    return '"' + _escape(str(value)) + '"'


def _escape(text):
    out = []
    for ch in text:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20:
            out.append("\\u%04x" % ord(ch))
        else:
            out.append(ch)
    return "".join(out)


def write_document(doc):
    """The exact bytes QJsonDocument(root).toJson(QJsonDocument::Indented) produces."""
    return (qt_json(doc) + "\n").encode("utf-8")
