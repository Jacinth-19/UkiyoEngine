// Asset previews for the Project view. Textures are decoded with QImageReader. VRM and GLB files
// carry their own thumbnail inside the binary chunk, which is read directly without loading the
// whole model. Everything is cached per file and refreshed when the file's modification time
// changes.
#pragma once

#include <functional>

#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <QStringList>

namespace editor::assets {

constexpr int kPreviewPx = 256;

// Short type name shown in the details pane: "Folder", "VRM model", "Texture", ...
QString kindOf(const QFileInfo& info);

// Thumbnail image (at most kPreviewPx on its longest side). Null when the file has none.
QImage previewImage(const QString& path);

// Icon for file-system views: the thumbnail when there is one, otherwise a Material Symbols glyph.
QIcon fileIcon(const QFileInfo& info);

// "Label: value" lines for the details pane (format, dimensions, model metadata, clips, size).
QStringList details(const QString& path);

// Maps a .ukprefab to the project file of the model its preview renders. Set by the editor. An empty result
// means no preview.
void setPrefabModelResolver(std::function<QString(const QString& prefabPath)> resolver);

// Announces that a live render (a material sphere or a 3D model) has finished for a file. Views
// refresh that file's icon when it arrives; until then they show the placeholder.
class PreviewHub : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

signals:
    void ready(const QString& path);
};

PreviewHub& previewHub();

} // namespace editor::assets
