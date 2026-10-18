#include "material_thumbnail.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/camera.h"
#include "engine/graph_material.h"
#include "engine/material_asset.h"
#include "engine/material_preview.h"
#include "engine/model.h"
#include "engine/renderer.h"
#include "rhi/rhi.h"

namespace editor::assets {
namespace {

// The shared render backbone. Only the preview worker thread touches it, so it needs no locking.
// Members are destroyed in reverse declaration order: caches first, then the renderer, and the
// device last.
struct Backbone {
    int size = 0;
    bool failed = false;                       // device creation failed; do not retry each time
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Texture> fallback;
    std::unique_ptr<ukiyo::Renderer> renderer;
    struct Compiled {
        QDateTime modified;
        engine::ShaderAsset shader;
        std::unique_ptr<ukiyo::GraphMaterial> material;
    };
    struct Loaded {
        QDateTime modified;
        std::unique_ptr<ukiyo::Model> model;
    };
    std::map<std::string, Compiled> shaders;   // keyed by shader path (Compiled holds a unique_ptr)
    std::map<std::string, Loaded> models;      // keyed by model path
};

// Changing the size means a new device, and every cached object is tied to the old one.
Backbone& backbone(int size)
{
    static Backbone b;
    if (b.size != size) {
        b.models.clear();
        b.shaders.clear();
        b.renderer.reset();
        b.fallback.reset();
        b.device.reset();
        b.failed = false;
        b.size = size;
    }
    return b;
}

bool ensureDevice(Backbone& r, int sizePx, QString* error)
{
    if (r.device) return true;
    if (r.failed) {
        if (error) *error = QStringLiteral("Vulkan is not available");
        return false;
    }
    try {
        rhi::DeviceConfig cfg;
        cfg.width = static_cast<uint32_t>(sizePx);
        cfg.height = static_cast<uint32_t>(sizePx);
        cfg.headless = true;
        r.device = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);
        const uint8_t white[4] = {255, 255, 255, 255};
        r.fallback = r.device->createTexture(rhi::TextureDesc{}, white);
        r.renderer = std::make_unique<ukiyo::Renderer>(*r.device);
        return true;
    } catch (const std::exception& e) {
        r.failed = true;
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

QImage copyRgba(const std::vector<uint8_t>& px, int w, int h)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y)
        std::memcpy(img.scanLine(y), px.data() + size_t(y) * size_t(w) * 4, size_t(w) * 4);
    return img.copy();
}

} // namespace

QImage renderMaterialThumbnail(const QString& ukmatPath, int sizePx, QString* error)
{
    auto fail = [error](const QString& why) {
        if (error) *error = why;
        return QImage();
    };
    Backbone& r = backbone(sizePx);
    QString deviceError;
    if (!ensureDevice(r, sizePx, &deviceError)) return fail(deviceError);

    engine::MaterialAsset mat;
    std::string err;
    if (!engine::loadMaterialAsset(ukmatPath.toStdString(), mat, err)) return fail(QString::fromStdString(err));

    // Shader paths in a material are relative to the assets folder, which is the parent of Materials/.
    QDir assets(QFileInfo(ukmatPath).absolutePath());
    assets.cdUp();
    const QString shaderPath = assets.filePath(QString::fromStdString(mat.shaderPath));
    const QFileInfo shaderInfo(shaderPath);
    if (!shaderInfo.exists()) return fail(QStringLiteral("Missing shader: ") + QString::fromStdString(mat.shaderPath));

    try {
        const std::string key = shaderPath.toStdString();
        auto it = r.shaders.find(key);
        if (it == r.shaders.end() || it->second.modified != shaderInfo.lastModified()) {
            Backbone::Compiled c;
            c.modified = shaderInfo.lastModified();
            if (!engine::loadShaderAsset(key, c.shader, err)) return fail(QString::fromStdString(err));
            c.material = std::make_unique<ukiyo::GraphMaterial>(*r.device, c.shader, *r.fallback,
                                                                rhi::Format::Backbuffer);
            // emplace does not replace an existing key, so the stale entry is erased first.
            r.shaders.erase(key);
            it = r.shaders.emplace(key, std::move(c)).first;
        }
        it->second.material->setParameters(engine::packMaterialParameters(it->second.shader, mat));
        // The cached material is shared by every material that uses this shader, so each render binds this
        // material's own textures. An empty map restores the fallback for every slot. A texture that cannot be
        // read leaves its slot on the fallback; the sphere still draws.
        std::string texErr;
        it->second.material->setTextureFiles(*r.device,
            engine::resolveTexturePaths(it->second.shader, mat, assets.absolutePath().toStdString()), texErr);
        const std::vector<uint8_t> px = ukiyo::renderMaterialSphere(*r.device, *it->second.material);
        return copyRgba(px, sizePx, sizePx);
    } catch (const std::exception& e) {
        return fail(QString::fromUtf8(e.what()));
    }
}

QImage renderModelThumbnail(const QString& modelPath, int sizePx, QString* error)
{
    auto fail = [error](const QString& why) {
        if (error) *error = why;
        return QImage();
    };
    Backbone& r = backbone(sizePx);
    QString deviceError;
    if (!ensureDevice(r, sizePx, &deviceError)) return fail(deviceError);

    const QFileInfo info(modelPath);
    if (!info.exists()) return fail(QStringLiteral("Missing model"));
    const std::string key = modelPath.toStdString();

    try {
        auto it = r.models.find(key);
        if (it == r.models.end() || it->second.modified != info.lastModified()) {
            auto m = ukiyo::loadModel(*r.device, key);
            if (!m) return fail(QStringLiteral("could not load the model"));
            m->physics = false;        // a still frame needs no spring-bone simulation
            m->armCollision = false;
            r.models.erase(key);
            it = r.models.emplace(key, Backbone::Loaded{info.lastModified(), std::move(m)}).first;
        }
        ukiyo::Model& model = *it->second.model;

        // Framed like the editor's default camera, which fits a person-sized model. Node origins
        // are not a reliable bounding box (a rig's feet and hands can span less than its body), so
        // the framing does not depend on them.
        ukiyo::OrbitCamera cam;
        cam.yaw = 0.0f;
        cam.pitch = 0.05f;
        ukiyo::updateModel(model, 0.0f, cam.eye());

        ukiyo::RenderSettings settings;
        settings.aaMode = 0;
        settings.shadows = false;
        settings.outlines = false;
        settings.bloom = false;

        if (!r.device->beginFrame()) return fail(QStringLiteral("the device skipped the frame"));
        std::vector<ukiyo::Model*> models{&model};
        r.renderer->render(cam, models, 0.0f, 0, settings);
        r.device->endFrame(true);
        const std::vector<uint8_t> px = r.device->takeCapture();
        r.device->waitIdle();
        const int w = static_cast<int>(r.device->backbufferWidth());
        const int h = static_cast<int>(r.device->backbufferHeight());
        if (px.size() < size_t(w) * size_t(h) * 4) return fail(QStringLiteral("no frame was captured"));
        return copyRgba(px, w, h);
    } catch (const std::exception& e) {
        return fail(QString::fromUtf8(e.what()));
    }
}

} // namespace editor::assets
