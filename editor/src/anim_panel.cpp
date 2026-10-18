#include "anim_panel.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>

#include "anim_file.h"
#include "anim_timeline.h"

namespace {

using Path = ukiyo::AnimTrack::Path;
using Interp = ukiyo::AnimTrack::Interp;

int strideOf(Path p) {
    return p == Path::Rotation ? 4 : 3;
}

QString pathName(Path p) {
    switch (p) {
    case Path::Translation: return "Translation";
    case Path::Rotation: return "Rotation";
    case Path::Scale: return "Scale";
    }
    return "?";
}

const char* kChannelNames[4] = {"X", "Y", "Z", "W"};

} // namespace

AnimationPanel::AnimationPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    // Toolbar.
    auto* bar = new QHBoxLayout;
    timeLabel_ = new QLabel("0.00 / 0.00 s");
    addTrackBtn_ = new QPushButton("Add track...");
    removeTrackBtn_ = new QPushButton("Remove track");
    deleteKeysBtn_ = new QPushButton("Delete keys");
    addEventBtn_ = new QPushButton("Add event at playhead...");
    removeEventBtn_ = new QPushButton("Remove event");
    bar->addWidget(timeLabel_);
    bar->addStretch(1);
    bar->addWidget(addTrackBtn_);
    bar->addWidget(removeTrackBtn_);
    bar->addWidget(deleteKeysBtn_);
    bar->addWidget(addEventBtn_);
    bar->addWidget(removeEventBtn_);
    root->addLayout(bar);

    placeholder_ = new QLabel("Choose a clip in the toolbar to edit its keys and events.");
    placeholder_->setAlignment(Qt::AlignCenter);
    root->addWidget(placeholder_, 1);

    body_ = new QWidget;
    auto* cols = new QHBoxLayout(body_);
    cols->setContentsMargins(0, 0, 0, 0);

    // Left: tracks and events.
    auto* left = new QVBoxLayout;
    left->addWidget(new QLabel("Tracks"));
    trackList_ = new QListWidget;
    trackList_->setMinimumWidth(220);
    left->addWidget(trackList_, 2);
    left->addWidget(new QLabel("Events"));
    eventList_ = new QListWidget;
    left->addWidget(eventList_, 1);
    cols->addLayout(left);

    // Centre: channel picker, timeline, curve.
    auto* centre = new QVBoxLayout;
    auto* chRow = new QHBoxLayout;
    chRow->addWidget(new QLabel("Curve channel"));
    channelBox_ = new QComboBox;
    chRow->addWidget(channelBox_);
    chRow->addStretch(1);
    centre->addLayout(chRow);
    timeline_ = new AnimTimeline;
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(timeline_);
    scroll->setMinimumHeight(180);
    centre->addWidget(scroll, 2);
    curve_ = new AnimCurve;
    centre->addWidget(curve_, 2);
    cols->addLayout(centre, 1);

    // Right: properties of the selected key.
    auto* right = new QVBoxLayout;
    right->addWidget(new QLabel("Track interpolation"));
    interpBox_ = new QComboBox;
    interpBox_->addItems({"Linear", "Step"});
    right->addWidget(interpBox_);
    right->addWidget(new QLabel("Key"));
    keyInfo_ = new QLabel("No key selected");
    keyInfo_->setWordWrap(true);
    right->addWidget(keyInfo_);
    auto* timeRow = new QHBoxLayout;
    timeRow->addWidget(new QLabel("Time (s)"));
    keyTime_ = new QDoubleSpinBox;
    keyTime_->setDecimals(3);
    keyTime_->setRange(0.0, 3600.0);
    keyTime_->setSingleStep(1.0 / 30.0);
    timeRow->addWidget(keyTime_);
    right->addLayout(timeRow);
    for (int c = 0; c < 4; ++c) {
        auto* row = new QHBoxLayout;
        keyValueLabel_[c] = new QLabel(kChannelNames[c]);
        row->addWidget(keyValueLabel_[c]);
        keyValue_[c] = new QDoubleSpinBox;
        keyValue_[c]->setDecimals(4);
        keyValue_[c]->setRange(-1000.0, 1000.0);
        keyValue_[c]->setSingleStep(0.01);
        row->addWidget(keyValue_[c]);
        right->addLayout(row);
    }
    right->addStretch(1);
    cols->addLayout(right);
    root->addWidget(body_, 1);

    status_ = new QLabel;
    root->addWidget(status_);

    // Wiring.
    connect(trackList_, &QListWidget::currentRowChanged, this, &AnimationPanel::onTrackRowChanged);
    connect(timeline_, &AnimTimeline::seek, this, &AnimationPanel::scrubbed);
    connect(timeline_, &AnimTimeline::trackPicked, this, [this](int row) {
        QSignalBlocker b(trackList_);
        trackList_->setCurrentRow(row);
        onTrackRowChanged(row);
    });
    connect(timeline_, &AnimTimeline::selectionChanged, this, &AnimationPanel::onTimelineSelection);
    connect(timeline_, &AnimTimeline::keysMoved, this, &AnimationPanel::moveSelectedKeys);
    connect(timeline_, &AnimTimeline::addKeyRequested, this, &AnimationPanel::addKey);
    connect(timeline_, &AnimTimeline::deleteRequested, this, &AnimationPanel::deleteSelected);
    connect(timeline_, &AnimTimeline::undoRequested, this, [this] { undo(); });
    connect(timeline_, &AnimTimeline::redoRequested, this, [this] { redo(); });
    connect(timeline_, &AnimTimeline::addEventRequested, this, [this](float t) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Add event", "Event name", QLineEdit::Normal, "footstep", &ok);
        if (ok && !name.trimmed().isEmpty()) addEvent(t, name.trimmed());
    });
    connect(timeline_, &AnimTimeline::renameEventRequested, this, [this](int i) {
        if (i < 0 || i >= static_cast<int>(clip_.events.size())) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Rename event", "Event name", QLineEdit::Normal,
                                                   QString::fromStdString(clip_.events[i].name), &ok);
        if (ok && !name.trimmed().isEmpty()) renameEvent(i, name.trimmed());
    });
    connect(curve_, &AnimCurve::valueEdited, this, &AnimationPanel::onCurveValue);
    connect(curve_, &AnimCurve::addKeyRequested, this, [this](float t) { addKey(trackSel_, t); });
    connect(channelBox_, &QComboBox::currentIndexChanged, this, [this] {
        if (!refreshing_) refreshProperties();
    });
    connect(interpBox_, &QComboBox::activated, this, [this](int i) {
        if (trackSel_ < 0 || trackSel_ >= static_cast<int>(clip_.tracks.size())) return;
        setInterpolation(trackSel_, i == 1 ? Interp::Step : Interp::Linear);
    });
    connect(keyTime_, &QDoubleSpinBox::valueChanged, this, &AnimationPanel::onKeyEdited);
    for (int c = 0; c < 4; ++c) connect(keyValue_[c], &QDoubleSpinBox::valueChanged, this, &AnimationPanel::onKeyEdited);
    connect(addTrackBtn_, &QPushButton::clicked, this, [this] {
        if (clipIndex_ < 0 || rig_.names.empty()) return;
        QStringList nodes;
        for (const auto& n : rig_.names) nodes << QString::fromStdString(n);
        bool ok = false;
        const QString node = QInputDialog::getItem(this, "Add track", "Node", nodes, 0, false, &ok);
        if (!ok) return;
        const QStringList paths = {"Translation", "Rotation", "Scale"};
        const QString path = QInputDialog::getItem(this, "Add track", "Property", paths, 1, false, &ok);
        if (!ok) return;
        const int idx = nodes.indexOf(node);
        const Path p = path == "Translation" ? Path::Translation : path == "Scale" ? Path::Scale : Path::Rotation;
        if (idx >= 0) addTrack(idx, p);
    });
    connect(removeTrackBtn_, &QPushButton::clicked, this, [this] { removeTrack(trackSel_); });
    connect(deleteKeysBtn_, &QPushButton::clicked, this, [this] { deleteSelected(); });
    connect(addEventBtn_, &QPushButton::clicked, this, [this] {
        if (clipIndex_ < 0) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Add event", "Event name", QLineEdit::Normal, "footstep", &ok);
        if (ok && !name.trimmed().isEmpty()) addEvent(playhead_, name.trimmed());
    });
    connect(removeEventBtn_, &QPushButton::clicked, this, [this] {
        const int e = timeline_->selectedEvent();
        if (e >= 0) removeEvent(e);
    });
    connect(eventList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (refreshing_) return;
        timeline_->selectEvent(row);
        refreshProperties();
    });

    body_->setVisible(false);
    setClip(QString());
}

