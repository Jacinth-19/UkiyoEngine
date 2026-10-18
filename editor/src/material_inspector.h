#pragma once
// Inspector for a selected .ukmat: one editor per exposed property of its shader. Edits are
// written straight back to the material file; the thumbnail and details refresh from the new
// modification time.
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace editor {

class MaterialInspector : public QWidget {
    Q_OBJECT
public:
    explicit MaterialInspector(QWidget* parent = nullptr);

    // Shows the properties of a .ukmat. An empty path (or a non-material) hides the inspector.
    void setMaterial(const QString& ukmatPath);

    // Actions. Each one writes the .ukmat file, emits edited(), and rebuilds the panel.
    // A property that is reset goes back to the shader's default.
    void resetProperty(const QString& name);
    // Reset All clears every value and every texture override.
    void resetAllProperties();
    // Copy puts the material's values on the clipboard as JSON. Paste applies the clipboard's
    // values that this shader exposes, and returns false when none applied.
    void copyValues();
    bool pasteValues();
    // Switches the material to another shader. The path is relative to the assets folder.
    void setShader(const QString& shaderPath);
    // Texture slots. setTexture points a slot at an image (stored relative to the assets folder when the
    // image is inside it); clearTexture removes the override, so the shader's default is used again.
    void setTexture(const QString& slot, const QString& imagePath);
    void clearTexture(const QString& slot);

signals:
    void edited(const QString& ukmatPath);

private:
    void rebuild();
    void writeProperty(const QString& name, const QList<double>& values);
    void writeTexture(const QString& slot, const QString& value);
    QJsonObject readRoot() const;
    bool writeRoot(const QJsonObject& root);

    QString path_;
    QLabel* title_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* outer_ = nullptr;
};

} // namespace editor
