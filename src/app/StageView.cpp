// SPDX-License-Identifier: GPL-3.0-or-later
#include "StageView.h"
#include "Icons.h"
#include "Theme.h"
#include "tools/BasicTools.h"
#include "tools/DrawTools.h"
#include "tools/SelectTools.h"

#include "core/Evaluate.h"
#include "StageCanvas.h"

#include "render/Blend.h"
#include "render/GlRenderer.h"
#include "render/QtConvert.h"
#include "render/Renderer.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <cmath>

namespace vx::app {

// --- Tool helpers -----------------------------------------------------------------------

QPointF Tool::toWidget(Vec2 p) const { return toQPoint(view->timelineToWidget().map(p)); }
double Tool::unitsPerPixel() const { return view->unitsPerPixel(); }
void Tool::update() const { view->update(); }
void Tool::update(const QRectF& r) const
{
    if (!r.isEmpty()) view->update(r.toAlignedRect().adjusted(-2, -2, 2, 2));
}

// --- StageView -----------------------------------------------------------------------------

StageView::StageView(Editor* editor, QWidget* parent) : QWidget(parent), m_ed(editor)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_TabletTracking);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMinimumSize(200, 150);
    m_clock.start();

    m_tools[ToolId::Selection] = std::make_unique<SelectionTool>(m_ed, this);
    m_tools[ToolId::Subselection] = std::make_unique<SubselectTool>(m_ed, this);
    m_tools[ToolId::FreeTransform] = std::make_unique<FreeTransformTool>(m_ed, this);
    m_tools[ToolId::Lasso] = std::make_unique<LassoTool>(m_ed, this);
    m_tools[ToolId::Pen] = std::make_unique<PenTool>(m_ed, this);
    for (ToolId id : {ToolId::Line, ToolId::Rectangle, ToolId::Oval, ToolId::PolyStar})
        m_tools[id] = std::make_unique<ShapeDragTool>(m_ed, this, id);
    m_tools[ToolId::Pencil] = std::make_unique<PencilTool>(m_ed, this);
    m_tools[ToolId::Brush] = std::make_unique<BrushTool>(m_ed, this);
    m_tools[ToolId::PaintBrush] = std::make_unique<PaintBrushTool>(m_ed, this);
    m_tools[ToolId::Eraser] = std::make_unique<EraserTool>(m_ed, this);
    m_tools[ToolId::PaintBucket] = std::make_unique<PaintBucketTool>(m_ed, this);
    m_tools[ToolId::InkBottle] = std::make_unique<InkBottleTool>(m_ed, this);
    m_tools[ToolId::Eyedropper] = std::make_unique<EyedropperTool>(m_ed, this);
    m_tools[ToolId::Hand] = std::make_unique<HandTool>(m_ed, this);
    m_tools[ToolId::Zoom] = std::make_unique<ZoomTool>(m_ed, this);
    m_active = m_tools[m_ed->tool()].get();

    m_zoomAnim = new QVariantAnimation(this);
    m_zoomAnim->setDuration(170);
    m_zoomAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_zoomAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        const double z = v.toDouble();
        const QPointF stagePt = (m_zoomAnchor - m_pan) / m_zoom;
        m_pan = m_zoomAnchor - stagePt * z;
        m_zoom = z;
        invalidate();
        emit zoomChanged(m_zoom);
    });

    connect(m_ed, &Editor::documentChanged, this, &StageView::invalidate);
    connect(m_ed, &Editor::previewChanged, this, &StageView::invalidate);
    connect(m_ed, &Editor::frameChanged, this, &StageView::invalidate);
    connect(m_ed, &Editor::contextChanged, this, &StageView::invalidate);
    connect(m_ed, &Editor::onionChanged, this, &StageView::invalidate);
    connect(m_ed, &Editor::simpleButtonsChanged, this, [this](bool on) {
        if (!on) {
            m_hotButton = nullptr;
            m_buttonDown = false;
        }
        invalidate();
        refreshCursor();
    });
    // Element identities change with the document: forget the hot button.
    connect(m_ed, &Editor::documentChanged, this, [this]() { m_hotButton = nullptr; });
    connect(m_ed, &Editor::contextChanged, this, [this]() { m_hotButton = nullptr; });
    connect(m_ed, &Editor::selectionChanged, this, qOverload<>(&StageView::update));
    connect(m_ed, &Editor::settingsChanged, this, qOverload<>(&StageView::update));
    connect(m_ed, &Editor::layerChanged, this, qOverload<>(&StageView::update));
    connect(m_ed, &Editor::toolChanged, this, &StageView::activateTool);
    connect(ui::Theme::instance(), &ui::Theme::changed, this, &StageView::invalidate);
    setGpuStage(GlRenderer::enabled());
}

