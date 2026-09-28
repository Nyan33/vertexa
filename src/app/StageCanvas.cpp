// SPDX-License-Identifier: GPL-3.0-or-later
#include "StageCanvas.h"

#ifdef VERTEXA_HAVE_RHI

#include "StageView.h"

#include "render/GlRenderer.h"

#include <rhi/qrhi.h>

#include <QGuiApplication>
#include <QPainter>
#include <QSettings>
#include <QTimer>

namespace vx::app {

namespace {

QRhiWidget::Api apiFor(const QString& id)
{
    if (id == "vulkan") return QRhiWidget::Api::Vulkan;
    if (id == "metal") return QRhiWidget::Api::Metal;
    if (id == "d3d11") return QRhiWidget::Api::Direct3D11;
    if (id == "d3d12") return QRhiWidget::Api::Direct3D12;
    if (id == "opengl") return QRhiWidget::Api::OpenGL;
    return QRhiWidget::Api::Null;
}

} // namespace

QList<QPair<QString, QString>> rhiApis()
{
    QList<QPair<QString, QString>> apis;
#if defined(Q_OS_WIN)
    apis = {{"d3d11", "Direct3D 11"}, {"d3d12", "Direct3D 12"}, {"vulkan", "Vulkan"}, {"opengl", "OpenGL"}};
#elif defined(Q_OS_APPLE)
    apis = {{"metal", "Metal"}, {"opengl", "OpenGL"}};
#else
    apis = {{"vulkan", "Vulkan"}, {"opengl", "OpenGL"}};
#endif
    return apis;
}

const RhiChoice& rhiChoice()
{
    static const RhiChoice choice = [] {
        RhiChoice c;
        const QString platform = QGuiApplication::platformName();
        if (platform == "offscreen" || platform == "minimal") {
            c.why = QObject::tr("no window system");
            return c;
        }
        QString wanted = qEnvironmentVariable("VERTEXA_RHI").toLower();
        if (wanted.isEmpty()) wanted = QSettings().value("render/api", "auto").toString();
        QStringList order;
        if (wanted == "auto") {
            for (const auto& [id, label] : rhiApis()) order << id;
        } else {
            order << wanted;
        }
        const bool force = qEnvironmentVariable("VERTEXA_GPU") == QLatin1String("force");
        for (const QString& id : order) {
            QString why;
            // A device of its own, only to see what this backend offers.
            const std::unique_ptr<RhiRenderer> probe = RhiRenderer::createOffscreen(id, &why);
            if (!probe || !probe->isValid()) {
                c.why = why;
                continue;
            }
            if (probe->isSoftware() && !force) {
                c.why = QObject::tr("%1 runs on the CPU").arg(probe->deviceName());
                continue;
            }
            c.usable = true;
            c.api = apiFor(id);
            c.backend = probe->backendName();
            c.device = probe->deviceName();
            break;
        }
        return c;
    }();
    return choice;
}

// --- StageCanvas ------------------------------------------------------------------------

StageCanvas::StageCanvas(StageView* view) : QRhiWidget(view), m_view(view)
{
    setApi(rhiChoice().api);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    connect(this, &QRhiWidget::renderFailed, this, [this]() { fail(); });
}

StageCanvas::~StageCanvas() = default;

void StageCanvas::redraw()
{
    m_redraw = true;
    update();
}

QString StageCanvas::deviceName() const { return m_renderer ? m_renderer->deviceName() : rhiChoice().device; }
double StageCanvas::lastSubmitMs() const { return m_renderer ? m_renderer->lastSubmitMs() : 0.0; }

void StageCanvas::initialize(QRhiCommandBuffer*)
{
    if (!m_renderer || m_renderer->rhi() != rhi()) {
        m_renderer.reset();
        if (rhi()) m_renderer = std::make_unique<RhiRenderer>(rhi());
        if (!m_renderer || !m_renderer->isValid()) {
            fail();
            return;
        }
    }
    m_redraw = true;
}

void StageCanvas::render(QRhiCommandBuffer* cb)
{
    if (m_failed || !m_renderer) return;
    QRhiRenderTarget* rt = renderTarget();
    const QSize px = rt->pixelSize();
    m_renderer->present(cb, rt, px, [this](Surface& root) { m_view->drawFrame(root); }, m_redraw, m_view->backdrop());
    m_redraw = false;
}

void StageCanvas::releaseResources() { m_renderer.reset(); }

void StageCanvas::fail()
{
    if (m_failed) return;
    m_failed = true;
    m_renderer.reset();
    // The view draws on the CPU from now on.
    QTimer::singleShot(0, m_view, [view = m_view]() { view->setGpuStage(false); });
}

// --- StageOverlay ---------------------------------------------------------------------------

StageOverlay::StageOverlay(StageView* view) : QWidget(view), m_view(view)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
}

void StageOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    m_view->paintOverlays(p);
}

} // namespace vx::app

#endif // VERTEXA_HAVE_RHI
