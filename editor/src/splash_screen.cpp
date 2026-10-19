#include "splash_screen.h"

#include <algorithm>
#include <cmath>

#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRadialGradient>
#include <QScreen>
#include <QShowEvent>
#include <QSizeF>

namespace editor {
namespace {

const QColor kAccent(0x4a, 0x9e, 0xf0);
const QColor kMint(0x7e, 0xe3, 0xd4);

float easeToward(float current, float target, float factor) {
    return current + (target - current) * factor;
}

QFont spaced(QFont font, int pixels, qreal spacing, QFont::Weight weight = QFont::Normal) {
    font.setPixelSize(pixels);
    font.setWeight(weight);
    font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    return font;
}

} // namespace

SplashScreen::SplashScreen(const QString& artPath, const QString& heading, QWidget* parent)
    : QWidget(parent), heading_(heading) {
    setObjectName(QStringLiteral("ukiyoSplash"));
    setWindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    // The window paints its own rounded artwork; the app-wide dark surface must not show through.
    setStyleSheet(QStringLiteral("background: transparent;"));
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFixedSize(960, 540);

    if (!artPath.isEmpty()) art_.load(artPath);

    connect(&timer_, &QTimer::timeout, this, [this] { animate(); });
    timer_.start(16);
}

void SplashScreen::setStage(float fraction, const QString& message) {
    fraction_ = std::clamp(fraction, 0.0f, 1.0f);
    message_ = message;
    update();
}

void SplashScreen::setCaption(const QString& caption) {
    caption_ = caption;
    update();
}

void SplashScreen::finish() {
    if (fading_) return;
    fading_ = true;
    fraction_ = 1.0f;
}

void SplashScreen::showEvent(QShowEvent* event) {
    if (!centred_) {
        centred_ = true;
        if (QScreen* screen = QGuiApplication::primaryScreen()) {
            const QRect available = screen->availableGeometry();
            // Keep the splash comfortably inside small screens.
            const QSize fitted(std::min(width(), int(available.width() * 0.86)),
                               std::min(height(), int(available.height() * 0.86)));
            if (fitted != size()) setFixedSize(fitted);
            move(available.center() - rect().center());
        }
    }
    opacity_ = 0.0f;
    setWindowOpacity(0.0f);
    QWidget::showEvent(event);
}

void SplashScreen::animate() {
    const float step = 0.016f;
    phase_ += step;

    if (fading_) {
        opacity_ = std::max(0.0f, opacity_ - 0.065f);
        setWindowOpacity(opacity_);
        if (opacity_ <= 0.0f) {
            timer_.stop();
            close();
            return;
        }
    } else if (opacity_ < 1.0f) {
        opacity_ = std::min(1.0f, opacity_ + 0.075f);
        setWindowOpacity(opacity_);
    }

    shown_ = easeToward(shown_, fraction_, 0.14f);
    if (std::fabs(shown_ - fraction_) < 0.0005f) shown_ = fraction_;
    update();
}

void SplashScreen::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath rounded;
    rounded.addRoundedRect(frame, 16.0, 16.0);

    painter.save();
    painter.setClipPath(rounded);
    if (art_.isNull()) paintFallbackArt(painter, frame);
    else paintArt(painter, frame);

    // Darken the top for the wordmark and the bottom for the status block.
    QLinearGradient top(frame.topLeft(), QPointF(frame.left(), frame.top() + frame.height() * 0.42));
    top.setColorAt(0.0, QColor(6, 9, 14, 178));
    top.setColorAt(1.0, QColor(6, 9, 14, 0));
    painter.fillRect(frame, top);

    QLinearGradient bottom(QPointF(frame.left(), frame.top() + frame.height() * 0.42), frame.bottomLeft());
    bottom.setColorAt(0.0, QColor(6, 9, 14, 0));
    bottom.setColorAt(0.55, QColor(6, 9, 14, 176));
    bottom.setColorAt(1.0, QColor(6, 9, 14, 236));
    painter.fillRect(frame, bottom);

