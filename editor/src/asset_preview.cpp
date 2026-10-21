#include "asset_preview.h"

#include <algorithm>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>

#include <functional>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRunnable>
#include <QThreadPool>
#include <QCoreApplication>
#include <QSet>
#include <QPixmap>
#include <QtEndian>

#include "icons.h"
#include "material_thumbnail.h"

#include "engine/material_asset.h"

namespace editor::assets {

namespace {

constexpr quint32 kChunkJson = 0x4E4F534Au; // "JSON"
constexpr quint32 kChunkBin = 0x004E4942u;  // "BIN\0"
constexpr int kIconPx = 256;

struct Entry {
    QDateTime modified;
    QDateTime dependency;   // .ukmat: modification time of its shader, so a shader edit rebuilds it
    QString kind;
    QImage preview;
    QStringList details;
    QIcon icon;
};

QHash<QString, Entry>& cache() {
    static QHash<QString, Entry> c;
    return c;
}

const QStringList& imageSuffixes() {
    static const QStringList s{"png", "jpg", "jpeg", "bmp", "tga", "webp", "gif", "tif", "tiff"};
    return s;
}

const QStringList& audioSuffixes() {
    static const QStringList s{"wav", "mp3", "ogg", "flac", "opus", "m4a"};
    return s;
}

const QStringList& videoSuffixes() {
    static const QStringList s{"mp4", "webm", "mov", "mkv", "avi"};
    return s;
}

QString humanSize(qint64 bytes) {
    if (bytes < 1024) return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 2) + " MB";
}

// A binary glTF file: the parsed JSON chunk plus the location of the BIN chunk.
struct Glb {
    bool valid = false;
    QJsonObject json;
    qint64 binStart = 0;
    qint64 binLength = 0;
};

Glb readGlb(const QString& path) {
    Glb g;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return g;

    const QByteArray header = f.read(12);
    if (header.size() != 12 || header.left(4) != "glTF") return g;
    const quint32 version = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header.constData() + 4));
    if (version != 2) return g;

    while (!f.atEnd()) {
        const QByteArray chunk = f.read(8);
        if (chunk.size() != 8) break;
        const quint32 len = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(chunk.constData()));
        const quint32 type = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(chunk.constData() + 4));
        const qint64 start = f.pos();
        if (type == kChunkJson) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(f.read(len), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) return g;
            g.json = doc.object();
            g.valid = true;
        } else if (type == kChunkBin) {
            g.binStart = start;
            g.binLength = len;
        }
        if (!f.seek(start + len)) break;
    }
    return g;
}

QImage decodeImage(const QString& path, QSize* fullSize, QString* format) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (format) *format = QString::fromLatin1(reader.format()).toUpper();
    const QSize full = reader.size();
    if (fullSize) *fullSize = full;
    if (full.isValid() && std::max(full.width(), full.height()) > kPreviewPx)
        reader.setScaledSize(full.scaled(kPreviewPx, kPreviewPx, Qt::KeepAspectRatio));
    return reader.read();
}

// VRM 1.0 names the thumbnail image directly. VRM 0.x names a texture, whose source is the image.
// --- Live renders -----------------------------------------------------------------------------
// Materials and 3D models are drawn by the engine on a background thread, so browsing never
// waits for a render. A file shows its placeholder until the render arrives; then its cache entry
// is rebuilt with the live image and PreviewHub announces the file.

qint64 renderStamp(const QFileInfo& info);

struct LiveRender {
    qint64 modifiedMs = -1;
    QImage image;
    QString error;
};

QHash<QString, LiveRender>& liveRenders() {
    static QHash<QString, LiveRender> r;
    return r;
}

QSet<QString>& pendingRenders() {
    static QSet<QString> p;
    return p;
}

// One thread: the render backbone is not shared, and the Vulkan device stays on the thread that
// created it. Idle threads are kept alive so the device survives between renders.
QThreadPool* renderPool() {
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(1);
        p->setExpiryTimeout(-1);
        return p;
    }();
    return pool;
}

