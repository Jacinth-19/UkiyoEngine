#include "splash_screen.h"

#include "icons.h"

#include <algorithm>
#include <cmath>

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScreen>
#include <QShowEvent>
#include <QSizeF>

namespace editor {
namespace {

// Palette. Four colours and two neutrals, which is enough for the whole screen.
const QColor kInk(241, 235, 223);        // warm off-white, like the print's paper
const QColor kMuted(191, 192, 194);
const QColor kDim(134, 141, 150);
const QColor kVermilion(204, 72, 50);    // the one accent
const QColor kNight(26, 40, 68);         // flat indigo, sampled from the artwork
const QColor kScrim(8, 12, 20);

// The design is laid out on a 960 x 540 grid; everything is multiplied by `s`.
constexpr qreal kDesignWidth = 960.0;
constexpr qreal kMargin = 44.0;
constexpr qreal kBarTop = 452.0;   // the track sits at the foot of the status block
constexpr qreal kBarWidth = 340.0;
constexpr qreal kMarkHoldMs = 950.0;     // the mark's beat before the artwork
constexpr qreal kCrossfadeMs = 450.0;
constexpr qint64 kMinimumVisibleMs = 1350; // no flash for instant work
constexpr qreal kFadeOutMs = 280.0;

// The 07 print puts its cloud mass against the card's right rounded corner, so the framing is pulled four per cent to
// the left: the source starts four per cent in, which moves the clouds off the corner and opens the right margin.
// Tuned for that artwork; revisit if the splash image changes.
constexpr qreal kArtCropLeft = 0.04;
// The flat scrim under the gradient. 58 rather than the old 46: the 07 print is bright at the top right and the small
// caption line needs the extra contrast (3.86:1 as it stands).
constexpr int kArtScrimAlpha = 58;

QFont sized(const QFont& base, qreal pixels, QFont::Weight weight, qreal spacing = 0.0) {
    QFont font = base;
    font.setPixelSize(std::max(1, int(std::lround(pixels))));
    font.setWeight(weight);
    if (spacing > 0.0) font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    else font.setLetterSpacing(QFont::PercentageSpacing, 100.0);
    return font;
}

// The tracked wordmarks carry a trailing letter-space, so their measured width is wider
// than the ink. Pulling half of it back keeps the optical centre.
qreal opticalCentre(qreal width, qreal spacing) { return (width - spacing) * 0.5; }

} // namespace

#ifndef UKIYO_ROOT
#define UKIYO_ROOT "."
#endif

SplashScreen::SplashScreen(const QString& artPath, const QString& heading, QWidget* parent)
    : QWidget(parent), heading_(heading) {
    setObjectName(QStringLiteral("ukiyoSplash"));
    setWindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setAutoFillBackground(false);
    // The window paints its own rounded card; the application's dark surface must not
    // show through the corners.
    setStyleSheet(QStringLiteral("background: transparent;"));
    setFixedSize(960, 540);

    if (!artPath.isEmpty()) art_.load(artPath);
    // The designed mark, if the checkout ships it. Without it the text mark below still stands in.
    brand_.load(QStringLiteral(UKIYO_ROOT) + QStringLiteral("/editor/res/brand/ukiyo-splash.png"));
    brand_.setDevicePixelRatio(1.0);

    markEnd_ = kMarkHoldMs / 1000.0;
    clock_.start();
    connect(&timer_, &QTimer::timeout, this, [this] { animate(); });
    timer_.start(16);
}

void SplashScreen::setEyebrow(const QString& text) {
    eyebrow_ = text;
    update();
}

void SplashScreen::setHeading(const QString& text) {
    heading_ = text;
    update();
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
    finishAt_ = std::max(clock_.elapsed() + qint64(60), qint64(kMinimumVisibleMs));
}

void SplashScreen::skipIntro() {
    skipped_ = true;
    markEnd_ = 0.0;
    update();
}

void SplashScreen::showEvent(QShowEvent* event) {
    if (!centred_) {
        centred_ = true;
        QSize target(960, 540);
        if (QScreen* screen = QGuiApplication::primaryScreen()) {
            const QRect available = screen->availableGeometry();
            // Keep the splash comfortably inside small screens, and keep its 16:9 shape
            // so the artwork and the type block stay where they were designed to be.
            const int maxWidth = int(available.width() * 0.86);
            const int maxHeight = int(available.height() * 0.86);
            if (target.width() > maxWidth) target = QSize(maxWidth, maxWidth * 9 / 16);
            if (target.height() > maxHeight) target = QSize(maxHeight * 16 / 9, maxHeight);
            move(available.center() - QPoint(target.width() / 2, target.height() / 2));
        }
        setFixedSize(target);
    }
    // The window itself fades in over the first ~200 ms (see animate()).
    setWindowOpacity(0.0);
    QWidget::showEvent(event);
}

void SplashScreen::animate() {
    const qint64 elapsed = clock_.elapsed();
    age_ = elapsed / 1000.0;

    // Real work is already moving: don't sit on the mark while the bar is filling.
    if (shown_ > 0.45f && age_ < markEnd_) markEnd_ = age_;

    // Ease toward the real fraction at a fixed rate per second, whatever the frame rate.
    const qreal dt = std::clamp(age_ - lastAge_, 0.0, 0.05);
    lastAge_ = age_;
    const qreal target = fraction_;
    shown_ += float((target - shown_) * std::min(1.0, dt * 9.0));
    if (std::fabs(shown_ - target) < 0.0006f) shown_ = float(target);

    // Fade the window in, then out once finish() has held it for its minimum beat.
    qreal windowAlpha = std::clamp(elapsed / 190.0, 0.0, 1.0);
    if (fading_ && elapsed >= finishAt_) {
        const qreal fade = (elapsed - finishAt_) / kFadeOutMs;
        windowAlpha = std::clamp(1.0 - fade, 0.0, 1.0);
        if (fade >= 1.0) {
            timer_.stop();
            close();
            return;
        }
    }
    setWindowOpacity(windowAlpha);

    update();
}

qreal SplashScreen::artAlpha() const {
    if (skipped_) return 1.0;
    if (age_ <= markEnd_) return 0.0;
    const qreal fade = (age_ - markEnd_) * 1000.0 / kCrossfadeMs;
    return std::clamp(fade, 0.0, 1.0);
}

void SplashScreen::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // The widget keeps its 16:9 shape (see showEvent), so scaling from the width also
    // scales the height, and the type block lands where it was designed to.
    const qreal s = width() / kDesignWidth;
    const QRectF frame(rect());
    QPainterPath rounded;
    rounded.addRoundedRect(frame.adjusted(0.5, 0.5, -0.5, -0.5), 14.0 * s, 14.0 * s);

