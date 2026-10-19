// The loading splash shared by the Ukiyo Hub and the World Editor: the splash artwork, the project
// name, a progress bar, and the line that says what is loading right now. It is a plain widget that
// paints itself and animates on a timer; call setStage() as work completes and finish() when the
// editor or the new project is ready.
#pragma once

#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

class QPainter;

namespace editor {

class SplashScreen : public QWidget {
public:
    // `artPath` is the splash image (editor/res/splash.png). A missing image falls back to a painted
    // wave scene, so the splash is never blank. `heading` sits under the wordmark: "World Editor".
    explicit SplashScreen(const QString& artPath, const QString& heading = QStringLiteral("World Editor"),
                          QWidget* parent = nullptr);

    // 0..1 and the line under the bar. Safe to call from the GUI thread only; worker threads should
    // post through QMetaObject::invokeMethod.
    void setStage(float fraction, const QString& message);
    // A second, dimmer line (the project path, say).
    void setCaption(const QString& caption);
    // Fades out and closes. The widget deletes itself when the fade ends.
    void finish();

    bool finishing() const { return fading_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void animate();
    void paintArt(QPainter& painter, const QRectF& area);
    void paintFallbackArt(QPainter& painter, const QRectF& area);
    void paintProgress(QPainter& painter, const QRectF& area);

    QPixmap art_;
    QString heading_;
    QString caption_;
    QString message_ = QStringLiteral("Preparing");
    float fraction_ = 0.0f;
    float shown_ = 0.0f;      // eased toward fraction_
    float phase_ = 0.0f;      // seconds, for the shimmer and the spinner
    float opacity_ = 0.0f;
    bool fading_ = false;
    bool centred_ = false;
    QTimer timer_;
};

} // namespace editor
