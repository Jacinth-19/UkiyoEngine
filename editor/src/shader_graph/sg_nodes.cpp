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
    { NodeSpec n = node("Time", kInput, "Time");
      n.out = {P("Out", Kind::Float)}; n.outExpr = outs({"frame.screenParams.z"}); add(n); }
    { NodeSpec n = node("Normal", kInput, "Normal (World)");
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"normalize(normal)"}); add(n); }
    { NodeSpec n = node("WorldPosition", kInput, "Position (World)");
      n.out = {P("Out", Kind::Vec3)}; n.outExpr = outs({"v_world"}); add(n); }

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
      n.outExpr = outs({"ukiyo_normal_map(%0, %1, normal, uv, v_world)"});
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
          "    vec3 m = (mapSample * 2.0 - 1.0) * vec3(strength, strength, 1.0);\n"
          "    return normalize(TBN * m);\n"
          "}");
      add(n); }

    // ---- Masters (one per graph) ----
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
    // Toon Master: cel shading. The sun term is a two-tone ramp: the shade colour where N.L falls below
    // the threshold, the base colour above it (softness widens the edge). Hemisphere ambient adds the
    // sky and ground light, as in the built-in lit master.
    { NodeSpec n = node("ToonMaster", kMaster, "Toon Master");
      n.master = true;
      n.in = {P("Base Color", Kind::Vec3, "vec3(0.5)"), P("Shade Color", Kind::Vec3, "vec3(0.6, 0.5, 0.6)"),
              P("Threshold", Kind::Float, "0.0"), P("Softness", Kind::Float, "0.05"),
              P("Normal", Kind::Vec3, "normal"), P("Emission", Kind::Vec3, "vec3(0.0)"),
              P("Alpha", Kind::Float, "1.0")};
      n.surfaceFields = {QStringLiteral("baseColor"), QStringLiteral("toonShade"), QStringLiteral("toonThreshold"),
                         QStringLiteral("toonSoftness"), QStringLiteral("normal"), QStringLiteral("emission"),
                         QStringLiteral("alpha")};
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