// ---------------------------------------------------------------- library and clip

void AnimationPanel::setLibrary(const ukiyo::SourceRig& rig, const std::vector<ukiyo::AnimClip>& source,
                                const ukiyo::ClipEdits& edits, const QString& libraryPath) {
    const QString keep = clipName();
    rig_ = rig;
    source_ = source;
    edits_ = edits;
    libraryPath_ = libraryPath;
    undo_.clear();
    redo_.clear();
    effective_ = ukiyo::applyClipEdits(rig_, source_, edits_);
    clipIndex_ = -1;
    for (int i = 0; i < static_cast<int>(source_.size()); ++i) {
        if (QString::fromStdString(source_[i].name) == keep) clipIndex_ = i;
    }
    trackSel_ = -1;
    refreshClip();
}

void AnimationPanel::setClip(const QString& name) {
    clipIndex_ = -1;
    for (int i = 0; i < static_cast<int>(source_.size()); ++i) {
        if (QString::fromStdString(source_[i].name) == name) clipIndex_ = i;
    }
    trackSel_ = -1;
    playhead_ = 0.0f;
    refreshClip();
}

QString AnimationPanel::clipName() const {
    return clipIndex_ >= 0 ? QString::fromStdString(source_[clipIndex_].name) : QString();
}

QString AnimationPanel::nodeName(int node) const {
    if (node < 0 || node >= static_cast<int>(rig_.names.size())) return QStringLiteral("?");
    return QString::fromStdString(rig_.names[node]);
}

