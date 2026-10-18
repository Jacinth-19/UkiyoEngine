#include "material_inspector.h"

#include <set>
#include <string>
#include <vector>

#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "engine/material_asset.h"

namespace editor {

namespace {

// The assets folder is the parent of the material's Materials folder.
QDir assetsFolderOf(const QString& ukmatPath)
{
    QDir assets(QFileInfo(ukmatPath).absolutePath());
    assets.cdUp();
    return assets;
}

} // namespace

MaterialInspector::MaterialInspector(QWidget* parent)
    : QWidget(parent)
{
    outer_ = new QVBoxLayout(this);
    outer_->setContentsMargins(0, 6, 0, 0);
    outer_->setSpacing(4);
    title_ = new QLabel;
    title_->setObjectName(QStringLiteral("materialInspectorTitle"));
    title_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    outer_->addWidget(title_);
    hide();
}

void MaterialInspector::setMaterial(const QString& ukmatPath)
{
    const bool isMaterial = !ukmatPath.isEmpty() && QFileInfo(ukmatPath).suffix().compare(QLatin1String("ukmat"), Qt::CaseInsensitive) == 0;
    path_ = isMaterial ? ukmatPath : QString();
    if (!isMaterial) {
        hide();
        return;
    }
    show();
    rebuild();
}

void MaterialInspector::rebuild()
{
    // Later than this call: a shader switch or a reset is made from inside one of these widgets.
    if (body_) {
        body_->hide();
        body_->deleteLater();
        body_ = nullptr;
    }
    title_->setText(QStringLiteral("Material: ") + QFileInfo(path_).completeBaseName());

    body_ = new QWidget;
    auto* form = new QFormLayout(body_);
    form->setContentsMargins(0, 0, 0, 0);
    // Narrow panels: let the labels and fields share the width instead of forcing a horizontal scroll.
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    outer_->addWidget(body_);

    engine::MaterialAsset mat;
    std::string err;
    if (!engine::loadMaterialAsset(path_.toStdString(), mat, err)) {
        form->addRow(new QLabel(QString::fromStdString(err)));
        return;
    }
    const QDir assets = assetsFolderOf(path_);

    // The shader choice lists every .ukshader in the project's Shaders folder.
    auto* shaderBox = new QComboBox;
    shaderBox->setObjectName(QStringLiteral("materialShader"));
    const QString current = QString::fromStdString(mat.shaderPath);
    for (const QString& file : QDir(assets.filePath(QStringLiteral("Shaders")))
             .entryList({QStringLiteral("*.ukshader")}, QDir::Files, QDir::Name)) {
        shaderBox->addItem(file, QStringLiteral("Shaders/") + file);
    }
    int at = shaderBox->findData(current);
    if (at < 0) {
        shaderBox->addItem(current + QStringLiteral(" (missing)"), current);
        at = shaderBox->count() - 1;
    }
    shaderBox->setCurrentIndex(at);
    connect(shaderBox, QOverload<int>::of(&QComboBox::activated), this, [this, shaderBox](int) {
        setShader(shaderBox->currentData().toString());
    });
    form->addRow(QStringLiteral("Shader"), shaderBox);

    auto* actions = new QWidget;
    auto* ah = new QHBoxLayout(actions);
    ah->setContentsMargins(0, 0, 0, 0);
    ah->setSpacing(4);
    auto* copy = new QPushButton(QStringLiteral("Copy Values"));
    copy->setObjectName(QStringLiteral("materialCopy"));
    auto* paste = new QPushButton(QStringLiteral("Paste Values"));
    paste->setObjectName(QStringLiteral("materialPaste"));
    auto* resetAll = new QPushButton(QStringLiteral("Reset All"));
    resetAll->setObjectName(QStringLiteral("materialResetAll"));
    resetAll->setToolTip(QStringLiteral("Use the shader's default for every value and texture"));
    connect(copy, &QPushButton::clicked, this, [this] { copyValues(); });
    connect(paste, &QPushButton::clicked, this, [this] { pasteValues(); });
    connect(resetAll, &QPushButton::clicked, this, [this] { resetAllProperties(); });
    ah->addWidget(copy);
    ah->addWidget(paste);
    ah->addWidget(resetAll);
    ah->addStretch(1);
    form->addRow(actions);

    engine::ShaderAsset shader;
    if (!engine::loadShaderAsset(assets.filePath(current).toStdString(), shader, err)) {
        form->addRow(new QLabel(QStringLiteral("Shader: ") + QString::fromStdString(err)));
        return;
    }
    if (shader.properties.empty() && shader.textures.empty()) {
        form->addRow(new QLabel(QStringLiteral("This shader exposes no properties. Name a value or texture node in the Shader Graph to expose it.")));
        return;
    }

    // Texture slots: a thumbnail of the image in use, its path, and Choose or Clear. A slot the material does not
    // override shows the shader's default. Clear is offered only for an override.
    for (const engine::ShaderTexture& tex : shader.textures) {
        const QString slot = QString::fromStdString(tex.name);
        const auto override = mat.textures.find(tex.name);
        const bool overridden = override != mat.textures.end() && !override->second.empty();
        const QString shown = overridden ? QString::fromStdString(override->second) : QString::fromStdString(tex.defaultPath);
        const QString resolved = shown.isEmpty() ? QString() : QDir::cleanPath(assets.filePath(shown));

        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        auto* thumb = new QLabel;
        thumb->setObjectName(QStringLiteral("materialTextureThumb"));
        thumb->setFixedSize(40, 40);
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setStyleSheet(QStringLiteral("border:1px solid #333; background:#1b1b1b;"));
        const QImage img(resolved);
        if (!resolved.isEmpty() && !img.isNull()) {
            thumb->setPixmap(QPixmap::fromImage(img.scaled(38, 38, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        } else {
            thumb->setText(QStringLiteral("?"));
        }
        h->addWidget(thumb);

        QString pathText = shown.isEmpty() ? QStringLiteral("(none)") : QFileInfo(shown).fileName();
        if (!shown.isEmpty() && !QFileInfo::exists(resolved)) pathText += QStringLiteral(" (missing)");
        pathText += overridden ? QStringLiteral(" [override]") : QStringLiteral(" [default]");
        auto* label = new QLabel(pathText);
        label->setToolTip(resolved.isEmpty() ? QString() : resolved);
        label->setWordWrap(true);
        h->addWidget(label, 1);

        auto* choose = new QToolButton;
        choose->setText(QStringLiteral("Choose..."));
        choose->setObjectName(QStringLiteral("materialTextureChoose"));
        choose->setProperty("slot", slot);
        connect(choose, &QToolButton::clicked, this, [this, slot] {
            const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Choose texture for %1").arg(slot),
                                                              assetsFolderOf(path_).absolutePath(),
                                                              QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.tga)"));
            if (!file.isEmpty()) setTexture(slot, file);
        });
        h->addWidget(choose);

        auto* clear = new QToolButton;
        clear->setText(QStringLiteral("Clear"));
        clear->setObjectName(QStringLiteral("materialTextureClear"));
        clear->setProperty("slot", slot);
        clear->setToolTip(QStringLiteral("Use the shader's default texture"));
        clear->setEnabled(overridden);
        connect(clear, &QToolButton::clicked, this, [this, slot] { clearTexture(slot); });
        h->addWidget(clear);

        form->addRow(slot + QStringLiteral(" (texture)"), row);
    }

    for (const engine::ShaderProperty& prop : shader.properties) {
        const int n = prop.kind == "vec3" ? 3 : prop.kind == "vec2" ? 2 : 1;
        std::vector<float> values = prop.defaults;
        auto it = mat.values.find(prop.name);
        const bool overridden = it != mat.values.end();
        if (overridden) values = it->second;
        values.resize(static_cast<size_t>(n), 0.0f);

        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        QList<QDoubleSpinBox*> boxes;
        for (int k = 0; k < n; ++k) {
            auto* box = new QDoubleSpinBox;
            box->setMinimumWidth(40);
            box->setRange(-100000.0, 100000.0);
            box->setDecimals(3);
            box->setSingleStep(0.05);
            box->setValue(values[static_cast<size_t>(k)]);
            boxes << box;
            h->addWidget(box);
        }
        const QString name = QString::fromStdString(prop.name);
        for (QDoubleSpinBox* box : boxes) {
            connect(box, &QDoubleSpinBox::valueChanged, this, [this, name, boxes] {
                QList<double> v;
                for (QDoubleSpinBox* b : boxes) v << b->value();
                writeProperty(name, v);
            });
        }
        // Reset is only offered when the material overrides the shader's default.
        auto* reset = new QToolButton;
        reset->setText(QStringLiteral("Reset"));
        reset->setObjectName(QStringLiteral("materialReset"));
        reset->setToolTip(QStringLiteral("Use the shader's default value"));
        reset->setEnabled(overridden);
        connect(reset, &QToolButton::clicked, this, [this, name] { resetProperty(name); });
        h->addWidget(reset);
        form->addRow(name + QStringLiteral(" (") + QString::fromStdString(prop.kind) + QStringLiteral(")"), row);
    }
}

QJsonObject MaterialInspector::readRoot() const
{
    QFile in(path_);
    if (!in.open(QIODevice::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(in.readAll()).object();
}

bool MaterialInspector::writeRoot(const QJsonObject& root)
{
    QFile out(path_);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();
    emit edited(path_);
    return true;
}

void MaterialInspector::writeProperty(const QString& name, const QList<double>& values)
{
    QJsonObject root = readRoot();
    if (root.isEmpty()) return;
    QJsonObject props = root.value(QStringLiteral("properties")).toObject();
    if (values.size() == 1) {
        props.insert(name, values.first());
    } else {
        QJsonArray arr;
        for (double v : values) arr.append(v);
        props.insert(name, arr);
    }
    root.insert(QStringLiteral("properties"), props);
    writeRoot(root);
}

void MaterialInspector::writeTexture(const QString& slot, const QString& value)
{
    QJsonObject root = readRoot();
    if (root.isEmpty()) return;
    QJsonObject textures = root.value(QStringLiteral("textures")).toObject();
    if (value.isEmpty()) {
        textures.remove(slot);
    } else {
        textures.insert(slot, value);
    }
    root.insert(QStringLiteral("textures"), textures);
    writeRoot(root);
}

void MaterialInspector::setTexture(const QString& slot, const QString& imagePath)
{
    if (path_.isEmpty() || slot.isEmpty() || imagePath.isEmpty()) return;
    // Inside the assets folder the path is stored relative to it, like shader paths; elsewhere it stays absolute.
    const QDir assets = assetsFolderOf(path_);
    const QString clean = QDir::cleanPath(imagePath);
    const QString root = QDir::cleanPath(assets.absolutePath()) + QLatin1Char('/');
    const QString stored = clean.startsWith(root) ? clean.mid(root.size()) : clean;
    writeTexture(slot, stored);
    rebuild();
}

void MaterialInspector::clearTexture(const QString& slot)
{
    if (path_.isEmpty()) return;
    writeTexture(slot, QString());
    rebuild();
}

void MaterialInspector::resetProperty(const QString& name)
{
    if (path_.isEmpty()) return;
    QJsonObject root = readRoot();
    QJsonObject props = root.value(QStringLiteral("properties")).toObject();
    props.remove(name);
    root.insert(QStringLiteral("properties"), props);
    writeRoot(root);
    rebuild();
}

void MaterialInspector::resetAllProperties()
{
    if (path_.isEmpty()) return;
    QJsonObject root = readRoot();
    root.insert(QStringLiteral("properties"), QJsonObject());
    // Reset All also drops every texture override, so each slot shows the shader's default again.
    root.insert(QStringLiteral("textures"), QJsonObject());
    writeRoot(root);
    rebuild();
}

void MaterialInspector::copyValues()
{
    if (path_.isEmpty()) return;
    const QJsonObject props = readRoot().value(QStringLiteral("properties")).toObject();
    QGuiApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(props).toJson(QJsonDocument::Compact)));
}

bool MaterialInspector::pasteValues()
{
    if (path_.isEmpty()) return false;
    const QJsonObject incoming = QJsonDocument::fromJson(QGuiApplication::clipboard()->text().toUtf8()).object();
    if (incoming.isEmpty()) return false;

    // Only the properties this material's shader exposes are pasted; the rest of the clipboard is ignored.
    engine::MaterialAsset mat;
    std::string err;
    if (!engine::loadMaterialAsset(path_.toStdString(), mat, err)) return false;
    engine::ShaderAsset shader;
    if (!engine::loadShaderAsset(assetsFolderOf(path_).filePath(QString::fromStdString(mat.shaderPath)).toStdString(), shader, err)) {
        return false;
    }
    std::set<std::string> exposed;
    for (const engine::ShaderProperty& p : shader.properties) exposed.insert(p.name);

    QJsonObject root = readRoot();
    QJsonObject props = root.value(QStringLiteral("properties")).toObject();
    int applied = 0;
    for (auto it = incoming.begin(); it != incoming.end(); ++it) {
        if (!exposed.count(it.key().toStdString())) continue;
        props.insert(it.key(), it.value());
        ++applied;
    }
    if (applied == 0) return false;
    root.insert(QStringLiteral("properties"), props);
    writeRoot(root);
    rebuild();
    return true;
}

void MaterialInspector::setShader(const QString& shaderPath)
{
    if (path_.isEmpty() || shaderPath.isEmpty()) return;
    QJsonObject root = readRoot();
    if (root.isEmpty()) return;
    root.insert(QStringLiteral("shader"), shaderPath);
    writeRoot(root);
    rebuild();
}

} // namespace editor