StageView::~StageView() = default;

Tool* StageView::toolFor(ToolId id) const
{
    auto it = m_tools.find(id);
    return it == m_tools.end() ? nullptr : it->second.get();
}

void StageView::activateTool(ToolId id)
{
    Tool* next = toolFor(id);
    if (!next || next == m_active) return;
    if (m_active) m_active->deactivate();
    m_ed->setPreview(std::nullopt);
    m_active = next;
    m_active->activate();
    refreshCursor();
    update();
}

Affine StageView::stageToWidget() const { return Affine::translate(m_pan.x(), m_pan.y()) * Affine::scale(m_zoom); }
Affine StageView::timelineToWidget() const { return stageToWidget() * m_ed->contextMatrix(); }
Vec2 StageView::widgetToTimeline(QPointF p) const { return timelineToWidget().inverted().map(fromQPoint(p)); }
double StageView::unitsPerPixel() const { return 1.0 / std::max(1e-9, m_zoom * m_ed->contextMatrix().meanScale()); }

void StageView::setZoom(double z, QPointF anchor, bool animated)
{
    z = std::clamp(z, 0.02, 64.0);
    m_zoomAnim->stop();
    if (!animated) {
        const QPointF stagePt = (anchor - m_pan) / m_zoom;
        m_pan = anchor - stagePt * z;
        m_zoom = z;
        invalidate();
        emit zoomChanged(m_zoom);
        return;
    }
    m_zoomAnchor = anchor;
    m_zoomAnim->setStartValue(m_zoom);
    m_zoomAnim->setEndValue(z);
    m_zoomAnim->start();
}

void StageView::zoomBy(double factor) { setZoom(m_zoom * factor, rect().center()); }
void StageView::zoomTo100() { setZoom(1.0, rect().center()); }

void StageView::fitStage()
{
    const Document& d = m_ed->doc();
    const double z = std::max(0.02, std::min((width() - 80.0) / d.width, (height() - 80.0) / d.height));
    m_zoomAnim->stop();
    m_zoom = z;
    m_pan = QPointF((width() - d.width * z) / 2.0, (height() - d.height * z) / 2.0);
    invalidate();
    emit zoomChanged(m_zoom);
}

void StageView::centerStage()
{
    const Document& d = m_ed->doc();
    m_pan = QPointF((width() - d.width * m_zoom) / 2.0, (height() - d.height * m_zoom) / 2.0);
    invalidate();
}

void StageView::invalidate()
{
    m_cacheValid = false;
#ifdef VERTEXA_HAVE_RHI
    if (m_gpuStage) static_cast<StageCanvas*>(m_canvas)->redraw();
#endif
    update();
}

void StageView::refreshCursor()
{
    if (m_panning) setCursor(Qt::ClosedHandCursor);
    else if (m_spaceDown) setCursor(Qt::OpenHandCursor);
    else if (m_hotButton) setCursor(Qt::PointingHandCursor);
    else if (Tool* t = strokeTool()) setCursor(t->cursor());
}

QCursor StageView::toolCursor(const QString& kind) const
{
    static QHash<QString, QCursor> cache;
    auto it = cache.find(kind);
    if (it != cache.end()) return *it;
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath arrow;
    arrow.moveTo(2, 2);
    arrow.lineTo(2, 18);
    arrow.lineTo(6.5, 14);
    arrow.lineTo(9.5, 20.5);
    arrow.lineTo(12, 19.5);
    arrow.lineTo(9.2, 13.2);
    arrow.lineTo(15, 13);
    arrow.closeSubpath();
    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(Qt::black);
    p.drawPath(arrow);
    p.setPen(QPen(Qt::black, 3.2));
    p.setBrush(Qt::NoBrush);
    QPainterPath badge;
    if (kind == "bend") {
        badge.moveTo(16, 27);
        badge.quadTo(22, 17, 28, 27);
    } else if (kind == "corner") {
        badge.moveTo(16, 28);
        badge.lineTo(22, 20);
        badge.lineTo(28, 28);
    } else {
        badge.arcMoveTo(QRectF(15, 15, 13, 13), 60);
        badge.arcTo(QRectF(15, 15, 13, 13), 60, 270);
    }
    p.drawPath(badge);
    p.setPen(QPen(Qt::white, 1.4));
    p.drawPath(badge);
    p.end();
    QCursor c(pm, 2, 2);
    cache.insert(kind, c);
    return c;
}