void AnimationPanel::setTime(float seconds) {
    playhead_ = seconds;
    timeline_->setPlayhead(seconds);
    timeLabel_->setText(QString("%1 / %2 s").arg(seconds, 0, 'f', 2).arg(clip_.duration, 0, 'f', 2));
}

void AnimationPanel::refreshClip() {
    refreshing_ = true;
    const bool has = clipIndex_ >= 0 && clipIndex_ < static_cast<int>(effective_.size());
    clip_ = has ? effective_[clipIndex_] : ukiyo::AnimClip{};
    body_->setVisible(has);
    placeholder_->setVisible(!has);
    if (!has) placeholder_->setText("Choose a clip in the toolbar to edit its keys and events.");
    if (has && rig_.names.empty()) placeholder_->setText("This library has no rig.");

    trackList_->clear();
    for (int i = 0; i < static_cast<int>(clip_.tracks.size()); ++i) {
        const auto& t = clip_.tracks[i];
        const int keys = static_cast<int>(t.times.size());
        trackList_->addItem(QString("%1 · %2   (%3 keys)").arg(nodeName(t.node), pathName(t.path)).arg(keys));
    }
    if (trackSel_ >= static_cast<int>(clip_.tracks.size())) trackSel_ = -1;
    trackList_->setCurrentRow(trackSel_);

    eventList_->clear();
    for (const auto& ev : clip_.events) {
        eventList_->addItem(QString("%1 s   %2").arg(ev.time, 0, 'f', 3).arg(QString::fromStdString(ev.name)));
    }
    timeline_->setClip(clip_);
    timeline_->setSelectedTrack(trackSel_);
    timeline_->setPlayhead(playhead_);
    setTime(playhead_);
    refreshing_ = false;
    onTimelineSelection();
    status_->setText(has ? QString("%1 · %2 s · %3 tracks, %4 events")
                               .arg(clipName())
                               .arg(clip_.duration, 0, 'f', 2)
                               .arg(clip_.tracks.size())
                               .arg(clip_.events.size())
                         : QString());
}