    painter.save();
    painter.setClipPath(rounded);

    paintMark(painter, s);
    const qreal a = artAlpha();
    if (a > 0.0) {
        painter.setOpacity(a);
        paintArtwork(painter, s);
        painter.setOpacity(1.0);
    }

    painter.restore();

    // A hairline edge keeps the rounded card readable on dark desktops.
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(241, 235, 223, 32), 1.0));
    painter.drawPath(rounded);
}

// ---------------------------------------------------------------------------------------------
// Beat 1: the mark

void SplashScreen::paintMark(QPainter& painter, qreal s) {
    painter.fillRect(rect(), kNight);

    // The designed lockup, drawn by tools/brand.py from the same geometry as the mark assets. When it is missing,
    // the text below still stands in, so a checkout without the generated PNGs is not a blank screen.
    const qreal alpha = skipped_ ? 1.0 : std::clamp(age_ / 0.34, 0.0, 1.0);
    if (!brand_.isNull()) {
        // The mark settles: a hair of scale and a short fade. The artwork crossfades over it.
        const qreal settle = 0.986 + 0.014 * alpha;
        const qreal targetW = 430.0 * s * settle;
        const qreal targetH = targetW * qreal(brand_.height()) / qreal(brand_.width());
        QRectF box(0.0, 0.0, targetW, targetH);
        box.moveCenter(QPointF(width() * 0.5, height() * 0.5 - 4.0 * s));
        painter.save();
        painter.setOpacity(alpha);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(box, brand_, QRectF(brand_.rect()));
        painter.restore();
        return;
    }

    // Fallback: the wordmark set in the display face, which is the same Mincho the PNG is set in.
    const qreal w = width();
    const qreal h = height();
    const QString family = displayFamily();
    QFont wordFont = family.isEmpty() ? font() : QFont(family);
    wordFont.setPixelSize(std::max(1, int(std::lround(44.0 * s))));
    wordFont.setWeight(QFont::Bold);
    wordFont.setLetterSpacing(QFont::AbsoluteSpacing, 11.0 * s);
    const QFontMetricsF wordMetrics(wordFont);
    const QString word = QStringLiteral("UKIYO");
    const qreal wordWidth = wordMetrics.horizontalAdvance(word);

    const qreal baseline = h * 0.5 - 6.0 * s;
    painter.setFont(wordFont);
    painter.setPen(kInk);
    painter.drawText(QPointF(w * 0.5 - opticalCentre(wordWidth, 11.0 * s), baseline), word);

    QFont jpFont = wordFont;
    jpFont.setPixelSize(std::max(1, int(std::lround(15.0 * s))));
    jpFont.setWeight(QFont::Normal);
    jpFont.setLetterSpacing(QFont::AbsoluteSpacing, 4.0 * s);
    const QString jp = QStringLiteral("浮世エンジン");
    const qreal jpWidth = QFontMetricsF(jpFont).horizontalAdvance(jp);
    painter.setFont(jpFont);
    painter.setPen(kMuted);
    painter.drawText(QPointF(w * 0.5 - opticalCentre(jpWidth, 4.0 * s), baseline + 34.0 * s), jp);
}