QColor StageView::colorAt(QPointF w) const
{
    const qreal dpr = devicePixelRatioF();
    const QPoint px(int(w.x() * dpr), int(w.y() * dpr));
    QRgb c = 0;
    if (m_gpuStage) {
        // The frame is on the GPU: draw the one pixel on the CPU.
        if (!rect().contains(w.toPoint())) return {};
        QImage one(1, 1, QImage::Format_ARGB32_Premultiplied);
        one.fill(0);
        const Document& d = m_ed->displayDoc();
        Renderer(d, frameOptions()).render(one, m_ed->timelineOf(d), m_ed->frame(),
                                           Affine::translate(-px.x(), -px.y()) * deviceView() * m_ed->contextMatrix());
        c = one.pixel(0, 0); // unpremultiplied by QImage::pixel
    } else {
        if (m_cache.isNull() || !m_cache.rect().contains(px)) return {};
        c = m_cache.pixel(px);
    }
    const Document& d = m_ed->doc();
    const QRectF stage = toQTransform(stageToWidget()).mapRect(QRectF(0, 0, d.width, d.height));
    const QColor bg = stage.contains(w) ? toQColor(d.background) : ui::Theme::p().bg0;
    const double a = qAlpha(c) / 255.0;
    return QColor::fromRgbF(float(qRed(c) / 255.0 * a + bg.redF() * (1 - a)), float(qGreen(c) / 255.0 * a + bg.greenF() * (1 - a)),
                            float(qBlue(c) / 255.0 * a + bg.blueF() * (1 - a)));
}

Affine StageView::deviceView() const { return Affine::scale(devicePixelRatioF()) * stageToWidget(); }

RenderOptions StageView::frameOptions() const
{
    RenderOptions o;
    o.clipFrame = m_ed->isPlaying() ? m_ed->frame() : 0;
    o.hotButton = m_hotButton;
    o.hotState = m_buttonDown ? ButtonState::Down : ButtonState::Over;
    return o;
}

std::vector<StageView::Underlay> StageView::underlays(const Document& d) const
{
    std::vector<Underlay> out;
    const Affine devView = deviceView();
    const Timeline& tl = m_ed->timelineOf(d);
    const Affine ctx = devView * m_ed->contextMatrix();
    const ui::Palette& pal = ui::Theme::p();
    if (m_ed->inSymbol()) {
        const auto path = m_ed->focusPath();
        if (!path.empty()) {
            RenderOptions o;
            o.focusPath = path;
            out.push_back({&d.scenes[std::clamp(m_ed->scene(), 0, int(d.scenes.size()) - 1)], m_ed->rootFrame(), devView, {}, o, 0.3});
        }
    }
    if (m_ed->onionSkin && !m_ed->isPlaying()) {
        auto onion = [&](int f, double alpha, const QColor& tint) {
            if (f < 0 || f >= tl.frameCount()) return;
            RenderOptions o;
            ColorTransform ct;
            if (m_ed->onionOutline) {
                o.forceOutline = true;
                o.outlineColor = tint;
            } else {
                ct.rm = ct.gm = ct.bm = 0.35;
                ct.ro = tint.red() * 0.65;
                ct.go = tint.green() * 0.65;
                ct.bo = tint.blue() * 0.65;
            }
            out.push_back({&tl, f, ctx, ct, o, alpha});
        };
        for (int k = m_ed->onionBefore; k >= 1; --k)
            onion(m_ed->frame() - k, 0.55 * (1.0 - double(k - 1) / std::max(1, m_ed->onionBefore)), pal.selection);
        for (int k = m_ed->onionAfter; k >= 1; --k)
            onion(m_ed->frame() + k, 0.55 * (1.0 - double(k - 1) / std::max(1, m_ed->onionAfter)), pal.mint);
    }
    return out;
}

void StageView::drawFrame(Surface& target)
{
    const Document& d = m_ed->displayDoc();
    for (const Underlay& u : underlays(d)) {
        std::unique_ptr<Surface> layer = target.makeLayer(target.size());
        Renderer(d, u.opts).render(*layer, *u.timeline, u.frame, u.view, u.ct);
        target.composite(*layer, QPoint(0, 0), BlendMode::Normal, u.alpha);
    }
    Renderer(d, frameOptions()).render(target, m_ed->timelineOf(d), m_ed->frame(), deviceView() * m_ed->contextMatrix());
}