    QRadialGradient glow(QPointF(frame.left() + 120.0, frame.bottom() - 40.0), 360.0);
    glow.setColorAt(0.0, QColor(74, 158, 240, 34));
    glow.setColorAt(1.0, QColor(74, 158, 240, 0));
    painter.fillRect(frame, glow);

    const qreal margin = 48.0;

    // Wordmark, top left.
    painter.setPen(QColor(240, 246, 252));
    painter.setFont(spaced(font(), 30, 9.0, QFont::Black));
    painter.drawText(QPointF(margin, 60.0), QStringLiteral("UKIYO"));
    painter.setPen(QColor(kMint.red(), kMint.green(), kMint.blue(), 210));
    painter.setFont(spaced(font(), 10, 3.4, QFont::DemiBold));
    painter.drawText(QPointF(margin + 2.0, 80.0), QStringLiteral("VULKAN  ·  ANIME PIPELINE"));

    // Heading above the loading line.
    const qreal baseY = frame.bottom() - 132.0;
    painter.setPen(QColor(214, 227, 239, 190));
    painter.setFont(spaced(font(), 10, 2.6, QFont::DemiBold));
    painter.drawText(QPointF(margin, baseY - 34.0), QStringLiteral("LOADING"));

    painter.setPen(QColor(255, 255, 255, 240));
    painter.setFont(spaced(font(), 26, 0.6, QFont::Bold));
    painter.drawText(QPointF(margin, baseY), heading_);

    if (!caption_.isEmpty()) {
        painter.setPen(QColor(168, 186, 203, 200));
        painter.setFont(spaced(font(), 11, 0.2));
        const QString line = QFontMetrics(painter.font()).elidedText(
            caption_, Qt::ElideMiddle, int(frame.width() - margin * 2.0));
        painter.drawText(QPointF(margin, baseY + 20.0), line);
    }

    paintProgress(painter, frame);
    painter.restore();

    // A hairline border keeps the rounded edge readable on dark desktops.
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(120, 160, 200, 60), 1.0));
    painter.drawPath(rounded);
}

void SplashScreen::paintArt(QPainter& painter, const QRectF& area) {
    const float zoom = 1.045f + 0.012f * std::sin(phase_ * 0.12f);
    QSizeF scaled = QSizeF(art_.size()).scaled(area.size() * zoom, Qt::KeepAspectRatioByExpanding);
    QRectF target(QPointF(0, 0), scaled);
    target.moveCenter(area.center() + QPointF(std::sin(phase_ * 0.08f) * 8.0, std::cos(phase_ * 0.06f) * 4.0));
    painter.drawPixmap(target, art_, QRectF(art_.rect()));
}

void SplashScreen::paintFallbackArt(QPainter& painter, const QRectF& area) {
    QLinearGradient sky(area.topLeft(), area.bottomLeft());
    sky.setColorAt(0.0, QColor(16, 28, 46));
    sky.setColorAt(0.45, QColor(34, 58, 78));
    sky.setColorAt(1.0, QColor(12, 24, 32));
    painter.fillRect(area, sky);

    // A low sun.
    QRadialGradient sun(QPointF(area.width() * 0.72, area.height() * 0.42), area.height() * 0.34);
    sun.setColorAt(0.0, QColor(255, 196, 140, 200));
    sun.setColorAt(1.0, QColor(255, 150, 120, 0));
    painter.fillRect(area, sun);

    // Three ink waves.
    const QColor waveColors[] = {QColor(30, 96, 110, 210), QColor(20, 70, 92, 220), QColor(10, 40, 56, 240)};
    for (int i = 0; i < 3; ++i) {
        QPainterPath wave;
        const qreal base = area.height() * (0.60 + 0.10 * i);
        wave.moveTo(area.left(), area.bottom());
        wave.lineTo(area.left(), base);
        for (int x = 0; x <= int(area.width()); x += 8) {
            const qreal y = base + std::sin(x * 0.012 + phase_ * (0.6 + 0.2 * i) + i * 1.7) * (10.0 + 4.0 * i) +
                            std::sin(x * 0.031 + i) * 3.0;
            wave.lineTo(area.left() + x, y);
        }
        wave.lineTo(area.right(), area.bottom());
        wave.closeSubpath();
        painter.fillPath(wave, waveColors[i]);
    }

    // A dark torii silhouette on the right, like the artwork.
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(22, 18, 24, 235));
    const qreal cx = area.width() * 0.74;
    const qreal top = area.height() * 0.25;
    painter.drawRect(QRectF(cx - 62, top, 124, 9));
    painter.drawRect(QRectF(cx - 50, top + 22, 100, 7));
    painter.drawRect(QRectF(cx - 44, top + 9, 9, 130));
    painter.drawRect(QRectF(cx + 35, top + 9, 9, 130));
    painter.restore();
}

