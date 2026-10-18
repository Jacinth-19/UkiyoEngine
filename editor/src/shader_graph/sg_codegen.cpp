#include "sg_codegen.h"

#include "sg_nodes.h"

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtNodes/DataFlowGraphModel>

#include <unordered_set>

namespace sg {

using QtNodes::ConnectionId;
using QtNodes::NodeId;
using QtNodes::PortType;

namespace {

// Replaces the placeholders in one output expression.
// Material set layout: textures at bindings 0-7, parameters at binding 15.
constexpr int kMaxGraphTextures = 8;

QString expand(QString tmpl, const QStringList& inputs, const QString& param, const QString& id, int slot)
{
    tmpl.replace(QStringLiteral("%P"), param);
    tmpl.replace(QStringLiteral("%I"), id);
    tmpl.replace(QStringLiteral("%B"), QString::number(slot));
    for (int k = inputs.size() - 1; k >= 0; --k)
        tmpl.replace(QLatin1Char('%') + QString::number(k), inputs[k]);
    return tmpl;
}

struct Gen {
    QtNodes::DataFlowGraphModel& graph;
    QStringList body;                        // statements inside ukiyo_surface()
    QStringList decls;                       // top-level declarations (textures)
    QHash<NodeId, QStringList> resolved;     // node -> output variable names
    QHash<NodeId, QString> ids;              // node -> short id used in names
    std::unordered_set<NodeId> visiting;
    int texSlot = 0;
    QStringList errors;
    QList<PropertyInfo> props;               // exposed values, in slot order
    QList<TextureInfo> textures;             // exposed textures, in binding order
    QHash<QString, NodeId> bodyOwner;        // body statement text -> the node that wrote it

    // Returns the GLSL for an exposed value. The first use of a name allocates its slot.
    QString propertyExpr(const QString& name, ParamKind kind, const QStringList& defaults)
    {
        for (const PropertyInfo& p : props) {
            if (p.name != name) continue;
            if (p.kind != kind) errors << QStringLiteral("Exposed name '%1' is used for two different types.").arg(name);
            return QStringLiteral("p%1%2").arg(p.slot).arg(kind == ParamKind::Float ? QStringLiteral(".x")
                                                          : kind == ParamKind::Vec2 ? QStringLiteral(".xy")
                                                                                    : QStringLiteral(".xyz"));
        }
        const int slot = static_cast<int>(props.size());
        props.append(PropertyInfo{name, kind, slot, defaults});
        return QStringLiteral("p%1%2").arg(slot).arg(kind == ParamKind::Float ? QStringLiteral(".x")
                                                    : kind == ParamKind::Vec2 ? QStringLiteral(".xy")
                                                                              : QStringLiteral(".xyz"));
    }

    QString idFor(NodeId id)
    {
        if (!ids.contains(id)) ids.insert(id, QStringLiteral("n%1").arg(ids.size() + 1));
        return ids.value(id);
    }

    // Returns the expression that feeds input port `port` of `id`.
    QString inputExpr(NodeId id, int port, const PortSpec& spec)
    {
        const auto conns = graph.connections(id, PortType::In, static_cast<QtNodes::PortIndex>(port));
        if (conns.empty()) return spec.def;
        const ConnectionId c = *conns.begin();
        const QStringList outs = resolve(c.outNodeId);
        if (static_cast<int>(c.outPortIndex) >= outs.size()) {
            errors << QStringLiteral("Input '%1' is connected to a missing output.").arg(spec.name);
            return spec.def;
        }
        return outs[static_cast<int>(c.outPortIndex)];
    }

