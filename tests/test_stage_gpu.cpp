// SPDX-License-Identifier: GPL-3.0-or-later
// The stage shown on the GPU (StageCanvas, through RHI) must look like the
// stage drawn on the CPU: the same window is grabbed both ways and compared.
//
// Opt-in like test_gpu: VERTEXA_TEST_GPU=1 with a window system (CI runs it
// under xvfb-run with Mesa's Vulkan and OpenGL, and on Windows with Direct3D
// in software). Needs a build with RHI.
#include "TestMain.h"

#include "app/DemoDocument.h"
#include "app/Editor.h"
#include "app/StageView.h"
#include "app/Theme.h"
#include "core/VectorBrush.h"
#include "render/QtConvert.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>

#include <cstdio>

using namespace vx;
using namespace vx::app;

namespace {

bool enabled()
{
#ifndef VERTEXA_HAVE_RHI
    std::printf("  built without RHI: skipped\n");
    return false;
#else
    if (qEnvironmentVariable("VERTEXA_TEST_GPU") != QLatin1String("1")) {
        std::printf("  set VERTEXA_TEST_GPU=1 to compare the GPU stage with the CPU one\n");
        return false;
    }
    return true;
#endif
}

void settle(int ms = 300)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
}

/// Mean channel difference and the fraction of pixels differing by more than 48.
std::pair<double, double> compare(const QImage& a0, const QImage& b0)
{
    const QImage a = a0.convertToFormat(QImage::Format_RGB32), b = b0.convertToFormat(QImage::Format_RGB32);
    if (a.size() != b.size()) return {255, 1};
    long long sum = 0, bad = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto* pa = reinterpret_cast<const quint32*>(a.constScanLine(y));
        const auto* pb = reinterpret_cast<const quint32*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            int worst = 0;
            for (int s = 0; s < 24; s += 8) {
                const int v = std::abs(int((pa[x] >> s) & 0xff) - int((pb[x] >> s) & 0xff));
                sum += v;
                worst = std::max(worst, v);
            }
            bad += worst > 48;
        }
    }
    const double n = double(a.width()) * a.height();
    return {sum / (n * 3), bad / n};
}

} // namespace

VX_TEST(gpu_stage_looks_like_the_cpu_stage)
{
    if (!enabled()) return;
    Editor ed;
    ed.setDocument(createDemoDocument(), {});
    StageView view(&ed);
    view.resize(900, 600);
    view.show();
    settle();
    view.fitStage();
    for (int frame : {0, 20}) {
        ed.setFrame(frame);
        view.setGpuStage(false);
        settle();
        const QImage cpu = view.grab().toImage();
        view.setGpuStage(true);
        CHECK(view.gpuStage());
        if (!view.gpuStage()) return;
        settle();
        const QImage gpu = view.grab().toImage();
        const auto [mean, bad] = compare(cpu, gpu);
        std::printf("  frame %2d: %s  mean %.3f  edge-ish %.3f%%\n", frame, qPrintable(view.gpuStageDevice()), mean, bad * 100);
        if (!(mean < 1.5 && bad < 0.01)) {
            cpu.save(QString("stage-fail-%1-cpu.png").arg(frame));
            gpu.save(QString("stage-fail-%1-gpu.png").arg(frame));
        }
        CHECK(mean < 1.5);
        CHECK(bad < 0.01);
        // The eyedropper reads the same colours.
        for (QPointF p : {QPointF(450, 300), QPointF(300, 200), QPointF(20, 20)}) {
            view.setGpuStage(false);
            settle(50);
            const QColor a = view.colorAt(p);
            view.setGpuStage(true);
            settle(50);
            const QColor b = view.colorAt(p);
            CHECK(std::abs(a.red() - b.red()) <= 3 && std::abs(a.green() - b.green()) <= 3 && std::abs(a.blue() - b.blue()) <= 3);
        }
    }
}

VX_TEST(gpu_stage_paints_and_follows_edits)
{
    if (!enabled()) return;
    Editor ed;
    ed.setDocument(createDemoDocument(), {});
    StageView view(&ed);
    view.resize(900, 600);
    view.show();
    settle();
    view.fitStage();
    view.setGpuStage(true);
    CHECK(view.gpuStage());
    if (!view.gpuStage()) return;
    settle();
    // A textured stroke: previewed over the canvas, merged, then drawn by the GPU.
    ed.settings().paint = *builtinVectorBrush("chalk");
    ed.settings().paint.size = 30;
    ed.setTool(ToolId::PaintBrush);
    auto send = [&](QEvent::Type type, Vec2 p, Qt::MouseButtons buttons) {
        const QPointF pos = toQPoint(view.timelineToWidget().map(p));
        QMouseEvent ev(type, pos, view.mapToGlobal(pos), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
        QApplication::sendEvent(&view, &ev);
    };
    const size_t before = ed.mergeShape(ed.layerIndex()) ? ed.mergeShape(ed.layerIndex())->edges.size() : 0;
    send(QEvent::MouseButtonPress, {200, 300}, Qt::LeftButton);
    for (int i = 1; i <= 40; ++i) {
        send(QEvent::MouseMove, {200.0 + i * 20, 300 + std::sin(i * 0.3) * 40}, Qt::LeftButton);
        QApplication::processEvents();
    }
    send(QEvent::MouseButtonRelease, {1000, 300}, Qt::NoButton);
    while (view.hasPendingWork()) QApplication::processEvents(QEventLoop::AllEvents, 5);
    settle();
    const ShapeGraphPtr g = ed.mergeShape(ed.layerIndex());
    CHECK(g && g->edges.size() > before);
    const QImage gpu = view.grab().toImage();
    view.setGpuStage(false);
    settle();
    const QImage cpu = view.grab().toImage();
    const auto [mean, bad] = compare(cpu, gpu);
    std::printf("  after a stroke: mean %.3f  edge-ish %.3f%%\n", mean, bad * 100);
    CHECK(mean < 1.5);
    CHECK(bad < 0.01);
}

int main(int argc, char** argv)
{
#if !defined(Q_OS_WIN) && !defined(Q_OS_APPLE)
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
        qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    // Software Vulkan / OpenGL (Mesa in CI) is fine for checking pixels.
    qputenv("VERTEXA_GPU", "force");
    QApplication app(argc, argv);
    app.setOrganizationName("VertexaTests");
    app.setApplicationName("VertexaTests");
    vx::ui::Theme::init(app);
    return vxtest::runAll(argc, argv);
}