void SplashScreen::paintProgress(QPainter& painter, const QRectF& area) {
    const qreal margin = 48.0;
    const qreal barWidth = area.width() - margin * 2.0;
    const qreal barY = area.bottom() - 46.0;
    const QRectF track(margin, barY, barWidth, 6.0);
    const int percent = int(std::lround(shown_ * 100.0f));

    // Status line, with a small turning ring.
    const QRectF spinner(margin, barY - 32.0, 12.0, 12.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(120, 205, 255, 120), 1.6));
    painter.drawEllipse(spinner);
    painter.setPen(QPen(kMint, 2.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(spinner.adjusted(1, 1, -1, -1), int(phase_ * 220.0) * 16, 100 * 16);

    painter.setFont(spaced(font(), 11, 0.3));
    painter.setPen(QColor(226, 237, 247, 235));
    const qreal textWidth = barWidth - 70.0;
    const QString line = QFontMetrics(painter.font()).elidedText(message_, Qt::ElideRight, int(textWidth));
    painter.drawText(QPointF(margin + 20.0, barY - 20.0), line);

    painter.setPen(QColor(150, 195, 232, 220));
    painter.setFont(spaced(font(), 11, 0.6, QFont::DemiBold));
    painter.drawText(QRectF(margin, barY - 34.0, barWidth, 20.0), Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(percent) + QStringLiteral("%"));

    // Track, then the gradient fill.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 40));
    painter.drawRoundedRect(track, 3.0, 3.0);

    const qreal filled = std::clamp<qreal>(barWidth * shown_, 0.0, barWidth);
    if (filled > 0.5) {
        painter.save();
        QPainterPath clip;
        clip.addRoundedRect(QRectF(track.topLeft(), QSizeF(filled, track.height())), 3.0, 3.0);
        painter.setClipPath(clip);
        QLinearGradient fill(track.topLeft(), QPointF(track.left() + barWidth, track.top()));
        fill.setColorAt(0.0, kAccent);
        fill.setColorAt(0.65, QColor(0x63, 0xc8, 0xe8));
        fill.setColorAt(1.0, kMint);
        painter.fillRect(track, fill);

        // A soft highlight sweeping along the bar.
        const qreal sweep = std::fmod(phase_ * 0.42, 1.0) * (barWidth + 180.0) - 90.0;
        QLinearGradient shine(QPointF(track.left() + sweep - 60.0, track.top()),
                              QPointF(track.left() + sweep + 60.0, track.top()));
        shine.setColorAt(0.0, QColor(255, 255, 255, 0));
        shine.setColorAt(0.5, QColor(255, 255, 255, 95));
        shine.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter.fillRect(track, shine);
        painter.restore();

        // A small glow at the tip of the fill.
        QRadialGradient tip(QPointF(track.left() + filled, track.center().y()), 18.0);
        tip.setColorAt(0.0, QColor(kMint.red(), kMint.green(), kMint.blue(), 120));
        tip.setColorAt(1.0, QColor(kMint.red(), kMint.green(), kMint.blue(), 0));
        painter.setBrush(tip);
        painter.drawEllipse(QPointF(track.left() + filled, track.center().y()), 18.0, 18.0);
    }

    // Version, bottom right, and the engine line bottom left.
    painter.setFont(spaced(font(), 9, 0.8));
    painter.setPen(QColor(150, 168, 184, 170));
    painter.drawText(QRectF(margin, area.bottom() - 24.0, barWidth, 14.0), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("ukiyo engine"));
    painter.drawText(QRectF(margin, area.bottom() - 24.0, barWidth, 14.0), Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("v0.1"));
}

} // namespace editor