// ---------------------------------------------------------------- selection and properties

void AnimationPanel::selectKey(int track, int key) {
    trackSel_ = track;
    {
        QSignalBlocker b(trackList_);
        trackList_->setCurrentRow(track);
    }
    timeline_->setSelectedTrack(track);
    timeline_->selectKey(track, key, false);
    onTimelineSelection();
}

std::set<std::pair<int, int>> AnimationPanel::selectedKeys() const {
    return timeline_->selectedKeys();
}

void AnimationPanel::onTrackRowChanged(int row) {
    if (refreshing_) return;
    trackSel_ = row;
    timeline_->setSelectedTrack(row);
    refreshProperties();
}

void AnimationPanel::onTimelineSelection() {
    if (refreshing_) return;
    const int ev = timeline_->selectedEvent();
    if (ev >= 0) {
        QSignalBlocker b(eventList_);
        eventList_->setCurrentRow(ev);
    } else {
        QSignalBlocker b(eventList_);
        eventList_->setCurrentRow(-1);
    }
    refreshProperties();
}

void AnimationPanel::refreshProperties() {
    refreshing_ = true;
    const bool hasTrack = trackSel_ >= 0 && trackSel_ < static_cast<int>(clip_.tracks.size());
    const auto keys = timeline_->selectedKeys();
    const int ev = timeline_->selectedEvent();

    // Channel picker follows the selected track's property.
    const int n = hasTrack ? strideOf(clip_.tracks[trackSel_].path) : 3;
    {
        QSignalBlocker b(channelBox_);
        const int keep = std::clamp(channelBox_->currentIndex(), 0, n - 1);
        channelBox_->clear();
        for (int c = 0; c < n; ++c) channelBox_->addItem(kChannelNames[c]);
        channelBox_->setCurrentIndex(keep);
    }
    const int channel = std::clamp(channelBox_->currentIndex(), 0, n - 1);

    interpBox_->setEnabled(hasTrack);
    {
        QSignalBlocker b(interpBox_);
        const bool step = hasTrack && clip_.tracks[trackSel_].interp == Interp::Step;
        interpBox_->setCurrentIndex(step ? 1 : 0);
    }
    removeTrackBtn_->setEnabled(hasTrack);
    removeEventBtn_->setEnabled(ev >= 0);
    deleteKeysBtn_->setEnabled(!keys.empty() || ev >= 0);
    addEventBtn_->setEnabled(clipIndex_ >= 0);
    addTrackBtn_->setEnabled(clipIndex_ >= 0);

    // Key properties: only with exactly one key selected.
    bool oneKey = keys.size() == 1 && ev < 0;
    for (int c = 0; c < 4; ++c) {
        keyValue_[c]->setVisible(false);
        keyValueLabel_[c]->setVisible(false);
    }
    if (oneKey) {
        const auto [ti, k] = *keys.begin();
        const auto& tr = clip_.tracks[ti];
        const int s = strideOf(tr.path);
        const std::vector<float> vals = animfile::keyValues(tr);
        {
            QSignalBlocker b(keyTime_);
            keyTime_->setValue(tr.times[k]);
        }
        for (int c = 0; c < s; ++c) {
            QSignalBlocker b(keyValue_[c]);
            keyValue_[c]->setValue(static_cast<size_t>(k * s + c) < vals.size() ? vals[k * s + c] : 0.0f);
            keyValueLabel_[c]->setVisible(true);
            keyValue_[c]->setVisible(true);
        }
        keyInfo_->setText(QString("Key %1 of %2 on %3 · %4 s").arg(k + 1).arg(tr.times.size())
                              .arg(pathName(tr.path)).arg(tr.times[k], 0, 'f', 3));
        keyTime_->setEnabled(true);
    } else {
        keyTime_->setEnabled(false);
        if (ev >= 0 && ev < static_cast<int>(clip_.events.size())) {
            keyInfo_->setText(QString("Event '%1' at %2 s — double-click to rename")
                                  .arg(QString::fromStdString(clip_.events[ev].name))
                                  .arg(clip_.events[ev].time, 0, 'f', 3));
        } else if (keys.size() > 1) {
            keyInfo_->setText(QString("%1 keys selected. Drag them on the timeline to move them.").arg(keys.size()));
        } else {
            keyInfo_->setText("No key selected. Double-click a lane to add a key.");
        }
    }

    // Curve shows the selected track, with its selected keys.
    if (hasTrack) {
        std::set<int> sel;
        for (const auto& [ti, k] : keys) if (ti == trackSel_) sel.insert(k);
        curve_->setTrack(clip_.tracks[trackSel_], clip_.duration, channel, sel);
    } else {
        curve_->clearTrack();
    }
    refreshing_ = false;
}