void StageView::renderCache()
{
    const qreal dpr = devicePixelRatioF();
    const QSize sz(std::max(1, int(width() * dpr)), std::max(1, int(height() * dpr)));
    if (m_cache.size() != sz) m_cache = QImage(sz, QImage::Format_ARGB32_Premultiplied);
    m_cache.setDevicePixelRatio(1.0);
    m_cache.fill(0);
    const Document& d = m_ed->displayDoc();
    const Timeline& tl = m_ed->timelineOf(d);
    const std::vector<Underlay> under = underlays(d);
    for (const Underlay& u : under) {
        QImage buf(sz, QImage::Format_ARGB32_Premultiplied);
        buf.fill(0);
        renderAccelerated(buf, d, *u.timeline, u.frame, u.view, u.ct, u.opts, true);
        compositeImage(m_cache, buf, QPoint(0, 0), BlendMode::Normal, u.alpha);
    }
    QString context = QString::number(m_ed->scene());
    for (const ContextEntry& c : m_ed->contextStack()) context += '/' + QString::fromStdString(c.symbolId);
    m_layers.render(m_cache, d, tl, m_ed->frame(), deviceView() * m_ed->contextMatrix(), frameOptions(), context, under.empty());
    m_cache.setDevicePixelRatio(dpr);
    composeView();
    m_cacheValid = true;
}

void StageView::composeView()
{
    // Everything under the tool overlays in one opaque image: painting the
    // stage (on every pointer move while a tool shows feedback) is then a
    // plain copy. The backdrop (margin, drop shadow, stage colour) is kept
    // until the view or the stage changes.
    const qreal dpr = devicePixelRatioF();
    const ui::Palette& pal = ui::Theme::p();
    const Document& d = m_ed->displayDoc();
    const QRectF stage = toQTransform(stageToWidget()).mapRect(QRectF(0, 0, d.width, d.height));
    const QColor margin = pal.dark ? ui::mix(pal.bg0, pal.bg1, 0.35) : ui::mix(pal.bg0, pal.bg1, 0.2);
    const QColor paper = toQColor(d.background);
    if (m_backdrop.size() != m_cache.size() || m_backdropStage != stage || m_backdropColors != qMakePair(margin.rgba(), paper.rgba()) ||
        m_backdropDark != pal.dark) {
        m_backdrop = QImage(m_cache.size(), QImage::Format_RGB32);
        m_backdrop.setDevicePixelRatio(dpr);
        QPainter p(&m_backdrop);
        p.fillRect(QRectF(QPointF(0, 0), QSizeF(width(), height())), margin);
        // Soft drop shadow under the stage.
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, pal.dark ? 16 : 8));
        for (int i = 6; i >= 1; --i) p.drawRoundedRect(stage.adjusted(-i, -i + 3, i, i + 3), i, i);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(stage, paper);
        p.end();
        m_backdropStage = stage;
        m_backdropColors = qMakePair(margin.rgba(), paper.rgba());
        m_backdropDark = pal.dark;
    }
    m_view = m_backdrop.copy();
    m_view.setDevicePixelRatio(dpr);
    compositeImage(m_view, m_cache, QPoint(0, 0), BlendMode::Normal, 1.0);
}

