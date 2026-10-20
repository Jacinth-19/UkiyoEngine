// The startup splash shared by the Ukiyo Hub and the World Editor.
//
// It runs in two beats, the way an editor splash usually does:
//
//   1. the mark: the ensō with its hanko, and UKIYO set in Shippori Mincho, on flat indigo
//      (editor/res/brand/ukiyo-splash.png, drawn by tools/brand.py);
//   2. the artwork: editor/res/splash.png, with the identity set in the open sky at the left,
//      and the project name, the line that says what is loading, and a vermilion progress bar
//      low in that same column.
//
// The second beat is a composition, not artwork with a scrim over it: the print's subject is on the right and its
// empty sky is on the left, so the type goes where the print leaves room, and the wash under it runs out before the
// island so the print is not dulled.
//
// The two fade into each other, the artwork drifts a few pixels so the screen is never quite still, and finish()
// fades the window out and closes it. The layout is designed at 960 x 540 and scaled from the widget's width, so it
// also holds up when the window is clamped on a small screen.
#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

class QPainter;

namespace editor {

class SplashScreen : public QWidget {
public:
    // `artPath` is editor/res/splash.png. When it is missing the splash paints a flat
    // indigo-and-vermilion wave instead, so the screen is never blank.
    // `heading` is the big line under the eyebrow: the project name, or "World Editor".
    explicit SplashScreen(const QString& artPath, const QString& heading, QWidget* parent = nullptr);

    // The small line above the heading: "CREATING PROJECT", "OPENING PROJECT",
    // "STARTING EDITOR". Upper case reads best at this size.
    void setEyebrow(const QString& text);
    void setHeading(const QString& text);

    // 0..1 and the line under the heading. Call as work actually completes; the bar
    // eases toward the value rather than jumping. GUI thread only.
    void setStage(float fraction, const QString& message);

    // A dimmer second line (the project folder, say), set in the quiet water at the
    // bottom of the artwork.
    void setCaption(const QString& caption);

    // Fades out and closes. The widget deletes itself when the fade ends. A splash
    // stays up for a moment even when the work finished instantly, so it never flashes.
    void finish();

    // Skips the mark and goes straight to the artwork. For self-tests, and for hosts
    // that only show the splash once loading is already well under way.
    void skipIntro();

    bool finishing() const { return fading_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void animate();
    qreal artAlpha() const;

    void paintMark(QPainter& painter, qreal s);
    void paintArtwork(QPainter& painter, qreal s);
    void paintArt(QPainter& painter, const QRectF& area);
    void paintFallbackArt(QPainter& painter, const QRectF& area);
    void paintProgress(QPainter& painter, qreal s);

    QPixmap art_;
    QPixmap brand_;   // editor/res/brand/ukiyo-splash.png, the stacked mark
    QString heading_;
    QString eyebrow_ = QStringLiteral("LOADING");
    QString caption_;
    QString message_ = QStringLiteral("Preparing");
    float fraction_ = 0.0f;
    float shown_ = 0.0f;   // eased toward fraction_
    qreal age_ = 0.0;      // seconds since the splash appeared
    qreal lastAge_ = 0.0;  // previous frame, for a time-based ease
    qreal markEnd_ = 0.95; // seconds the mark holds before the artwork fades in
    bool fading_ = false;
    bool skipped_ = false;
    bool centred_ = false;
    qint64 finishAt_ = 0;  // milliseconds; the fade waits until then
    QElapsedTimer clock_;
    QTimer timer_;
};

} // namespace editor