// ---------------------------------------------------------------- edits

void AnimationPanel::setTrackKeys(int track, std::vector<float> times, std::vector<float> values, Interp interp,
                                  ukiyo::ClipEdits* into) {
    const auto& tr = clip_.tracks[track];
    const Interp out = interp == Interp::Step ? Interp::Step : Interp::Linear;
    animfile::setTrackEdit(into, clipName().toStdString(), rig_.names[tr.node], tr.path, out, std::move(times),
                           std::move(values));
}

void AnimationPanel::applyEdits(const ukiyo::ClipEdits& next) {
    edits_ = next;
    effective_ = ukiyo::applyClipEdits(rig_, source_, edits_);
    refreshClip();
    if (!libraryPath_.isEmpty()) {
        QString err;
        if (!animfile::writeEdits(libraryPath_, edits_, &err)) status_->setText("Could not save edits: " + err);
    }
    emit editsChanged(edits_);
}

void AnimationPanel::commit(const ukiyo::ClipEdits& next, const QString& what) {
    undo_.push_back(edits_);
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
    applyEdits(next);
    status_->setText(what);
}

bool AnimationPanel::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(edits_);
    const ukiyo::ClipEdits prev = undo_.back();
    undo_.pop_back();
    applyEdits(prev);
    status_->setText("Undo");
    return true;
}

bool AnimationPanel::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(edits_);
    const ukiyo::ClipEdits next = redo_.back();
    redo_.pop_back();
    applyEdits(next);
    status_->setText("Redo");
    return true;
}

void AnimationPanel::addKey(int track, float seconds) {
    if (clipIndex_ < 0 || track < 0 || track >= static_cast<int>(clip_.tracks.size())) return;
    const auto& tr = clip_.tracks[track];
    const int s = strideOf(tr.path);
    const float t = std::clamp(seconds, 0.0f, clip_.duration);
    std::vector<float> times = tr.times;
    std::vector<float> vals = animfile::keyValues(tr);
    const std::vector<float> at = animfile::valueAt(tr, t);
    const auto pos = std::lower_bound(times.begin(), times.end(), t) - times.begin();
    if (pos < static_cast<long>(times.size()) && std::abs(times[pos] - t) < 1e-4f) {
        for (int c = 0; c < s; ++c) vals[pos * s + c] = at[c];
    } else {
        times.insert(times.begin() + pos, t);
        vals.insert(vals.begin() + pos * s, at.begin(), at.begin() + s);
    }
    ukiyo::ClipEdits next = edits_;
    setTrackKeys(track, times, vals, tr.interp, &next);
    commit(next, "Added key");
    trackSel_ = track;
    timeline_->selectKey(track, static_cast<int>(pos), false);
}