// ---------------------------------------------------------------------------------------------
// Beat 2: the artwork

void SplashScreen::paintArtwork(QPainter& painter, qreal s) {
    const QRectF area(rect());
    paintArt(painter, area);

    // The print is a woodblock with its subject on the right and open sky on the left. The left is where the type
    // goes, so the scrim is a horizontal wash that seats it without dulling the island: denser at the margin, gone
    // by two thirds across. A vertical gradient under the status block adds weight at the foot.
    QLinearGradient wash(area.topLeft(), area.topRight());
    wash.setColorAt(0.00, QColor(kScrim.red(), kScrim.green(), kScrim.blue(), 172));
    wash.setColorAt(0.34, QColor(kScrim.red(), kScrim.green(), kScrim.blue(), 118));
    wash.setColorAt(0.62, QColor(kScrim.red(), kScrim.green(), kScrim.blue(), 22));
    wash.setColorAt(1.00, QColor(kScrim.red(), kScrim.green(), kScrim.blue(), 0));
    painter.fillRect(area, wash);

    QLinearGradient foot(QPointF(0.0, area.height() * 0.62), QPointF(0.0, area.height()));
    foot.setColorAt(0.00, QColor(kScrim.red(), kScrim.green(), kScrim.blue(), 0));
    foot.setColorAt(1.00, QColor(kScrim.red(), kScrim.green(), kScrim.blue(), 132));
    painter.fillRect(area, foot);

    const qreal a = skipped_ ? 1.0 : artAlpha();
    painter.save();
    painter.translate(0.0, (1.0 - a) * 9.0 * s);
    painter.setOpacity(a);

    // ---- the identity, in the open sky
    const QString family = displayFamily();
    const bool mincho = !family.isEmpty();
    QFont wordFont = mincho ? QFont(family) : font();
    wordFont.setPixelSize(std::max(1, int(std::lround(58.0 * s))));
    wordFont.setWeight(QFont::Bold);
    wordFont.setLetterSpacing(QFont::AbsoluteSpacing, 7.0 * s);
    painter.setFont(wordFont);
    painter.setPen(kInk);
    const qreal wordX = kMargin * s + 150.0 * s;
    const qreal wordY = 176.0 * s;
    painter.drawText(QPointF(wordX, wordY), QStringLiteral("UKIYO"));

    // The mark sits on the wordmark's cap line, at the left of it: the seal is the only red here, as in the PNG.
    // Loaded at 256 and drawn at 128: the seal's glyph is only ~5 % of the mark's width, so it needs the source pixels
    // to survive the downscale.
    const QPixmap mark = QPixmap(QStringLiteral(UKIYO_ROOT) + QStringLiteral("/editor/res/brand/ukiyo-mark-256.png"))
                             .scaled(int(128.0 * s), int(128.0 * s), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (!mark.isNull())
        painter.drawPixmap(QPointF(kMargin * s, wordY - 92.0 * s), mark);

    QFont subFont = mincho ? QFont(family) : font();
    subFont.setPixelSize(std::max(1, int(std::lround(13.0 * s))));
    subFont.setWeight(QFont::Normal);
    subFont.setLetterSpacing(QFont::AbsoluteSpacing, 6.0 * s);
    painter.setFont(subFont);
    painter.setPen(kMuted);
    painter.drawText(QPointF(wordX + 3.0 * s, wordY + 26.0 * s), QStringLiteral("ENGINE"));

    // The engine's other name, after the Latin, so the interface still reads as English by default.
    QFont jpFont = subFont;
    jpFont.setPixelSize(std::max(1, int(std::lround(15.0 * s))));
    jpFont.setLetterSpacing(QFont::AbsoluteSpacing, 3.0 * s);
    painter.setFont(jpFont);
    painter.setPen(QColor(kMuted.red(), kMuted.green(), kMuted.blue(), 210));
    painter.drawText(QPointF(wordX + 2.0 * s, wordY + 54.0 * s), QStringLiteral("浮世エンジン"));

    // Version, bottom right, on the dark cloud mass. It was in the top right corner, where it sat on the sun disc in a
    // grey that disappeared into the orange; the foot of the card is where a print puts its colophon anyway.
    const QString version = QStringLiteral("v0.1");
    const QFont versionFont = sized(font(), 8.0 * s, QFont::Normal, 0.8 * s);
    const qreal versionWidth = QFontMetricsF(versionFont).horizontalAdvance(version);
    painter.setFont(versionFont);
    painter.setPen(QColor(kMuted.red(), kMuted.green(), kMuted.blue(), 150));
    painter.drawText(QPointF(width() - kMargin * s - versionWidth, 511.0 * s), version);

    // ---- the status block, low in the left column
    const qreal typeWidth = std::min(430.0 * s, width() - kMargin * 2.0 * s);
    const QFont eyebrowFont = sized(font(), 7.0 * s, QFont::Bold, 2.2 * s);
    painter.setFont(eyebrowFont);
    painter.setPen(QColor(196, 184, 166, 240));
    painter.drawText(QPointF(kMargin * s, 372.0 * s), eyebrow_);

    const QFont headingFont = sized(font(), 24.0 * s, QFont::Bold, 0.2 * s);
    const QFontMetricsF headingMetrics(headingFont);
    painter.setFont(headingFont);
    painter.setPen(kInk);
    painter.drawText(QPointF(kMargin * s, 402.0 * s),
                     headingMetrics.elidedText(heading_, Qt::ElideRight, typeWidth));

    const QFont stageFont = sized(font(), 10.0 * s, QFont::Normal, 0.2 * s);
    const QFontMetricsF stageMetrics(stageFont);
    painter.setFont(stageFont);
    painter.setPen(kMuted);
    painter.drawText(QPointF(kMargin * s, 428.0 * s),
                     stageMetrics.elidedText(message_, Qt::ElideRight, typeWidth));

    paintProgress(painter, s);

    if (!caption_.isEmpty()) {
        const QFont captionFont = sized(font(), 8.0 * s, QFont::Normal, 0.2 * s);
        const QFontMetricsF captionMetrics(captionFont);
        painter.setFont(captionFont);
        painter.setPen(kDim);
        painter.drawText(QPointF(kMargin * s, 511.0 * s),
                         captionMetrics.elidedText(caption_, Qt::ElideMiddle, width() - kMargin * 2.0 * s));
    }

    painter.restore();
}

void SplashScreen::paintArt(QPainter& painter, const QRectF& area) {
    if (art_.isNull()) {
        paintFallbackArt(painter, area);
        return;
    }

    // A slow drift keeps the screen alive without ever pulling the eye.
    const qreal zoom = 1.025 + 0.010 * std::sin(age_ * 0.30);
    // Crop from the left, not by panning the target: at the 1.025 base zoom a pan would expose a bare edge on the
    // other side, while a narrower source still covers the card.
    const QRectF source(art_.width() * kArtCropLeft, 0.0,
                        art_.width() * (1.0 - kArtCropLeft), art_.height());
    const QSizeF scaled = QSizeF(source.size()).scaled(area.size() * zoom, Qt::KeepAspectRatioByExpanding);
    QRectF target(QPointF(0.0, 0.0), scaled);
    target.moveCenter(area.center() + QPointF(std::sin(age_ * 0.18) * 5.0,
                                              std::cos(age_ * 0.14) * 3.0));
    painter.drawPixmap(target, art_, source);
}

void SplashScreen::paintFallbackArt(QPainter& painter, const QRectF& area) {
    painter.fillRect(area, kNight);

    const qreal unit = area.height() / 540.0;
    const qreal horizon = area.height() * 0.79;

    // The sun, flat, sitting on the horizon line.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kVermilion);
    const qreal sun = area.width() * 0.072;
    painter.drawEllipse(QPointF(area.width() * 0.13, horizon - sun * 0.15), sun, sun);

    // The water, two flat bands.
    painter.setBrush(QColor(18, 26, 44));
    painter.drawRect(QRectF(0.0, horizon, area.width(), area.height() - horizon));

    QPainterPath wave;
    wave.moveTo(area.width() * 0.34, horizon);
    wave.cubicTo(area.width() * 0.52, horizon - 150.0 * unit,
                 area.width() * 0.72, horizon - 190.0 * unit,
                 area.width() * 0.92, horizon - 120.0 * unit);
    wave.lineTo(area.right(), area.bottom());
    wave.lineTo(area.width() * 0.34, area.bottom());
    wave.closeSubpath();
    painter.setBrush(QColor(30, 42, 66));
    painter.drawPath(wave);

    QPainterPath crest;
    crest.moveTo(area.width() * 0.55, area.bottom());
    crest.cubicTo(area.width() * 0.62, horizon - 40.0 * unit,
                  area.width() * 0.82, horizon - 60.0 * unit,
                  area.width() * 0.98, horizon + 10.0 * unit);
    crest.lineTo(area.right(), area.bottom());
    crest.closeSubpath();
    painter.setBrush(QColor(10, 16, 30));
    painter.drawPath(crest);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(kInk.red(), kInk.green(), kInk.blue(), 210), std::max(1.0, 1.2 * unit)));
    painter.drawLine(QPointF(0.0, horizon), QPointF(area.width(), horizon));
}

