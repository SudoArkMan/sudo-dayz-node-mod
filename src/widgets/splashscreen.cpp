#include "splashscreen.h"

#include "branding.h"
#include "theme.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QPainter>
#include <QRect>
#include <QScreen>
#include <QTimer>
#include <QWindow>

namespace {

// The art's own grid, measured off resources/brand/splash/sudo-splash-760x440.png:
// its rules run x=31 to x=735 and the lowest artwork ends at y=407. Everything
// drawn here sits inside those margins and below that line, so nothing the
// pack laid out is covered. The gaps tighten towards the edge, which keeps the
// status row off the lockup without leaving the bar floating.
constexpr int kLeft = 31;
constexpr int kRight = 735;
constexpr int kRowTop = 415;
constexpr int kRowHeight = 14;
constexpr int kBarTop = 433;
constexpr int kBarHeight = 3;

// One tick's share of the distance left to the ceiling. Tuned against the
// measured stages: a fifth of a second of work still moves the bar a third of
// the way to its mark, and two seconds of it never quite arrives.
constexpr qreal kEase = 0.04;
constexpr int kTickMs = 16;

// How long finish() will wait for the editor's window. Every platform this
// ships on has mapped it by the time show() returns, so this is only here so
// that one which defers it cannot hang startup instead.
constexpr int kMapWaitMs = 250;

qreal screenRatio()
{
    const QScreen *screen = QGuiApplication::primaryScreen();
    return screen ? screen->devicePixelRatio() : 1.0;
}

} // namespace

SplashScreen::SplashScreen()
    : QWidget(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint)
    , m_art(branding::splashArt(screenRatio()))
    , m_ease(new QTimer(this))
    , m_version(QStringLiteral("v%1").arg(QCoreApplication::applicationVersion()))
{
    // The art covers every pixel, so Qt can skip erasing the widget first.
    setAttribute(Qt::WA_OpaquePaintEvent);
    const QSize size = m_art.isNull() ? QSize(760, 440)
                                      : m_art.deviceIndependentSize().toSize();
    resize(size);
    if (const QScreen *screen = QGuiApplication::primaryScreen())
        move(screen->geometry().center() - QRect(QPoint(), size).center());

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

void SplashScreen::finish(QWidget *window)
{
    if (window) {
        if (!window->windowHandle()) window->createWinId();
        QElapsedTimer waited;
        waited.start();
        while (window->windowHandle() && !window->windowHandle()->isVisible()
               && waited.elapsed() < kMapWaitMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, kMapWaitMs);
        }
    }
    close();
}

void SplashScreen::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (m_art.isNull())
        painter.fillRect(rect(), branding::ground());
    else
        painter.drawPixmap(0, 0, m_art);

    // Monospace for the status and build strings, which is what the pack's
    // README asks for and what the art's own header row uses.
    painter.setFont(theme::monoFont(8));
    const QRect row(kLeft, kRowTop, kRight - kLeft, kRowHeight);
    painter.setPen(branding::mutedText());
    painter.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, m_stage);
    painter.drawText(row, Qt::AlignRight | Qt::AlignVCenter, m_version);

    const QRect track(kLeft, kBarTop, kRight - kLeft, kBarHeight);
    QColor trackColour = branding::rule();
    trackColour.setAlpha(110);
    painter.fillRect(track, trackColour);

    const int filled = qRound(track.width() * qBound(0.0, m_shown, 1.0));
    if (filled > 0)
        painter.fillRect(QRect(track.x(), track.y(), filled, track.height()),
                         branding::accent());
}