void AnimationPanel::moveSelectedKeys(float seconds) {
    const auto sel = timeline_->selectedKeys();
    if (sel.empty() || std::abs(seconds) < 1e-6f) return;
    std::set<int> tracks;
    for (const auto& p : sel) tracks.insert(p.first);
    ukiyo::ClipEdits next = edits_;
    for (int ti : tracks) {
        const auto& tr = clip_.tracks[ti];
        const int s = strideOf(tr.path);
        const std::vector<float> vals = animfile::keyValues(tr);
        const int n = static_cast<int>(tr.times.size());
        std::vector<float> moved(n);
        std::vector<bool> isMoved(n, false);
        for (int k = 0; k < n; ++k) {
            isMoved[k] = sel.count({ti, k}) > 0;
            moved[k] = std::max(0.0f, tr.times[k] + (isMoved[k] ? seconds : 0.0f));
        }
        std::vector<int> order(n);
        for (int k = 0; k < n; ++k) order[k] = k;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return moved[a] < moved[b]; });
        std::vector<float> times, values;
        for (int k : order) {
            // Two keys on one time: the moved one wins.
            if (!times.empty() && std::abs(times.back() - moved[k]) < 1e-4f) {
                if (!isMoved[k]) continue;
                times.pop_back();
                values.resize(values.size() - s);
            }
            times.push_back(moved[k]);
            values.insert(values.end(), vals.begin() + k * s, vals.begin() + k * s + s);
        }
        setTrackKeys(ti, times, values, tr.interp, &next);
    }
    commit(next, "Moved keys");
    timeline_->clearKeySelection();
}

void AnimationPanel::deleteSelected() {
    const int ev = timeline_->selectedEvent();
    if (ev >= 0) {
        removeEvent(ev);
        return;
    }
    const auto sel = timeline_->selectedKeys();
    if (sel.empty()) return;
    std::set<int> tracks;
    for (const auto& p : sel) tracks.insert(p.first);
    ukiyo::ClipEdits next = edits_;
    for (int ti : tracks) {
        const auto& tr = clip_.tracks[ti];
        const int s = strideOf(tr.path);
        const std::vector<float> vals = animfile::keyValues(tr);
        std::vector<float> times, values;
        for (int k = 0; k < static_cast<int>(tr.times.size()); ++k) {
            if (sel.count({ti, k})) continue;
            times.push_back(tr.times[k]);
            values.insert(values.end(), vals.begin() + k * s, vals.begin() + k * s + s);
        }
        setTrackKeys(ti, times, values, tr.interp, &next); // empty times remove the track
    }
    commit(next, "Deleted keys");
    timeline_->clearKeySelection();
}

void AnimationPanel::onKeyEdited() {
    if (refreshing_) return;
    const auto keys = timeline_->selectedKeys();
    if (keys.size() != 1) return;
    const auto [ti, k] = *keys.begin();
    const auto& tr = clip_.tracks[ti];
    const int s = strideOf(tr.path);
    std::vector<float> times = tr.times;
    std::vector<float> vals = animfile::keyValues(tr);
    times[k] = std::max(0.0f, static_cast<float>(keyTime_->value()));
    for (int c = 0; c < s; ++c) vals[k * s + c] = static_cast<float>(keyValue_[c]->value());
    // Keep the keys in time order.
    std::vector<int> order(times.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return times[a] < times[b]; });
    std::vector<float> st, sv;
    for (int i : order) {
        st.push_back(times[i]);
        sv.insert(sv.end(), vals.begin() + i * s, vals.begin() + i * s + s);
    }
    ukiyo::ClipEdits next = edits_;
    setTrackKeys(ti, st, sv, tr.interp, &next);
    commit(next, "Edited key");
    timeline_->clearKeySelection();
}

void AnimationPanel::onCurveValue(int key, int channel, float value) {
    if (trackSel_ < 0 || trackSel_ >= static_cast<int>(clip_.tracks.size())) return;
    const auto& tr = clip_.tracks[trackSel_];
    const int s = strideOf(tr.path);
    std::vector<float> vals = animfile::keyValues(tr);
    if (key < 0 || key * s + channel >= static_cast<int>(vals.size())) return;
    if (std::abs(vals[key * s + channel] - value) < 1e-7f) return;
    vals[key * s + channel] = value;
    ukiyo::ClipEdits next = edits_;
    setTrackKeys(trackSel_, tr.times, vals, tr.interp, &next);
    commit(next, "Edited value");
}

