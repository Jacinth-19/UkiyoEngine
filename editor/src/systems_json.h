// Scene-file form of the game system settings. Paths are project-relative in the file and absolute
// in memory, the same convention as the model paths.
#pragma once

#include <QJsonObject>
#include <QString>

#include "systems/components.h"

namespace editor::systems_json {

// Writes only the components that are enabled, so untouched instances stay as small as before.
// Returns an empty object when nothing is enabled.
QJsonObject toJson(const ukiyo::SystemsComponents& c, const QString& projectRoot);

// Reads whatever keys are present. Missing components keep their defaults and stay disabled.
ukiyo::SystemsComponents fromJson(const QJsonObject& o, const QString& projectRoot);

} // namespace editor::systems_json
