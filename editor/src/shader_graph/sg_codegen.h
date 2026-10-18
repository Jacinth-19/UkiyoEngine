#pragma once

// Turns a shader graph into GLSL. The graph is walked from its master node back through the
// connections, so only nodes that feed the master end up in the shader.

#include "sg_nodes.h"

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace QtNodes {
class DataFlowGraphModel;
}

namespace sg {

// A value exposed as a material property. It lives in the material block as vec4 slot `slot`.
struct PropertyInfo {
    QString name;
    ParamKind kind;
    int slot;
    QStringList defaults;   // default parameter text from the graph node
};

// A texture exposed by a Sample Texture 2D node that has an exposed name. The material can override its
// image. `binding` is the sampler's binding in the material set and `defaultPath` the node's image path.
struct TextureInfo {
    QString name;
    int binding;
    QString defaultPath;
};

struct CodegenResult {
    // ukiyo_surface(uv, normal) returns SurfaceData. The engine's lit and unlit passes call it.
    QString surfaceFunction;
    // A complete GLSL 4.50 fragment shader built from the same code.
    QString fragmentShader;
    QList<PropertyInfo> properties;
    QList<TextureInfo> textures;
    QStringList errors;
    // Fragment line (1-based) -> the node whose statement is on that line. Compile errors use it.
    QHash<int, unsigned int> fragmentLineNodes;
    // True when the master's Alpha input is wired: the material draws in the transparent pass.
    bool transparent = false;
    bool ok() const { return errors.isEmpty(); }
};

// Binding of the material property block (set 1). Textures use the lower bindings.
constexpr int kMaterialBlockBinding = 15;

CodegenResult generate(QtNodes::DataFlowGraphModel& graph);

// JSON name of a property kind: "float", "vec2" or "vec3".
QString propertyKindName(ParamKind k);

// The .ukshader document: the editable graph plus the compiled output that the engine loads.
QJsonObject shaderDocument(QtNodes::DataFlowGraphModel& graph);

} // namespace sg
