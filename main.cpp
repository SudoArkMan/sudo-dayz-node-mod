// SUDO DayZ Node Mod: visual scripting for DayZ Enforce Script.
//
// Every vanilla class, method, enum, global and constant is a node; the graph
// generates compilable Enforce Script. The catalogue is generated from the
// dayz-script-api index and loaded at startup from resources/catalog.json.
#include "document.h"
#include "mainwindow.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QTimer>

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

    Document doc;
    const QString catalogPath = parser.isSet(catalogOpt)
                                    ? parser.value(catalogOpt)
                                    : findResource(QStringLiteral("catalog.json"));
    if (catalogPath.isEmpty() || !doc.loadCatalog(catalogPath)) {
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

    MainWindow win(&doc);
    win.resize(1600, 950);
    win.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) win.openProjectPath(args.first());

    if (parser.isSet(shotOpt)) {
        const QString out = parser.value(shotOpt);
        // Let layout settle, paint once, then quit.
        QTimer::singleShot(1200, &app, [&win, out]() {
            win.grab().save(out);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
