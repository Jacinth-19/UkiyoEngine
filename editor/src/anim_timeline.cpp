#include "anim_timeline.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "anim_file.h"

namespace {

constexpr int kMargin = 10;
constexpr float kFps = 30.0f;

QColor kBg{0x1d, 0x1d, 0x21};
QColor kLaneA{0x25, 0x25, 0x2a};
QColor kLaneB{0x2a, 0x2a, 0x30};
QColor kLaneSel{0x33, 0x3d, 0x4f};
QColor kGrid{0x3a, 0x3a, 0x42};
QColor kText{0xc8, 0xc8, 0xd0};
QColor kKey{0xb8, 0xd4, 0xff};
QColor kKeySel{0xff, 0xa8, 0x40};
QColor kEvent{0x7fd6a0};
QColor kPlayhead{0xff, 0x55, 0x55};

int comps(ukiyo::AnimTrack::Path p) {
    return p == ukiyo::AnimTrack::Path::Rotation ? 4 : 3;
}

QColor channelColor(int c) {
    static const QColor cols[4] = {QColor(0xe0, 0x60, 0x60), QColor(0x70, 0xd0, 0x70),
                                   QColor(0x70, 0x90, 0xe8), QColor(0xb0, 0xb0, 0xb0)};
    return cols[std::clamp(c, 0, 3)];
}

} // namespace

// ---------------------------------------------------------------- AnimTimeline

AnimTimeline::AnimTimeline(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setMinimumHeight(kRulerH + kEventH + kLaneH + 4);
}

void AnimTimeline::setClip(const ukiyo::AnimClip& clip) {
    clip_ = clip;
    const int lanes = std::max<int>(1, static_cast<int>(clip_.tracks.size()));
    setMinimumHeight(kRulerH + kEventH + lanes * kLaneH + 4);
    if (track_ >= static_cast<int>(clip_.tracks.size())) track_ = -1;
    if (event_ >= static_cast<int>(clip_.events.size())) event_ = -1;
    // Drop key selections that point past the end of their track.
    for (auto it = keys_.begin(); it != keys_.end();) {
        const bool ok = it->first >= 0 && it->first < static_cast<int>(clip_.tracks.size()) &&
                        it->second >= 0 && it->second < static_cast<int>(clip_.tracks[it->first].times.size());
        it = ok ? std::next(it) : keys_.erase(it);
    }
    dragging_ = false;
    dragDt_ = 0.0f;
    update();
}

void AnimTimeline::setPlayhead(float seconds) {
    playhead_ = seconds;
    update();
}

void AnimTimeline::setSelectedTrack(int track) {
    track_ = track;
    update();
}

void AnimTimeline::selectEvent(int event) {
    event_ = event;
    update();
}

void AnimTimeline::selectKey(int track, int key, bool add) {
    if (!add) keys_.clear();
    keys_.insert({track, key});
    update();
}

void AnimTimeline::clearKeySelection() {
    keys_.clear();
    update();
}

float AnimTimeline::pxPerSec() const {
    const float dur = std::max(clip_.duration, 0.05f);
    return std::max(1.0f, static_cast<float>(width() - 2 * kMargin) / dur);
}

float AnimTimeline::xOf(float t) const {
    return kMargin + t * pxPerSec();
}

float AnimTimeline::tOf(float x) const {
    return (x - kMargin) / pxPerSec();
}

float AnimTimeline::snapTime(float t, bool fine) const {
    const float v = fine ? t : std::round(t * kFps) / kFps;
    return std::clamp(v, 0.0f, clip_.duration);
}

int AnimTimeline::laneAt(int y) const {
    const int lane = (y - kRulerH - kEventH) / kLaneH;
    if (y < kRulerH + kEventH || lane < 0 || lane >= static_cast<int>(clip_.tracks.size())) return -1;
    return lane;
}

int AnimTimeline::eventAt(int x, int y) const {
    if (y < kRulerH || y >= kRulerH + kEventH) return -1;
    for (int i = 0; i < static_cast<int>(clip_.events.size()); ++i) {
        if (std::abs(xOf(clip_.events[i].time) - x) <= 7) return i;
    }
    return -1;
}

