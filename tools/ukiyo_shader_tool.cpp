// ukiyo_shader_tool: builds the default shader graphs, checks every node template, and verifies
// .ukshader save/load. Used by the build and by hand; the editor does not depend on it.
//
//   ukiyo_shader_tool defaults <assetsDir> <outDir>   write Lit/Unlit/Toon .ukshader files and .frag files
//   ukiyo_shader_tool coverage <outDir>                write a graph that uses every node type
//   ukiyo_shader_tool roundtrip <file.ukshader>        load, save, reload, and compare the GLSL
//   ukiyo_shader_tool compile <graphDir> <assetsDir>   compile every <Name>.json graph into <assetsDir>/Shaders/<Name>.ukshader

#include "sg_codegen.h"
#include "sg_nodes.h"
#include "engine/graph_material.h"
#include "engine/material_preview.h"
#include "engine/material_asset.h"
#include "engine/shader_compiler.h"
#include "rhi/rhi.h"
#include "engine/camera.h"
#include "engine/graph_material.h"
#include "engine/material_asset.h"
#include "engine/model.h"
#include "engine/renderer.h"

#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include <GLFW/glfw3.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNodes/DataFlowGraphModel>
#include <QtWidgets/QApplication>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include <stb_image_write.h>
#include <memory>

using QtNodes::ConnectionId;
using QtNodes::NodeId;
using QtNodes::PortIndex;

namespace {

struct Builder {
    std::unique_ptr<QtNodes::DataFlowGraphModel> model;

    Builder()
        : model(std::make_unique<QtNodes::DataFlowGraphModel>(sg::makeRegistry()))
    {}

    NodeId add(const char* type, const QStringList& params = {}, const char* exposed = nullptr)
    {
        NodeId id = model->addNode(QString::fromLatin1(type));
        if (auto* m = model->delegateModel<sg::ShaderNodeModel>(id)) {
            if (!params.isEmpty()) m->setParams(params);
            if (exposed) m->setReference(QString::fromLatin1(exposed));
        }
        return id;
    }

    void link(NodeId out, int outPort, NodeId in, int inPort)
    {
        model->addConnection(ConnectionId{out, static_cast<PortIndex>(outPort), in, static_cast<PortIndex>(inPort)});
    }
};

QJsonObject wrap(QtNodes::DataFlowGraphModel& m)
{
    return sg::shaderDocument(m);
}

bool writeText(const QString& path, const QString& text)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    f.write(text.toUtf8());
    return true;
}

// ---- Default shader graphs (shipped in assets/Shaders) ----

