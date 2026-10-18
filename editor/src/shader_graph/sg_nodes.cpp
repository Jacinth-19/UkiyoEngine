#include "sg_nodes.h"

#include <QtCore/QJsonArray>
#include <QtGui/QColor>
#include <QtWidgets/QColorDialog>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>

namespace sg {

using QtNodes::NodeDataType;
using QtNodes::PortIndex;
using QtNodes::PortType;

QString glslType(Kind k)
{
    switch (k) {
    case Kind::Float: return QStringLiteral("float");
    case Kind::Vec2: return QStringLiteral("vec2");
    case Kind::Vec3: return QStringLiteral("vec3");
    }
    return QStringLiteral("float");
}

QString kindLabel(Kind k)
{
    switch (k) {
    case Kind::Float: return QStringLiteral("Float");
    case Kind::Vec2: return QStringLiteral("Vector 2");
    case Kind::Vec3: return QStringLiteral("Vector 3");
    }
    return QString();
}

NodeDataType ShaderValue::type() const
{
    return NodeDataType{glslType(kind_), kindLabel(kind_)};
}

QString glslFloat(double v)
{
    QString s = QString::number(v, 'g', 9);
    if (!s.contains('.') && !s.contains('e') && !s.contains("inf") && !s.contains("nan")) s += QStringLiteral(".0");
    return s;
}

namespace {

constexpr const char* kInput = "Input";
constexpr const char* kMath = "Math";
constexpr const char* kVector = "Vector";
constexpr const char* kTexture = "Texture";
constexpr const char* kProcedural = "Procedural";
constexpr const char* kStylize = "Stylize";
constexpr const char* kNpr = "NPR";
constexpr const char* kMaster = "Master";

PortSpec P(const char* name, Kind k, const char* def = "") { return PortSpec{QString::fromLatin1(name), k, QString::fromLatin1(def)}; }

std::vector<NodeSpec> buildSpecs()
{
    std::vector<NodeSpec> s;
    auto add = [&](NodeSpec n) { s.push_back(std::move(n)); };
    auto node = [](const char* id, const char* cat, const char* caption) {
        NodeSpec n;
        n.id = QString::fromLatin1(id);
        n.category = QString::fromLatin1(cat);
        n.caption = QString::fromLatin1(caption);
        return n;
    };
    auto outs = [](std::initializer_list<const char*> e) {
        QStringList l;
        for (const char* x : e) l << QString::fromLatin1(x);
        return l;
    };

    // ---- Inputs ----
    { NodeSpec n = node("Float", kInput, "Float");
      n.param = ParamKind::Float; n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"%P"}); add(n); }
    { NodeSpec n = node("Vector2", kInput, "Vector 2");
      n.param = ParamKind::Vec2; n.out = {P("Out", Kind::Vec2)}; n.outExpr = outs({"%P"}); add(n); }
    { NodeSpec n = node("Color", kInput, "Color");
      n.param = ParamKind::Color; n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"%P"}); add(n); }
    { NodeSpec n = node("UV", kInput, "UV");
      n.out = {P("Out", Kind::Vec2)}; n.outExpr = outs({"uv"}); add(n); }
    // TEXCOORD_1 (a copy of TEXCOORD_0 when the mesh has none): detail maps, face SDFs, lightmaps.
    { NodeSpec n = node("UV1", kInput, "UV (Channel 1)");
      n.out = {P("Out", Kind::Vec2)}; n.outExpr = outs({"v_uv1"}); add(n); }
    { NodeSpec n = node("Time", kInput, "Time");
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"frame.screenParams.z"}); add(n); }
    { NodeSpec n = node("Normal", kInput, "Normal (World)");
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"normalize(normal)"}); add(n); }
    { NodeSpec n = node("WorldPosition", kInput, "Position (World)");
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"v_world"}); add(n); }
    // Light, view and screen inputs. A style built on the Unlit Master shades itself, so it needs the
    // same vectors the engine's lit branch uses. All directions are world space and normalised.
    { NodeSpec n = node("ViewDirection", kInput, "View Direction");
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"normalize(frame.camPos.xyz - v_world)"}); add(n); }
    { NodeSpec n = node("SunDirection", kInput, "Sun Direction");
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"normalize(frame.sunDir.xyz)"}); add(n); }
    { NodeSpec n = node("SunColor", kInput, "Sun Color");
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"frame.sunColor.rgb"}); add(n); }
    // Hemisphere ambient, the same mix the lit and toon branches use.
    { NodeSpec n = node("AmbientColor", kInput, "Ambient (Hemisphere)");
      n.out = {P("Out", Kind::Vec3)};
      n.outExpr = outs({"mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * normalize(normal).y)"}); add(n); }
    // Fragment position in pixels over the internal resolution, so 0..1 across the screen.
    { NodeSpec n = node("ScreenPosition", kInput, "Screen Position");
      n.out = {P("Out", Kind::Vec2)};
      n.outExpr = outs({"gl_FragCoord.xy / max(frame.screenParams.xy, vec2(1.0))"}); add(n); }
    // COLOR_0 from graph.vert (white when the mesh has none): painted AO, rim masks, per-strand tints.
    { NodeSpec n = node("VertexColor", kInput, "Vertex Color");
      n.out = {P("RGB", Kind::Vec3), P("A", Kind::Float)};
      n.outExpr = outs({"v_color.rgb", "v_color.a"}); add(n); }
    // The head's forward direction (x, z) that the engine updates per frame for face shading.
    { NodeSpec n = node("HeadForward", kInput, "Head Forward (Face)");
      n.out = {P("Out", Kind::Vec2)};
      n.outExpr = outs({"normalize(frame.toonD.xy + vec2(1e-5, 0.0))"}); add(n); }

    // ---- Float math ----
    { NodeSpec n = node("AddF", kMath, "Add");
      n.in = {P("A", Kind::Float, "0.0"), P("B", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"%0 + %1"}); add(n); }
    { NodeSpec n = node("MultiplyF", kMath, "Multiply");
      n.in = {P("A", Kind::Float, "1.0"), P("B", Kind::Float, "1.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"%0 * %1"}); add(n); }
    { NodeSpec n = node("PowerF", kMath, "Power");
      n.in = {P("Base", Kind::Float, "1.0"), P("Exponent", Kind::Float, "1.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"pow(%0, %1)"}); add(n); }
    { NodeSpec n = node("OneMinusF", kMath, "One Minus");
      n.in = {P("In", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"1.0 - %0"}); add(n); }
    { NodeSpec n = node("SaturateF", kMath, "Saturate");
      n.in = {P("In", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"clamp(%0, 0.0, 1.0)"}); add(n); }
    { NodeSpec n = node("SmoothstepF", kMath, "Smoothstep");
      n.in = {P("Edge0", Kind::Float, "0.0"), P("Edge1", Kind::Float, "1.0"), P("X", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"smoothstep(%0, %1, %2)"}); add(n); }
    { NodeSpec n = node("LerpF", kMath, "Lerp");
      n.in = {P("A", Kind::Float, "0.0"), P("B", Kind::Float, "1.0"), P("T", Kind::Float, "0.5")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"mix(%0, %1, %2)"}); add(n); }
    { NodeSpec n = node("SubtractF", kMath, "Subtract");
      n.in = {P("A", Kind::Float, "0.0"), P("B", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"%0 - %1"}); add(n); }
    { NodeSpec n = node("AbsF", kMath, "Absolute");
      n.in = {P("In", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"abs(%0)"}); add(n); }
    { NodeSpec n = node("StepF", kMath, "Step");
      n.in = {P("Edge", Kind::Float, "0.5"), P("X", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"step(%0, %1)"}); add(n); }
    { NodeSpec n = node("ClampF", kMath, "Clamp");
      n.in = {P("Min", Kind::Float, "0.0"), P("Max", Kind::Float, "1.0"), P("X", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"clamp(%2, %0, %1)"}); add(n); }
    // In Min == In Max is guarded, so a style can expose both ends without dividing by zero.
    { NodeSpec n = node("Remap", kMath, "Remap");
      n.in = {P("In", Kind::Float, "0.0"), P("In Min", Kind::Float, "0.0"), P("In Max", Kind::Float, "1.0"),
              P("Out Min", Kind::Float, "0.0"), P("Out Max", Kind::Float, "1.0")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"mix(%3, %4, clamp((%0 - %1) / max(abs(%2 - %1), 1e-5), 0.0, 1.0))"}); add(n); }
    { NodeSpec n = node("SineF", kMath, "Sine");
      n.in = {P("In", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"sin(%0)"}); add(n); }
    { NodeSpec n = node("FractF", kMath, "Fract");
      n.in = {P("In", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"fract(%0)"}); add(n); }
    // Rounds to the nearest of `Steps` levels over 0..1: the quantisation behind a poster, palette or
    // retro look. Cel shading wants a light ramp instead; that is Cel Bands in the Stylize category.
    { NodeSpec n = node("Posterize", kMath, "Posterize");
      n.in = {P("In", Kind::Float, "0.0"), P("Steps", Kind::Float, "4.0")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"floor(clamp(%0, 0.0, 1.0) * max(%1, 1.0) + 0.5) / max(%1, 1.0)"}); add(n); }

    // ---- Vector math ----
    { NodeSpec n = node("AddV3", kVector, "Add (Vector 3)");
      n.in = {P("A", Kind::Vec3, "vec3(0.0)"), P("B", Kind::Vec3, "vec3(0.0)")};
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"%0 + %1"}); add(n); }
    { NodeSpec n = node("MultiplyV3", kVector, "Multiply (Vector 3)");
      n.in = {P("A", Kind::Vec3, "vec3(1.0)"), P("B", Kind::Vec3, "vec3(1.0)")};
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"%0 * %1"}); add(n); }
    { NodeSpec n = node("LerpV3", kVector, "Lerp (Vector 3)");
      n.in = {P("A", Kind::Vec3, "vec3(0.0)"), P("B", Kind::Vec3, "vec3(1.0)"), P("T", Kind::Float, "0.5")};
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"mix(%0, %1, %2)"}); add(n); }
    { NodeSpec n = node("Combine", kVector, "Combine RGB");
      n.in = {P("R", Kind::Float, "0.0"), P("G", Kind::Float, "0.0"), P("B", Kind::Float, "0.0")};
      n.out = {P("RGB", Kind::Vec3)}; n.outExpr = outs({"vec3(%0, %1, %2)"}); add(n); }
    { NodeSpec n = node("Split", kVector, "Split RGB");
      n.in = {P("RGB", Kind::Vec3, "vec3(0.0)")};
      n.out = {P("R", Kind::Float), P("G", Kind::Float), P("B", Kind::Float)};
      n.outExpr = outs({"%0.x", "%0.y", "%0.z"}); add(n); }
    { NodeSpec n = node("SubtractV3", kVector, "Subtract (Vector 3)");
      n.in = {P("A", Kind::Vec3, "vec3(0.0)"), P("B", Kind::Vec3, "vec3(0.0)")};
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"%0 - %1"}); add(n); }
    { NodeSpec n = node("DotV3", kVector, "Dot Product (Vector 3)");
      n.in = {P("A", Kind::Vec3, "vec3(0.0)"), P("B", Kind::Vec3, "vec3(0.0)")};
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"dot(%0, %1)"}); add(n); }
    // A colour times a mask: the usual way a rim, band or grain term is folded into a style.
    { NodeSpec n = node("ScaleV3", kVector, "Scale (Vector 3 by Float)");
      n.in = {P("A", Kind::Vec3, "vec3(1.0)"), P("B", Kind::Float, "1.0")};
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"%0 * %1"}); add(n); }
    { NodeSpec n = node("NormalizeV3", kVector, "Normalize (Vector 3)");
      n.in = {P("In", Kind::Vec3, "vec3(0.0, 0.0, 1.0)")};
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"normalize(%0)"}); add(n); }
    { NodeSpec n = node("SaturateV3", kVector, "Saturate (Vector 3)");
      n.in = {P("In", Kind::Vec3, "vec3(0.0)")};
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"clamp(%0, vec3(0.0), vec3(1.0))"}); add(n); }
    { NodeSpec n = node("Combine2", kVector, "Combine XY");
      n.in = {P("X", Kind::Float, "0.0"), P("Y", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Vec2)}; n.outExpr = outs({"vec2(%0, %1)"}); add(n); }
    // The other half of UV maths: split a vec2 so each component can be edited on its own.
    { NodeSpec n = node("Split2", kVector, "Split XY");
      n.in = {P("XY", Kind::Vec2, "vec2(0.0)")};
      n.out = {P("X", Kind::Float), P("Y", Kind::Float)};
      n.outExpr = outs({"%0.x", "%0.y"}); add(n); }
    // Rec.709 luma. Halftone, hatch and monochrome styles quantise this rather than a channel.
    { NodeSpec n = node("Luminance", kVector, "Luminance (Vector 3)");
      n.in = {P("In", Kind::Vec3, "vec3(0.0)")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"dot(%0, vec3(0.2126, 0.7152, 0.0722))"}); add(n); }
    // Saturation 1 keeps the colour, 0 gives its luma: Ghibli pastels and horror desaturation.
    { NodeSpec n = node("Desaturate", kVector, "Desaturate (Vector 3)");
      n.in = {P("In", Kind::Vec3, "vec3(0.0)"), P("Saturation", Kind::Float, "1.0")};
      n.out = {P("Out", Kind::Vec3)};
      n.outExpr = outs({"mix(vec3(dot(%0, vec3(0.2126, 0.7152, 0.0722))), %0, %1)"}); add(n); }
    // Rotates the hue by Degrees, keeping luma and saturation. Cool-shadow anime palettes are a
    // rotation of the base colour, not a second colour the artist has to pick.
    { NodeSpec n = node("HueRotate", kVector, "Hue Rotate (Vector 3)");
      n.in = {P("In", Kind::Vec3, "vec3(1.0)"), P("Degrees", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Vec3)};
      n.outExpr = outs({"ukiyo_hue_rotate(%0, %1)"});
      n.helper = QStringLiteral(
          "vec3 ukiyo_hue_rotate(vec3 c, float degrees) {\n"
          "    float a = radians(degrees);\n"
          "    float co = cos(a);\n"
          "    float si = sin(a);\n"
          "    vec3 k = vec3(0.57735027);   // the grey axis, normalised\n"
          "    // Rodrigues' rotation around the grey axis: luma is kept, the hue turns.\n"
          "    return c * co + cross(k, c) * si + k * dot(k, c) * (1.0 - co);\n"
          "}");
      add(n); }

    // ---- Texture ----
    { NodeSpec n = node("SampleTexture2D", kTexture, "Sample Texture 2D");
      n.param = ParamKind::Texture;
      n.in = {P("UV", Kind::Vec2, "uv")};
      n.out = {P("RGB", Kind::Vec3), P("A", Kind::Float)};
      n.outExpr = outs({"texture(tex%I, %0).rgb", "texture(tex%I, %0).a"});
      n.decl = QStringLiteral("layout(set = 1, binding = %B) uniform sampler2D tex%I;");
      add(n); }

    // Normal map: turns a tangent-space map sample into a world-space normal. The tangent frame
    // comes from screen-space derivatives (no vertex tangents needed), so it works on any mesh.
    { NodeSpec n = node("NormalMap", kTexture, "Normal Map");
      n.in = {P("Map", Kind::Vec3, "vec3(0.5, 0.5, 1.0)"), P("Strength", Kind::Float, "1.0")};
      n.out = {P("Normal", Kind::Vec3)};
      n.outExpr = outs({"ukiyo_normal_map(%0, %1, normalize(normal), uv, v_world)"});
      n.helper = QStringLiteral(
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
          "}");
      add(n); }

    // ---- Procedural ----
    // Patterns built from maths alone, so a style needs no image: noise for grain and paper, dots and
    // lines for manga screentone and hatching, a Bayer matrix for dithered retro edges.
    { NodeSpec n = node("WhiteNoise", kProcedural, "White Noise");
      n.in = {P("UV", Kind::Vec2, "uv"), P("Scale", Kind::Float, "64.0")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"fract(sin(dot(%0 * %1, vec2(12.9898, 78.233))) * 43758.5453)"}); add(n); }
    { NodeSpec n = node("ValueNoise", kProcedural, "Value Noise");
      n.in = {P("UV", Kind::Vec2, "uv"), P("Scale", Kind::Float, "8.0")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_value_noise(%0 * %1)"});
      n.helper = QStringLiteral(
          "float ukiyo_value_noise(vec2 p) {\n"
          "    vec2 i = floor(p);\n"
          "    vec2 f = fract(p);\n"
          "    vec2 u = f * f * (3.0 - 2.0 * f);\n"
          "    float a = fract(sin(dot(i, vec2(12.9898, 78.233))) * 43758.5453);\n"
          "    float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(12.9898, 78.233))) * 43758.5453);\n"
          "    float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(12.9898, 78.233))) * 43758.5453);\n"
          "    float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(12.9898, 78.233))) * 43758.5453);\n"
          "    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);\n"
          "}");
      add(n); }
    // 4x4 Bayer matrix over the framebuffer. Compare a value against it to break an edge into dots, the
    // way the dithered cel fades and retro console gradients do.
    { NodeSpec n = node("BayerDither", kProcedural, "Ordered Dither (Bayer 4x4)");
      n.in = {P("Pixel Size", Kind::Float, "1.0")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_bayer4(%0)"});
      n.helper = QStringLiteral(
          "float ukiyo_bayer4(float pixelSize) {\n"
          "    const int bayer[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);\n"
          "    vec2 p = floor(gl_FragCoord.xy / max(pixelSize, 1.0));\n"
          "    int i = int(mod(p.y, 4.0)) * 4 + int(mod(p.x, 4.0));\n"
          "    return (float(bayer[i]) + 0.5) / 16.0;\n"
          "}");
      add(n); }
    // Manga screentone: a rotated dot grid whose dots grow with Value. Out is 1 inside a dot.
    { NodeSpec n = node("Halftone", kProcedural, "Halftone Dots");
      n.in = {P("UV", Kind::Vec2, "uv"), P("Frequency", Kind::Float, "24.0"),
              P("Angle", Kind::Float, "45.0"), P("Value", Kind::Float, "0.5")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_halftone(%0, %1, %2, %3)"});
      n.helper = QStringLiteral(
          "float ukiyo_halftone(vec2 uv, float frequency, float angleDegrees, float value) {\n"
          "    float a = radians(angleDegrees);\n"
          "    mat2 rot = mat2(cos(a), -sin(a), sin(a), cos(a));\n"
          "    vec2 p = rot * (uv * max(frequency, 1.0));\n"
          "    float d = length(fract(p) - 0.5);\n"
          "    float w = fwidth(d) + 1e-5;\n"
          "    float r = sqrt(clamp(value, 0.0, 1.0)) * 0.7071;\n"
          "    return 1.0 - smoothstep(r - w, r + w, d);\n"
          "}");
      add(n); }
    // Pencil or ink hatching: rotated stripes whose width grows with Value. Out is 1 on a line.
    { NodeSpec n = node("HatchLines", kProcedural, "Hatch Lines");
      n.in = {P("UV", Kind::Vec2, "uv"), P("Frequency", Kind::Float, "40.0"),
              P("Angle", Kind::Float, "45.0"), P("Value", Kind::Float, "0.35")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_hatch(%0, %1, %2, %3)"});
      n.helper = QStringLiteral(
          "float ukiyo_hatch(vec2 uv, float frequency, float angleDegrees, float value) {\n"
          "    float a = radians(angleDegrees);\n"
          "    float t = (uv.x * cos(a) - uv.y * sin(a)) * max(frequency, 1.0);\n"
          "    float f = abs(fract(t) - 0.5) * 2.0;\n"
          "    float w = clamp(value, 0.0, 1.0);\n"
          "    float aa = fwidth(t) + 1e-4;\n"
          "    return 1.0 - smoothstep(w - aa, w + aa, f);\n"
          "}");
      add(n); }
    // CRT or hologram bars scrolling over the framebuffer. Out is 0..1, one cycle per Line Height pixels.
    { NodeSpec n = node("Scanlines", kProcedural, "Scanlines");
      n.in = {P("Line Height", Kind::Float, "3.0"), P("Speed", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_scanlines(%0, %1)"});
      n.helper = QStringLiteral(
          "float ukiyo_scanlines(float lineHeight, float speed) {\n"
          "    float y = gl_FragCoord.y / max(lineHeight, 1.0) + frame.screenParams.z * speed;\n"
          "    return 0.5 + 0.5 * cos(y * 6.2831853);\n"
          "}");
      add(n); }
    // Blocks the UVs into a coarse grid, so a texture reads as big pixels: retro and low-res looks.
    { NodeSpec n = node("PixelateUV", kProcedural, "Pixelate UV");
      n.in = {P("UV", Kind::Vec2, "uv"), P("Blocks", Kind::Float, "64.0")};
      n.out = {P("Out", Kind::Vec2)};
      n.outExpr = outs({"(floor(%0 * max(%1, 1.0)) + 0.5) / max(%1, 1.0)"}); add(n); }
    { NodeSpec n = node("TilingAndOffset", kProcedural, "Tiling And Offset (UV)");
      n.in = {P("UV", Kind::Vec2, "uv"), P("Tiling", Kind::Vec2, "vec2(1.0)"),
              P("Offset", Kind::Vec2, "vec2(0.0)")};
      n.out = {P("Out", Kind::Vec2)};
      n.outExpr = outs({"%0 * %1 + %2"}); add(n); }

    // ---- Stylize ----
    // The anime building blocks: the rim mask, the cel ramp, the hard specular shapes, the matcap and
    // face-SDF lookups. Each one is a small function over the frame's sun and camera, so a style built
    // from them reacts to the scene's lights the same way the engine's own toon shaders do.
    // Rim mask: 1 where the surface turns away from the camera. Every rim light and contour shade is
    // this times a colour.
    { NodeSpec n = node("Fresnel", kStylize, "Fresnel (Rim Mask)");
      n.in = {P("Normal", Kind::Vec3, "normal"), P("Power", Kind::Float, "4.0")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"pow(1.0 - clamp(dot(normalize(%0), normalize(frame.camPos.xyz - v_world)), 0.0, 1.0), max(%1, 0.0001))"});
      add(n); }
    // The cel ramp. Light is the term to band (usually N.L); Bands is the number of tones, 2 being the
    // classic hard cel and 4 the softer RealToon banding; Softness widens each edge to a gradient.
    { NodeSpec n = node("CelBands", kStylize, "Cel Bands (Ramp)");
      n.in = {P("Light", Kind::Float, "0.5"), P("Bands", Kind::Float, "2.0"), P("Softness", Kind::Float, "0.1")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_cel_bands(%0, %1, %2)"});
      n.helper = QStringLiteral(
          "float ukiyo_cel_bands(float light, float bands, float softness) {\n"
          "    float n = max(bands, 2.0);\n"
          "    float t = clamp(light, 0.0, 1.0) * (n - 1.0);\n"
          "    float i = floor(t);\n"
          "    float f = t - i;\n"
          "    float s = clamp(softness, 0.0, 1.0);\n"
          "    float e = s > 0.0001 ? smoothstep(0.5 - 0.5 * s, 0.5 + 0.5 * s, f) : step(0.5, f);\n"
          "    return min(i + e, n - 1.0) / (n - 1.0);\n"
          "}");
      add(n); }
    // The hard, flat specular shape anime hair, eyes and lacquer use instead of a glossy lobe.
    { NodeSpec n = node("SpecBand", kStylize, "Specular Band");
      n.in = {P("Normal", Kind::Vec3, "normal"), P("Threshold", Kind::Float, "0.9"),
              P("Softness", Kind::Float, "0.02")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_spec_band(normalize(%0), %1, %2)"});
      n.helper = QStringLiteral(
          "float ukiyo_spec_band(vec3 N, float threshold, float softness) {\n"
          "    vec3 V = normalize(frame.camPos.xyz - v_world);\n"
          "    vec3 H = normalize(normalize(frame.sunDir.xyz) + V);\n"
          "    float nh = max(dot(N, H), 0.0);\n"
          "    float s = max(softness, 1e-4);\n"
          "    return smoothstep(threshold - s, threshold + s, nh);\n"
          "}");
      add(n); }
    // Two-lobe anisotropic highlight: the ring that runs across anime hair. The strand direction is the
    // normal shifted along world up, which is what a hair card's normal already approximates.
    { NodeSpec n = node("HairSpecular", kStylize, "Hair Specular (Anisotropic)");
      n.in = {P("Normal", Kind::Vec3, "normal"), P("Shift", Kind::Float, "0.3"),
              P("Exponent", Kind::Float, "40.0"), P("Strength", Kind::Float, "1.0")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_hair_spec(normalize(%0), %1, %2, %3)"});
      n.helper = QStringLiteral(
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
          "}");
      add(n); }
    // View-space normal as a UV, for a matcap: the painted studio reflection on eyes, hair and chrome.
    { NodeSpec n = node("MatcapUV", kStylize, "Matcap UV");
      n.in = {P("Normal", Kind::Vec3, "normal")};
      n.out = {P("Out", Kind::Vec2)};
      n.outExpr = outs({"ukiyo_matcap_uv(normalize(%0))"});
      n.helper = QStringLiteral(
          "vec2 ukiyo_matcap_uv(vec3 N) {\n"
          "    vec3 vn = (frame.view * vec4(N, 0.0)).xyz;\n"
          "    return vn.xy * 0.5 + 0.5;\n"
          "}");
      add(n); }
    // The sun's angle around the head, from frame.toonD (the head forward the engine keeps per frame).
    // x is left/right of the face, y front/back, both -1..1: the pair a face SDF map is indexed by.
    { NodeSpec n = node("FaceLightAngle", kStylize, "Face Light Angle");
      n.out = {P("Out", Kind::Vec2)};
      n.outExpr = outs({"ukiyo_face_light_angle()"});
      n.helper = QStringLiteral(
          "vec2 ukiyo_face_light_angle() {\n"
          "    vec2 Lh = frame.sunDir.xz;\n"
          "    float lh = length(Lh);\n"
          "    if (lh < 1e-4) return vec2(0.0);\n"
          "    Lh /= lh;\n"
          "    vec2 f = normalize(frame.toonD.xy + vec2(1e-5, 0.0));\n"
          "    vec2 r = vec2(f.y, -f.x);\n"
          "    return vec2(dot(r, Lh), dot(f, Lh));\n"
          "}");
      add(n); }
    // Face shadow from an SDF map: the sampled SDF against a threshold the head angle moves. Out is 1
    // where the face is lit. A map stored the other way round is corrected with Bias or One Minus.
    { NodeSpec n = node("FaceSDFTerm", kStylize, "Face Shadow (SDF)");
      n.in = {P("SDF", Kind::Float, "0.5"), P("Bias", Kind::Float, "0.0"), P("Softness", Kind::Float, "0.05")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_face_sdf(%0, %1, %2)"});
      n.helper = QStringLiteral(
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
          "}");
      add(n); }
    // Three-stop ramp: the tone map a cel style puts under its banded light term. T 0 gives A, 0.5 gives
    // B and 1 gives C.
    { NodeSpec n = node("Gradient3", kStylize, "Gradient (3 Stops)");
      n.in = {P("A", Kind::Vec3, "vec3(0.0)"), P("B", Kind::Vec3, "vec3(0.5)"),
              P("C", Kind::Vec3, "vec3(1.0)"), P("T", Kind::Float, "0.5")};
      n.out = {P("Out", Kind::Vec3)};
      n.outExpr = outs({"mix(mix(%0, %1, clamp(%3 * 2.0, 0.0, 1.0)), mix(%1, %2, clamp(%3 * 2.0 - 1.0, 0.0, 1.0)),"
                        " clamp(%3 * 2.0 - 1.0, 0.0, 1.0))"});
      add(n); }


    // ---- NPR ----
    // The ramp and ILM (illustration light map) blocks the character shaders are built from: the same
    // formulas as tools/endfield/EndfieldHybrid.body.glsl and NahidaFace.body.glsl, exposed as nodes so
    // a graph can reproduce a hand-written character shader and be compared against it term for term.
    // An ILM packs four masks into one texture. Both engine packings are covered: Endfield reads
    // r = metallic/spec, g = roughness, b = occlusion, a = style ramp; Nahida reads r = spec,
    // g = ambient occlusion, b = highlight mask, a = material type (which picks the ramp row).
    { NodeSpec n = node("ILMUnpack", kNpr, "ILM Unpack");
      n.in = {P("RGB", Kind::Vec3, "vec3(0.5)"), P("A", Kind::Float, "0.0")};
      n.out = {P("Spec (R)", Kind::Float), P("AO (G)", Kind::Float), P("Mask (B)", Kind::Float),
               P("Type (A)", Kind::Float)};
      n.outExpr = outs({"%0.r", "%0.g", "%0.b", "%1"}); add(n); }
    // Row select by material type, as BodyAndHair does it: five exposed rows and hard steps at the
    // midpoints of the five enum values 0, 0.3, 0.5, 0.7 and 1 (0.15, 0.4, 0.6, 0.85).
    { NodeSpec n = node("ILMRampRow", kNpr, "ILM Ramp Row (5 Types)");
      n.in = {P("Type", Kind::Float, "0.0"), P("Row 0", Kind::Float, "1.0"), P("Row 1", Kind::Float, "2.0"),
              P("Row 2", Kind::Float, "3.0"), P("Row 3", Kind::Float, "4.0"), P("Row 4", Kind::Float, "5.0")};
      n.out = {P("Row", Kind::Float)};
      n.outExpr = outs({"mix(mix(mix(mix(%5, %4, step(%0, 0.85)), %3, step(%0, 0.6)), %2, step(%0, 0.4)),"
                        " %1, step(%0, 0.15))"}); add(n); }
    // Day/night factor for the ramp row: a negative override follows the sun's height, 0..1 forces
    // night..day. The night row is the day row shifted by half the atlas.
    { NodeSpec n = node("DayNight", kNpr, "Day Night Factor");
      n.in = {P("Override", Kind::Float, "-1.0")};
      n.out = {P("Out", Kind::Float)};
      n.outExpr = outs({"ukiyo_day_night(%0)"});
      n.helper = QStringLiteral(
          "float ukiyo_day_night(float dayOverride) {\n"
          "    if (dayOverride < 0.0) return (normalize(frame.sunDir.xyz).y + 1.0) * 0.5;\n"
          "    return clamp(dayOverride, 0.0, 1.0);\n"
          "}");
      add(n); }
    // UV into a ramp atlas row: U is the shadow term clamped off the seam, V is the row's centre, and
    // Night shifts to the night half of the atlas. Feed it to a Sample Texture 2D on the ramp map.
    { NodeSpec n = node("RampUV", kNpr, "Ramp UV (Row)");
      n.in = {P("Lit", Kind::Float, "0.5"), P("Row", Kind::Float, "1.0"), P("Rows", Kind::Float, "10.0"),
              P("Night", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Vec2)};
      n.outExpr = outs({"vec2(clamp(%0, 0.003, 0.997), (%1 - 0.5) / max(%2, 1.0) + 0.5 * clamp(%3, 0.0, 1.0))"});
      add(n); }
    // The ramp lookup itself: one sampler, two fetches. Day and night are separate atlas rows, so the
    // engine mixes two colours; blending the coordinate instead would sample whatever rows lie between
    // them. Both fetches come from the same slot, which is why this is a node and not a RampUV plus a
    // Sample Texture 2D.
    { NodeSpec n = node("RampSample", kNpr, "Ramp Sample (Day/Night)");
      n.param = ParamKind::Texture;
      n.in = {P("U", Kind::Float, "0.5"), P("Row", Kind::Float, "1.0"), P("Rows", Kind::Float, "10.0"),
              P("Day", Kind::Float, "1.0")};
      n.out = {P("RGB", Kind::Vec3)};
      n.outExpr = outs({"ukiyo_ramp_sample(tex%I, %0, %1, %2, %3)"});
      n.decl = QStringLiteral("layout(set = 1, binding = %B) uniform sampler2D tex%I;");
      n.helper = QStringLiteral(
          "vec3 ukiyo_ramp_sample(sampler2D rampTex, float u, float row, float rows, float day) {\n"
          "    float v = (row - 0.5) / max(rows, 1.0);\n"
          "    float cu = clamp(u, 0.003, 0.997);\n"
          "    return mix(texture(rampTex, vec2(cu, v + 0.5)).rgb, texture(rampTex, vec2(cu, v)).rgb,\n"
          "             clamp(day, 0.0, 1.0));\n"
          "}");
      add(n); }
    // The shadow ramp coordinate, both engine variants behind one switch. Legacy (Kaze 0) is the
    // squared half-Lambert through a 0.3 edge; Kaze 1 is GetShadow: half-Lambert times AO, forced lit
    // where AO reaches 0.9, then a smoothstep over [0.5 + offset - width, 0.5 + offset]. Lit is 1 past
    // the top edge, where the ramp colour is forced to white.
    { NodeSpec n = node("ShadowRampU", kNpr, "Shadow Ramp U (ILM / Kaze)");
      n.in = {P("Normal", Kind::Vec3, "normal"), P("AO", Kind::Float, "1.0"), P("Offset", Kind::Float, "0.0"),
              P("Softness", Kind::Float, "1.0"), P("Kaze", Kind::Float, "0.0")};
      n.out = {P("U", Kind::Float), P("Lit", Kind::Float)};
      n.outExpr = outs({"ukiyo_shadow_ramp_u(%0, %1, %2, %3, %4).x",
                        "ukiyo_shadow_ramp_u(%0, %1, %2, %3, %4).y"});
      n.helper = QStringLiteral(
          "vec2 ukiyo_shadow_ramp_u(vec3 N, float ao, float offset, float softness, float kaze) {\n"
          "    float ndl = dot(normalize(N), normalize(frame.sunDir.xyz));\n"
          "    float edge = max(0.1 * softness, 1e-4);\n"
          "    float legacy = clamp(smoothstep(0.3 + offset - edge, 0.3 + offset + edge,\n"
          "                                    pow(clamp(ndl, 0.0, 1.0) * 0.5 + 0.5, 2.0)), 0.003, 0.997);\n"
          "    if (kaze < 0.5) return vec2(legacy, 0.0);\n"
          "    float kazeAo = clamp(ao, 0.0, 1.0);\n"
          "    float s = clamp(2.0 * (ndl * 0.5 + 0.5) * kazeAo, 0.0, 1.0);\n"
          "    s = mix(s, 1.0, step(0.9, kazeAo));\n"
          "    float top = 0.5 + offset;\n"
          "    return vec2(clamp(smoothstep(top - edge, top, s), 0.003, 0.997), step(top, s));\n"
          "}");
      add(n); }
    // UV into a colour-grade LUT: the luma of the colour against the LUT row. Sample it and mix the
    // result back over the colour by the grade strength.
    { NodeSpec n = node("LutUV", kNpr, "LUT UV (Grade)");
      n.in = {P("Color", Kind::Vec3, "vec3(0.5)"), P("Row", Kind::Float, "0.5")};
      n.out = {P("Out", Kind::Vec2)};
      n.outExpr = outs({"vec2(clamp(dot(%0, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0), %1)"}); add(n); }
    // Face shadow from a signed-distance atlas: the exact logic of EndfieldHybrid and NahidaFace,
    // exposed as a single node. The head's forward and right come from the frame (toonD, the character's
    // head direction) when the face shadow is on, else from the material vectors. Out is 1 where the
    // face is in shadow. The node owns the sampler so it can fetch the mirrored UV itself.
    { NodeSpec n = node("NprFaceSdf", kNpr, "Face SDF Shadow (Kaze)");
      n.param = ParamKind::Texture;
      n.in = {P("UV", Kind::Vec2, "uv"), P("Threshold", Kind::Float, "0.48"), P("Softness", Kind::Float, "0.08"),
              P("Forward (Material)", Kind::Vec3, "vec3(0.0, 0.0, 1.0)"),
              P("Right (Material)", Kind::Vec3, "vec3(1.0, 0.0, 0.0)")};
      n.out = {P("Shadow", Kind::Float)};
      n.outExpr = outs({"ukiyo_npr_face_sdf(tex%I, %0, %1, %2, %3, %4)"});
      n.decl = QStringLiteral("layout(set = 1, binding = %B) uniform sampler2D tex%I;");
      n.helper = QStringLiteral(
          "float ukiyo_npr_face_sdf(sampler2D sdfTex, vec2 uv, float threshold, float softness, vec3 matFwd, vec3 matRight) {\n"
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
          "}");
      add(n); }
    // UV into a specular ramp: N.H against the paint's style value (ILM alpha on the Endfield body).
    { NodeSpec n = node("SpecRampUV", kNpr, "Spec Ramp UV (N.H)");
      n.in = {P("Normal", Kind::Vec3, "normal"), P("Style", Kind::Float, "0.0")};
      n.out = {P("Out", Kind::Vec2)};
      n.outExpr = outs({"ukiyo_spec_ramp_uv(%0, %1)"});
      n.helper = QStringLiteral(
          "vec2 ukiyo_spec_ramp_uv(vec3 N, float style) {\n"
          "    vec3 V = normalize(frame.camPos.xyz - v_world);\n"
          "    vec3 H = normalize(normalize(frame.sunDir.xyz) + V);\n"
          "    return vec2(clamp(dot(normalize(N), H), 0.0, 1.0), style);\n"
          "}");
      add(n); }

    // ---- Masters (one per graph) ----
    // Hybrid Master: the full Endfield-style character shader. Blends a PBR base with an NPR base,
    // then adds matcap, specular, rim, hair highlight, wetness and emission.
    //
    // Inputs (28 total):
    //   Base Color, Metallic, Smoothness, Normal, Baked Light, Clear Coat, Clear Coat Smoothness
    //   Ramp Color, Occlusion, Shadow, Scene Light, Light Wrap, Shadow Mask, Shadow Mask Mix
    //   Ramp Blend, Shadow Floor, Ambient, Lit Boost, Normal Shade
    //   SDF Shadow, Shadow Tone
    //   Rim Light, Emission, Alpha
    //   Spec Mask, Hair Specular, Matcap, Wetness
    { NodeSpec n = node("HybridMaster", kMaster, "Hybrid Master");
      n.master = true;
      n.in = {P("Base Color", Kind::Vec3, "vec3(0.5)"), P("Metallic", Kind::Float, "0.0"),
              P("Smoothness", Kind::Float, "0.5"), P("Normal", Kind::Vec3, "normal"),
              P("Baked Light", Kind::Vec3, "vec3(1.0)"),
              P("Clear Coat", Kind::Float, "0.0"), P("Clear Coat Smoothness", Kind::Float, "0.5"),
              P("Ramp Color", Kind::Vec3, "vec3(1.0)"), P("Occlusion", Kind::Float, "1.0"),
              P("Shadow", Kind::Float, "1.0"), P("Scene Light", Kind::Float, "1.0"),
              P("Light Wrap", Kind::Float, "0.0"),
              P("Shadow Mask", Kind::Float, "0.0"), P("Shadow Mask Mix", Kind::Float, "0.0"),
              P("Ramp Blend", Kind::Float, "1.0"),
              P("Shadow Floor", Kind::Float, "0.0"), P("Ambient", Kind::Float, "0.55"),
              P("Lit Boost", Kind::Float, "1.0"), P("Normal Shade", Kind::Float, "0.0"),
              P("SDF Shadow", Kind::Float, "0.0"), P("Shadow Tone", Kind::Vec3, "vec3(0.5)"),
              P("Rim Light", Kind::Vec3, "vec3(0.0)"), P("Emission", Kind::Vec3, "vec3(0.0)"),
              P("Alpha", Kind::Float, "1.0"),
              P("Spec Mask", Kind::Float, "1.0"), P("Hair Specular", Kind::Vec3, "vec3(0.0)"),
              P("Matcap", Kind::Vec3, "vec3(0.0)"), P("Wetness", Kind::Float, "0.0")};
      n.surfaceFields = {QStringLiteral("baseColor"), QStringLiteral("metallic"), QStringLiteral("smoothness"),
                         QStringLiteral("normal"), QStringLiteral("bakedLight"), QStringLiteral("clearCoat"),
                         QStringLiteral("clearCoatSmoothness"), QStringLiteral("toonRamp"),
                         QStringLiteral("toonOcclusion"), QStringLiteral("toonShadow"), QStringLiteral("toonScene"),
                         QStringLiteral("toonWrap"), QStringLiteral("toonMask"), QStringLiteral("toonMaskMix"),
                         QStringLiteral("toonRampBlend"), QStringLiteral("toonFloor"), QStringLiteral("toonAmbient"),
                         QStringLiteral("toonBoost"), QStringLiteral("toonNormalShade"), QStringLiteral("sdfShadow"),
                         QStringLiteral("toonShade"), QStringLiteral("toonRim"), QStringLiteral("emission"),
                         QStringLiteral("alpha"), QStringLiteral("specMask"), QStringLiteral("hairSpecular"),
                         QStringLiteral("matcap"), QStringLiteral("wetness")};
      add(n); }

    // Outline Master: a shader that draws only the inverted-hull outline and nothing else (no
    // forward pass at all). Discards the fragment when the main pass runs.
    { NodeSpec n = node("OutlineMaster", kMaster, "Outline Master");
      n.master = true;
      n.in = {};
      n.surfaceFields = {};
      add(n); }

    // Face Master: a specialised master for character faces. Like the NPR Master, it shades with a
    // ramp under hemisphere ambient, but the threshold is driven by the face SDF shadow rather than
    // N.L, and there are no spec or metallic inputs.
    //
    //   SDF Shadow      0 = fully lit, 1 = fully shadowed (from NprFaceSdf or FaceSDFTerm)
    //   Ambient         hemisphere ambient strength
    //   Shadow Tone     multiplied into the ramp shadow
    //   Rim Light       additive rim light
    { NodeSpec n = node("FaceMaster", kMaster, "Face Master");
      n.master = true;
      n.in = {P("Base Color", Kind::Vec3, "vec3(0.5)"), P("Ramp Color", Kind::Vec3, "vec3(1.0)"),
              P("SDF Shadow", Kind::Float, "0.0"), P("Shadow Tone", Kind::Vec3, "vec3(0.5)"),
              P("Ambient", Kind::Float, "0.55"), P("Rim Light", Kind::Vec3, "vec3(0.0)"),
              P("Emission", Kind::Vec3, "vec3(0.0)"), P("Alpha", Kind::Float, "1.0")};
      n.surfaceFields = {QStringLiteral("baseColor"), QStringLiteral("toonRamp"), QStringLiteral("toonShadow"),
                         QStringLiteral("toonShade"), QStringLiteral("toonAmbient"), QStringLiteral("toonRim"),
                         QStringLiteral("emission"), QStringLiteral("alpha")};
      add(n); }

    { NodeSpec n = node("PBRMaster", kMaster, "PBR Master");
      n.master = true;
      n.in = {P("Base Color", Kind::Vec3, "vec3(0.5)"), P("Metallic", Kind::Float, "0.0"),
              P("Smoothness", Kind::Float, "0.5"), P("Normal", Kind::Vec3, "normal"),
              P("Emission", Kind::Vec3, "vec3(0.0)"), P("Alpha", Kind::Float, "1.0")};
      n.surfaceFields = {QStringLiteral("baseColor"), QStringLiteral("metallic"), QStringLiteral("smoothness"),
                         QStringLiteral("normal"), QStringLiteral("emission"), QStringLiteral("alpha")};
      add(n); }
    // Complex Lit: PBR plus a normal map input and a clear-coat layer (a second, sharper highlight
    // over the base, as on car paint or varnish). Clear Coat 0 gives plain PBR.
    { NodeSpec n = node("ComplexLitMaster", kMaster, "Complex Lit Master");
      n.master = true;
      n.in = {P("Base Color", Kind::Vec3, "vec3(0.5)"), P("Metallic", Kind::Float, "0.0"),
              P("Smoothness", Kind::Float, "0.5"), P("Normal", Kind::Vec3, "normal"),
              P("Emission", Kind::Vec3, "vec3(0.0)"), P("Alpha", Kind::Float, "1.0"),
              P("Clear Coat", Kind::Float, "0.0"), P("Clear Coat Smoothness", Kind::Float, "0.5")};
      n.surfaceFields = {QStringLiteral("baseColor"), QStringLiteral("metallic"), QStringLiteral("smoothness"),
                         QStringLiteral("normal"), QStringLiteral("emission"), QStringLiteral("alpha"),
                         QStringLiteral("clearCoat"), QStringLiteral("clearCoatSmoothness")};
      add(n); }
    // Baked Lit: no sun or sky term. The base colour is multiplied by a baked light value
    // (a lightmap or a flat colour), so the surface shows the bake and nothing else.
    { NodeSpec n = node("BakedLitMaster", kMaster, "Baked Lit Master");
      n.master = true;
      n.in = {P("Base Color", Kind::Vec3, "vec3(0.5)"), P("Baked Light", Kind::Vec3, "vec3(1.0)"),
              P("Emission", Kind::Vec3, "vec3(0.0)"), P("Alpha", Kind::Float, "1.0")};
      n.surfaceFields = {QStringLiteral("baseColor"), QStringLiteral("bakedLight"),
                         QStringLiteral("emission"), QStringLiteral("alpha")};
      add(n); }
    // NPR Master: cel shading grown into the generic ramp-shaded character master. The sun term is a
    // ramp: the shade colour (times the ramp lookup) where the light term falls below the threshold,
    // the base colour above it, with hemisphere ambient for sky and ground light.
    //
    // Every input past Alpha is identity at its default, so a graph wired only to Base Color, Shade
    // Color, Threshold and Softness shades exactly as the old two-tone Toon Master did. The id stays
    // "ToonMaster": it is the model name saved in every existing .ukshader.
    //
    //   Ramp Color      ramp atlas lookup multiplied into the shade colour
    //   Occlusion       ILM green or vertex colour red, on the ambient term
    //   Shadow          1 where the sun is visible, 0 in a realtime shadow
    //   Light Wrap      0 = terminator at N.L 0, 1 = half-Lambert (light wraps around the form)
    //   Shadow Mask     replaces the computed ramp: a painted style ramp or an ILM mask
    //   Shadow Mask Mix how far the mask replaces the ramp
    //   Ramp Blend      how strongly the ramp tone replaces the lit tone (1 = fully)
    //   Shadow Floor    keeps a floor of albedo in the shadow instead of letting it go black
    //   Ambient         hemisphere ambient strength
    //   Lit Boost       multiplies the lit side of the ramp
    //   Normal Shade    the Endfield 0.92..1.08 modulation of the lit side by the wrapped light
    //   Scene Light     0 drops the sun and sky terms, for a ramp atlas that already carries the
    //                   light response - how the Endfield and Nahida character shaders are authored
    { NodeSpec n = node("ToonMaster", kMaster, "NPR Master (Toon)");
      n.master = true;
      n.in = {P("Base Color", Kind::Vec3, "vec3(0.5)"), P("Shade Color", Kind::Vec3, "vec3(0.6, 0.5, 0.6)"),
              P("Threshold", Kind::Float, "0.0"), P("Softness", Kind::Float, "0.05"),
              P("Normal", Kind::Vec3, "normal"), P("Emission", Kind::Vec3, "vec3(0.0)"),
              P("Alpha", Kind::Float, "1.0"),
              P("Ramp Color", Kind::Vec3, "vec3(1.0)"), P("Occlusion", Kind::Float, "1.0"),
              P("Shadow", Kind::Float, "1.0"), P("Scene Light", Kind::Float, "1.0"),
              P("Light Wrap", Kind::Float, "0.0"),
              P("Shadow Mask", Kind::Float, "0.0"), P("Shadow Mask Mix", Kind::Float, "0.0"),
              P("Ramp Blend", Kind::Float, "1.0"),
              P("Shadow Floor", Kind::Float, "0.0"), P("Ambient", Kind::Float, "0.55"),
              P("Lit Boost", Kind::Float, "1.0"), P("Normal Shade", Kind::Float, "0.0")};
      n.surfaceFields = {QStringLiteral("baseColor"), QStringLiteral("toonShade"), QStringLiteral("toonThreshold"),
                         QStringLiteral("toonSoftness"), QStringLiteral("normal"), QStringLiteral("emission"),
                         QStringLiteral("alpha"), QStringLiteral("toonRamp"), QStringLiteral("toonOcclusion"),
                         QStringLiteral("toonShadow"), QStringLiteral("toonScene"), QStringLiteral("toonWrap"),
                         QStringLiteral("toonMask"), QStringLiteral("toonMaskMix"),
                         QStringLiteral("toonRampBlend"), QStringLiteral("toonFloor"),
                         QStringLiteral("toonAmbient"), QStringLiteral("toonBoost"),
                         QStringLiteral("toonNormalShade")};
      add(n); }
    { NodeSpec n = node("UnlitMaster", kMaster, "Unlit Master");
      n.master = true;
      n.in = {P("Color", Kind::Vec3, "vec3(1.0)"), P("Alpha", Kind::Float, "1.0")};
      n.surfaceFields = {QStringLiteral("baseColor"), QStringLiteral("alpha")};
      add(n); }
    return s;
}

int paramCount(ParamKind k)
{
    switch (k) {
    case ParamKind::None: return 0;
    case ParamKind::Float: return 1;
    case ParamKind::Vec2: return 2;
    case ParamKind::Color: return 3;
    case ParamKind::Texture: return 1;
    }
    return 0;
}

QStringList defaultParams(ParamKind k)
{
    switch (k) {
    case ParamKind::Float: return {QStringLiteral("1")};
    case ParamKind::Vec2: return {QStringLiteral("0"), QStringLiteral("0")};
    case ParamKind::Color: return {QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1")};
    case ParamKind::Texture: return {QString()};
    case ParamKind::None: return {};
    }
    return {};
}

} // namespace

const std::vector<NodeSpec>& nodeSpecs()
{
    static const std::vector<NodeSpec> specs = buildSpecs();
    return specs;
}

const NodeSpec* findSpec(const QString& id)
{
    for (const NodeSpec& n : nodeSpecs())
        if (n.id == id) return &n;
    return nullptr;
}

ShaderNodeModel::ShaderNodeModel(const NodeSpec* spec)
    : spec_(spec)
    , params_(defaultParams(spec->param))
{}

void ShaderNodeModel::setParams(const QStringList& p)
{
    params_ = p;
    while (params_.size() < paramCount(spec_->param)) params_ << QString();
}

QString ShaderNodeModel::expression() const
{
    switch (spec_->param) {
    case ParamKind::None:
    case ParamKind::Texture:
        return QString();
    case ParamKind::Float:
        return glslFloat(params_.value(0).toDouble());
    case ParamKind::Vec2:
        return QStringLiteral("vec2(%1, %2)").arg(glslFloat(params_.value(0).toDouble()),
                                                  glslFloat(params_.value(1).toDouble()));
    case ParamKind::Color:
        return QStringLiteral("vec3(%1, %2, %3)").arg(glslFloat(params_.value(0).toDouble()),
                                                      glslFloat(params_.value(1).toDouble()),
                                                      glslFloat(params_.value(2).toDouble()));
    }
    return QString();
}

QString ShaderNodeModel::portCaption(PortType t, PortIndex i) const
{
    const auto& ports = (t == PortType::In) ? spec_->in : spec_->out;
    return (i < ports.size()) ? ports[i].name : QString();
}

unsigned int ShaderNodeModel::nPorts(PortType t) const
{
    if (t == PortType::In) return static_cast<unsigned int>(spec_->in.size());
    if (t == PortType::Out) return static_cast<unsigned int>(spec_->out.size());
    return 0;
}

NodeDataType ShaderNodeModel::dataType(PortType t, PortIndex i) const
{
    const auto& ports = (t == PortType::In) ? spec_->in : spec_->out;
    return ShaderValue(i < ports.size() ? ports[i].kind : Kind::Float).type();
}

std::shared_ptr<QtNodes::NodeData> ShaderNodeModel::outData(PortIndex i)
{
    return std::make_shared<ShaderValue>(i < spec_->out.size() ? spec_->out[i].kind : Kind::Float);
}

QWidget* ShaderNodeModel::embeddedWidget()
{
    if (spec_->param == ParamKind::None) return nullptr;
    if (!widget_) buildWidget();
    return widget_;
}

void ShaderNodeModel::buildWidget()
{
    auto spin = [](double v) {
        auto* sb = new QDoubleSpinBox;
        sb->setRange(-1.0e6, 1.0e6);
        sb->setDecimals(4);
        sb->setValue(v);
        sb->setMinimumWidth(70);
        return sb;
    };
    QWidget* value = nullptr;
    switch (spec_->param) {
    case ParamKind::None:
        return;
    case ParamKind::Float: {
        auto* sb = spin(params_.value(0).toDouble());
        QObject::connect(sb, &QDoubleSpinBox::valueChanged, sb, [this](double v) {
            params_[0] = QString::number(v, 'g', 10);
        });
        value = sb;
        break;
    }
    case ParamKind::Vec2: {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        for (int k = 0; k < 2; ++k) {
            auto* sb = spin(params_.value(k).toDouble());
            QObject::connect(sb, &QDoubleSpinBox::valueChanged, sb, [this, k](double v) {
                params_[k] = QString::number(v, 'g', 10);
            });
            h->addWidget(sb);
        }
        value = w;
        break;
    }
    case ParamKind::Color: {
        auto* b = new QPushButton;
        b->setMinimumWidth(90);
        auto paint = [b](const QColor& c) {
            b->setStyleSheet(QStringLiteral("background:%1; border:1px solid #111;").arg(c.name()));
        };
        auto current = [this] {
            return QColor::fromRgbF(params_.value(0).toDouble(), params_.value(1).toDouble(),
                                    params_.value(2).toDouble());
        };
        paint(current());
        QObject::connect(b, &QPushButton::clicked, b, [this, b, paint, current] {
            const QColor c = QColorDialog::getColor(current(), b, QStringLiteral("Color"));
            if (!c.isValid()) return;
            params_ = {QString::number(c.redF(), 'g', 10), QString::number(c.greenF(), 'g', 10),
                       QString::number(c.blueF(), 'g', 10)};
            paint(c);
        });
        value = b;
        break;
    }
    case ParamKind::Texture: {
        auto* e = new QLineEdit(params_.value(0));
        e->setPlaceholderText(QStringLiteral("Texture path"));
        e->setMinimumWidth(110);
        QObject::connect(e, &QLineEdit::textChanged, e, [this](const QString& t) { params_[0] = t; });
        // Falls through to the exposed-name field: a named texture is a material slot that a material can override.
        value = e;
        break;
    }
    }

    // Value widgets also get an "Exposed name" field. A name makes the value a material property.
    auto* holder = new QWidget;
    auto* v = new QVBoxLayout(holder);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    v->addWidget(value);
    auto* name = new QLineEdit(reference_);
    name->setPlaceholderText(QStringLiteral("Exposed name"));
    name->setMinimumWidth(110);
    QObject::connect(name, &QLineEdit::textChanged, name, [this](const QString& t) { reference_ = t.trimmed(); });
    v->addWidget(name);
    widget_ = holder;
}

QJsonObject ShaderNodeModel::save() const
{
    QJsonObject j = NodeDelegateModel::save();
    j[QStringLiteral("reference")] = reference_;
    QJsonArray p;
    for (const QString& s : params_) p.append(s);
    j[QStringLiteral("params")] = p;
    return j;
}

void ShaderNodeModel::load(QJsonObject const& json)
{
    reference_ = json.value(QStringLiteral("reference")).toString();
    QStringList p;
    for (const QJsonValue& v : json.value(QStringLiteral("params")).toArray()) p << v.toString();
    setParams(p);
}

std::shared_ptr<QtNodes::NodeDelegateModelRegistry> makeRegistry()
{
    auto reg = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for (const NodeSpec& spec : nodeSpecs()) {
        const NodeSpec* s = &spec;
        reg->registerModel<ShaderNodeModel>(
            [s]() { return std::make_unique<ShaderNodeModel>(s); }, spec.category);
    }
    return reg;
}

} // namespace sg
