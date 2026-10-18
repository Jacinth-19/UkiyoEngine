// The Animation panel's timeline: the keys of each track of a library clip, the events, and a curve view.
// Every change becomes a ClipEdits value that is saved beside the library (.ukanim) and sent to the worker.
#pragma once

#include <QWidget>

#include <set>
#include <utility>
#include <vector>

#include "engine/anim.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class AnimTimeline;
class AnimCurve;

class AnimationPanel : public QWidget {
    Q_OBJECT
public:
    explicit AnimationPanel(QWidget* parent = nullptr);

    // The library's rig, its unedited clips, the edits so far, and the library's path. Edits are saved
    // beside the library. An empty path keeps them in memory.
    void setLibrary(const ukiyo::SourceRig& rig, const std::vector<ukiyo::AnimClip>& source,
                    const ukiyo::ClipEdits& edits, const QString& libraryPath);
    // Shows a library clip by name. An empty or unknown name shows the placeholder.
    void setClip(const QString& name);
    void setTime(float seconds);
    const ukiyo::ClipEdits& edits() const { return edits_; }
    bool undo();
    bool redo();
    // The clip currently shown (after edits), for tests and the status line.
    const ukiyo::AnimClip& shownClip() const { return clip_; }

    // Test hooks: the same operations the mouse and the property controls call.
    void addKey(int track, float seconds);
    void moveSelectedKeys(float seconds);
    void deleteSelected();
    void addTrack(int node, ukiyo::AnimTrack::Path path);
    void removeTrack(int track);
    void setInterpolation(int track, ukiyo::AnimTrack::Interp interp);
    void addEvent(float seconds, const QString& name);
    void renameEvent(int event, const QString& name);
    void removeEvent(int event);
    void selectKey(int track, int key);
    std::set<std::pair<int, int>> selectedKeys() const;

signals:
    void scrubbed(float seconds);
    void editsChanged(const ukiyo::ClipEdits& edits);

private:
    void applyEdits(const ukiyo::ClipEdits& next);
    void commit(const ukiyo::ClipEdits& next, const QString& what);
    void refreshClip();
    void refreshProperties();
    void setTrackKeys(int track, std::vector<float> times, std::vector<float> values, ukiyo::AnimTrack::Interp interp,
                      ukiyo::ClipEdits* into);
    QString clipName() const;
    QString nodeName(int node) const;
    void onTimelineSelection();
    void onKeyEdited();
    void onCurveValue(int key, int channel, float value);
    void onTrackRowChanged(int row);

    ukiyo::SourceRig rig_;
    std::vector<ukiyo::AnimClip> source_;
    ukiyo::ClipEdits edits_;
    QString libraryPath_;
    std::vector<ukiyo::AnimClip> effective_;
    int clipIndex_ = -1;
    ukiyo::AnimClip clip_;
    std::vector<ukiyo::ClipEdits> undo_;
    std::vector<ukiyo::ClipEdits> redo_;
    int trackSel_ = -1;
    float playhead_ = 0.0f;

    QListWidget* trackList_ = nullptr;
    AnimTimeline* timeline_ = nullptr;
    AnimCurve* curve_ = nullptr;
    QComboBox* channelBox_ = nullptr;
    QComboBox* interpBox_ = nullptr;
    QDoubleSpinBox* keyTime_ = nullptr;
    QDoubleSpinBox* keyValue_[4] = {nullptr, nullptr, nullptr, nullptr};
    QLabel* keyValueLabel_[4] = {nullptr, nullptr, nullptr, nullptr};
    QLabel* keyInfo_ = nullptr;
    QListWidget* eventList_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* addTrackBtn_ = nullptr;
    QPushButton* removeTrackBtn_ = nullptr;
    QPushButton* deleteKeysBtn_ = nullptr;
    QPushButton* addEventBtn_ = nullptr;
    QPushButton* removeEventBtn_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QWidget* body_ = nullptr;
    bool refreshing_ = false;
};