int AnimTimeline::keyAt(int track, int x) const {
    if (track < 0 || track >= static_cast<int>(clip_.tracks.size())) return -1;
    const auto& times = clip_.tracks[track].times;
    for (int k = 0; k < static_cast<int>(times.size()); ++k) {
        if (std::abs(xOf(times[k]) - x) <= 6) return k;
    }
    return -1;
}

void AnimTimeline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), kBg);

    const float dur = std::max(clip_.duration, 0.05f);
    const float pps = pxPerSec();
    QFont f = font();
    f.setPointSizeF(std::max(7.0, f.pointSizeF() * 0.9));
    p.setFont(f);

    // Ruler: pick a tick step that leaves room for labels.
    static const float steps[] = {0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f};
    float step = steps[std::size(steps) - 1];
    for (float s : steps) {
        if (s * pps >= 64.0f) { step = s; break; }
    }
    p.setPen(kText);
    for (float t = 0.0f; t <= dur + 1e-4f; t += step) {
        const float x = xOf(t);
        p.setPen(kGrid);
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
        p.setPen(kText);
        p.drawText(QPointF(x + 3, kRulerH - 7), QString::number(t, 'g', 3) + " s");
    }
    p.setPen(kGrid);
    p.drawLine(0, kRulerH, width(), kRulerH);

    // Event lane.
    p.fillRect(QRect(0, kRulerH, width(), kEventH), kLaneA);
    for (int i = 0; i < static_cast<int>(clip_.events.size()); ++i) {
        const auto& ev = clip_.events[i];
        const float x = xOf(ev.time);
        const bool sel = i == event_;
        QPolygonF tri;
        tri << QPointF(x, kRulerH + 3) << QPointF(x + 6, kRulerH + kEventH / 2) << QPointF(x, kRulerH + kEventH - 3)
            << QPointF(x - 6, kRulerH + kEventH / 2);
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? QColor(0xff, 0xff, 0xff) : kEvent);
        p.drawPolygon(tri);
        p.setBrush(Qt::NoBrush);
        p.setPen(kText);
        p.drawText(QPointF(x + 9, kRulerH + 15), QString::fromStdString(ev.name));
    }

    // Track lanes and their keys.
    const bool dragPreview = dragging_ && std::abs(dragDt_) > 1e-6f;
    for (int i = 0; i < static_cast<int>(clip_.tracks.size()); ++i) {
        const auto& tr = clip_.tracks[i];
        const int y0 = kRulerH + kEventH + i * kLaneH;
        p.fillRect(QRect(0, y0, width(), kLaneH), i == track_ ? kLaneSel : (i % 2 ? kLaneB : kLaneA));
        p.setPen(kGrid);
        p.drawLine(0, y0 + kLaneH - 1, width(), y0 + kLaneH - 1);
        const float cy = y0 + kLaneH / 2.0f;
        for (int k = 0; k < static_cast<int>(tr.times.size()); ++k) {
            const bool sel = keys_.count({i, k}) > 0;
            float t = tr.times[k];
            if (sel && dragPreview) t = std::max(0.0f, t + dragDt_);
            const float x = xOf(t);
            QPolygonF d;
            d << QPointF(x, cy - 5) << QPointF(x + 5, cy) << QPointF(x, cy + 5) << QPointF(x - 5, cy);
            p.setPen(sel ? QPen(QColor(0x20, 0x20, 0x20), 1) : Qt::NoPen);
            p.setBrush(sel ? kKeySel : kKey);
            p.drawPolygon(d);
            p.setBrush(Qt::NoBrush);
        }
    }
    if (clip_.tracks.empty()) {
        p.setPen(kText);
        p.drawText(QRect(0, kRulerH + kEventH, width(), kLaneH), Qt::AlignCenter, "No tracks. Use Add track.");
    }

    // Playhead.
    p.setPen(QPen(kPlayhead, 1.5));
    const float px = xOf(std::clamp(playhead_, 0.0f, dur));
    p.drawLine(QPointF(px, 0), QPointF(px, height()));
}