void SplashScreen::paintProgress(QPainter& painter, qreal s) {
    const QRectF track(kMargin * s, 452.0 * s, kBarWidth * s, std::max(2.0, 2.0 * s));
    const qreal filled = std::clamp<qreal>(track.width() * shown_, 0.0, track.width());

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(241, 235, 223, 46));
    painter.drawRect(track);

    if (filled > 0.0) {
        painter.setBrush(kVermilion);
        painter.drawRect(QRectF(track.topLeft(), QSizeF(filled, track.height())));

        // A restrained shine along the filled part, so a slow step never looks frozen.
        painter.save();
        painter.setClipRect(QRectF(track.topLeft(), QSizeF(filled, track.height())));
        const qreal travel = std::fmod(age_ * 0.55, 1.0);
        const qreal shineX = track.left() - 70.0 * s + travel * (filled + 140.0 * s);
        QLinearGradient shine(QPointF(shineX, 0.0), QPointF(shineX + 70.0 * s, 0.0));
        shine.setColorAt(0.0, QColor(241, 235, 223, 0));
        shine.setColorAt(0.5, QColor(241, 235, 223, 52));
        shine.setColorAt(1.0, QColor(241, 235, 223, 0));
        painter.fillRect(track, shine);
        painter.restore();
    }

    // The percentage sits above the right end of the track, in the same column.
    const QString percent = QString::number(int(std::lround(shown_ * 100.0f))) + QLatin1Char('%');
    const QFont percentFont = sized(font(), 8.0 * s, QFont::Normal, 0.8 * s);
    const qreal percentWidth = QFontMetricsF(percentFont).horizontalAdvance(percent);
    painter.setFont(percentFont);
    painter.setPen(QColor(208, 202, 190));
    painter.drawText(QPointF(track.right() - percentWidth, 442.0 * s), percent);
}

} // namespace editor