    // Emits the statements for a node (once) and returns its output variable names.
    QStringList resolve(NodeId id)
    {
        if (resolved.contains(id)) return resolved.value(id);
        if (visiting.count(id)) {
            errors << QStringLiteral("The graph has a loop.");
            return {};
        }
        visiting.insert(id);

        auto* model = graph.delegateModel<ShaderNodeModel>(id);
        if (!model) {
            errors << QStringLiteral("A node has no shader model.");
            visiting.erase(id);
            return {};
        }
        const NodeSpec* spec = model->spec();
        QStringList inputs;
        for (int i = 0; i < static_cast<int>(spec->in.size()); ++i)
            inputs << inputExpr(id, i, spec->in[i]);

        const QString nid = idFor(id);
        QStringList outVars;
        if (!spec->master) {
            int slot = 0;   // texture binding slot, when the node declares a sampler
            if (!spec->decl.isEmpty()) {
                if (texSlot >= kMaxGraphTextures) {
                    errors << QStringLiteral("Too many textures: a graph supports %1 sampler nodes.")
                                  .arg(kMaxGraphTextures);
                    return {};
                }
                slot = texSlot++;
                decls << expand(spec->decl, {}, QString(), nid, slot);
                if (spec->param == ParamKind::Texture && !model->reference().isEmpty()) {
                    // An exposed texture is one material slot. Two nodes cannot share a name, and a name
                    // cannot be both a value and a texture.
                    const QString ref = model->reference();
                    bool taken = false;
                    for (const TextureInfo& t : textures) taken = taken || t.name == ref;
                    for (const PropertyInfo& p : props) taken = taken || p.name == ref;
                    if (taken) errors << QStringLiteral("Exposed name '%1' is used twice.").arg(ref);
                    else textures.append(TextureInfo{ref, slot, model->params().value(0)});
                }
            }
            // Helper functions are shared: emit each one once however many nodes use it.
            if (!spec->helper.isEmpty() && !decls.contains(spec->helper)) decls << spec->helper;
            QString param = model->expression();
            if (!model->reference().isEmpty()
                && (spec->param == ParamKind::Float || spec->param == ParamKind::Vec2 || spec->param == ParamKind::Color)) {
                param = propertyExpr(model->reference(), spec->param, model->params());
            }
            for (int k = 0; k < static_cast<int>(spec->out.size()) && k < spec->outExpr.size(); ++k) {
                const QString var = QStringLiteral("%1_%2").arg(nid).arg(k);
                const QString expr = expand(spec->outExpr[k], inputs, param, nid, slot);
                const QString stmt = QStringLiteral("    %1 %2 = %3;").arg(glslType(spec->out[k].kind), var, expr);
                body << stmt;
                bodyOwner.insert(stmt, id);
                outVars << var;
            }
        }
        visiting.erase(id);
        resolved.insert(id, outVars);
        return outVars;
    }
};

// Master nodes write their inputs into the SurfaceData fields.
void emitMaster(Gen& g, NodeId id, const NodeSpec* spec, QStringList& surface)
{
    for (int i = 0; i < static_cast<int>(spec->in.size()) && i < spec->surfaceFields.size(); ++i) {
        const QString expr = g.inputExpr(id, i, spec->in[i]);
        surface << QStringLiteral("    s.%1 = %2;").arg(spec->surfaceFields[i], expr);
    }
}

} // namespace

CodegenResult generate(QtNodes::DataFlowGraphModel& graph)
{
    CodegenResult out;
    Gen g{graph};

    // Find the master.
    std::vector<NodeId> masters;
    for (NodeId id : graph.allNodeIds()) {
        auto* m = graph.delegateModel<ShaderNodeModel>(id);
        if (m && m->spec()->master) masters.push_back(id);
    }
    if (masters.empty()) {
        out.errors << QStringLiteral("Add a master node (PBR Master or Unlit Master).");
        return out;
    }
    if (masters.size() > 1) {
        out.errors << QStringLiteral("A graph can have only one master node.");
        return out;
    }

    const NodeId masterId = masters.front();
    const NodeSpec* masterSpec = graph.delegateModel<ShaderNodeModel>(masterId)->spec();
    // A wired Alpha input makes the material transparent. The exception is an exposed Float at 1 or
    // more (the default Lit and Unlit alpha), which is opaque.
    for (int i = 0; i < masterSpec->surfaceFields.size(); ++i) {
        if (masterSpec->surfaceFields[i] != QLatin1String("alpha")) continue;
        const auto conns = graph.connections(masterId, QtNodes::PortType::In, static_cast<QtNodes::PortIndex>(i));
        out.transparent = !conns.empty();
        if (conns.empty()) continue;
        const ShaderNodeModel* src = graph.delegateModel<ShaderNodeModel>(conns.begin()->outNodeId);
        if (src && src->spec()->id == QLatin1String("Float") && !src->reference().isEmpty()
            && !src->params().isEmpty() && src->params().first().toFloat() >= 1.0f)
            out.transparent = false;
    }
    // An outline is a material contract rather than a surface field: shaders/graph_outline.vert reads
    // slots 0, 1 and 2 of the same block (colour, width, alpha). A graph asks for the outline pass by
    // exposing three values with exactly those names. They are allocated here, before the master is
    // walked, so they land in those slots whatever order the rest of the graph uses. The values do not
    // have to feed the master: the outline pass reads them straight from the block.
    {
        const QString names[3] = {QStringLiteral("outlineColor"), QStringLiteral("outlineWidth"),
                                  QStringLiteral("outlineAlpha")};
        const ParamKind kinds[3] = {ParamKind::Color, ParamKind::Float, ParamKind::Float};
        QStringList defaults[3];
        bool found[3] = {false, false, false};
        for (NodeId id : graph.allNodeIds()) {
            auto* m = graph.delegateModel<ShaderNodeModel>(id);
            if (!m || m->reference().isEmpty()) continue;
            for (int k = 0; k < 3; ++k) {
                if (found[k] || m->reference() != names[k] || m->spec()->param != kinds[k]) continue;
                found[k] = true;
                defaults[k] = m->params();
            }
        }
        if (found[0] && found[1] && found[2]) {
            for (int k = 0; k < 3; ++k) g.propertyExpr(names[k], kinds[k], defaults[k]);
            out.outline = true;
        }
    }

    QStringList surfaceLines;
    emitMaster(g, masterId, masterSpec, surfaceLines);
    out.errors << g.errors;

    // Only the master's own lines are emitted into the function; nodes it feeds are already in
    // g.body in dependency order.
    QStringList fn;
    fn << QStringLiteral("struct SurfaceData {")
       << QStringLiteral("    vec3 baseColor;")
       << QStringLiteral("    float metallic;")
       << QStringLiteral("    float smoothness;")
       << QStringLiteral("    vec3 normal;")
       << QStringLiteral("    vec3 emission;")
       << QStringLiteral("    vec3 bakedLight;   // baked light multiplier; 1 when no bake is connected")
       << QStringLiteral("    float alpha;")
       << QStringLiteral("    float clearCoat;            // 0 = none (clear-coat layer strength)")
       << QStringLiteral("    float clearCoatSmoothness;  // clear-coat highlight sharpness")
       << QStringLiteral("    vec3 toonShade;     // Toon Master: shade colour in the shadow band")
       << QStringLiteral("    float toonThreshold;// Toon Master: N.L where the band starts")
       << QStringLiteral("    float toonSoftness; // Toon Master: width of the band edge")
       << QStringLiteral("    vec3 toonRamp;      // NPR Master: ramp atlas colour, multiplied into the shade")
       << QStringLiteral("    float toonOcclusion;// NPR Master: ambient occlusion (ILM green, vertex colour red)")
       << QStringLiteral("    float toonShadow;   // NPR Master: 1 where the sun is visible")
       << QStringLiteral("    float toonScene;    // NPR Master: 0 = no sun or sky term, the ramp carries the light")
       << QStringLiteral("    float toonRampBlend;// NPR Master: how strongly the ramp tone replaces the lit tone")
       << QStringLiteral("    float toonWrap;     // NPR Master: 0 = N.L terminator, 1 = half-Lambert")
       << QStringLiteral("    float toonMask;     // NPR Master: replaces the ramp (painted style ramp, ILM mask)")
       << QStringLiteral("    float toonMaskMix;  // NPR Master: how far toonMask replaces the ramp")
       << QStringLiteral("    float toonFloor;    // NPR Master: albedo floor kept in the shadow")
       << QStringLiteral("    float toonAmbient;  // NPR Master: hemisphere ambient strength")
       << QStringLiteral("    float toonBoost;    // NPR Master: multiplies the lit term")
       << QStringLiteral("    float toonNormalShade; // NPR Master: 0.92..1.08 lit modulation by the wrap")
       << QStringLiteral("    vec3 toonRim;       // Face Master: additive rim light")
       << QStringLiteral("    float toon;         // 1 = NPR (Toon) Master lighting")
       << QStringLiteral("    float sdfShadow;    // Hybrid/Face: 0 = lit, 1 = shadowed")
       << QStringLiteral("    float specMask;     // Hybrid: specular mask")
       << QStringLiteral("    vec3 hairSpecular;  // Hybrid: anisotropic hair")
       << QStringLiteral("    vec3 matcap;        // Hybrid: matcap reflection")
       << QStringLiteral("    float wetness;      // Hybrid: rain coverage")
       << QStringLiteral("    float hybrid;       // 1 = Hybrid Master lighting")
       << QStringLiteral("    float face;         // 1 = Face Master lighting")
       << QStringLiteral("    float lit;   // 1 = shaded by the frame's sun and sky, 0 = unlit")
       << QStringLiteral("};")
       << QString()
       << QStringLiteral("SurfaceData ukiyo_surface(vec2 uv, vec3 normal) {")
       << QStringLiteral("    SurfaceData s;")
       << QStringLiteral("    s.baseColor = vec3(1.0);")
       << QStringLiteral("    s.metallic = 0.0;")
       << QStringLiteral("    s.smoothness = 0.5;")
       << QStringLiteral("    s.normal = normal;")
       << QStringLiteral("    s.emission = vec3(0.0);")
       << QStringLiteral("    s.bakedLight = vec3(1.0);")
       << QStringLiteral("    s.alpha = 1.0;")
       << QStringLiteral("    s.clearCoat = 0.0;")
       << QStringLiteral("    s.clearCoatSmoothness = 0.5;")
       << QStringLiteral("    s.toonShade = vec3(0.6, 0.5, 0.6);")
       << QStringLiteral("    s.toonThreshold = 0.0;")
       << QStringLiteral("    s.toonSoftness = 0.05;")
       << QStringLiteral("    s.toonRamp = vec3(1.0);")
       << QStringLiteral("    s.toonOcclusion = 1.0;")
       << QStringLiteral("    s.toonShadow = 1.0;")
       << QStringLiteral("    s.toonScene = 1.0;")
       << QStringLiteral("    s.toonRampBlend = 1.0;")
       << QStringLiteral("    s.toonWrap = 0.0;")
       << QStringLiteral("    s.toonMask = 0.0;")
       << QStringLiteral("    s.toonMaskMix = 0.0;")
       << QStringLiteral("    s.toonFloor = 0.0;")
       << QStringLiteral("    s.toonAmbient = 0.55;")
       << QStringLiteral("    s.toonBoost = 1.0;")
       << QStringLiteral("    s.toonNormalShade = 0.0;")
       << QStringLiteral("    s.toonRim = vec3(0.0);")
       << QStringLiteral("    s.toon = 0.0;")
       << QStringLiteral("    s.sdfShadow = 0.0;")
       << QStringLiteral("    s.specMask = 1.0;")
       << QStringLiteral("    s.hairSpecular = vec3(0.0);")
       << QStringLiteral("    s.matcap = vec3(0.0);")
       << QStringLiteral("    s.wetness = 0.0;")
       << QStringLiteral("    s.hybrid = 0.0;")
       << QStringLiteral("    s.face = 0.0;")
       << QStringLiteral("    s.lit = 1.0;");
    if (masterSpec->id == QLatin1String("UnlitMaster") || masterSpec->id == QLatin1String("BakedLitMaster"))
        surfaceLines << QStringLiteral("    s.lit = 0.0;");
    if (masterSpec->id == QLatin1String("ToonMaster"))
        surfaceLines << QStringLiteral("    s.toon = 1.0;");
    if (masterSpec->id == QLatin1String("FaceMaster"))
        surfaceLines << QStringLiteral("    s.face = 1.0;");
    if (masterSpec->id == QLatin1String("HybridMaster"))
        surfaceLines << QStringLiteral("    s.hybrid = 1.0;");
    if (masterSpec->id == QLatin1String("OutlineMaster")) {
        surfaceLines << QStringLiteral("    s.lit = 0.0;");
        surfaceLines << QStringLiteral("    s.alpha = 0.0;"); // Discarded by alpha clip or blend
    }
    fn << g.body << surfaceLines << QStringLiteral("    return s;") << QStringLiteral("}");
    out.surfaceFunction = fn.join('\n') + '\n';

    QStringList frag;
    frag << QStringLiteral("#version 450")
         << QStringLiteral("// Generated by the Ukiyo Shader Graph. Do not edit by hand.")
         << QStringLiteral("layout(location = 0) in vec2 v_uv;")
         << QStringLiteral("layout(location = 1) in vec3 v_normal;")
         << QStringLiteral("layout(location = 2) in vec3 v_world;")
         // graph.vert writes both, so a graph can read painted masks and a second UV set.
         << QStringLiteral("layout(location = 3) in vec4 v_color;   // COLOR_0, white when the mesh has none")
         << QStringLiteral("layout(location = 4) in vec2 v_uv1;     // TEXCOORD_1, a copy of v_uv when the mesh has none")
         << QStringLiteral("layout(location = 0) out vec4 o_color;")
         << QStringLiteral("layout(set = 0, binding = 0, std140) uniform FrameBlock {")
         << QStringLiteral("    mat4 view;")
         << QStringLiteral("    mat4 proj;")
         << QStringLiteral("    mat4 viewProj;")
         << QStringLiteral("    mat4 invViewProj;")
         << QStringLiteral("    mat4 shadowMatrix[3];")
         << QStringLiteral("    vec4 camPos;")
         << QStringLiteral("    vec4 sunDir;")
         << QStringLiteral("    vec4 sunColor;")
         << QStringLiteral("    vec4 skyColor;")
         << QStringLiteral("    vec4 groundColor;")
         << QStringLiteral("    vec4 cascadeSplits;")
         << QStringLiteral("    vec4 shadowParams;")
         << QStringLiteral("    vec4 screenParams;   // z = time (s)")
         << QStringLiteral("    vec4 pointPos[4];")
         << QStringLiteral("    vec4 pointColor[4];")
         // The scene's toon parameters, so a graph style reacts to the same global sliders the engine's
         // own toon shaders use (see shaders/frame.glsl for what each row carries).
         << QStringLiteral("    vec4 toonA;          // x hair highlight, y hair shift, z hair exponent, w self shadow on")
         << QStringLiteral("    vec4 toonB;          // x self threshold, y self hardness, z gloss, w gloss power")
         << QStringLiteral("    vec4 toonC;          // x cel band on, y normal map strength, z outline noise, w reflection")
         << QStringLiteral("    vec4 toonD;          // xy head forward (x, z) for face shadow, z face shadow on, w smear")
         << QStringLiteral("    vec4 toonE;          // x soft light, y light sweep strength, z sweep speed")
         << QStringLiteral("    vec4 toonF;          // camera dither fade: x dithered-out distance, y visible distance")
         << QStringLiteral("    vec4 toonG;          // x stylised PBR, y eye flatten, z eye parallax, w eye matcap")
         << QStringLiteral("    vec4 toonH;          // x eye highlight map strength")
         << QStringLiteral("    vec4 toonI;          // occlusion target: xyz world point, w radius")
         << QStringLiteral("} frame;");
    if (!g.props.isEmpty()) {
        frag << QStringLiteral("layout(set = 1, binding = %1, std140) uniform UkMaterial {")
                    .arg(kMaterialBlockBinding);
        for (const PropertyInfo& p : g.props)
            frag << QStringLiteral("    vec4 p%1; // %2 (%3)").arg(p.slot).arg(p.name, propertyKindName(p.kind));
        frag << QStringLiteral("};");
    }
    out.properties = g.props;
    out.textures = g.textures;
    frag << g.decls << QString() << fn
         << QString()
         << QStringLiteral("void main() {")
         << QStringLiteral("    SurfaceData s = ukiyo_surface(v_uv, normalize(v_normal));")
         << QStringLiteral("    vec3 color = s.baseColor * s.bakedLight + s.emission;")
         << QStringLiteral("    if (s.toon > 0.5) {")
         // Every term past the ramp is identity at its default, so the branch reduces to the old
         // two-tone formula (and the same floating point operations) when nothing new is wired.
         << QStringLiteral("        // NPR Master: ramp-toned sun light plus hemisphere ambient.")
         << QStringLiteral("        vec3 Nt = normalize(s.normal);")
         << QStringLiteral("        float ndlt = dot(Nt, normalize(frame.sunDir.xyz));")
         << QStringLiteral("        float wrapped = mix(ndlt, clamp(ndlt, 0.0, 1.0) * 0.5 + 0.5, s.toonWrap);")
         << QStringLiteral("        float ramp = smoothstep(s.toonThreshold - s.toonSoftness, s.toonThreshold + s.toonSoftness, wrapped);")
         << QStringLiteral("        ramp = mix(ramp, clamp(s.toonMask, 0.0, 1.0), s.toonMaskMix);")
         // mix() rather than 1 - (1 - ramp) * blend: the same curve, but exact at the blend default of 1,
        // so a graph that never touches Ramp Blend keeps the two-tone result bit for bit.
        << QStringLiteral("        float rampBase = ramp;")
        << QStringLiteral("        ramp = mix(1.0, ramp, s.toonRampBlend);")
         << QStringLiteral("        vec3 albedo = s.baseColor * s.bakedLight;")
         << QStringLiteral("        vec3 hemiT = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * Nt.y);")
         // Boost and the normal shade belong to the lit side of the ramp only, as they do in the
         // Endfield NPR path, where the ramp tone is the shaded side and carries neither.
         << QStringLiteral("        float normalShade = mix(1.0, mix(0.92, 1.08, clamp(wrapped, 0.0, 1.0)), s.toonNormalShade);")
         << QStringLiteral("        vec3 tone = albedo * mix(s.toonShade * s.toonRamp, vec3(s.toonBoost * normalShade), ramp);")
         << QStringLiteral("        color = tone * frame.sunColor.rgb * s.toonShadow * s.toonScene")
         << QStringLiteral("              + albedo * hemiT * s.toonAmbient * s.toonOcclusion * s.toonScene")
         << QStringLiteral("              + tone * (1.0 - s.toonScene);")
         // The floor is measured on the shadow before Ramp Blend weakens it, and the additive terms
         // (emission, rim, specular) come after it - both as the character shaders do.
         << QStringLiteral("        vec3 floorTone = albedo * s.toonFloor * (1.0 - rampBase);")
         << QStringLiteral("        color = mix(color, max(color, floorTone), step(0.0, s.toonFloor));")
         << QStringLiteral("        color += s.emission;")
         << QStringLiteral("    } else if (s.hybrid > 0.5) {")
         << QStringLiteral("        // Hybrid Master: PBR and NPR blended, plus matcap, hair, rim and wetness.")
         << QStringLiteral("        vec3 N = normalize(s.normal);")
         << QStringLiteral("        vec3 L = normalize(frame.sunDir.xyz);")
         << QStringLiteral("        vec3 V = normalize(frame.camPos.xyz - v_world);")
         << QStringLiteral("        vec3 H = normalize(L + V);")
         << QStringLiteral("        float ndl = max(dot(N, L), 0.0);")
         << QStringLiteral("        float ndv = max(dot(N, V), 0.0);")
         << QStringLiteral("        float ndh = max(dot(N, H), 0.0);")
         << QStringLiteral("        vec3 albedo = s.baseColor * s.bakedLight;")
         << QStringLiteral("        vec3 hemi = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * N.y);")
         << QStringLiteral("        float shin = exp2(1.0 + 10.0 * s.smoothness);")
         << QStringLiteral("        float spec = pow(ndh, shin) * (shin + 8.0) / 25.13274;")
         << QStringLiteral("        vec3 F0 = mix(vec3(0.04), albedo, s.metallic);")
         << QStringLiteral("        vec3 pbr = albedo * (1.0 - s.metallic) * (frame.sunColor.rgb * ndl + hemi) * s.toonOcclusion")
         << QStringLiteral("                 + F0 * frame.sunColor.rgb * spec * step(0.0, ndl) * s.specMask;")
         << QStringLiteral("        float wrapped = mix(dot(N, L), clamp(dot(N, L), 0.0, 1.0) * 0.5 + 0.5, s.toonWrap);")
         << QStringLiteral("        float ramp = smoothstep(s.toonThreshold - s.toonSoftness, s.toonThreshold + s.toonSoftness, wrapped);")
         << QStringLiteral("        ramp = mix(ramp, clamp(s.toonMask, 0.0, 1.0), s.toonMaskMix);")
         << QStringLiteral("        float rampBase = ramp;")
         << QStringLiteral("        ramp = mix(1.0, ramp, s.toonRampBlend);")
         << QStringLiteral("        float normalShade = mix(1.0, mix(0.92, 1.08, clamp(wrapped, 0.0, 1.0)), s.toonNormalShade);")
         << QStringLiteral("        vec3 nprLit = albedo * s.toonBoost * normalShade;")
         << QStringLiteral("        vec3 rampTone = albedo * mix(s.toonShade * s.toonRamp, vec3(1.0), ramp);")
         << QStringLiteral("        float shadow = clamp(max(1.0 - s.toonShadow, s.sdfShadow), 0.0, 1.0);")
         << QStringLiteral("        vec3 litBase = mix(nprLit, pbr, s.toonScene);") // using toonScene as pbrBlend
         << QStringLiteral("        color = mix(litBase, rampTone, shadow * s.toonRampBlend);")
         << QStringLiteral("        color = max(color, albedo * s.toonFloor * shadow);")
         << QStringLiteral("        color += s.hairSpecular * s.toonRamp;") // reusing toonRamp as specRamp
         << QStringLiteral("        color += s.matcap;")
         << QStringLiteral("        color += s.toonRim + s.emission;")
         << QStringLiteral("    } else if (s.face > 0.5) {")
         << QStringLiteral("        // Face Master: SDF shadow over the ramp, plus hemisphere ambient.")
         << QStringLiteral("        vec3 albedo = s.baseColor * s.bakedLight;")
         << QStringLiteral("        vec3 hemiT = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * s.normal.y);")
         << QStringLiteral("        vec3 tone = albedo * mix(vec3(1.0), s.toonRamp * s.toonShade, clamp(s.toonShadow, 0.0, 1.0));")
         << QStringLiteral("        color = tone * frame.sunColor.rgb")
         << QStringLiteral("              + albedo * hemiT * s.toonAmbient")
         << QStringLiteral("              + s.toonRim + s.emission;")
         << QStringLiteral("    } else if (s.lit > 0.5) {")
         << QStringLiteral("        vec3 N = normalize(s.normal);")
         << QStringLiteral("        float ndl = max(dot(N, normalize(frame.sunDir.xyz)), 0.0);")
         << QStringLiteral("        vec3 hemi = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * N.y);")
         << QStringLiteral("        // Blinn-Phong highlight: metallic tints F0, smoothness sets the exponent.")
         << QStringLiteral("        vec3 V = normalize(frame.camPos.xyz - v_world);")
         << QStringLiteral("        vec3 H = normalize(normalize(frame.sunDir.xyz) + V);")
         << QStringLiteral("        float shin = exp2(1.0 + 10.0 * s.smoothness);")
         << QStringLiteral("        float spec = pow(max(dot(N, H), 0.0), shin) * (shin + 8.0) / 25.13274;")
         << QStringLiteral("        vec3 F0 = mix(vec3(0.04), s.baseColor, s.metallic);")
         << QStringLiteral("        vec3 diffuse = s.baseColor * (1.0 - s.metallic) * (frame.sunColor.rgb * ndl + hemi);")
         << QStringLiteral("        color = diffuse + F0 * frame.sunColor.rgb * spec * step(0.0, ndl) + s.emission;")
         << QStringLiteral("        // Clear coat: a sharper white highlight over the base; it also dims the base by its Fresnel.")
         << QStringLiteral("        float ccExp = exp2(1.0 + 10.0 * s.clearCoatSmoothness);")
         << QStringLiteral("        float ccSpec = pow(max(dot(N, H), 0.0), ccExp) * (ccExp + 8.0) / 25.13274;")
         << QStringLiteral("        color = color * (1.0 - 0.04 * s.clearCoat) + frame.sunColor.rgb * ccSpec * s.clearCoat * step(0.0, ndl);")
         << QStringLiteral("    } else if (s.toon < 0.5 && s.face < 0.5 && s.lit < 0.5) {")
         << QStringLiteral("        discard; // Outline Master")
         << QStringLiteral("    }")
         << QStringLiteral("    o_color = vec4(color, s.alpha);")
         << QStringLiteral("}");
    out.fragmentShader = frag.join('\n') + '\n';
    // Record which node wrote each body line, so a compile error on that line can name its node.
    const QStringList fragLines = out.fragmentShader.split(QLatin1Char('\n'));
    for (int i = 0; i < fragLines.size(); ++i) {
        const auto owner = g.bodyOwner.constFind(fragLines[i]);
        if (owner != g.bodyOwner.constEnd()) out.fragmentLineNodes.insert(i + 1, owner.value());
    }
    out.errors.removeDuplicates();
    return out;
}

QString propertyKindName(ParamKind k)
{
    switch (k) {
    case ParamKind::Float: return QStringLiteral("float");
    case ParamKind::Vec2: return QStringLiteral("vec2");
    case ParamKind::Color: return QStringLiteral("vec3");
    default: return QStringLiteral("none");
    }
}

QJsonObject shaderDocument(QtNodes::DataFlowGraphModel& graph)
{
    const CodegenResult r = generate(graph);

    QJsonArray errors;
    for (const QString& e : r.errors) errors.append(e);
    QJsonArray props;
    for (const PropertyInfo& p : r.properties) {
        QJsonArray defaults;
        for (const QString& d : p.defaults) defaults.append(d);
        QJsonObject o;
        o.insert(QStringLiteral("name"), p.name);
        o.insert(QStringLiteral("kind"), propertyKindName(p.kind));
        o.insert(QStringLiteral("slot"), p.slot);
        o.insert(QStringLiteral("defaults"), defaults);
        props.append(o);
    }
    QJsonObject compiled;
    compiled.insert(QStringLiteral("ok"), r.ok());
    compiled.insert(QStringLiteral("errors"), errors);
    compiled.insert(QStringLiteral("fragment"), r.fragmentShader);
    compiled.insert(QStringLiteral("surface"), r.surfaceFunction);
    compiled.insert(QStringLiteral("properties"), props);
    QJsonArray textures;
    for (const TextureInfo& t : r.textures) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), t.name);
        o.insert(QStringLiteral("binding"), t.binding);
        o.insert(QStringLiteral("defaultPath"), t.defaultPath);
        textures.append(o);
    }
    compiled.insert(QStringLiteral("textures"), textures);
    compiled.insert(QStringLiteral("materialBinding"), kMaterialBlockBinding);
    compiled.insert(QStringLiteral("blend"), r.transparent ? QStringLiteral("alpha") : QStringLiteral("none"));
    // Only written when the graph asks for outlines, so a plain graph's document stays as it was.
    if (r.outline) compiled.insert(QStringLiteral("outline"), true);

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("ukiyo-shader-graph"));
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("graph"), graph.save());
    root.insert(QStringLiteral("compiled"), compiled);
    return root;
}

} // namespace sg
