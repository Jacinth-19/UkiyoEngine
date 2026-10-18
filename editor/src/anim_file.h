// Animation editing data: the .ukanim file that holds keyframe and event edits beside a clip library, the
// state machine built from a locomotion component, and the checks the state machine editor shows.
// No Qt widgets here, so the selftest can call these directly.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QString>
#include <QStringList>

#include "engine/anim.h"
#include "systems/components.h"

namespace animfile {

// dir/name.ukanim for a library at dir/name.glb.
QString editsPath(const QString& libraryPath);

// Reads the edits for a library. A missing file is not an error; it gives empty edits.
bool readEdits(const QString& libraryPath, ukiyo::ClipEdits* out, QString* error);

// Writes the edits. When there are none, the file is removed so the project stays tidy.
bool writeEdits(const QString& libraryPath, const ukiyo::ClipEdits& edits, QString* error);

// Replaces the track edit for (clip, node, path), or adds one. Empty times remove the track from the clip.
void setTrackEdit(ukiyo::ClipEdits* edits, const std::string& clip, const std::string& node,
                  ukiyo::AnimTrack::Path path, ukiyo::AnimTrack::Interp interp, std::vector<float> times,
                  std::vector<float> values);

// Replaces the event list for a clip, or adds one.
void setEventEdit(ukiyo::ClipEdits* edits, const std::string& clip, std::vector<ukiyo::AnimEvent> events);

// The track's key values, one group of 3 or 4 floats per key. Cubic tracks give their values, not tangents.
std::vector<float> keyValues(const ukiyo::AnimTrack& track);

// The track's value at time t, for a new key: linear or step between the keys around t.
std::vector<float> valueAt(const ukiyo::AnimTrack& track, float t);

// The state machine for a locomotion component: the custom graph when `custom` is set, otherwise the
// three-clip Idle, Walk and Run machine that the game has always used.
// durations: clip lengths in seconds, by clip name. Used for exit times and fractional crossfades.
std::unique_ptr<ukiyo::AnimStateMachine> buildStateMachine(const ukiyo::AnimComponent& a,
                                                           const std::map<std::string, float>& durations = {});

// Problems in a state graph, one message each. An empty list means the graph can run.
QStringList validateGraph(const ukiyo::AnimComponent& a, const QStringList& clipNames);

} // namespace animfile