// A .cube colour LUT thumbnail: an 8x8 grid of colours sampled from the table at mid blue, so the grade
// is visible at a glance. Null when the file is not a readable 3D .cube.
QImage cubeThumbnail(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QImage();
    int n = 0;
    std::vector<float> vals;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        if (line.startsWith(QLatin1String("LUT_3D_SIZE"))) {
            n = line.mid(11).trimmed().toInt();
            continue;
        }
        const QStringList t = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (t.size() != 3) continue;
        bool ok0 = false, ok1 = false, ok2 = false;
        const float r = t[0].toFloat(&ok0), g = t[1].toFloat(&ok1), b = t[2].toFloat(&ok2);
        if (ok0 && ok1 && ok2) {
            vals.push_back(r);
            vals.push_back(g);
            vals.push_back(b);
        }
    }
    if (n < 2 || vals.size() != size_t(n) * size_t(n) * size_t(n) * 3) return QImage();
    constexpr int cells = 8, cell = 12;
    QImage img(cells * cell, cells * cell, QImage::Format_ARGB32);
    const int b = (n - 1) / 2;
    QPainter painter(&img);
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            const int r = i * (n - 1) / (cells - 1), g = j * (n - 1) / (cells - 1);
            const size_t idx = (size_t(r) + size_t(g) * n + size_t(b) * n * n) * 3;
            auto to8 = [](float v) { return std::clamp(int(v * 255.0f + 0.5f), 0, 255); };
            painter.fillRect(QRect(i * cell, j * cell, cell, cell),
                             QColor(to8(vals[idx]), to8(vals[idx + 1]), to8(vals[idx + 2])));
        }
    painter.end();
    return img;
}

bool isLiveSuffix(const QString& suffix) {
    return suffix == QLatin1String("ukmat") || suffix == QLatin1String("vrm")
        || suffix == QLatin1String("glb") || suffix == QLatin1String("gltf");
}

void finishLiveRender(const QString& path, qint64 ms, const QImage& image, const QString& error) {
    pendingRenders().remove(path);
    liveRenders()[path] = LiveRender{ms, image, error};
    cache().remove(path);   // the next lookup rebuilds the entry with the live image
    emit previewHub().ready(path);
}

std::function<QString(const QString&)>& prefabModelResolver() {
    static std::function<QString(const QString&)> r;
    return r;
}

void requestLiveRender(const QFileInfo& info) {
    const QString path = info.absoluteFilePath();
    const QString suffix = info.suffix().toLower();
    const bool prefabFile = suffix == QLatin1String("ukprefab");
    if ((!isLiveSuffix(suffix) && !prefabFile) || pendingRenders().contains(path)) return;
    // A prefab is drawn from its first model.
    QString modelPath = path;
    if (prefabFile) {
        modelPath = prefabModelResolver() ? prefabModelResolver()(path) : QString();
        if (modelPath.isEmpty()) return;
    }
    const qint64 ms = renderStamp(info);
    pendingRenders().insert(path);
    const bool material = suffix == QLatin1String("ukmat");
    renderPool()->start(QRunnable::create([path, modelPath, ms, material] {
        QString error;
        const QImage image = material ? editor::assets::renderMaterialThumbnail(path, kIconPx, &error)
                                      : editor::assets::renderModelThumbnail(modelPath, kIconPx, &error);
        QMetaObject::invokeMethod(qApp, [path, ms, image, error] { finishLiveRender(path, ms, image, error); },
                                  Qt::QueuedConnection);
    }));
}

// Applies a finished live render to an entry, or asks for one. The placeholder stays otherwise.
void applyLiveRender(const QFileInfo& info, Entry& e) {
    const QString path = info.absoluteFilePath();
    const qint64 ms = renderStamp(info);
    const auto it = liveRenders().constFind(path);
    if (it == liveRenders().constEnd() || it->modifiedMs != ms) {
        requestLiveRender(info);
        return;
    }
    if (!it->image.isNull()) {
        e.preview = it->image;
        e.icon = QIcon(QPixmap::fromImage(e.preview));
    }
    if (!it->error.isEmpty()) e.details << QStringLiteral("Preview: ") + it->error;
}

