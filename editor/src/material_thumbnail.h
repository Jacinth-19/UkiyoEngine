#pragma once

// Live previews for assets, drawn by the engine's Vulkan pipeline through one shared render
// backbone (a headless device, the engine renderer, and caches of compiled materials and loaded
// models). Every call runs on the preview worker thread (asset_preview.cpp), never the GUI thread.
// A null image means the preview could not be made; the caller then keeps the placeholder.

#include <QImage>
#include <QString>

namespace editor::assets {

// A .ukmat drawn on a sphere.
QImage renderMaterialThumbnail(const QString& ukmatPath, int sizePx, QString* error);

// A .vrm, .glb or .gltf model drawn by the engine renderer and framed to its bounds.
QImage renderModelThumbnail(const QString& modelPath, int sizePx, QString* error);

} // namespace editor::assets
