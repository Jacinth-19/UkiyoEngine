#include "icons.h"

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QStringList>

#include "material_icons.h"

namespace editor {

namespace {
QString gIconFamily;
} // namespace

bool loadIconFont(const QString& ttfPath) {
    const int id = QFontDatabase::addApplicationFont(ttfPath);
    if (id < 0) return false;
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    if (families.isEmpty()) return false;
    gIconFamily = families.first();
    return true;
}

QString loadBrandFont(const QString& fontsDir) {
    // Four static weights rather than the variable font: Qt picks the right face directly and the rasteriser does
    // not have to synthesise a weight for 浮世.
    QString family;
    for (const char* weight : {"Regular", "Medium", "SemiBold", "Bold"}) {
        const int id = QFontDatabase::addApplicationFont(fontsDir + QStringLiteral("/IBMPlexSansJP-") +
                                                         QString::fromLatin1(weight) + QStringLiteral(".ttf"));
        if (id < 0) continue;
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty() && family.isEmpty()) family = families.first();
    }
    if (family.isEmpty()) return family;
    QFont font(family);
    font.setStyleStrategy(QFont::PreferAntialias);
    QGuiApplication::setFont(font);
    return family;
}

QIcon materialIcon(const char* name, const QColor& color, int px) {
    QIcon icon;
    const char16_t code = materialGlyph(name);
    if (!code || gIconFamily.isEmpty() || px <= 0) return icon;

    QFont font(gIconFamily);
    font.setPixelSize(px);
    const QString text = QString(QChar(code));

    QColor disabled = color;
    disabled.setAlpha(96);
    const struct { QIcon::Mode mode; QColor color; } states[] = {
        {QIcon::Normal, color},
        {QIcon::Disabled, disabled},
    };
    for (const auto& st : states) {
        QPixmap pm(px, px);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.setFont(font);
        p.setPen(st.color);
        p.drawText(pm.rect(), Qt::AlignCenter, text);
        p.end();
        icon.addPixmap(pm, st.mode);
    }
    return icon;
}

QColor iconColor() { return QColor(0xc4, 0xc4, 0xc4); }
QColor iconDimColor() { return QColor(0x8c, 0x8c, 0x8c); }
QColor folderColor() { return QColor(0xd9, 0xb3, 0x5f); }
QColor infoColor() { return QColor(0x6f, 0xa8, 0xff); }
QColor warningColor() { return QColor(0xe6, 0xc0, 0x6a); }
QColor errorColor() { return QColor(0xf0, 0x60, 0x5d); }

} // namespace editor
