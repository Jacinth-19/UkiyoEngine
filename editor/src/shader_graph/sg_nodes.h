#pragma once

// Shader graph node library. Each node is described by a NodeSpec (ports, parameters and the
// GLSL template it expands to). ShaderNodeModel is the one QtNodes delegate that every spec uses.

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <QtNodes/NodeData>
#include <QtNodes/NodeDelegateModel>
#include <QtNodes/NodeDelegateModelRegistry>

#include <memory>
#include <vector>

class QWidget;

namespace sg {

// Port value types. Connections only join ports of the same type (QtNodes checks type ids), so
// conversions are explicit nodes (Combine, Split), the way Unity's Shader Graph exposes them.
enum class Kind { Float, Vec2, Vec3 };

QString glslType(Kind k);       // "float", "vec2", "vec3"
QString kindLabel(Kind k);      // "Float", "Vector 2", "Vector 3"

class ShaderValue : public QtNodes::NodeData
{
public:
    explicit ShaderValue(Kind k) : kind_(k) {}
    QtNodes::NodeDataType type() const override;
    Kind kind() const { return kind_; }

private:
    Kind kind_;
};

struct PortSpec {
    QString name;
    Kind kind;
    QString def;   // GLSL expression used when the port is not connected (masters and inputs)
};

enum class ParamKind { None, Float, Vec2, Color, Texture };

struct NodeSpec {
    QString id;                // registry name, saved in .ukshader files
    QString category;          // palette category
    QString caption;
    std::vector<PortSpec> in;
    std::vector<PortSpec> out;
    ParamKind param = ParamKind::None;
    // One GLSL expression per output. Placeholders: %0.. = input variables, %P = parameter
    // expression, %I = unique node id, %B = texture binding slot.
    QStringList outExpr;
    QString decl;              // optional declaration emitted once per node (textures)
    QString helper;            // optional GLSL helper function, emitted once per graph
    bool master = false;       // master nodes write SurfaceData fields instead of outputs
    QStringList surfaceFields; // master: SurfaceData field for each input port
};

const std::vector<NodeSpec>& nodeSpecs();
const NodeSpec* findSpec(const QString& id);

class ShaderNodeModel : public QtNodes::NodeDelegateModel
{
    Q_OBJECT
public:
    explicit ShaderNodeModel(const NodeSpec* spec);

    const NodeSpec* spec() const { return spec_; }
    QStringList params() const { return params_; }
    // Exposed name: when set on a value node, the value becomes a material property with this name.
    QString reference() const { return reference_; }
    void setReference(const QString& r) { reference_ = r; }
    // GLSL expression for the node parameter (float, vec2, vec3); empty when the node has none.
    QString expression() const;
    void setParams(const QStringList& p);

    QString name() const override { return spec_->id; }
    QString caption() const override { return spec_->caption; }
    QString portCaption(QtNodes::PortType t, QtNodes::PortIndex i) const override;
    bool portCaptionVisible(QtNodes::PortType, QtNodes::PortIndex) const override { return true; }
    bool captionVisible() const override { return true; }

    unsigned int nPorts(QtNodes::PortType t) const override;
    QtNodes::NodeDataType dataType(QtNodes::PortType t, QtNodes::PortIndex i) const override;
    std::shared_ptr<QtNodes::NodeData> outData(QtNodes::PortIndex i) override;
    void setInData(std::shared_ptr<QtNodes::NodeData>, QtNodes::PortIndex) override {}
    QWidget* embeddedWidget() override;

    QJsonObject save() const override;
    void load(QJsonObject const& json) override;

private:
    void buildWidget();

    const NodeSpec* spec_;
    QStringList params_;   // parameter values as text, see paramCount()
    QString reference_;
    QWidget* widget_ = nullptr;
};

// The palette shown by the node editor's right-click menu.
std::shared_ptr<QtNodes::NodeDelegateModelRegistry> makeRegistry();

// Formats a float as GLSL: always a decimal point, so "1" becomes "1.0".
QString glslFloat(double v);

} // namespace sg
