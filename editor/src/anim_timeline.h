// Drawing and mouse handling for the Animation panel: the timeline (ruler, event lane, one lane per track
// with its keys) and the curve view (one track's channels, with draggable key values).
#pragma once

#include <QWidget>

#include <set>
#include <utility>
#include <vector>

#include "engine/anim.h"

class AnimTimeline : public QWidget {
    Q_OBJECT
public:
    explicit AnimTimeline(QWidget* parent = nullptr);

    void setClip(const ukiyo::AnimClip& clip);   // copies the clip
    void setPlayhead(float seconds);
    void setSelectedTrack(int track);
    void selectEvent(int event);
    void selectKey(int track, int key, bool add);
    void clearKeySelection();
    std::set<std::pair<int, int>> selectedKeys() const { return keys_; }
    int selectedEvent() const { return event_; }

    static constexpr int kRulerH = 22;
    static constexpr int kEventH = 22;
    static constexpr int kLaneH = 24;

signals:
    void seek(float seconds);
    void trackPicked(int track);
    void selectionChanged();
    void keysMoved(float seconds);
    void addKeyRequested(int track, float seconds);
    void addEventRequested(float seconds);
    void renameEventRequested(int event);
    void deleteRequested();
    void undoRequested();
    void redoRequested();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;

private:
    float pxPerSec() const;
    float xOf(float t) const;
    float tOf(float x) const;
    float snapTime(float t, bool fine) const;
    int laneAt(int y) const;      // track index under y, or -1
    int eventAt(int x, int y) const;
    int keyAt(int track, int x) const;

    ukiyo::AnimClip clip_;
    float playhead_ = 0.0f;
    int track_ = -1;
    int event_ = -1;
    std::set<std::pair<int, int>> keys_;
    bool dragging_ = false;
    bool scrubbing_ = false;
    int dragX0_ = 0;
    float dragDt_ = 0.0f;
};

class AnimCurve : public QWidget {
    Q_OBJECT
public:
    explicit AnimCurve(QWidget* parent = nullptr);

    // Shows one track. Channel is 0..2 (or 3 for rotation's w). Selected keys are drawn highlighted.
    void setTrack(const ukiyo::AnimTrack& track, float duration, int channel, std::set<int> selected);
    void clearTrack();

signals:
    void valueEdited(int key, int channel, float value);
    void addKeyRequested(float seconds);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    int stride() const;
    QPointF pointOf(float t, float v) const;
    float valueFromY(int y) const;
    float timeFromX(int x) const;

    ukiyo::AnimTrack track_;
    float duration_ = 1.0f;
    int channel_ = 0;
    std::set<int> selected_;
    bool hasTrack_ = false;
    float lo_ = 0.0f;
    float hi_ = 1.0f;
    int dragKey_ = -1;
    float dragValue_ = 0.0f;
};