QImage vrmThumbnail(const QString& path, const Glb& g) {
    const QJsonObject ext = g.json.value("extensions").toObject();
    const QJsonArray images = g.json.value("images").toArray();
    const QJsonArray textures = g.json.value("textures").toArray();
    const QJsonArray views = g.json.value("bufferViews").toArray();

    int image = -1;
    if (ext.contains("VRMC_vrm")) {
        const QJsonObject meta = ext.value("VRMC_vrm").toObject().value("meta").toObject();
        image = meta.value("thumbnailImage").toInt(-1);
    } else if (ext.contains("VRM")) {
        const QJsonObject meta = ext.value("VRM").toObject().value("meta").toObject();
        const int tex = meta.value("texture").toInt(-1);
        if (tex >= 0 && tex < textures.size())
            image = textures.at(tex).toObject().value("source").toInt(-1);
    }
    if (image < 0 || image >= images.size()) return QImage();

    const int view = images.at(image).toObject().value("bufferView").toInt(-1);
    if (view < 0 || view >= views.size()) return QImage();
    const QJsonObject bv = views.at(view).toObject();
    const qint64 offset = bv.value("byteOffset").toInteger(0);
    const qint64 length = bv.value("byteLength").toInteger(0);
    if (length <= 0 || offset < 0 || offset + length > g.binLength) return QImage();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(g.binStart + offset)) return QImage();
    QImage thumb = QImage::fromData(f.read(length));
    if (thumb.width() > kPreviewPx || thumb.height() > kPreviewPx)
        thumb = thumb.scaled(kPreviewPx, kPreviewPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return thumb;
}

QString joinNames(const QJsonArray& arr, int limit) {
    QStringList names;
    for (const QJsonValue& v : arr) {
        const QString n = v.toObject().value("name").toString();
        if (!n.isEmpty()) names << n;
        if (names.size() >= limit) break;
    }
    if (arr.size() > limit) names << QStringLiteral("...");
    return names.join(QStringLiteral(", "));
}