void AnimTimeline::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    setFocus();
    const int x = static_cast<int>(e->position().x());
    const int y = static_cast<int>(e->position().y());
    if (y < kRulerH) {
        scrubbing_ = true;
        emit seek(std::clamp(tOf(x), 0.0f, clip_.duration));
        return;
    }
    if (y < kRulerH + kEventH) {
        event_ = eventAt(x, y);
        if (event_ >= 0) keys_.clear();
        emit selectionChanged();
        update();
        return;
    }
    const int lane = laneAt(y);
    if (lane < 0) return;
    track_ = lane;
    event_ = -1;
    emit trackPicked(lane);
    const int hit = keyAt(lane, x);
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    if (hit >= 0) {
        const bool already = keys_.count({lane, hit}) > 0;
        if (ctrl) {
            if (already) keys_.erase({lane, hit}); else keys_.insert({lane, hit});
        } else if (!already) {
            keys_.clear();
            keys_.insert({lane, hit});
        }
        if (keys_.count({lane, hit})) {
            dragging_ = true;
            dragX0_ = x;
            dragDt_ = 0.0f;
        }
    } else if (!ctrl && !shift) {
        keys_.clear();
    }
    emit selectionChanged();
    update();
}

void AnimTimeline::mouseMoveEvent(QMouseEvent* e) {
    const int x = static_cast<int>(e->position().x());
    if (scrubbing_) {
        emit seek(std::clamp(tOf(x), 0.0f, clip_.duration));
        return;
    }
    if (dragging_) {
        const bool fine = e->modifiers() & Qt::ShiftModifier;
        const float raw = (x - dragX0_) / pxPerSec();
        float snapped = snapTime(raw, fine);
        // Keep the earliest moved key at or after zero.
        float minT = 1e9f;
        for (const auto& [t, k] : keys_) {
            if (t >= 0 && t < static_cast<int>(clip_.tracks.size()) && k < static_cast<int>(clip_.tracks[t].times.size()))
                minT = std::min(minT, clip_.tracks[t].times[k]);
        }
        if (minT < 1e8f && minT + snapped < 0.0f) snapped = -minT;
        dragDt_ = snapped;
        update();
    }
}

void AnimTimeline::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    scrubbing_ = false;
    if (dragging_) {
        dragging_ = false;
        const float dt = dragDt_;
        dragDt_ = 0.0f;
        if (std::abs(dt) > 1e-6f) emit keysMoved(dt);
    }
    update();
}

void AnimTimeline::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const int x = static_cast<int>(e->position().x());
    const int y = static_cast<int>(e->position().y());
    if (y < kRulerH) return;
    if (y < kRulerH + kEventH) {
        const int i = eventAt(x, y);
        if (i >= 0) emit renameEventRequested(i);
        else emit addEventRequested(snapTime(tOf(x), false));
        return;
    }
    const int lane = laneAt(y);
    if (lane >= 0 && keyAt(lane, x) < 0) emit addKeyRequested(lane, snapTime(tOf(x), false));
}

void AnimTimeline::keyPressEvent(QKeyEvent* e) {
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        emit deleteRequested();
    } else if (ctrl && e->key() == Qt::Key_Z && !(e->modifiers() & Qt::ShiftModifier)) {
        emit undoRequested();
    } else if ((ctrl && e->key() == Qt::Key_Y) || (ctrl && e->key() == Qt::Key_Z)) {
        emit redoRequested();
    } else {
        QWidget::keyPressEvent(e);
    }
}