void StageView::drawSelection(QPainter& p)
{
    if (m_ed->hasPreview() && m_active && m_active->busy()) return;
    const ui::Palette& pal = ui::Theme::p();
    const Affine T = timelineToWidget();
    const QTransform qt = toQTransform(T);
    const Document& d = m_ed->doc();
    const Timeline& tl = m_ed->timeline();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    // Several things selected: a box around all of them (widget space).
    QRectF all;
    int count = 0;
    for (const ElementRef& r : m_ed->selection()) {
        const Layer* l = tl.layerById(r.layerId);
        const Keyframe* k = l ? l->keyAt(m_ed->frame()) : nullptr;
        const ElementPtr e = m_ed->shownElement(r);
        if (!k || !e) continue;
        ++count;
        all |= qt.mapRect(toQRect(elementBounds(d, *e, m_ed->frame() - k->start)));
        if (const ShapeElement* s = asShape(e); s && !s->isObject) {
            const QPainterPath fills = qt.map(toQPath(s->graph->fillRegion(0)));
            QBrush hatch(ui::withAlpha(pal.selection, 200), Qt::Dense6Pattern);
            p.fillPath(fills, hatch);
            QPainterPath strokes;
            for (const auto& ch : strokeChains(*s->graph)) appendChain(strokes, ch, false);
            p.setPen(QPen(pal.selection, 2.0, Qt::DotLine));
            p.setBrush(Qt::NoBrush);
            p.drawPath(qt.map(strokes));
            continue;
        }
        const Rect lb = localBoundsOf(d, *e, m_ed->frame() - k->start);
        if (lb.isEmpty()) continue;
        const Affine m = T * e->matrix;
        QPolygonF poly;
        for (Vec2 c : {Vec2{lb.x0, lb.y0}, Vec2{lb.x1, lb.y0}, Vec2{lb.x1, lb.y1}, Vec2{lb.x0, lb.y1}}) poly << toQPoint(m.map(c));
        p.setPen(QPen(pal.selection, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(poly);
        if (e->type() == ElementType::Instance) {
            const QPointF reg = toQPoint(m.map({0, 0}));
            p.setPen(QPen(pal.text, 1.0));
            p.drawLine(reg + QPointF(-5, 0), reg + QPointF(5, 0));
            p.drawLine(reg + QPointF(0, -5), reg + QPointF(0, 5));
            const QPointF piv = toQPoint(m.map(e->pivot));
            p.setPen(QPen(pal.selection, 1.2));
            p.setBrush(QColor(255, 255, 255));
            p.drawEllipse(piv, 3.5, 3.5);
        }
    }
    const ShapePick& pick = m_ed->shapePick();
    if (pick.valid()) {
        ShapeGraph rest, lifted;
        if (pick.region) cutByRegion(*pick.graph, *pick.region, rest, lifted);
        else liftSelection(*pick.graph, pick.sel, rest, lifted);
        const QPainterPath fills = qt.map(toQPath(lifted.fillRegion(0)));
        p.fillPath(fills, QBrush(ui::withAlpha(pal.selection, 210), Qt::Dense6Pattern));
        QPainterPath strokes;
        for (const auto& ch : strokeChains(lifted)) appendChain(strokes, ch, false);
        p.setPen(QPen(pal.selection, 2.0, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawPath(qt.map(strokes));
        ++count;
        all |= qt.mapRect(toQRect(lifted.bounds(true)));
    }
    // Free Transform draws its own box around the selection.
    if (count > 1 && !all.isEmpty() && m_ed->tool() != ToolId::FreeTransform) {
        p.setPen(QPen(ui::withAlpha(pal.selection, 170), 1.0, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(all.adjusted(-3, -3, 3, 3));
    }
    p.restore();
}

void StageView::paintEvent(QPaintEvent*)
{
    // On the GPU the canvas and the overlay above it paint everything.
    if (m_gpuStage) return;
    if (!m_cacheValid) renderCache();
    QPainter p(this);
    p.drawImage(QPointF(0, 0), m_view);
    paintOverlays(p);
}

void StageView::paintOverlays(QPainter& p)
{
    const ui::Palette& pal = ui::Theme::p();
    if (m_ed->inSymbol()) {
        // Registration point of the symbol being edited.
        const QPointF reg = toQPoint(timelineToWidget().map({0, 0}));
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(pal.text, 1.2));
        p.drawLine(reg + QPointF(-7, 0), reg + QPointF(7, 0));
        p.drawLine(reg + QPointF(0, -7), reg + QPointF(0, 7));
        p.restore();
    }
    drawSliceGuides(p);
    drawSelection(p);
    if (m_active) m_active->paint(p);
    if (m_strokeTool && m_strokeTool != m_active) m_strokeTool->paint(p);
}

void StageView::setGpuStage(bool on)
{
#ifdef VERTEXA_HAVE_RHI
    if (on && !rhiChoice().usable) on = false;
    if (on && !m_canvas) {
        m_canvas = new StageCanvas(this);
        m_overlay = new StageOverlay(this);
    }
    if (on && static_cast<StageCanvas*>(m_canvas)->failed()) on = false;
    if (m_canvas) {
        m_canvas->setGeometry(rect());
        m_overlay->setGeometry(rect());
        m_canvas->setVisible(on);
        m_overlay->setVisible(on);
        m_overlay->raise();
    }
    m_gpuStage = on;
#else
    (void)on;
#endif
    invalidate();
}

QString StageView::gpuStageDevice() const
{
#ifdef VERTEXA_HAVE_RHI
    if (m_gpuStage && m_canvas) return static_cast<StageCanvas*>(m_canvas)->deviceName();
#endif
    return {};
}

#ifdef VERTEXA_HAVE_RHI
RhiRenderer::Backdrop StageView::backdrop() const
{
    const ui::Palette& pal = ui::Theme::p();
    const Document& d = m_ed->displayDoc();
    const qreal dpr = devicePixelRatioF();
    RhiRenderer::Backdrop b;
    const QRectF stage = toQTransform(stageToWidget()).mapRect(QRectF(0, 0, d.width, d.height));
    b.stage = QRectF(stage.topLeft() * dpr, stage.size() * dpr);
    b.margin = pal.dark ? ui::mix(pal.bg0, pal.bg1, 0.35) : ui::mix(pal.bg0, pal.bg1, 0.2);
    b.paper = toQColor(d.background);
    b.shadowAlpha = (pal.dark ? 16 : 8) / 255.0;
    b.dpr = dpr;
    return b;
}
#endif

void StageView::resizeEvent(QResizeEvent*)
{
    if (m_canvas) {
        m_canvas->setGeometry(rect());
        m_overlay->setGeometry(rect());
    }
    if (!m_placed && width() > 100 && height() > 100) {
        m_placed = true;
        fitStage();
    }
    invalidate();
}

ToolEvent StageView::makeEvent(QPointF w, Qt::KeyboardModifiers mods) const
{
    ToolEvent e;
    e.widget = w;
    e.pos = widgetToTimeline(w);
    e.mods = mods;
    e.time = m_clock.elapsed() / 1000.0;
    return e;
}

void StageView::mousePressEvent(QMouseEvent* ev)
{
    setFocus();
    if (m_tabletDown) return;
    const bool panButton = ev->button() == Qt::MiddleButton ||
                           (ev->button() == Qt::LeftButton && (m_spaceDown || m_ed->tool() == ToolId::Hand));
    if (panButton) {
        m_panning = true;
        m_panAnchor = ev->position();
        m_panStart = m_pan;
        refreshCursor();
        return;
    }
    if (ev->button() != Qt::LeftButton) return;
    ToolEvent te = makeEvent(ev->position(), ev->modifiers());
    te.button = ev->button();
    te.buttons = ev->buttons();
    if (const int guide = guideAt(te.pos); guide >= 0) {
        m_guideDrag = guide;
        m_guideGrid = *sliceSymbol()->scale9;
        update();
        return;
    }
    if (updateHotButton(te.pos)) {
        // An enabled button takes the click instead of the tool.
        m_buttonDown = true;
        invalidate();
        return;
    }
    if (Tool* t = strokeTool()) t->press(te);
}

void StageView::contextMenuEvent(QContextMenuEvent* ev)
{
    if (m_panning) return;
    if (Tool* t = strokeTool(); t && t->busy()) return;
    selectUnder(m_ed, widgetToTimeline(ev->pos()), 4.0 * unitsPerPixel());
    emit contextMenuRequested(ev->globalPos());
}

bool StageView::hasPendingWork() const
{
    for (const auto& [id, tool] : m_tools)
        if (tool->hasPendingWork()) return true;
    return false;
}

const Symbol* StageView::sliceSymbol() const
{
    if (!m_ed->inSymbol()) return nullptr;
    const Symbol* s = m_ed->doc().symbol(m_ed->contextStack().back().symbolId);
    return s && s->scale9 ? s : nullptr;
}

int StageView::guideAt(Vec2 pos) const
{
    const Symbol* s = sliceSymbol();
    const ToolId t = m_ed->tool();
    if (!s || (t != ToolId::Selection && t != ToolId::Subselection && t != ToolId::FreeTransform)) return -1;
    const Rect& g = *s->scale9;
    const double tol = 4.0 * unitsPerPixel();
    const double d[4] = {std::abs(pos.x - g.x0), std::abs(pos.x - g.x1), std::abs(pos.y - g.y0), std::abs(pos.y - g.y1)};
    int best = -1;
    for (int i = 0; i < 4; ++i)
        if (d[i] <= tol && (best < 0 || d[i] < d[best])) best = i;
    return best;
}

void StageView::drawSliceGuides(QPainter& p)
{
    const Symbol* s = sliceSymbol();
    if (!s) return;
    const Rect g = m_guideDrag >= 0 ? m_guideGrid : *s->scale9;
    const Affine T = timelineToWidget();
    const double far = 1e5;
    const ui::Palette& pal = ui::Theme::p();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QLineF lines[4] = {{toQPoint(T.map({g.x0, -far})), toQPoint(T.map({g.x0, far}))},
                             {toQPoint(T.map({g.x1, -far})), toQPoint(T.map({g.x1, far}))},
                             {toQPoint(T.map({-far, g.y0})), toQPoint(T.map({far, g.y0}))},
                             {toQPoint(T.map({-far, g.y1})), toQPoint(T.map({far, g.y1}))}};
    for (int i = 0; i < 4; ++i) {
        const bool hot = i == m_guideDrag || (m_guideDrag < 0 && i == m_guideHover);
        p.setPen(QPen(hot ? pal.accent : pal.selection, hot ? 1.6 : 1.0, Qt::DashLine));
        p.drawLine(lines[i]);
    }
    p.restore();
}

bool StageView::updateHotButton(Vec2 pos)
{
    const Element* hot = nullptr;
    const Tool* t = m_active;
    if (m_ed->simpleButtons() && !m_panning && !(t && t->busy()))
        hot = buttonAt(m_ed->doc(), m_ed->timeline(), m_ed->frame(), pos, 2.0 * unitsPerPixel(), m_ed->isPlaying() ? m_ed->frame() : 0);
    if (hot != m_hotButton) {
        m_hotButton = hot;
        if (!hot) m_buttonDown = false;
        invalidate();
        refreshCursor();
    }
    return hot != nullptr;
}

void StageView::mouseMoveEvent(QMouseEvent* ev)
{
    m_lastWidget = ev->position();
    m_hasPointer = true;
    if (m_panning) {
        m_pan = m_panStart + (ev->position() - m_panAnchor);
        invalidate();
        return;
    }
    if (m_tabletDown) return;
    ToolEvent te = makeEvent(ev->position(), ev->modifiers());
    te.buttons = ev->buttons();
    emit pointerMoved(te.pos.x, te.pos.y);
    if (m_guideDrag >= 0) {
        // Keep the guides in order with a sliver between them.
        const double gap = unitsPerPixel();
        Rect& g = m_guideGrid;
        switch (m_guideDrag) {
        case 0: g.x0 = std::min(te.pos.x, g.x1 - gap); break;
        case 1: g.x1 = std::max(te.pos.x, g.x0 + gap); break;
        case 2: g.y0 = std::min(te.pos.y, g.y1 - gap); break;
        case 3: g.y1 = std::max(te.pos.y, g.y0 + gap); break;
        }
        update();
        return;
    }
    if (const int hover = (ev->buttons() & Qt::LeftButton) ? -1 : guideAt(te.pos); hover != m_guideHover) {
        m_guideHover = hover;
        update();
    }
    if (m_guideHover >= 0) {
        setCursor(m_guideHover < 2 ? Qt::SplitHCursor : Qt::SplitVCursor);
        if (Tool* t = strokeTool()) t->hover(te);
        return;
    }
    if (updateHotButton(te.pos) || m_buttonDown) {
        refreshCursor();
        return;
    }
    if (Tool* t = strokeTool()) {
        if (ev->buttons() & Qt::LeftButton) t->move(te);
        else t->hover(te);
    }
    refreshCursor();
}

void StageView::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_panning && (ev->button() == Qt::MiddleButton || ev->button() == Qt::LeftButton)) {
        m_panning = false;
        refreshCursor();
        return;
    }
    if (m_tabletDown || ev->button() != Qt::LeftButton) return;
    ToolEvent te = makeEvent(ev->position(), ev->modifiers());
    te.button = ev->button();
    if (m_guideDrag >= 0) {
        m_guideDrag = -1;
        if (m_ed->inSymbol()) m_ed->setSymbolScale9(m_ed->contextStack().back().symbolId, m_guideGrid);
        update();
        return;
    }
    if (m_buttonDown) {
        m_buttonDown = false;
        invalidate();
        return;
    }
    if (Tool* t = strokeTool()) t->release(te);
}

void StageView::mouseDoubleClickEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    ToolEvent te = makeEvent(ev->position(), ev->modifiers());
    if (Tool* t = strokeTool()) t->doubleClick(te);
}

void StageView::tabletEvent(QTabletEvent* ev)
{
    ev->accept();
    ToolEvent te = makeEvent(ev->position(), ev->modifiers());
    te.tablet = true;
    te.rawPressure = ev->pressure();
    te.pressure = m_ed->settings().pressureCurve.eval(ev->pressure());
    te.tiltX = ev->xTilt();
    te.tiltY = ev->yTilt();
    te.rotation = ev->rotation();
    te.tangential = ev->tangentialPressure();
    te.eraserTip = ev->pointerType() == QPointingDevice::PointerType::Eraser;
    te.button = ev->button();
    te.buttons = ev->buttons();
    switch (ev->type()) {
    case QEvent::TabletPress: {
        setFocus();
        m_tabletActive = true;
        if (m_spaceDown || m_ed->tool() == ToolId::Hand || ev->button() == Qt::MiddleButton) {
            m_panning = true;
            m_panAnchor = ev->position();
            m_panStart = m_pan;
            refreshCursor();
            return;
        }
        m_tabletDown = true;
        m_strokeTool = (te.eraserTip && m_ed->settings().eraserTipSwitches) ? toolFor(ToolId::Eraser) : nullptr;
        refreshCursor();
        strokeTool()->press(te);
        break;
    }
    case QEvent::TabletMove:
        m_lastWidget = ev->position();
        m_hasPointer = true;
        if (m_panning) {
            m_pan = m_panStart + (ev->position() - m_panAnchor);
            invalidate();
            return;
        }
        emit pointerMoved(te.pos.x, te.pos.y);
        if (m_tabletDown) strokeTool()->move(te);
        else if (Tool* t = (te.eraserTip && m_ed->settings().eraserTipSwitches) ? toolFor(ToolId::Eraser) : m_active) t->hover(te);
        break;
    case QEvent::TabletRelease:
        if (m_panning) {
            m_panning = false;
            refreshCursor();
        } else if (m_tabletDown) {
            strokeTool()->release(te);
        }
        m_tabletDown = false;
        m_tabletActive = false;
        m_strokeTool = nullptr;
        refreshCursor();
        break;
    default: break;
    }
}

void StageView::wheelEvent(QWheelEvent* ev)
{
    if (ev->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
        const double steps = ev->angleDelta().y() / 120.0;
        setZoom(m_zoom * std::pow(1.2, steps), ev->position(), false);
        ev->accept();
        return;
    }
    QPointF delta = ev->pixelDelta().isNull() ? QPointF(ev->angleDelta()) / 2.0 : QPointF(ev->pixelDelta());
    if (ev->modifiers() & Qt::ShiftModifier) delta = QPointF(delta.y(), delta.x());
    m_pan += delta;
    invalidate();
    ev->accept();
}

bool StageView::event(QEvent* e)
{
    if (e->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            setZoom(m_zoom * (1.0 + g->value()), g->position(), false);
            return true;
        }
    }
    return QWidget::event(e);
}

void StageView::keyPressEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Space && !ev->isAutoRepeat()) {
        m_spaceDown = true;
        refreshCursor();
        return;
    }
    if (m_active && m_active->keyPress(ev)) return;
    if (ev->key() == Qt::Key_Escape) {
        if (m_active) m_active->cancel();
        m_ed->setPreview(std::nullopt);
        m_ed->clearSelection();
        update();
        return;
    }
    QWidget::keyPressEvent(ev);
}

void StageView::keyReleaseEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Space && !ev->isAutoRepeat()) {
        m_spaceDown = false;
        refreshCursor();
        return;
    }
    QWidget::keyReleaseEvent(ev);
}

void StageView::leaveEvent(QEvent*)
{
    m_hasPointer = false;
    if (m_hotButton && !m_buttonDown) {
        m_hotButton = nullptr;
        invalidate();
    }
    update();
}

void StageView::dragEnterEvent(QDragEnterEvent* ev)
{
    if (ev->mimeData()->hasFormat("application/x-vertexa-symbol")) ev->acceptProposedAction();
}

void StageView::dragMoveEvent(QDragMoveEvent* ev)
{
    if (ev->mimeData()->hasFormat("application/x-vertexa-symbol")) ev->acceptProposedAction();
}

void StageView::dropEvent(QDropEvent* ev)
{
    const QByteArray id = ev->mimeData()->data("application/x-vertexa-symbol");
    if (id.isEmpty()) return;
    m_ed->placeSymbol(id.toStdString(), widgetToTimeline(ev->position()));
    ev->acceptProposedAction();
    setFocus();
}

} // namespace vx::app