// Scene icon drawn in the Unity scene style: a blue cube standing on a grid floor under a sun.
QImage sceneThumbnail() {
    const int n = kIconPx;
    QImage img(n, n, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient bg(0, 0, 0, n);
    bg.setColorAt(0.0, QColor(0x3a, 0x45, 0x55));
    bg.setColorAt(1.0, QColor(0x20, 0x24, 0x2b));
    QPainterPath tile;
    tile.addRoundedRect(QRectF(8, 8, n - 16, n - 16), 26, 26);
    p.fillPath(tile, bg);

    // Floor: a perspective grid at the bottom of the tile.
    p.save();
    p.setClipPath(tile);
    QPen grid(QColor(0x5b, 0x67, 0x78), 2);
    p.setPen(grid);
    for (int i = -4; i <= 4; ++i) {
        p.drawLine(QPointF(n / 2.0 + i * 14, n * 0.66), QPointF(n / 2.0 + i * 46, n));
    }
    for (int j = 0; j < 4; ++j) {
        const double y = n * 0.66 + (n * 0.34) * (j + 1) / 4.0 * (j + 1) / 4.0;
        p.drawLine(QPointF(0, y), QPointF(n, y));
    }
    p.restore();

    // Sun.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xf5, 0xc0, 0x4a));
    p.drawEllipse(QPointF(n * 0.74, n * 0.27), n * 0.09, n * 0.09);

    // Cube: top, left and right faces.
    const QPointF c(n * 0.5, n * 0.52);
    const double r = n * 0.22;
    const QPointF top(c.x(), c.y() - r);
    const QPointF upL(c.x() - r * 0.9, c.y() - r * 0.5);
    const QPointF upR(c.x() + r * 0.9, c.y() - r * 0.5);
    const QPointF mid(c.x(), c.y());
    const QPointF loL(c.x() - r * 0.9, c.y() + r * 0.5);
    const QPointF loR(c.x() + r * 0.9, c.y() + r * 0.5);
    const QPointF bot(c.x(), c.y() + r);
    p.setPen(QPen(QColor(0x16, 0x3a, 0x5c), 2));
    p.setBrush(QColor(0x9c, 0xd2, 0xff));
    p.drawPolygon(QPolygonF() << top << upR << mid << upL);
    p.setBrush(QColor(0x3d, 0x8f, 0xe0));
    p.drawPolygon(QPolygonF() << upL << mid << bot << loL);
    p.setBrush(QColor(0x25, 0x6b, 0xb8));
    p.drawPolygon(QPolygonF() << upR << loR << bot << mid);
    p.end();
    return img;
}

// Shared dark tile used by the asset-type icons below.
QPainter& beginTile(QPainter& p, QImage& img)
{
    const int n = kIconPx;
    p.setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient bg(0, 0, 0, n);
    bg.setColorAt(0.0, QColor(0x3a, 0x45, 0x55));
    bg.setColorAt(1.0, QColor(0x20, 0x24, 0x2b));
    QPainterPath tile;
    tile.addRoundedRect(QRectF(8, 8, n - 16, n - 16), 26, 26);
    p.fillPath(tile, bg);
    (void)img;
    return p;
}

// Shader icon: three linked nodes, the way a shader graph looks in the editor.
QImage shaderThumbnail()
{
    const int n = kIconPx;
    QImage img(n, n, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    beginTile(p, img);
    const QColor wire(0x7f, 0xd6, 0xc4);
    p.setPen(QPen(wire, 6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(n * 0.30, n * 0.36), QPointF(n * 0.52, n * 0.36));
    p.drawLine(QPointF(n * 0.30, n * 0.64), QPointF(n * 0.52, n * 0.64));
    p.drawLine(QPointF(n * 0.52, n * 0.50), QPointF(n * 0.70, n * 0.50));
    auto node = [&](double x, double y, const QColor& c) {
        p.setPen(QPen(QColor(0x16, 0x1a, 0x20), 3));
        p.setBrush(c);
        p.drawRoundedRect(QRectF(x - n * 0.10, y - n * 0.08, n * 0.20, n * 0.16), 8, 8);
    };
    node(n * 0.24, n * 0.36, QColor(0xf5, 0xc0, 0x4a));
    node(n * 0.24, n * 0.64, QColor(0x9c, 0xd2, 0xff));
    node(n * 0.60, n * 0.50, QColor(0x7f, 0xd6, 0xc4));
    node(n * 0.80, n * 0.50, QColor(0xe0, 0x7a, 0x7a));
    p.end();
    return img;
}

// Material placeholder: a lit sphere. The live material preview replaces it later.
QImage materialThumbnail()
{
    const int n = kIconPx;
    QImage img(n, n, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    beginTile(p, img);
    QRadialGradient g(QPointF(n * 0.40, n * 0.36), n * 0.36);
    g.setColorAt(0.0, QColor(0xf2, 0xf4, 0xf8));
    g.setColorAt(0.5, QColor(0x6f, 0x8f, 0xc4));
    g.setColorAt(1.0, QColor(0x1e, 0x26, 0x3a));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(QPointF(n * 0.5, n * 0.5), n * 0.28, n * 0.28);
    p.end();
    return img;
}

// The newest time among the files a .ukmat draws with: its shader, and each texture the material
// binds (its own override or the shader's default). Assets folder: the parent of Materials/.
QDateTime dependencyTime(const QFileInfo& info) {
    if (info.suffix().toLower() != QLatin1String("ukmat")) return QDateTime();
    engine::MaterialAsset mat;
    std::string err;
    if (!engine::loadMaterialAsset(info.absoluteFilePath().toStdString(), mat, err) || mat.shaderPath.empty()) return QDateTime();
    QDir assets(info.absolutePath());
    assets.cdUp();
    const QString shaderPath = assets.filePath(QString::fromStdString(mat.shaderPath));
    QDateTime newest = QFileInfo(shaderPath).lastModified();
    engine::ShaderAsset shader;
    if (engine::loadShaderAsset(shaderPath.toStdString(), shader, err)) {
        for (const auto& [binding, path] : engine::resolveTexturePaths(shader, mat, assets.absolutePath().toStdString())) {
            const QDateTime t = QFileInfo(QString::fromStdString(path)).lastModified();
            if (t.isValid() && (!newest.isValid() || t > newest)) newest = t;
        }
    }
    return newest;
}

// The time a live render is keyed on: the file's own time, or for a material the newest of its dependencies,
// so an edit to its shader or to one of its images draws it again.
qint64 renderStamp(const QFileInfo& info) {
    qint64 ms = info.lastModified().toMSecsSinceEpoch();
    const QDateTime dep = dependencyTime(info);
    if (dep.isValid()) ms = std::max(ms, dep.toMSecsSinceEpoch());
    return ms;
}

Entry build(const QFileInfo& info) {
    Entry e;
    e.modified = info.lastModified();
    e.dependency = dependencyTime(info);
    const QString path = info.absoluteFilePath();
    const QString suffix = info.suffix().toLower();

    if (info.isDir()) {
        // Empty folders get the plain outline; folders with contents get a document inside.
        const int items = QDir(path, QString(), QDir::NoSort,
                               QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)
                              .count();
        e.kind = QStringLiteral("Folder");
        e.icon = materialIcon(items == 0 ? "folder" : "snippet_folder", folderColor(), kIconPx);
        e.details << (items == 0 ? QStringLiteral("Contents: empty")
                                 : QString("Contents: %1 item%2").arg(items).arg(items == 1 ? "" : "s"));
        e.details << "Modified: " + info.lastModified().toString("yyyy-MM-dd hh:mm");
        return e;
    }

    if (imageSuffixes().contains(suffix)) {
        QSize full;
        QString format;
        e.preview = decodeImage(path, &full, &format);
        e.kind = QStringLiteral("Texture");
        e.details << QString("Dimensions: %1 x %2").arg(full.width()).arg(full.height())
                  << "Format: " + format
                  << QString("Alpha: ") + (e.preview.hasAlphaChannel() ? "yes" : "no");
        if (!e.preview.isNull()) e.icon = QIcon(QPixmap::fromImage(e.preview));
    } else if (suffix == "vrm" || suffix == "glb") {
        const Glb g = readGlb(path);
        if (!g.valid) {
            e.kind = QStringLiteral("Unreadable model");
            e.details << "Not a valid GLB file";
        } else {
            const QJsonObject ext = g.json.value("extensions").toObject();
            const bool vrm1 = ext.contains("VRMC_vrm");
            const bool vrm0 = !vrm1 && ext.contains("VRM");
            const QJsonArray meshes = g.json.value("meshes").toArray();
            const QJsonArray anims = g.json.value("animations").toArray();
            const QJsonArray materials = g.json.value("materials").toArray();
            const QJsonArray nodes = g.json.value("nodes").toArray();
            const QJsonArray textures = g.json.value("textures").toArray();
            const QJsonArray skins = g.json.value("skins").toArray();

            if (vrm1) e.kind = QStringLiteral("VRM 1.0 model");
            else if (vrm0) e.kind = QStringLiteral("VRM 0.x model");
            else if (!meshes.isEmpty()) e.kind = QStringLiteral("glTF model");
            else if (!anims.isEmpty()) e.kind = QStringLiteral("Animation library");
            else e.kind = QStringLiteral("glTF binary");

            if (vrm1 || vrm0) {
                const QJsonObject ext0 = ext.value(vrm1 ? "VRMC_vrm" : "VRM").toObject();
                const QJsonObject meta = ext0.value("meta").toObject();
                const QString title = vrm1 ? meta.value("name").toString() : meta.value("title").toString();
                const QJsonArray authors = vrm1 ? meta.value("authors").toArray()
                                                : QJsonArray{meta.value("author").toString()};
                if (!title.isEmpty()) e.details << "Title: " + title;
                if (!authors.isEmpty() && !authors.first().toString().isEmpty())
                    e.details << "Author: " + authors.first().toString();
                if (vrm0) {
                    const QString ver = meta.value("version").toString();
                    if (!ver.isEmpty()) e.details << "Model version: " + ver;
                }
                e.preview = vrmThumbnail(path, g);
                if (!e.preview.isNull()) e.icon = QIcon(QPixmap::fromImage(e.preview));
            }
            e.details << QString("Meshes: %1").arg(meshes.size())
                      << QString("Materials: %1").arg(materials.size())
                      << QString("Textures: %1").arg(textures.size())
                      << QString("Nodes: %1   Skins: %2").arg(nodes.size()).arg(skins.size())
                      << QString("Animation clips: %1").arg(anims.size());
            if (!anims.isEmpty()) e.details << "Clips: " + joinNames(anims, 12);
        }
        applyLiveRender(info, e);   // a 3D render replaces the embedded thumbnail when it is ready
        if (e.icon.isNull())
            e.icon = materialIcon(e.kind.contains("Animation") ? "animation" : "person", iconColor(), kIconPx);
    } else if (suffix == "gltf") {
        e.kind = QStringLiteral("glTF (JSON)");
        e.icon = materialIcon("deployed_code", iconColor(), kIconPx);
        applyLiveRender(info, e);
    } else if (suffix == "cube") {
        e.kind = QStringLiteral("Colour LUT");
        e.preview = cubeThumbnail(path);
        if (!e.preview.isNull()) e.icon = QIcon(QPixmap::fromImage(e.preview));
        e.details << "Format: .cube 3D LUT";
    } else if (audioSuffixes().contains(suffix)) {
        e.kind = QStringLiteral("Audio");
        e.icon = materialIcon("audio_file", iconColor(), kIconPx);
        e.details << "Format: " + suffix.toUpper();
    } else if (videoSuffixes().contains(suffix)) {
        e.kind = QStringLiteral("Video");
        e.icon = materialIcon("movie", iconColor(), kIconPx);
        e.details << "Format: " + suffix.toUpper();
    } else if (suffix == "ukshader") {
        e.kind = QStringLiteral("Shader Graph");
        e.preview = shaderThumbnail();
        e.icon = QIcon(QPixmap::fromImage(e.preview));
        e.details << "Format: UKSHADER (JSON graph)";
    } else if (suffix == "ukmat") {
        e.kind = QStringLiteral("Material");
        e.preview = materialThumbnail();
        e.icon = QIcon(QPixmap::fromImage(e.preview));
        e.details << "Format: UKMAT (JSON)";
        applyLiveRender(info, e);   // the material drawn on a sphere replaces the placeholder
    } else if (suffix == "ukprefab") {
        e.kind = QStringLiteral("Ukiyo prefab");
        e.icon = materialIcon("view_in_ar", iconColor(), kIconPx);
        e.details << "Format: UKPREFAB (JSON)";
        applyLiveRender(info, e);   // a render of the prefab's first model replaces the placeholder
    } else if (suffix == "inputactions") {
        // An input asset is a first-class kind in this engine, so it gets a picture like the others instead of the
        // generic document. The audit sheet checks that every asset kind has one.
        e.kind = QStringLiteral("Input Actions");
        const QIcon ic = materialIcon("keyboard", iconColor(), kIconPx);
        e.icon = ic;
        e.preview = ic.pixmap(QSize(kPreviewPx / 2, kPreviewPx / 2)).toImage();
        e.details << "Format: INPUTACTIONS (JSON)";
    } else if (suffix == "ukscene") {
        e.kind = QStringLiteral("Ukiyo scene");
        e.preview = sceneThumbnail();
        e.icon = QIcon(QPixmap::fromImage(e.preview));
        e.details << "Format: UKSCENE (JSON)";
    } else {
        e.kind = suffix.isEmpty() ? QStringLiteral("File") : suffix.toUpper() + " file";
        e.icon = materialIcon("description", iconColor(), kIconPx);
    }

    e.details << "File size: " + humanSize(info.size())
              << "Modified: " + info.lastModified().toString("yyyy-MM-dd hh:mm");
    return e;
}

// Returns the cached entry, rebuilding it when the file changed.
Entry entryFor(const QFileInfo& info) {
    auto& c = cache();
    const QString key = info.absoluteFilePath();
    auto it = c.constFind(key);
    if (it != c.constEnd() && it->modified == info.lastModified() && it->dependency == dependencyTime(info))
        return it.value();
    Entry e = build(info);
    c.insert(key, e);
    return e;
}

} // namespace

QString kindOf(const QFileInfo& info) {
    return entryFor(info).kind;
}

void setPrefabModelResolver(std::function<QString(const QString&)> resolver) {
    prefabModelResolver() = std::move(resolver);
}

PreviewHub& previewHub() {
    static PreviewHub* hub = new PreviewHub;
    return *hub;
}

QImage previewImage(const QString& path) {
    return entryFor(QFileInfo(path)).preview;
}

QIcon fileIcon(const QFileInfo& info) {
    return entryFor(info).icon;
}

QStringList details(const QString& path) {
    const QFileInfo info(path);
    const Entry e = entryFor(info);
    return QStringList{"Kind: " + e.kind} + e.details;
}

} // namespace editor::assets
