// SUDO DayZ Node Mod: visual scripting for DayZ Enforce Script.
//
// Every vanilla class, method, enum, global and constant is a node; the graph
// generates compilable Enforce Script. The catalogue is generated from the
// dayz-script-api index and loaded at startup from resources/catalog.json.
#include "branding.h"
#include "document.h"
#include "mainwindow.h"
#include "theme.h"
#include "widgets/splashscreen.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <memory>
#include <utility>

namespace {

// resources/ sits next to the executable when installed, and two or three
// levels up inside a Qt Creator build tree.
QString findResource(const QString &name)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + "/resources/" + name,
        appDir + "/../resources/" + name,
        appDir + "/../../resources/" + name,
        appDir + "/../../../resources/" + name,
        appDir + "/../../../../resources/" + name,
    };
    for (const QString &c : candidates)
        if (QFileInfo::exists(c)) return QDir::cleanPath(c);
    return {};
}

// The catalogue parse and its search index are one call: 2.9 MB of JSON into
// 6,108 classes and 29,024 methods, and the one part of startup that can stall
// on a cold disk. It runs off the GUI thread so the splash keeps painting
// through it. Nothing else touches the Document until the thread has been
// joined, so the handoff is the join and there is no shared state to guard.
class CatalogLoader : public QThread {
public:
    CatalogLoader(Document *doc, QString path)
        : m_doc(doc), m_path(std::move(path)) {}
    bool ok = false;

protected:
    void run() override { ok = m_doc->loadCatalog(m_path); }

private:
    Document *m_doc;
    QString m_path;
};

bool loadCatalogue(Document &doc, const QString &path, SplashScreen *splash)
{
    if (path.isEmpty()) return false;
    if (!splash) return doc.loadCatalog(path);

    CatalogLoader loader(&doc, path);
    QEventLoop wait;
    QObject::connect(&loader, &QThread::finished, &wait, &QEventLoop::quit);
    loader.start();
    wait.exec();
    loader.wait();
    return loader.ok;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SUDO DayZ Node Mod"));
    QCoreApplication::setOrganizationName(QStringLiteral("SUDO"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Visual scripting for DayZ Enforce Script."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("project"),
                                 QStringLiteral("A .sdzn project to open."));
    QCommandLineOption catalogOpt(
        QStringLiteral("catalog"),
        QStringLiteral("Path to catalog.json (defaults to resources/catalog.json)."),
        QStringLiteral("path"));
    parser.addOption(catalogOpt);
    // Renders the window to a PNG and exits. Used to check the UI headlessly.
    QCommandLineOption shotOpt(QStringLiteral("screenshot"),
                               QStringLiteral("Save a window screenshot and exit."),
                               QStringLiteral("png"));
    parser.addOption(shotOpt);
    parser.process(app);

    theme::apply(app);
    app.setWindowIcon(branding::appIcon());

    // No splash for a screenshot run: it would be a second window in front of
    // the one being grabbed, and the shot has to work with no display at all.
    std::unique_ptr<SplashScreen> splash;
    if (!parser.isSet(shotOpt)) {
        splash = std::make_unique<SplashScreen>();
        splash->show();
    }

    Document doc;
    const QString catalogPath = parser.isSet(catalogOpt)
                                    ? parser.value(catalogOpt)
                                    : findResource(QStringLiteral("catalog.json"));
    // The ceilings are where each stage measured out on a warm start: the
    // catalogue about a fifth of the wait, the window build about half, showing
    // it the rest. A cold disk shifts weight into the first one, which is why
    // it gets more of the bar than a warm run alone would give it.
    if (splash)
        splash->beginStage(QStringLiteral("Loading node catalogue"), 0.45);
    const bool loaded = loadCatalogue(doc, catalogPath, splash.get());
    if (splash) splash->endStage();

    if (!loaded) {
        // Ahead of the box, or the splash sits over the one thing the user has
        // to read before the app gives up.
        splash.reset();
        QMessageBox::critical(
            nullptr, QStringLiteral("SUDO DayZ Node Mod"),
            QStringLiteral("Could not load the node catalogue.\n\n%1\n\nBuild it with "
                           "the dayz-script-api index and place catalog.json in "
                           "resources/.")
                .arg(catalogPath.isEmpty()
                         ? QStringLiteral("resources/catalog.json was not found.")
                         : doc.catalog().error()));
        return 1;
    }
    //splash to be used for future loading staging/garbage collection.
    if (splash)
        splash->beginStage(QStringLiteral("Building the editor window"), 0.85);
    MainWindow win(&doc);
    win.resize(1600, 950);
    if (splash) splash->endStage();

    const QStringList args = parser.positionalArguments();
    if (splash)
        splash->beginStage(args.isEmpty() ? QStringLiteral("Opening the editor")
                                          : QStringLiteral("Restoring project"),
                           1.0);
    if (!args.isEmpty()) win.openProjectPath(args.first());
    win.show();
    if (splash) {
        splash->endStage();
        splash->finish(&win);
    }

    if (parser.isSet(shotOpt)) {
        const QString out = parser.value(shotOpt);
        // Let layout settle, paint once, then leave. Not quit(): that closes the
        // window, the close handler asks whether to save, and a modal box with
        // nobody at the keyboard is a run that never ends. A screenshot changes
        // nothing on disk, so there is nothing here worth asking about.
        QTimer::singleShot(1200, &app, [&win, out]() {
            QPixmap shot = win.grab();
            // A menu, a combo popup and a tooltip are all top level windows of
            // their own, so grab() on the main window paints straight over the
            // place they sit. Painted back in at the offset they hold on screen,
            // or a picture of an open menu is a picture without one.
            QPainter into(&shot);
            const QPoint origin = win.mapToGlobal(QPoint(0, 0));
            for (QWidget *top : QApplication::topLevelWidgets()) {
                if (top == &win || !top->isVisible() || !top->isWindow()) continue;
                if (!qobject_cast<QMenu *>(top)) continue;
                into.drawPixmap(top->mapToGlobal(QPoint(0, 0)) - origin, top->grab());
            }
            into.end();
            const bool saved = shot.save(out);
            std::_Exit(saved ? 0 : 1);
        });
    }

    return app.exec();
}
