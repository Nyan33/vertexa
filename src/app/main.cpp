// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — open-source 2D vector animation studio.
#include "CrashHandler.h"
#include "DemoDocument.h"
#include "Editor.h"
#include "MainWindow.h"
#include "StageView.h"
#include "Theme.h"

#include "render/GlRenderer.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDockWidget>
#include <QIcon>
#include <QScreen>
#include <QSettings>
#include <QThreadPool>
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
    app.setWindowIcon(QIcon(":/icons/vertexa-256.png"));
    // Logs, crash reports and recovery of unsaved work.
    vx::app::crash::install();
    // GPU rendering (View > GPU Rendering); VERTEXA_GPU=0 turns it off.
    if (qEnvironmentVariable("VERTEXA_GPU") != QLatin1String("0"))
        vx::GlRenderer::setEnabled(QSettings().value("render/gpu", true).toBool());
    // Lets Wayland / X11 desktops match windows with vertexa.desktop.
    QGuiApplication::setDesktopFileName("vertexa");

    QCommandLineParser parser;
    parser.setApplicationDescription("Vertexa — open-source 2D vector animation studio");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption demo("demo", "Open the demo scene.");
    QCommandLineOption screenshot("screenshot", "Save a screenshot of the window to <file> and quit.", "file");
    QCommandLineOption frameOpt("frame", "Frame to show (with --screenshot).", "n", "1");
    QCommandLineOption stateOpt("state", "UI state for --screenshot: instance, edit, brushes, light, library, filters.", "name");
    parser.addOption(stateOpt);
    parser.addOption(demo);
    parser.addOption(screenshot);
    parser.addOption(frameOpt);
    parser.addPositionalArgument("file", "Document to open (.vtx).");
    parser.process(app);

    if (parser.isSet(screenshot)) {
        // Deterministic layout for screenshots.
        QSettings s;
        s.remove("ui/state");
        s.remove("ui/geometry");
        s.setValue("ui/dark", parser.value(stateOpt) != "light");
    }
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
        const QString state = parser.value(stateOpt);
        QTimer::singleShot(600, &window, [&window, &editor, path, frame, state]() {
            window.stage()->fitStage();
            editor.setFrame(frame);
            auto raise = [&window](const char* dock) {
                if (auto* d = window.findChild<QDockWidget*>(dock)) d->raise();
            };
            auto selectLayerElement = [&editor](const char* layerName) {
                const vx::Timeline& tl = editor.timeline();
                for (int i = 0; i < int(tl.layers.size()); ++i)
                    if (tl.layers[i].name == layerName) {
                        editor.setLayerIndex(i);
                        editor.setSelection({{tl.layers[i].id, 0}});
                        return i;
                    }
                return -1;
            };
            if (state == "filters") {
                selectLayerElement("Ball");
                editor.setTool(vx::app::ToolId::Selection);
                raise("properties");
                for (const char* dock : {"color", "brushes"})
                    if (auto* d = window.findChild<QDockWidget*>(dock)) d->hide();
            } else if (state == "instance") {
                selectLayerElement("Sun");
                editor.setTool(vx::app::ToolId::FreeTransform);
            } else if (state == "edit") {
                const int li = selectLayerElement("Sun");
                if (li >= 0) editor.enterInstance(li, 0);
                editor.setTool(vx::app::ToolId::Brush);
            } else if (state == "brushes") {
                raise("brushes");
                editor.setTool(vx::app::ToolId::PaintBrush);
            } else if (state == "library") {
                raise("library");
            }
            QTimer::singleShot(600, &window, [&window, path]() {
                window.grab().save(path);
                QApplication::quit();
            });
        });
    } else if (parser.isSet(demo) || !files.isEmpty()) {
        QTimer::singleShot(0, &window, [&window]() { window.stage()->fitStage(); });
    }
    // Runs that crashed (or were killed) last time: report and recovery.
    if (!parser.isSet(screenshot)) QTimer::singleShot(300, &window, [&window]() { window.checkLastSession(); });
    // Checks the crash reports end to end: VERTEXA_CRASH_TEST=segv|abort|exception.
    if (const QString how = qEnvironmentVariable("VERTEXA_CRASH_TEST"); !how.isEmpty())
        QTimer::singleShot(1000, &window, [how]() { vx::app::crash::crashForTesting(how); });
    const int code = app.exec();
    QThreadPool::globalInstance()->waitForDone(3000); // an autosave in flight
    vx::app::crash::shutdown();
    return code;
}
