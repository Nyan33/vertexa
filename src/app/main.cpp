// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — open-source 2D vector animation studio.
#include "DemoDocument.h"
#include "Editor.h"
#include "MainWindow.h"
#include "StageView.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QScreen>
#include <QSettings>
#include <QTimer>

#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    // Tablets: deliver every sample (no compression) for accurate strokes.
    QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents, false);
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    QCoreApplication::setOrganizationName("Vertexa");
    QCoreApplication::setApplicationName("Vertexa");

    std::vector<char*> args(argv, argv + argc);
#ifdef Q_OS_WIN
    // Optional WinTab driver path (like Krita): disable Windows Ink pointer input.
    static char platformFlag[] = "-platform";
    static char platformValue[] = "windows:nowmpointer";
    if (QSettings("Vertexa", "Vertexa").value("tablet/wintab", false).toBool()) {
        args.push_back(platformFlag);
        args.push_back(platformValue);
    }
#endif
    int newArgc = int(args.size());
    args.push_back(nullptr);
    QApplication app(newArgc, args.data());
    app.setApplicationVersion(VERTEXA_VERSION);
    app.setApplicationDisplayName("Vertexa");

    QCommandLineParser parser;
    parser.setApplicationDescription("Vertexa — open-source 2D vector animation studio");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption demo("demo", "Open the demo scene.");
    QCommandLineOption screenshot("screenshot", "Save a screenshot of the window to <file> and quit.", "file");
    QCommandLineOption frameOpt("frame", "Frame to show (with --screenshot).", "n", "1");
    parser.addOption(demo);
    parser.addOption(screenshot);
    parser.addOption(frameOpt);
    parser.addPositionalArgument("file", "Document to open (.vtx).");
    parser.process(app);

    vx::ui::Theme::init(app);
    vx::app::Editor editor;
    vx::app::MainWindow window(&editor);
    window.show();

    const QStringList files = parser.positionalArguments();
    if (!files.isEmpty()) window.openFile(files.front());
    else if (parser.isSet(demo)) editor.setDocument(vx::app::createDemoDocument(), {});

    if (parser.isSet(screenshot)) {
        const QString path = parser.value(screenshot);
        const int frame = std::max(1, parser.value(frameOpt).toInt()) - 1;
        window.resize(1600, 980);
        QTimer::singleShot(600, &window, [&window, &editor, path, frame]() {
            window.stage()->fitStage();
            editor.setFrame(frame);
            QTimer::singleShot(600, &window, [&window, path]() {
                window.grab().save(path);
                QApplication::quit();
            });
        });
    } else if (parser.isSet(demo) || !files.isEmpty()) {
        QTimer::singleShot(0, &window, [&window]() { window.stage()->fitStage(); });
    }
    return app.exec();
}