void AnimationPanel::addTrack(int node, Path path) {
    if (clipIndex_ < 0 || node < 0 || node >= static_cast<int>(rig_.names.size())) return;
    std::vector<float> rest;
    switch (path) {
    case Path::Translation: {
        const auto& v = rig_.restT[node];
        rest = {v.x, v.y, v.z};
        break;
    }
    case Path::Rotation: {
        const auto& q = rig_.restR[node];
        rest = {q.x, q.y, q.z, q.w};
        break;
    }
    case Path::Scale: {
        const auto& v = rig_.restS[node];
        rest = {v.x, v.y, v.z};
        break;
    }
    }
    const float end = clip_.duration > 0.0f ? clip_.duration : 1.0f;
    std::vector<float> times = {0.0f, end};
    std::vector<float> values = rest;
    values.insert(values.end(), rest.begin(), rest.end());
    ukiyo::ClipEdits next = edits_;
    animfile::setTrackEdit(&next, clipName().toStdString(), rig_.names[node], path, Interp::Linear, times, values);
    commit(next, "Added track");
    for (int i = 0; i < static_cast<int>(effective_[clipIndex_].tracks.size()); ++i) {
        const auto& t = effective_[clipIndex_].tracks[i];
        if (t.node == node && t.path == path) trackSel_ = i;
    }
    refreshClip();
}

void AnimationPanel::removeTrack(int track) {
    if (track < 0 || track >= static_cast<int>(clip_.tracks.size())) return;
    const auto& tr = clip_.tracks[track];
    ukiyo::ClipEdits next = edits_;
    animfile::setTrackEdit(&next, clipName().toStdString(), rig_.names[tr.node], tr.path, tr.interp, {}, {});
    trackSel_ = -1;
    commit(next, "Removed track");
}

void AnimationPanel::setInterpolation(int track, Interp interp) {
    if (track < 0 || track >= static_cast<int>(clip_.tracks.size())) return;
    const auto& tr = clip_.tracks[track];
    ukiyo::ClipEdits next = edits_;
    setTrackKeys(track, tr.times, animfile::keyValues(tr), interp, &next);
    commit(next, "Changed interpolation");
}

void AnimationPanel::addEvent(float seconds, const QString& name) {
    if (clipIndex_ < 0) return;
    std::vector<ukiyo::AnimEvent> events = clip_.events;
    events.push_back({std::clamp(seconds, 0.0f, clip_.duration), name.toStdString()});
    std::stable_sort(events.begin(), events.end(),
                     [](const ukiyo::AnimEvent& a, const ukiyo::AnimEvent& b) { return a.time < b.time; });
    ukiyo::ClipEdits next = edits_;
    animfile::setEventEdit(&next, clipName().toStdString(), events);
    commit(next, "Added event");
}

void AnimationPanel::renameEvent(int event, const QString& name) {
    if (clipIndex_ < 0 || event < 0 || event >= static_cast<int>(clip_.events.size())) return;
    std::vector<ukiyo::AnimEvent> events = clip_.events;
    events[event].name = name.toStdString();
    ukiyo::ClipEdits next = edits_;
    animfile::setEventEdit(&next, clipName().toStdString(), events);
    commit(next, "Renamed event");
}

void AnimationPanel::removeEvent(int event) {
    if (clipIndex_ < 0 || event < 0 || event >= static_cast<int>(clip_.events.size())) return;
    std::vector<ukiyo::AnimEvent> events = clip_.events;
    events.erase(events.begin() + event);
    ukiyo::ClipEdits next = edits_;
    animfile::setEventEdit(&next, clipName().toStdString(), events);
    timeline_->selectEvent(-1);
    commit(next, "Removed event");
}
