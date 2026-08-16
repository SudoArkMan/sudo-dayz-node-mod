// The window that stands in for the editor while it starts.
//
// The bar reports real work. Startup is three stages the caller drives in
// order, and each one names itself before it blocks, so what the user reads is
// what the process is doing rather than a countdown invented up front.
//
// Not a QSplashScreen: that class answers QEvent::Show by waiting for its own
// window to be mapped, and Qt sends that event before it maps anything, so the
// wait runs its full one second timeout every time. A second is most of this
// app's startup, and it is the second the splash is meant to be covering.
#pragma once

#include <QPixmap>
#include <QString>
#include <QWidget>

class QTimer;

class SplashScreen : public QWidget {
    Q_OBJECT
public:
    SplashScreen();

    // Names the stage that is starting and the fraction the bar may creep to
    // while it runs. A stage that reports nothing from inside itself eases
    // towards that ceiling and never arrives, so the bar keeps moving without
    // claiming to know how far through the work is.
    void beginStage(const QString &text, qreal ceiling);

    // The stage returned. The bar takes the ceiling it was given and holds
    // there until the next stage starts.
    void endStage();

    // Closes once `window` is on screen, so the desktop never shows through
    // between the two.
    void finish(QWidget *window);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap m_art;
    QTimer *m_ease;
    QString m_stage;
    QString m_version;
    qreal m_shown = 0.0;
    qreal m_ceiling = 0.0;
};
