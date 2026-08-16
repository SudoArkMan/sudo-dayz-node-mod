#include "branding.h"

#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QString>

namespace {

// Where the 2x art starts being the better source. Below it the 1x art is
// already finer than the screen, and upscaling it would only soften the mark.
constexpr qreal kHiDpiCut = 1.25;

QString variant(const QString &stem, qreal dpr)
{
    return QStringLiteral(":/brand/%1%2.png")
        .arg(stem, dpr > kHiDpiCut ? QStringLiteral("@2x") : QString());
}

} // namespace

QIcon branding::appIcon()
{
    QIcon icon;
    for (const int px : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/brand/icon-%1.png").arg(px), QSize(px, px));
    return icon;
}

QPixmap branding::splashArt(qreal dpr)
{
    const QImage file(variant(QStringLiteral("splash"), dpr));
    if (file.isNull()) return {};

    // Dropped to RGB because the art is opaque to the last pixel: the splash
    // window paints it over its whole face, and an alpha channel it never uses
    // only costs a blend on every repaint the progress bar asks for.
    QPixmap art = QPixmap::fromImage(file.convertToFormat(QImage::Format_RGB32));

    // The two files hold the same picture at two densities, so the ratio is the
    // file's own, not the screen's: stamping the screen's would shrink the 1x
    // art on a 1.5x display instead of leaving it at 760x440.
    art.setDevicePixelRatio(dpr > kHiDpiCut ? 2.0 : 1.0);
    return art;
}

QPixmap branding::cornerMark(int logicalHeight, qreal dpr)
{
    if (logicalHeight <= 0 || dpr <= 0.0) return {};
    const QPixmap src(variant(QStringLiteral("corner"), dpr));
    if (src.isNull()) return src;

    // Resampled to the exact device height the toolbar will paint, then told
    // what that ratio was, so the mark lands on whole pixels and Qt has no
    // second scale to do at paint time.
    QPixmap out = src.scaledToHeight(qRound(logicalHeight * dpr),
                                     Qt::SmoothTransformation);
    out.setDevicePixelRatio(dpr);
    return out;
}
