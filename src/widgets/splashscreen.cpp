#include "splashscreen.h"

#include "branding.h"
#include "theme.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QRect>
#include <QScreen>
#include <QTimer>

namespace {

// The art's own grid, measured off resources/brand/splash/sudo-splash-760x440.png:
// its rules run x=31 to x=735 and the lowest artwork ends at y=407. Everything
// drawn here sits inside those margins and below that line, so nothing the
// pack laid out is covered.
constexpr int kLeft = 31;
constexpr int kRight = 735;
constexpr int kRowTop = 411;
constexpr int kRowHeight = 14;
constexpr int kBarTop = 431;
constexpr int kBarHeight = 3;

// One tick's share of the distance left to the ceiling. Small enough that a
// two second stage is still visibly moving at the end of it.
constexpr qreal kEase = 0.02;
constexpr int kTickMs = 16;

} // namespace

SplashScreen::SplashScreen()
    : QSplashScreen(branding::splashArt(
          QGuiApplication::primaryScreen()
              ? QGuiApplication::primaryScreen()->devicePixelRatio()
              : 1.0))
    , m_ease(new QTimer(this))
    , m_version(QStringLiteral("v%1").arg(QCoreApplication::applicationVersion()))
{
    m_ease->setInterval(kTickMs);
    connect(m_ease, &QTimer::timeout, this, [this]() {
        const qreal step = (m_ceiling - m_shown) * kEase;
        // Below a third of a pixel the creep is invisible and repainting for it
        // only steals time from the work the bar is reporting on.
        if (step * (kRight - kLeft) < 0.3) return;
        m_shown += step;
        update();
    });
}

void SplashScreen::beginStage(const QString &text, qreal ceiling)
{
    m_stage = text;
    m_ceiling = qBound(m_shown, ceiling, 1.0);
    // Painted now rather than on the next trip through the event loop: the
    // caller's next line is usually the blocking work this names.
    repaint();
    m_ease->start();
}

void SplashScreen::endStage()
{
    m_ease->stop();
    m_shown = m_ceiling;
    repaint();
}

void SplashScreen::drawContents(QPainter *painter)
{
    painter->setRenderHint(QPainter::Antialiasing, false);

    // Monospace for the status and build strings, which is what the pack's
    // README asks for and what the art's own header row uses.
    painter->setFont(theme::monoFont(8));
    const QRect row(kLeft, kRowTop, kRight - kLeft, kRowHeight);
    painter->setPen(branding::mutedText());
    painter->drawText(row, Qt::AlignLeft | Qt::AlignVCenter, m_stage);
    painter->drawText(row, Qt::AlignRight | Qt::AlignVCenter, m_version);

    const QRect track(kLeft, kBarTop, kRight - kLeft, kBarHeight);
    QColor trackColour = branding::rule();
    trackColour.setAlpha(110);
    painter->fillRect(track, trackColour);

    const int filled = qRound(track.width() * qBound(0.0, m_shown, 1.0));
    if (filled > 0)
        painter->fillRect(QRect(track.x(), track.y(), filled, track.height()),
                          branding::accent());
}