std::unique_ptr<QtNodes::DataFlowGraphModel> buildLit(bool textured)
{
    Builder b;
    NodeId master = b.add("PBRMaster");
    NodeId metallic = b.add("Float", {QStringLiteral("0")}, "metallic");
    NodeId smooth = b.add("Float", {QStringLiteral("0.5")}, "smoothness");
    NodeId emission = b.add("Color", {QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0")}, "emission");
    b.link(metallic, 0, master, 1);
    b.link(smooth, 0, master, 2);
    b.link(emission, 0, master, 4);
    if (textured) {
        NodeId uv = b.add("UV");
        NodeId tex = b.add("SampleTexture2D", {QString()});
        b.link(uv, 0, tex, 0);
        b.link(tex, 0, master, 0);
        NodeId alpha = b.add("Float", {QStringLiteral("1")}, "alpha");
        b.link(alpha, 0, master, 5);
    } else {
        NodeId base = b.add("Color", {QStringLiteral("0.8"), QStringLiteral("0.8"), QStringLiteral("0.8")}, "baseColor");
        NodeId alpha = b.add("Float", {QStringLiteral("1")}, "alpha");
        b.link(base, 0, master, 0);
        b.link(alpha, 0, master, 5);
    }
    return std::move(b.model);
}

std::unique_ptr<QtNodes::DataFlowGraphModel> buildUnlit(bool textured)
{
    Builder b;
    NodeId master = b.add("UnlitMaster");
    if (textured) {
        NodeId uv = b.add("UV");
        NodeId tex = b.add("SampleTexture2D", {QString()});
        b.link(uv, 0, tex, 0);
        b.link(tex, 0, master, 0);
        NodeId alpha = b.add("Float", {QStringLiteral("1")}, "alpha");
        b.link(alpha, 0, master, 1);
    } else {
        NodeId color = b.add("Color", {QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1")}, "color");
        NodeId alpha = b.add("Float", {QStringLiteral("1")}, "alpha");
        b.link(color, 0, master, 0);
        b.link(alpha, 0, master, 1);
    }
    return std::move(b.model);
}

// Complex Lit: albedo texture, normal map, and an exposed clear-coat layer.
std::unique_ptr<QtNodes::DataFlowGraphModel> buildComplexLit()
{
    Builder b;
    NodeId master = b.add("ComplexLitMaster");
    NodeId base = b.add("Color", {QStringLiteral("0.8"), QStringLiteral("0.8"), QStringLiteral("0.8")}, "baseColor");
    // The normal map's Map input is left unconnected on purpose: it falls back to a flat tangent
    // map, so the default shows no bumps. Materials cannot bind textures yet, so a sampled normal
    // map would read white and tilt every normal.
    NodeId normalMap = b.add("NormalMap");
    NodeId strength = b.add("Float", {QStringLiteral("1")}, "normalStrength");
    NodeId clear = b.add("Float", {QStringLiteral("0.6")}, "clearCoat");
    NodeId clearSmooth = b.add("Float", {QStringLiteral("0.9")}, "clearCoatSmoothness");
    NodeId metallic = b.add("Float", {QStringLiteral("0")}, "metallic");
    NodeId smooth = b.add("Float", {QStringLiteral("0.5")}, "smoothness");
    b.link(base, 0, master, 0);
    b.link(metallic, 0, master, 1);
    b.link(smooth, 0, master, 2);
    b.link(strength, 0, normalMap, 1);
    b.link(normalMap, 0, master, 3);
    b.link(clear, 0, master, 6);
    b.link(clearSmooth, 0, master, 7);
    return std::move(b.model);
}

// Terrain: height and slope blend of grass, rock, and snow, lit by the standard PBR path.
// No textures: materials cannot bind them yet, so the layers are flat colours with masks.
// Rock covers steep ground (slope = 1 - up), snow covers high ground (world y).
std::unique_ptr<QtNodes::DataFlowGraphModel> buildTerrain()
{
    Builder b;
    NodeId master = b.add("PBRMaster");
    NodeId grass = b.add("Color", {QStringLiteral("0.27"), QStringLiteral("0.42"), QStringLiteral("0.18")}, "grassColor");
    NodeId rock = b.add("Color", {QStringLiteral("0.42"), QStringLiteral("0.38"), QStringLiteral("0.34")}, "rockColor");
    NodeId snow = b.add("Color", {QStringLiteral("0.92"), QStringLiteral("0.94"), QStringLiteral("0.98")}, "snowColor");
    NodeId rockStart = b.add("Float", {QStringLiteral("0.3")}, "rockSlopeStart");
    NodeId rockEnd = b.add("Float", {QStringLiteral("0.55")}, "rockSlopeEnd");
    NodeId snowStart = b.add("Float", {QStringLiteral("6")}, "snowHeightStart");
    NodeId snowEnd = b.add("Float", {QStringLiteral("8")}, "snowHeightEnd");
    NodeId metallic = b.add("Float", {QStringLiteral("0")}, "metallic");
    NodeId smooth = b.add("Float", {QStringLiteral("0.3")}, "smoothness");
    NodeId normal = b.add("Normal");
    NodeId normalSplit = b.add("Split");
    NodeId oneMinus = b.add("OneMinusF");
    NodeId rockMask = b.add("SmoothstepF");
    NodeId pos = b.add("WorldPosition");
    NodeId posSplit = b.add("Split");
    NodeId snowMask = b.add("SmoothstepF");
    NodeId rockMix = b.add("LerpV3");
    NodeId snowMix = b.add("LerpV3");
    b.link(normal, 0, normalSplit, 0);
    b.link(normalSplit, 1, oneMinus, 0);      // up component -> slope = 1 - up
    b.link(rockStart, 0, rockMask, 0);
    b.link(rockEnd, 0, rockMask, 1);
    b.link(oneMinus, 0, rockMask, 2);
    b.link(pos, 0, posSplit, 0);
    b.link(snowStart, 0, snowMask, 0);
    b.link(snowEnd, 0, snowMask, 1);
    b.link(posSplit, 1, snowMask, 2);         // height -> snow mask
    b.link(grass, 0, rockMix, 0);
    b.link(rock, 0, rockMix, 1);
    b.link(rockMask, 0, rockMix, 2);
    b.link(rockMix, 0, snowMix, 0);
    b.link(snow, 0, snowMix, 1);
    b.link(snowMask, 0, snowMix, 2);
    b.link(snowMix, 0, master, 0);
    b.link(metallic, 0, master, 1);
    b.link(smooth, 0, master, 2);
    return std::move(b.model);
}

// Baked Lit: albedo texture times a baked light texture. No sun, sky, or normal term.
// The baked light texture is a second SampleTexture2D, so it takes its own material slot.
std::unique_ptr<QtNodes::DataFlowGraphModel> buildBakedLit()
{
    Builder b;
    NodeId master = b.add("BakedLitMaster");
    NodeId uv = b.add("UV");
    NodeId albedo = b.add("SampleTexture2D", {QString()});
    NodeId bake = b.add("SampleTexture2D", {QString()});
    b.link(uv, 0, albedo, 0);
    b.link(uv, 0, bake, 0);
    b.link(albedo, 0, master, 0);
    b.link(bake, 0, master, 1);
    return std::move(b.model);
}

// ---- Coverage graph: uses every node template at least once ----

// Unlit with a wired Alpha input at 0.5: the material is drawn alpha-blended.
std::unique_ptr<QtNodes::DataFlowGraphModel> buildUnlitTransparent()
{
    Builder b;
    NodeId master = b.add("UnlitMaster");
    NodeId color = b.add("Color", {QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1")}, "color");
    NodeId alpha = b.add("Float", {QStringLiteral("0.5")}, "alpha");
    b.link(color, 0, master, 0);
    b.link(alpha, 0, master, 1);
    return std::move(b.model);
}

// Lit with smoothness fixed at 0.5 and not exposed as a material property (URP Simple Lit).
std::unique_ptr<QtNodes::DataFlowGraphModel> buildSimpleLit()
{
    Builder b;
    NodeId master = b.add("PBRMaster");
    NodeId base = b.add("Color", {QStringLiteral("0.8"), QStringLiteral("0.8"), QStringLiteral("0.8")}, "baseColor");
    NodeId metallic = b.add("Float", {QStringLiteral("0")}, "metallic");
    NodeId smooth = b.add("Float", {QStringLiteral("0.5")});
    NodeId emission = b.add("Color", {QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0")}, "emission");
    NodeId alpha = b.add("Float", {QStringLiteral("1")}, "alpha");
    b.link(base, 0, master, 0);
    b.link(metallic, 0, master, 1);
    b.link(smooth, 0, master, 2);
    b.link(emission, 0, master, 4);
    b.link(alpha, 0, master, 5);
    return std::move(b.model);
}

// Unlit, alpha-blended, for particles. The graph has no vertex-colour input yet, so the tint is a
// single material colour.
std::unique_ptr<QtNodes::DataFlowGraphModel> buildParticlesUnlit()
{
    Builder b;
    NodeId master = b.add("UnlitMaster");
    NodeId color = b.add("Color", {QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1")}, "color");
    NodeId alpha = b.add("Float", {QStringLiteral("0.5")}, "alpha");
    b.link(color, 0, master, 0);
    b.link(alpha, 0, master, 1);
    return std::move(b.model);
}

std::unique_ptr<QtNodes::DataFlowGraphModel> buildCoverage()
{
    Builder b;
    NodeId master = b.add("PBRMaster");
    NodeId t = b.add("Time");
    NodeId s = b.add("SaturateF");
    NodeId pw = b.add("PowerF");
    NodeId two = b.add("Float", {QStringLiteral("2")});
    NodeId mul = b.add("MultiplyF");
    NodeId k = b.add("Float", {QStringLiteral("0.8")});
    NodeId lerpF = b.add("LerpF");
    NodeId lowF = b.add("Float", {QStringLiteral("0.1")});
    NodeId omF = b.add("OneMinusF");
    NodeId addF = b.add("AddF");
    NodeId uvV = b.add("Vector2", {QStringLiteral("0.25"), QStringLiteral("0.75")});
    NodeId tex = b.add("SampleTexture2D", {QString()});
    NodeId col = b.add("Color", {QStringLiteral("0.9"), QStringLiteral("0.4"), QStringLiteral("0.2")});
    NodeId mulV = b.add("MultiplyV3");
    NodeId lerpV = b.add("LerpV3");
    NodeId addV = b.add("AddV3");
    NodeId comb = b.add("Combine");
    NodeId split = b.add("Split");
    NodeId nrm = b.add("Normal");
    NodeId flatVec = b.add("Combine");

    b.link(t, 0, s, 0);
    b.link(s, 0, pw, 0);
    b.link(two, 0, pw, 1);
    b.link(pw, 0, mul, 0);
    b.link(k, 0, mul, 1);
    b.link(lowF, 0, lerpF, 0);
    b.link(mul, 0, lerpF, 1);
    b.link(s, 0, lerpF, 2);
    b.link(lerpF, 0, omF, 0);
    b.link(omF, 0, addF, 0);
    b.link(lerpF, 0, addF, 1);
    b.link(uvV, 0, tex, 0);
    b.link(col, 0, mulV, 0);
    b.link(tex, 0, mulV, 1);
    b.link(mulV, 0, lerpV, 0);
    b.link(col, 0, lerpV, 1);
    b.link(lerpF, 0, lerpV, 2);
    b.link(lerpV, 0, addV, 0);
    b.link(lerpF, 0, comb, 0);
    b.link(lerpF, 0, comb, 1);
    b.link(omF, 0, comb, 2);
    b.link(comb, 0, addV, 1);
    b.link(addV, 0, split, 0);
    b.link(split, 0, flatVec, 0);
    b.link(addV, 0, master, 0);
    b.link(split, 2, master, 1);
    b.link(addF, 0, master, 2);
    b.link(nrm, 0, master, 3);
    b.link(tex, 0, master, 4);
    b.link(tex, 1, master, 5);
    return std::move(b.model);
}

// Toon Master: a two-tone sun ramp with a cool shade colour, lit through the Toon Master's own path.
std::unique_ptr<QtNodes::DataFlowGraphModel> buildToon()
{
    Builder b;
    NodeId master = b.add("ToonMaster");
    NodeId base = b.add("Color", {QStringLiteral("0.85"), QStringLiteral("0.8"), QStringLiteral("0.85")}, "baseColor");
    NodeId shade = b.add("Color", {QStringLiteral("0.55"), QStringLiteral("0.5"), QStringLiteral("0.7")}, "shadeColor");
    NodeId thr = b.add("Float", {QStringLiteral("0.0")}, "threshold");
    NodeId soft = b.add("Float", {QStringLiteral("0.05")}, "softness");
    NodeId alpha = b.add("Float", {QStringLiteral("1")}, "alpha");
    b.link(base, 0, master, 0);
    b.link(shade, 0, master, 1);
    b.link(thr, 0, master, 2);
    b.link(soft, 0, master, 3);
    b.link(alpha, 0, master, 6);
    return std::move(b.model);
}

// Builds the fragment and surface text for a graph, printing errors and returning false on failure.
bool emitGraph(QtNodes::DataFlowGraphModel& m, const QString& outDir, const QString& name)
{
    const sg::CodegenResult r = sg::generate(m);
    if (!r.ok()) {
        std::fprintf(stderr, "%s: %s\n", qPrintable(name), qPrintable(r.errors.join(" | ")));
        return false;
    }
    if (!writeText(QDir(outDir).filePath(name + ".frag"), r.fragmentShader)
        || !writeText(QDir(outDir).filePath(name + ".surface.glsl"), r.surfaceFunction)) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(name));
        return false;
    }
    std::printf("wrote %s.frag\n", qPrintable(name));
    // Compile with the engine's glslang (the same path the engine will use at runtime).
    const engine::SpirvResult spv = engine::compileGlslToSpirv(r.fragmentShader.toStdString(), engine::ShaderStage::Fragment);
    if (!spv.ok) {
        std::fprintf(stderr, "%s: SPIR-V compile failed:\n%s\n", qPrintable(name), spv.log.c_str());
        return false;
    }
    std::printf("spirv OK: %s (%zu words)\n", qPrintable(name), spv.words.size());
    return true;
}

// Saves the graph as a .ukshader and reloads it into a fresh model. The two GLSL outputs must match.
bool roundtripCheck(QtNodes::DataFlowGraphModel& m, const QString& name)
{
    const QByteArray json = QJsonDocument(wrap(m)).toJson(QJsonDocument::Compact);
    const QJsonObject root = QJsonDocument::fromJson(json).object();

    QtNodes::DataFlowGraphModel reloaded(sg::makeRegistry());
    reloaded.load(root.value(QStringLiteral("graph")).toObject());

    const QString a = sg::generate(m).fragmentShader;
    const QString c = sg::generate(reloaded).fragmentShader;
    if (a != c) {
        std::fprintf(stderr, "roundtrip FAILED for %s\n", qPrintable(name));
        return false;
    }
    std::printf("roundtrip OK: %s\n", qPrintable(name));
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: ukiyo_shader_tool defaults <assetsDir> <outDir> | compile <graphDir> <assetsDir> | coverage <outDir> | roundtrip <file> | material <assetsDir> <Name.ukmat> | gpu <assetsDir> <Name.ukmat> | preview <assetsDir> <Name.ukmat> <out.png>\n");
        return 2;
    }
    const QString cmd = args[1];
    bool ok = true;

    if (cmd == QLatin1String("defaults") && args.size() == 4) {
        const QString assets = args[2];
        const QString out = args[3];
        QDir().mkpath(assets + QStringLiteral("/Shaders"));
        QDir().mkpath(out);
        // Each entry is written as <assets>/Shaders/<name>.ukshader.
        const std::pair<QString, std::unique_ptr<QtNodes::DataFlowGraphModel> (*)()> items[] = {
            {QStringLiteral("Lit"), [] { return buildLit(false); }},
            {QStringLiteral("LitTextured"), [] { return buildLit(true); }},
            {QStringLiteral("Unlit"), [] { return buildUnlit(false); }},
            {QStringLiteral("UnlitTextured"), [] { return buildUnlit(true); }},
            {QStringLiteral("Toon"), [] { return buildToon(); }},
            {QStringLiteral("UnlitTransparent"), [] { return buildUnlitTransparent(); }},
            {QStringLiteral("SimpleLit"), [] { return buildSimpleLit(); }},
            {QStringLiteral("ParticlesUnlit"), [] { return buildParticlesUnlit(); }},
            {QStringLiteral("BakedLit"), [] { return buildBakedLit(); }},
            {QStringLiteral("ComplexLit"), [] { return buildComplexLit(); }},
            {QStringLiteral("TerrainLit"), [] { return buildTerrain(); }},
        };
        for (const auto& it : items) {
            auto m = it.second();
            ok = emitGraph(*m, out, it.first) && ok;
            ok = roundtripCheck(*m, it.first) && ok;
            const QByteArray json = QJsonDocument(wrap(*m)).toJson(QJsonDocument::Indented);
            QFile f(assets + QStringLiteral("/Shaders/") + it.first + QStringLiteral(".ukshader"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(json);
            else ok = false;
        }
    } else if (cmd == QLatin1String("compile") && args.size() == 4) {
        // compile <graphDir> <assetsDir>: each <Name>.json holds a {"graph": ...} document. It is loaded,
        // compiled to GLSL and SPIR-V, round-trip checked, and saved as <assetsDir>/Shaders/<Name>.ukshader.
        const QDir graphs(args[2]);
        const QString shaders = args[3] + QStringLiteral("/Shaders");
        const QString scratch = QDir::tempPath() + QStringLiteral("/ukiyo_graph_build");
        QDir().mkpath(shaders);
        QDir().mkpath(scratch);
        const QStringList files = graphs.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        int count = 0;
        for (const QString& file : files) {
            const QString stem = QFileInfo(file).completeBaseName();
            QFile in(graphs.filePath(file));
            if (!in.open(QIODevice::ReadOnly)) {
                std::fprintf(stderr, "cannot read %s\n", qPrintable(file));
                ok = false;
                continue;
            }
            const QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
            auto m = std::make_unique<QtNodes::DataFlowGraphModel>(sg::makeRegistry());
            m->load(root.value(QStringLiteral("graph")).toObject());
            ok = emitGraph(*m, scratch, stem) && ok;
            ok = roundtripCheck(*m, stem) && ok;
            const QByteArray json = QJsonDocument(wrap(*m)).toJson(QJsonDocument::Indented);
            QFile out(shaders + QStringLiteral("/") + stem + QStringLiteral(".ukshader"));
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(json);
            else ok = false;
            ++count;
        }
        std::printf("compiled %d of %d graphs\n", count, static_cast<int>(files.size()));
    } else if (cmd == QLatin1String("coverage") && args.size() == 3) {
        auto m = buildCoverage();
        ok = emitGraph(*m, args[2], QStringLiteral("coverage"));
        ok = roundtripCheck(*m, QStringLiteral("coverage")) && ok;
    } else if (cmd == QLatin1String("material") && args.size() == 4) {
        // material <assetsDir> <Name.ukmat>: load, pack the values, and compile the shader.
        const QString assets = args[2];
        engine::MaterialAsset mat;
        engine::ShaderAsset shader;
        std::string error;
        if (!engine::loadMaterialAsset(assets.toStdString() + "/Materials/" + args[3].toStdString(), mat, error)
            || !engine::loadShaderAsset(assets.toStdString() + "/" + mat.shaderPath, shader, error)) {
            std::fprintf(stderr, "material check FAILED: %s\n", error.c_str());
            return 1;
        }
        const std::vector<float> block = engine::packMaterialParameters(shader, mat);
        std::printf("material %s -> %s: %d slots\n", mat.name.c_str(), mat.shaderPath.c_str(), shader.slotCount());
        for (const engine::ShaderProperty& p : shader.properties) {
            std::printf("  slot %d %-10s %-5s =", p.slot, p.name.c_str(), p.kind.c_str());
            for (int k = 0; k < 4; ++k) std::printf(" %.3f", block[static_cast<size_t>(p.slot) * 4 + static_cast<size_t>(k)]);
            std::printf("\n");
        }
        const engine::SpirvResult spv = engine::compileGlslToSpirv(shader.fragment, engine::ShaderStage::Fragment);
        if (!spv.ok) {
            std::fprintf(stderr, "material shader does not compile:\n%s\n", spv.log.c_str());
            return 1;
        }
        std::printf("material shader compiles to SPIR-V (%zu words)\n", spv.words.size());
    } else if (cmd == QLatin1String("sphere") && args.size() == 5) {
        // sphere <assetsDir> <Name.ukmat> <out.png>: draw the material on a sphere (Vulkan, headless) to a PNG.
        const QString assets = args[2];
        engine::MaterialAsset mat;
        engine::ShaderAsset shader;
        std::string error;
        if (!engine::loadMaterialAsset(assets.toStdString() + "/Materials/" + args[3].toStdString(), mat, error)
            || !engine::loadShaderAsset(assets.toStdString() + "/" + mat.shaderPath, shader, error)) {
            std::fprintf(stderr, "sphere FAILED: %s\n", error.c_str());
            return 1;
        }
        try {
            rhi::DeviceConfig cfg;
            cfg.width = 320;
            cfg.height = 320;
            cfg.headless = true;
            cfg.validation = true;
            auto device = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);
            rhi::TextureDesc td;
            td.width = 1;
            td.height = 1;
            const uint8_t white[4] = {255, 255, 255, 255};
            auto fallback = device->createTexture(td, white);
            ukiyo::GraphMaterial gm(*device, shader, *fallback, rhi::Format::Backbuffer);
            gm.setParameters(engine::packMaterialParameters(shader, mat));
            const std::vector<uint8_t> px = ukiyo::renderMaterialSphere(*device, gm);
            const int side = static_cast<int>(std::sqrt(px.size() / 4));
            if (!stbi_write_png(args[4].toUtf8().constData(), side, side, 4, px.data(), side * 4)) {
                std::fprintf(stderr, "cannot write %s\n", qPrintable(args[4]));
                return 1;
            }
            std::printf("sphere OK: %s -> %s (%dx%d)\n", mat.name.c_str(), qPrintable(args[4]), side, side);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sphere FAILED: %s\n", e.what());
            return 1;
        }
    } else if (cmd == QLatin1String("gpu") && (args.size() == 4 || args.size() == 5)) {
        // gpu <assetsDir> <Name.ukmat> [gl]: build the real pipeline and material set for the material's
        // shader, then upload its packed parameters. Vulkan by default (lavapipe works); "gl" uses OpenGL.
        const bool useGl = args.size() == 5 && args[4] == QLatin1String("gl");
        const QString assets = args[2];
        engine::MaterialAsset mat;
        engine::ShaderAsset shader;
        std::string error;
        if (!engine::loadMaterialAsset(assets.toStdString() + "/Materials/" + args[3].toStdString(), mat, error)
            || !engine::loadShaderAsset(assets.toStdString() + "/" + mat.shaderPath, shader, error)) {
            std::fprintf(stderr, "gpu check FAILED: %s\n", error.c_str());
            return 1;
        }
        try {
            rhi::DeviceConfig cfg;
            cfg.width = 64;
            cfg.height = 64;
            cfg.headless = true;
            cfg.validation = true;
            // The GL device needs a context, so make a hidden 4.5 core window as the app does.
            GLFWwindow* window = nullptr;
            if (useGl) {
                if (!glfwInit()) throw std::runtime_error("glfwInit failed");
                glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
                glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
                glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
                glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
                glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                window = glfwCreateWindow(64, 64, "ukiyo_shader_tool (GL)", nullptr, nullptr);
                if (!window) throw std::runtime_error("cannot create a GL context window");
            }
            auto device = rhi::createDevice(useGl ? rhi::Backend::OpenGL : rhi::Backend::Vulkan, cfg, window);
            rhi::TextureDesc td;
            td.width = 1;
            td.height = 1;
            const uint8_t white[4] = {255, 255, 255, 255};
            auto fallback = device->createTexture(td, white);
            ukiyo::GraphMaterial gm(*device, shader, *fallback, rhi::Format::Backbuffer);
            gm.setParameters(engine::packMaterialParameters(shader, mat));
            std::printf("gpu OK: %s -> pipeline, set and %d-slot parameter block (%s)\n",
                        mat.name.c_str(), gm.slotCount(), useGl ? "GL" : "Vulkan");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "gpu FAILED: %s\n", e.what());
            return 1;
        }
    } else if (cmd == QLatin1String("preview") && args.size() == 5) {
        // preview <assetsDir> <Name.ukmat> <out.png>: draws the material on a sphere through the
        // Vulkan pipeline (headless) and writes the frame as a PNG.
        const QString assets = args[2];
        engine::MaterialAsset mat;
        engine::ShaderAsset shader;
        std::string error;
        if (!engine::loadMaterialAsset(assets.toStdString() + "/Materials/" + args[3].toStdString(), mat, error)
            || !engine::loadShaderAsset(assets.toStdString() + "/" + mat.shaderPath, shader, error)) {
            std::fprintf(stderr, "preview FAILED: %s\n", error.c_str());
            return 1;
        }
        try {
            rhi::DeviceConfig cfg;
            cfg.width = 256;
            cfg.height = 256;
            cfg.headless = true;
            cfg.validation = true;
            auto device = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);
            rhi::TextureDesc td;
            const uint8_t white[4] = {255, 255, 255, 255};
            auto fallback = device->createTexture(td, white);
            ukiyo::GraphMaterial gm(*device, shader, *fallback, rhi::Format::Backbuffer);
            gm.setParameters(engine::packMaterialParameters(shader, mat));
            const std::vector<uint8_t> px = ukiyo::renderMaterialSphere(*device, gm);
            const std::string out = args[4].toStdString();
            const int w = int(device->backbufferWidth());
            const int h = int(device->backbufferHeight());
            if (!stbi_write_png(out.c_str(), w, h, 4, px.data(), w * 4)) {
                std::fprintf(stderr, "preview FAILED: cannot write %s\n", out.c_str());
                return 1;
            }
            std::printf("preview OK: %s -> %s (%dx%d)\n", mat.name.c_str(), out.c_str(), w, h);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "preview FAILED: %s\n", e.what());
            return 1;
        }
    } else if (cmd == QLatin1String("scene") && args.size() == 6) {
        // scene <assetsDir> <Name.ukmat> <model> <out.png>: renders a model through the engine
        // Renderer (headless Vulkan) with every primitive drawn by the material's shader graph.
        const QString assets = args[2];
        engine::MaterialAsset mat;
        engine::ShaderAsset shader;
        std::string error;
        if (!engine::loadMaterialAsset(assets.toStdString() + "/Materials/" + args[3].toStdString(), mat, error)
            || !engine::loadShaderAsset(assets.toStdString() + "/" + mat.shaderPath, shader, error)) {
            std::fprintf(stderr, "scene FAILED: %s\n", error.c_str());
            return 1;
        }
        try {
            rhi::DeviceConfig cfg;
            cfg.width = 640;
            cfg.height = 640;
            cfg.headless = true;
            cfg.validation = true;
            auto device = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);
            rhi::TextureDesc td;
            const uint8_t white[4] = {255, 255, 255, 255};
            auto fallback = device->createTexture(td, white);
            ukiyo::GraphMaterial gm(*device, shader, *fallback, rhi::Format::RGBA16F);
            gm.setParameters(engine::packMaterialParameters(shader, mat));
            auto model = ukiyo::loadModel(*device, args[4].toStdString());
            if (!model) throw std::runtime_error("cannot load model " + args[4].toStdString());
            ukiyo::Renderer renderer(*device);
            for (ukiyo::Primitive& prim : model->primitives) renderer.setGraphMaterial(&prim, &gm);
            ukiyo::OrbitCamera cam;
            ukiyo::RenderSettings settings;
            ukiyo::updateModel(*model, 0.0f, cam.eye());
            if (!device->beginFrame()) throw std::runtime_error("the device skipped the frame");
            std::vector<ukiyo::Model*> models{model.get()};
            renderer.render(cam, models, 0.0f, 0, settings);
            device->endFrame(true);
            const std::vector<uint8_t> px = device->takeCapture();
            device->waitIdle();
            const std::string out = args[5].toStdString();
            const int w = int(device->backbufferWidth());
            const int h = int(device->backbufferHeight());
            if (px.size() < size_t(w) * size_t(h) * 4 || !stbi_write_png(out.c_str(), w, h, 4, px.data(), w * 4)) {
                std::fprintf(stderr, "scene FAILED: cannot write %s\n", out.c_str());
                return 1;
            }
            std::printf("scene OK: %s on %s -> %s (%dx%d, %zu primitives)\n", mat.name.c_str(),
                        args[4].toStdString().c_str(), out.c_str(), w, h, model->primitives.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "scene FAILED: %s\n", e.what());
            return 1;
        }
    } else if (cmd == QLatin1String("roundtrip") && args.size() == 3) {
        QFile f(args[2]);
        if (!f.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "cannot read %s\n", qPrintable(args[2]));
            return 1;
        }
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QtNodes::DataFlowGraphModel m(sg::makeRegistry());
        m.load(root.value(QStringLiteral("graph")).toObject());
        ok = roundtripCheck(m, args[2]);
    } else {
        std::fprintf(stderr, "bad arguments\n");
        return 2;
    }
    return ok ? 0 : 1;
}