bool AnimTimeline::event(QEvent* e) {
    // Let the panel's undo, redo and delete keys reach this widget instead of the main window's shortcuts.
    if (e->type() == QEvent::ShortcutOverride) {
        auto* k = static_cast<QKeyEvent*>(e);
        const bool ctrl = k->modifiers() & Qt::ControlModifier;
        if (k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace ||
            (ctrl && (k->key() == Qt::Key_Z || k->key() == Qt::Key_Y))) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

// ---------------------------------------------------------------- AnimCurve

AnimCurve::AnimCurve(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(140);
    setFocusPolicy(Qt::StrongFocus);
}

int AnimCurve::stride() const {
    return comps(track_.path);
}

void AnimCurve::setTrack(const ukiyo::AnimTrack& track, float duration, int channel, std::set<int> selected) {
    track_ = track;
    duration_ = std::max(duration, 0.05f);
    channel_ = std::clamp(channel, 0, stride() - 1);
    selected_ = std::move(selected);
    hasTrack_ = !track_.times.empty();
    if (hasTrack_) {
        const std::vector<float> vals = animfile::keyValues(track_);
        lo_ = vals.empty() ? 0.0f : *std::min_element(vals.begin(), vals.end());
        hi_ = vals.empty() ? 1.0f : *std::max_element(vals.begin(), vals.end());
        const float pad = std::max(0.1f * (hi_ - lo_), 0.05f);
        lo_ -= pad;
        hi_ += pad;
    }
    update();
}

void AnimCurve::clearTrack() {
    track_ = ukiyo::AnimTrack{};
    hasTrack_ = false;
    selected_.clear();
    update();
}

QPointF AnimCurve::pointOf(float t, float v) const {
    const float w = std::max(1, width() - 2 * kMargin);
    const float h = std::max(1, height() - 2 * kMargin);
    const float x = kMargin + t / duration_ * w;
    const float y = kMargin + (1.0f - (v - lo_) / (hi_ - lo_)) * h;
    return {x, y};
}

float AnimCurve::valueFromY(int y) const {
    const float h = std::max(1, height() - 2 * kMargin);
    const float f = 1.0f - (y - kMargin) / h;
    return lo_ + f * (hi_ - lo_);
}

float AnimCurve::timeFromX(int x) const {
    const float w = std::max(1, width() - 2 * kMargin);
    return std::clamp((x - kMargin) / w * duration_, 0.0f, duration_);
}

void AnimCurve::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kBg);
    p.setPen(kGrid);
    p.drawRect(QRect(kMargin, kMargin, width() - 2 * kMargin, height() - 2 * kMargin));
    if (!hasTrack_) {
        p.setPen(kText);
        p.drawText(rect(), Qt::AlignCenter, "Select a track to see its curves.");
        return;
    }
    const int n = stride();
    constexpr int kSamples = 160;
    for (int c = 0; c < n; ++c) {
        QPolygonF poly;
        for (int s = 0; s <= kSamples; ++s) {
            const float t = duration_ * s / kSamples;
            const auto v = animfile::valueAt(track_, t);
            if (c < static_cast<int>(v.size())) poly << pointOf(t, v[c]);
        }
        p.setPen(QPen(channelColor(c), c == channel_ ? 2.0 : 1.0, c == channel_ ? Qt::SolidLine : Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(poly);
    }
    const std::vector<float> vals = animfile::keyValues(track_);
    const int keyCount = static_cast<int>(track_.times.size());
    for (int k = 0; k < keyCount; ++k) {
        if (static_cast<size_t>(k * n + channel_) >= vals.size()) break;
        float v = vals[k * n + channel_];
        if (k == dragKey_) v = dragValue_;
        const QPointF pt = pointOf(track_.times[k], v);
        const bool sel = selected_.count(k) > 0;
        p.setPen(QPen(QColor(0x20, 0x20, 0x20), 1));
        p.setBrush(sel ? kKeySel : channelColor(channel_));
        p.drawEllipse(pt, 4.5, 4.5);
    }
}

void AnimCurve::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !hasTrack_) return;
    setFocus();
    const std::vector<float> vals = animfile::keyValues(track_);
    const int n = stride();
    const QPointF at = e->position();
    int best = -1;
    double bestD = 81.0; // within 9 px
    for (int k = 0; k < static_cast<int>(track_.times.size()); ++k) {
        if (static_cast<size_t>(k * n + channel_) >= vals.size()) break;
        const QPointF pt = pointOf(track_.times[k], vals[k * n + channel_]);
        const double dx = pt.x() - at.x(), dy = pt.y() - at.y();
        const double d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = k; }
    }
    if (best >= 0) {
        dragKey_ = best;
        dragValue_ = vals[best * n + channel_];
        update();
    }
}

void AnimCurve::mouseMoveEvent(QMouseEvent* e) {
    if (dragKey_ < 0) return;
    dragValue_ = valueFromY(static_cast<int>(e->position().y()));
    update();
}

void AnimCurve::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || dragKey_ < 0) return;
    const int k = dragKey_;
    const float v = valueFromY(static_cast<int>(e->position().y()));
    dragKey_ = -1;
    emit valueEdited(k, channel_, v);
    update();
}

void AnimCurve::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !hasTrack_) return;
    emit addKeyRequested(timeFromX(static_cast<int>(e->position().x())));
}
