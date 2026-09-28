// SPDX-License-Identifier: GPL-3.0-or-later
#include "DrawTools.h"
#include "../StageView.h"
#include "../Theme.h"

#include "core/DocumentOps.h"
#include "core/Evaluate.h"
#include "geom/Fit.h"
#include "render/QtConvert.h"

#include <QCoreApplication>
#include <QPainter>
#include <QRandomGenerator>
#include <QThreadPool>

#include <algorithm>

#include <cmath>

namespace vx::app {

namespace {

FillStyle fitGradient(FillStyle f, const Rect& b)
{
    if (!f.isGradient() || b.isEmpty()) return f;
    f.gradient.matrix = Affine::translate(b.center()) * Affine::scale(std::max(1e-6, b.width() * 0.5), std::max(1e-6, b.height() * 0.5));
    return f;
}

void drawBrushCursor(QPainter& p, QPointF c, double diameterPx, const QColor& col)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const double r = std::max(1.5, diameterPx * 0.5);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 120), 2.2));
    p.drawEllipse(c, r, r);
    p.setPen(QPen(col, 1.2));
    p.drawEllipse(c, r, r);
    p.setPen(QPen(col, 1.0));
    p.drawLine(c + QPointF(-3, 0), c + QPointF(3, 0));
    p.drawLine(c + QPointF(0, -3), c + QPointF(0, 3));
    p.restore();
}

Region maskFromPick(const Editor* ed, int layer)
{
    const ShapePick& pick = ed->shapePick();
    if (!pick.valid()) return {};
    const Layer* l = ed->timeline().layers.size() > size_t(layer) ? &ed->timeline().layers[layer] : nullptr;
    if (!l || l->id != pick.layerId) return {};
    if (pick.region) return *pick.region;
    return selectionRegion(*pick.graph, pick.sel);
}

/// Mask for the Inside and Selection paint modes. False when painting must
/// not start (Paint Selection without a selected fill).
bool paintMask(Editor* ed, int layer, PaintMode mode, Vec2 at, std::optional<Region>& mask, bool& insideEmpty)
{
    mask.reset();
    insideEmpty = false;
    if (ed->settings().objectDrawing) return true;
    if (mode == PaintMode::Inside) {
        if (ShapeGraphPtr g = ed->mergeShape(layer)) {
            const Arrangement& a = g->topology();
            const int f = a.locate(at);
            if (f > 0 && a.value(f, 0)) {
                Region r;
                r.contours = a.faceContours(f);
                mask = r;
            } else {
                insideEmpty = true;
            }
        } else {
            insideEmpty = true;
        }
    }
    if (mode == PaintMode::Selection) {
        Region r = maskFromPick(ed, layer);
        if (r.isEmpty()) {
            ed->notify(QObject::tr("Paint Selection: select a fill first"));
            return false;
        }
        mask = r;
    }
    return true;
}

} // namespace

bool commitShape(Editor* ed, int layerIndex, const ShapeGraph& g, const QString& label, const OverlayOptions& opt)
{
    if (g.isEmpty()) return false;
    return ed->edit(label, [&](Document& d) {
        QString why;
        Keyframe* k = ed->editableKey(d, layerIndex, &why);
        if (!k) {
            ed->notify(why);
            return false;
        }
        if (ed->settings().objectDrawing) k->elements.push_back(makeShapeElement(g, true));
        else mergeIntoKeyframe(*k, g, opt);
        return true;
    });
}

bool eraseArea(Editor* ed, int layerIndex, const Region& r, EraseMode mode, const Region* mask, const QString& label)
{
    if (r.isEmpty()) return false;
    return ed->edit(label, [&](Document& d) {
        QString why;
        if (!ed->canEdit(layerIndex, &why)) return false;
        Timeline& tl = ed->mutableTimeline(d);
        Keyframe* k = tl.layers[layerIndex].keyAt(ed->frame());
        if (!k) return false;
        const Rect rb = r.bounds();
        bool changed = false;
        for (int i = int(k->elements.size()) - 1; i >= 0; --i) {
            const ShapeElement* sh = asShape(k->elements[i]);
            if (!sh || !sh->graph) continue;
            const Rect eb = elementBounds(d, *sh);
            if (!eb.intersects(rb)) continue;
            const bool merge = !sh->isObject;
            const Region local = merge ? r : r.transformed(sh->matrix.inverted());
            // Masks only make sense for the merge shape.
            ShapeGraph out = erase(*sh->graph, local, merge ? mode : (mode == EraseMode::Lines ? EraseMode::Lines : EraseMode::Normal),
                                   merge ? mask : nullptr);
            if (out.edges.size() == sh->graph->edges.size() && out.fills.size() == sh->graph->fills.size()) {
                bool same = true;
                for (size_t j = 0; j < out.edges.size() && same; ++j) same = out.edges[j] == sh->graph->edges[j];
                if (same) continue;
            }
            changed = true;
            if (out.isEmpty()) {
                k->elements.erase(k->elements.begin() + i);
            } else {
                auto c = sh->cloneAs<ShapeElement>();
                c->graph = std::make_shared<ShapeGraph>(std::move(out));
                k->elements[i] = c;
            }
        }
        return changed;
    });
}

// --- FreehandTool ------------------------------------------------------------------

InputSample FreehandTool::toSample(const ToolEvent& e)
{
    InputSample s;
    s.pos = e.pos;
    s.pressure = e.pressure;
    s.tiltX = e.tiltX;
    s.tiltY = e.tiltY;
    s.rotation = e.rotation;
    s.tangential = e.tangential;
    s.time = e.time;
    return s;
}

void FreehandTool::beginStroke(const ToolEvent& e, double smoothing)
{
    m_active = true;
    m_points.clear();
    m_stab.reset(smoothing, unitsPerPixel());
    for (const InputSample& s : m_stab.push(toSample(e))) m_points.push_back(s);
}

std::vector<InputSample> FreehandTool::addSample(const ToolEvent& e)
{
    auto out = m_stab.push(toSample(e));
    m_points.insert(m_points.end(), out.begin(), out.end());
    return out;
}

std::vector<InputSample> FreehandTool::endStroke(double smoothing)
{
    auto tail = m_stab.finish();
    m_points.insert(m_points.end(), tail.begin(), tail.end());
    m_active = false;
    std::vector<InputSample> pts = Stabilizer::smoothStroke(m_points, smoothing * 0.6, unitsPerPixel());
    m_points.clear();
    return pts;
}

void FreehandTool::cancel()
{
    m_active = false;
    m_points.clear();
    update();
}

// --- BrushTool -------------------------------------------------------------------------

std::vector<BrushPoint> BrushTool::brushPoints(const std::vector<InputSample>& pts) const
{
    const ToolSettings& s = ed->settings();
    const double base = s.brushSize * 0.5 * unitsPerPixel();
    std::vector<BrushPoint> out;
    out.reserve(pts.size());
    for (const InputSample& q : pts) {
        double f = 1.0;
        if (s.brushPressure) f = s.brushMinSize + (1.0 - s.brushMinSize) * std::clamp(q.pressure, 0.0, 1.0);
        double angle = 0.0;
        if (s.brushTilt && (q.tiltX != 0.0 || q.tiltY != 0.0)) angle = std::atan2(q.tiltY, q.tiltX);
        out.push_back({q.pos, std::max(base * f, unitsPerPixel() * 0.15), angle});
    }
    return out;
}

void BrushTool::rebuildPreview()
{
    m_preview = QPainterPath();
    m_preview.setFillRule(Qt::WindingFill);
    const std::vector<BrushPoint> bp = brushPoints(m_points);
    if (bp.empty()) return;
    const ToolSettings& s = ed->settings();
    if (s.brushShape == TipShape::Round) {
        m_preview = toQPath(roundSweepOutline(bp));
        return;
    }
    BrushTip tip;
    tip.shape = s.brushShape;
    tip.angle = s.brushAngle * kPi / 180.0;
    tip.aspect = 0.45;
    for (size_t i = 0; i < bp.size(); ++i) {
        std::vector<Vec2> pts = tipPolygon(tip, bp[i].r, tip.angle + bp[i].angle);
        for (Vec2& v : pts) v += bp[i].p;
        if (i > 0) {
            std::vector<Vec2> prev = tipPolygon(tip, bp[i - 1].r, tip.angle + bp[i - 1].angle);
            for (Vec2& v : prev) v += bp[i - 1].p;
            pts.insert(pts.end(), prev.begin(), prev.end());
        }
        const std::vector<Vec2> hull = convexHull(pts);
        if (hull.size() < 3) continue;
        QPolygonF poly;
        for (const Vec2& v : hull) poly << toQPoint(v);
        m_preview.addPolygon(poly);
        m_preview.closeSubpath();
    }
}

void BrushTool::press(const ToolEvent& e)
{
    m_layer = ed->layerIndex();
    QString why;
    if (!ed->canEdit(m_layer, &why)) {
        ed->notify(why);
        return;
    }
    const ToolSettings& s = ed->settings();
    if (!paintMask(ed, m_layer, s.brushMode, e.pos, m_insideMask, m_insideEmpty)) return;
    beginStroke(e, s.brushSmoothing);
    rebuildPreview();
    update();
}

void BrushTool::move(const ToolEvent& e)
{
    m_hover = e.pos;
    m_hasHover = true;
    if (!m_active) return;
    if (!addSample(e).empty()) rebuildPreview();
    update();
}

void BrushTool::release(const ToolEvent&)
{
    if (!m_active) return;
    const ToolSettings& s = ed->settings();
    const std::vector<InputSample> pts = endStroke(s.brushSmoothing);
    m_preview = QPainterPath();
    update();
    if (pts.empty()) return;
    std::vector<BrushPoint> bp = brushPoints(pts);
    BrushTip tip;
    tip.shape = s.brushShape;
    tip.angle = s.brushAngle * kPi / 180.0;
    tip.aspect = 0.45;
    tip.rotates = s.brushTilt;
    const double upp = unitsPerPixel();
    if (tip.shape != TipShape::Round) bp = resampleStroke(bp, std::clamp(s.brushSize * upp * 0.15, upp * 0.75, upp * 4.0));
    Region r = sweptRegion(bp, tip);
    if (r.isEmpty()) return;
    r = refitRegion(r, 0.08 * upp, tip.shape == TipShape::Round ? 0.9 : 0.5);
    const ShapeGraph g = graphFromRegion(r, fitGradient(s.fill, r.bounds()));
    OverlayOptions opt;
    opt.mode = s.brushMode;
    if (m_insideMask) opt.mask = &*m_insideMask;
    opt.insideEmpty = m_insideEmpty;
    commitShape(ed, m_layer, g, QObject::tr("Brush"), opt);
}

void BrushTool::hover(const ToolEvent& e)
{
    m_hover = e.pos;
    m_hasHover = true;
    update();
}

void BrushTool::paint(QPainter& p)
{
    const ToolSettings& s = ed->settings();
    if (m_active && !m_preview.isEmpty()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setTransform(toQTransform(view->timelineToWidget()), true);
        p.setPen(Qt::NoPen);
        p.setBrush(toQColor(s.fill.mainColor()));
        p.drawPath(m_preview);
        p.restore();
    }
    if (m_hasHover) drawBrushCursor(p, toWidget(m_hover), s.brushSize, ui::Theme::p().text);
}

QCursor BrushTool::cursor() const { return Qt::BlankCursor; }

// --- EraserTool ------------------------------------------------------------------------

std::vector<BrushPoint> EraserTool::brushPoints(const std::vector<InputSample>& pts) const
{
    const ToolSettings& s = ed->settings();
    const double base = s.eraserSize * 0.5 * unitsPerPixel();
    std::vector<BrushPoint> out;
    for (const InputSample& q : pts) {
        const double f = s.eraserPressure ? 0.15 + 0.85 * std::clamp(q.pressure, 0.0, 1.0) : 1.0;
        out.push_back({q.pos, base * f, 0.0});
    }
    return out;
}

void EraserTool::faucet(const ToolEvent& e)
{
    const int li = ed->layerIndex();
    const double tol = 4.0 * unitsPerPixel();
    ed->edit(QObject::tr("Faucet"), [&](Document& d) {
        QString why;
        if (!ed->canEdit(li, &why)) {
            ed->notify(why);
            return false;
        }
        Timeline& tl = ed->mutableTimeline(d);
        Keyframe* k = tl.layers[li].keyAt(ed->frame());
        if (!k) return false;
        // Drawing objects first (topmost), then the merge shape.
        for (int i = int(k->elements.size()) - 1; i >= 0; --i) {
            const ShapeElement* s = asShape(k->elements[i]);
            if (!s) continue;
            const Vec2 local = s->matrix.inverted().map(e.pos);
            const ShapeHit hit = hitTest(*s->graph, local, tol);
            if (hit.kind == ShapeHit::Kind::None) continue;
            ShapeSelection sel = hit.kind == ShapeHit::Kind::Fill ? selectFace(*s->graph, hit.face) : selectStrokeRun(*s->graph, hit.arrEdge);
            ShapeGraph rest, lifted;
            liftSelection(*s->graph, sel, rest, lifted);
            if (rest.isEmpty()) {
                k->elements.erase(k->elements.begin() + i);
            } else {
                auto c = s->cloneAs<ShapeElement>();
                c->graph = std::make_shared<ShapeGraph>(std::move(rest));
                k->elements[i] = c;
            }
            return true;
        }
        return false;
    });
}

void EraserTool::press(const ToolEvent& e)
{
    m_layer = ed->layerIndex();
    QString why;
    if (!ed->canEdit(m_layer, &why)) {
        ed->notify(why);
        return;
    }
    const ToolSettings& s = ed->settings();
    if (s.faucet) {
        faucet(e);
        return;
    }
    m_mask.reset();
    if (s.eraseMode == EraseMode::SelectedFills) {
        Region r = maskFromPick(ed, m_layer);
        if (r.isEmpty()) {
            ed->notify(QObject::tr("Erase Selected Fills: select a fill first"));
            return;
        }
        m_mask = r;
    } else if (s.eraseMode == EraseMode::Inside) {
        ShapeGraphPtr g = ed->mergeShape(m_layer);
        if (!g) return;
        const Arrangement& a = g->topology();
        const int f = a.locate(e.pos);
        if (f <= 0 || !a.value(f, 0)) return;
        Region r;
        r.contours = a.faceContours(f);
        m_mask = r;
    }
    beginStroke(e, 12.0);
    m_preview = toQPath(roundSweepOutline(brushPoints(m_points)));
    update();
}

void EraserTool::move(const ToolEvent& e)
{
    m_hover = e.pos;
    m_hasHover = true;
    if (!m_active) return;
    if (!addSample(e).empty()) m_preview = toQPath(roundSweepOutline(brushPoints(m_points)));
    update();
}

void EraserTool::release(const ToolEvent&)
{
    if (!m_active) return;
    const ToolSettings& s = ed->settings();
    const std::vector<InputSample> pts = endStroke(12.0);
    m_preview = QPainterPath();
    update();
    if (pts.empty()) return;
    const double upp = unitsPerPixel();
    BrushTip tip;
    tip.shape = s.eraserShape;
    std::vector<BrushPoint> bp = brushPoints(pts);
    if (tip.shape != TipShape::Round) bp = resampleStroke(bp, std::clamp(s.eraserSize * upp * 0.15, upp * 0.75, upp * 4.0));
    Region r = sweptRegion(bp, tip);
    if (r.isEmpty()) return;
    r = refitRegion(r, 0.08 * upp, 0.9);
    eraseArea(ed, m_layer, r, s.eraseMode, m_mask ? &*m_mask : nullptr, QObject::tr("Erase"));
}

void EraserTool::hover(const ToolEvent& e)
{
    m_hover = e.pos;
    m_hasHover = true;
    update();
}

void EraserTool::paint(QPainter& p)
{
    const ui::Palette& pal = ui::Theme::p();
    if (m_active && !m_preview.isEmpty()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setTransform(toQTransform(view->timelineToWidget()), true);
        p.setPen(Qt::NoPen);
        p.setBrush(ui::withAlpha(pal.bg0, 200));
        p.drawPath(m_preview);
        p.restore();
    }
    if (m_hasHover && !ed->settings().faucet) {
        const double d = ed->settings().eraserSize;
        const QPointF c = toWidget(m_hover);
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(pal.text, 1.2, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        if (ed->settings().eraserShape == TipShape::Square) p.drawRect(QRectF(c.x() - d / 2, c.y() - d / 2, d, d));
        else p.drawEllipse(c, d / 2, d / 2);
        p.restore();
    }
}

QCursor EraserTool::cursor() const { return ed->settings().faucet ? QCursor(Qt::PointingHandCursor) : QCursor(Qt::BlankCursor); }

// --- PencilTool --------------------------------------------------------------------------

std::vector<Cubic> PencilTool::buildChain(std::vector<Vec2> pts, bool& closed) const
{
    const ToolSettings& s = ed->settings();
    const double upp = unitsPerPixel();
    pts = dedupePoints(pts, upp * 0.25);
    std::vector<Cubic> chain;
    if (pts.size() < 2) return chain;
    closed = pts.size() > 3 && distance(pts.front(), pts.back()) < 6.0 * upp;
    if (s.pencilMode == PencilMode::Straighten) {
        std::vector<Vec2> simple = simplifyPolyline(pts, 3.0 * upp);
        if (closed) {
            // Shape recognition: near-circular closed strokes become ellipses.
            Vec2 c;
            for (const Vec2& q : pts) c += q;
            c = c / double(pts.size());
            double mean = 0, var = 0;
            for (const Vec2& q : pts) mean += distance(q, c);
            mean /= double(pts.size());
            for (const Vec2& q : pts) var += std::pow(distance(q, c) - mean, 2);
            const double dev = std::sqrt(var / double(pts.size())) / std::max(mean, 1e-9);
            Rect b;
            for (const Vec2& q : pts) b.include(q);
            if (simple.size() > 6 && dev < 0.18) {
                const Region e = Region::ellipse(b.center(), b.width() / 2, b.height() / 2);
                return e.contours.front();
            }
            if (!simple.empty()) simple.back() = simple.front();
        }
        for (size_t i = 0; i + 1 < simple.size(); ++i) chain.push_back(Cubic::line(simple[i], simple[i + 1]));
        return chain;
    }
    FitOptions opt;
    opt.tolerance = s.pencilMode == PencilMode::Ink ? 0.35 * upp : (0.6 + s.pencilSmoothing / 100.0 * 3.0) * upp;
    opt.cornerAngle = s.pencilMode == PencilMode::Ink ? 0.8 : 1.3;
    opt.cornerRadius = 4.0 * upp;
    if (closed) {
        pts.back() = pts.front();
        pts.pop_back();
        opt.closed = true;
    }
    chain = fitCurves(pts, opt);
    return chain;
}

void PencilTool::press(const ToolEvent& e)
{
    m_layer = ed->layerIndex();
    QString why;
    if (!ed->canEdit(m_layer, &why)) {
        ed->notify(why);
        return;
    }
    const ToolSettings& s = ed->settings();
    beginStroke(e, s.pencilMode == PencilMode::Smooth ? s.pencilSmoothing * 0.6 : 0.0);
    update();
}

void PencilTool::move(const ToolEvent& e)
{
    if (!m_active) return;
    addSample(e);
    update();
}

void PencilTool::release(const ToolEvent&)
{
    if (!m_active) return;
    const ToolSettings& s = ed->settings();
    const auto pts = endStroke(s.pencilMode == PencilMode::Smooth ? s.pencilSmoothing : 0.0);
    update();
    std::vector<Vec2> pos;
    for (const InputSample& q : pts) pos.push_back(q.pos);
    bool closed = false;
    std::vector<Cubic> chain = buildChain(pos, closed);
    if (chain.empty()) return;
    commitShape(ed, m_layer, graphFromPaths({chain}, s.stroke), QObject::tr("Pencil"));
}

void PencilTool::paint(QPainter& p)
{
    if (!m_active || m_points.size() < 2) return;
    const ToolSettings& s = ed->settings();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(toQColor(s.stroke.paint.mainColor()), std::max(1.0, s.stroke.width / unitsPerPixel()));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    QPolygonF poly;
    for (const InputSample& q : m_points) poly << toWidget(q.pos);
    p.drawPolyline(poly);
    p.restore();
}

QCursor PencilTool::cursor() const { return Qt::CrossCursor; }

// --- PaintBrushTool -------------------------------------------------------------------------
//
// Every brush produces vector fills. While drawing, textured brushes are
// previewed chunk by chunk into an image (their texture is anchored to the
// canvas, so the chunks line up); other brushes rebuild their exact stroke as
// often as its cost allows. Samples newer than the preview are shown as a
// quick round outline. On release the exact stroke is built and merged into
// the layer on a worker thread, and committed on the GUI thread in order;
// the preview stays up until then.

struct PaintBrushTool::Job {
    VectorBrushPreset preset;
    std::vector<BrushPoint> path;
    FillStyle paint;
    uint32_t seed = 1;
    double tolerance = 0.1;
    bool erase = false;
    bool object = false;
    PaintMode mode = PaintMode::Normal;
    std::optional<Region> mask;
    bool insideEmpty = false;
    uint32_t layerId = 0;
    ShapeGraphPtr base; ///< merge shape the result was computed on
    // Results, written by the worker.
    ShapeGraph stroke;
    std::optional<ShapeGraph> merged;
    Region area; ///< erase: the stroke's area

    OverlayOptions options() const
    {
        OverlayOptions opt;
        opt.mode = mode;
        if (mask) opt.mask = &*mask;
        opt.insideEmpty = insideEmpty;
        return opt;
    }

    void run()
    {
        std::vector<BrushPiece> pieces = vectorBrushStroke(preset, path, paint, seed, tolerance);
        if (pieces.empty()) return;
        if (erase) {
            Region all;
            for (BrushPiece& piece : pieces)
                for (Contour& c : piece.region.contours) all.contours.push_back(std::move(c));
            area = normalizeRegion(all);
            return;
        }
        Rect bounds;
        for (const BrushPiece& piece : pieces) bounds.include(piece.region.bounds());
        for (BrushPiece& piece : pieces)
            if (piece.fill == paint) piece.fill = fitGradient(piece.fill, bounds);
        stroke = vectorBrushGraph(pieces);
        if (!object && !stroke.isEmpty()) merged = overlay(base ? *base : ShapeGraph{}, stroke, options());
    }
};

struct PaintBrushTool::PreviewResult {
    size_t covered = 0;
    std::vector<PreviewPiece> pieces; ///< whole-stroke preview
    QImage chunk;                     ///< textured chunk, overlay pixels
    QPoint at;
};

PaintBrushTool::~PaintBrushTool() { *m_alive = false; }

FillStyle PaintBrushTool::paintStyle() const
{
    // Like Animate's Paint Brush, the stroke colour paints.
    return ed->settings().stroke.paint;
}

Affine PaintBrushTool::overlayTransform() const { return Affine::scale(view->devicePixelRatioF()) * view->timelineToWidget(); }

void PaintBrushTool::press(const ToolEvent& e)
{
    m_layer = ed->layerIndex();
    QString why;
    if (!ed->canEdit(m_layer, &why)) {
        ed->notify(why);
        return;
    }
    const ToolSettings& s = ed->settings();
    if (!s.paintErase && !paintMask(ed, m_layer, s.paintMode, e.pos, m_insideMask, m_insideEmpty)) return;
    if (s.paintErase) {
        m_insideMask.reset();
        m_insideEmpty = false;
    }
    m_preset = s.paint;
    m_chunked = m_preset.kind == VectorBrushKind::Textured;
    m_seed = QRandomGenerator::global()->generate() | 1u;
    m_pieces.clear();
    m_covered = 0;
    ++m_previewGen;
    m_clock.start();
    m_lastBuild = -1000;
    // The overlay may still show strokes waiting to be merged: keep it when
    // the view has not moved.
    const QSize px = view->size() * view->devicePixelRatioF();
    if (m_overlay.size() != px || !(overlayTransform() == m_overlayXf) || m_jobs.empty()) {
        m_overlay = QImage(px, QImage::Format_ARGB32_Premultiplied);
        m_overlay.setDevicePixelRatio(view->devicePixelRatioF());
        m_overlay.fill(0);
        m_overlayXf = overlayTransform();
    }
    beginStroke(e, m_preset.smoothing);
    requestPreview(true);
    update();
}

void PaintBrushTool::requestPreview(bool force)
{
    const size_t n = m_points.size();
    if (m_previewBusy || n == 0 || n <= m_covered) {
        rebuildTail();
        return;
    }
    size_t from = 0;
    if (m_chunked) {
        double fresh = 0.0;
        for (size_t i = std::max<size_t>(m_covered, 1); i < n; ++i) fresh += distance(m_points[i - 1].pos, m_points[i].pos);
        const double chunk = std::max(m_preset.size * 2.0, 24.0 * unitsPerPixel());
        if (!force && fresh < chunk && m_clock.elapsed() - m_lastBuild < 80) {
            rebuildTail();
            return;
        }
        // Start about one brush width back so neighbouring chunks overlap.
        from = m_covered;
        double back = 0.0;
        while (from > 0 && back < m_preset.size) {
            back += distance(m_points[from - 1].pos, m_points[from].pos);
            --from;
        }
    }
    const std::vector<InputSample> part(m_points.begin() + long(from), m_points.end());
    const VectorBrushPreset preset = m_preset;
    const FillStyle paint = paintStyle();
    const uint32_t seed = m_seed;
    const double tolerance = (m_chunked ? 0.3 : 0.25) * unitsPerPixel();
    const bool chunked = m_chunked;
    const bool erasing = ed->settings().paintErase;
    QColor eraseColor = ui::Theme::p().text;
    eraseColor.setAlpha(110);
    const QTransform toOverlay = toQTransform(m_overlayXf);
    const QRect overlayRect = m_overlay.rect();
    const uint64_t generation = m_previewGen;
    m_previewBusy = true;
    std::weak_ptr<bool> alive = m_alive;
    QThreadPool::globalInstance()->start([=, this]() {
        auto result = std::make_shared<PreviewResult>();
        result->covered = n;
        const std::vector<BrushPiece> pieces = vectorBrushStroke(preset, vectorBrushPath(preset, part), paint, seed, tolerance);
        if (chunked) {
            // Textured chunks are painted here, in overlay pixels (their
            // texture is anchored to the canvas, so the chunks line up).
            std::vector<QPainterPath> paths;
            QRectF box;
            for (const BrushPiece& piece : pieces) {
                QPainterPath qp = toOverlay.map(toQPath(piece.region));
                qp.setFillRule(Qt::WindingFill);
                box |= qp.boundingRect();
                paths.push_back(std::move(qp));
            }
            const QRect r = box.toAlignedRect().adjusted(-1, -1, 1, 1) & overlayRect;
            if (!r.isEmpty()) {
                result->chunk = QImage(r.size(), QImage::Format_ARGB32_Premultiplied);
                result->chunk.fill(0);
                result->at = r.topLeft();
                QPainter p(&result->chunk);
                p.setRenderHint(QPainter::Antialiasing);
                p.translate(-r.topLeft());
                p.setPen(Qt::NoPen);
                p.setBrush(erasing ? eraseColor : toQColor(paint.mainColor()));
                for (const QPainterPath& qp : paths) p.drawPath(qp);
            }
        } else {
            for (const BrushPiece& piece : pieces) {
                QPainterPath qp = toQPath(piece.region);
                qp.setFillRule(Qt::WindingFill);
                result->pieces.push_back({qp, erasing ? eraseColor : toQColor(piece.fill.mainColor())});
            }
        }
        if (QCoreApplication* app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [this, alive, generation, result]() {
                if (const auto a = alive.lock(); a && *a) finishPreview(generation, *result);
            }, Qt::QueuedConnection);
    });
}

void PaintBrushTool::finishPreview(uint64_t generation, PreviewResult& result)
{
    m_previewBusy = false;
    if (!m_active) return;
    if (generation == m_previewGen) {
        if (m_chunked) {
            if (!result.chunk.isNull() && overlayTransform() == m_overlayXf) {
                QPainter p(&m_overlay);
                const qreal dpr = m_overlay.devicePixelRatio();
                result.chunk.setDevicePixelRatio(dpr);
                p.drawImage(QPointF(result.at) / dpr, result.chunk);
            }
        } else {
            m_pieces = std::move(result.pieces);
        }
        m_covered = result.covered;
        m_lastBuild = m_clock.elapsed();
    }
    // Samples that arrived meanwhile.
    requestPreview(false);
    update();
}

void PaintBrushTool::rebuildTail()
{
    m_tail = QPainterPath();
    if (m_points.size() <= m_covered) return;
    const size_t from = m_covered > 0 ? m_covered - 1 : 0;
    const std::vector<InputSample> tail(m_points.begin() + long(from), m_points.end());
    m_tail = toQPath(roundSweepOutline(vectorBrushPath(m_preset, tail)));
    m_tail.setFillRule(Qt::WindingFill);
}

void PaintBrushTool::move(const ToolEvent& e)
{
    m_hover = e.pos;
    m_hasHover = true;
    if (!m_active) {
        update();
        return;
    }
    if (!addSample(e).empty()) requestPreview(false);
    update();
}

void PaintBrushTool::release(const ToolEvent&)
{
    if (!m_active) return;
    const ToolSettings& s = ed->settings();
    const std::vector<InputSample> pts = endStroke(m_preset.smoothing);
    ++m_previewGen; // a preview still being built is not needed any more
    // Keep what the user sees until the merged result arrives.
    if (!m_overlay.isNull() && (!m_pieces.empty() || !m_tail.isEmpty())) {
        QPainter p(&m_overlay);
        p.setRenderHint(QPainter::Antialiasing);
        p.setTransform(toQTransform(Affine::scale(1.0 / m_overlay.devicePixelRatio()) * m_overlayXf));
        p.setPen(Qt::NoPen);
        for (const PreviewPiece& piece : m_pieces) {
            p.setBrush(piece.color);
            p.drawPath(piece.path);
        }
        if (!m_tail.isEmpty()) {
            p.setBrush(m_pieces.empty() ? toQColor(paintStyle().mainColor()) : m_pieces.back().color);
            p.drawPath(m_tail);
        }
    }
    m_pieces.clear();
    m_tail = QPainterPath();
    const Layer* layer = m_layer >= 0 && m_layer < int(ed->timeline().layers.size()) ? &ed->timeline().layers[m_layer] : nullptr;
    if (pts.empty() || !layer) {
        update();
        return;
    }
    auto job = std::make_shared<Job>();
    job->preset = m_preset;
    job->path = vectorBrushPath(m_preset, pts);
    job->paint = paintStyle();
    job->seed = m_seed;
    job->tolerance = 0.08 * unitsPerPixel();
    job->erase = s.paintErase;
    job->object = s.objectDrawing;
    job->mode = s.paintMode;
    job->mask = m_insideMask;
    job->insideEmpty = m_insideEmpty;
    job->layerId = layer->id;
    m_jobs.push_back(job);
    startNextJob();
    update();
}

void PaintBrushTool::startNextJob()
{
    if (m_running || m_jobs.empty()) return;
    std::shared_ptr<Job> job = m_jobs.front();
    const int li = ed->timeline().layerIndex(job->layerId);
    job->base = (job->erase || job->object || li < 0) ? nullptr : ed->mergeShape(li);
    // Lazily built caches of shared artwork are filled here, not on the worker.
    if (job->preset.art) (void)job->preset.art->topology();
    m_running = true;
    std::weak_ptr<bool> alive = m_alive;
    QThreadPool::globalInstance()->start([this, job, alive]() {
        job->run();
        if (QCoreApplication* app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [this, alive]() {
                if (const auto a = alive.lock(); a && *a) finishJob();
            }, Qt::QueuedConnection);
    });
}

void PaintBrushTool::finishJob()
{
    m_running = false;
    if (m_jobs.empty()) return;
    const std::shared_ptr<Job> job = m_jobs.front();
    m_jobs.pop_front();
    const int li = ed->timeline().layerIndex(job->layerId);
    if (li >= 0) {
        if (job->erase) {
            if (!job->area.isEmpty()) eraseArea(ed, li, job->area, EraseMode::Normal, nullptr, QObject::tr("Paint Brush Erase"));
        } else if (!job->stroke.isEmpty()) {
            ed->edit(QObject::tr("Paint Brush"), [&](Document& d) {
                QString why;
                Keyframe* k = ed->editableKey(d, li, &why);
                if (!k) {
                    ed->notify(why);
                    return false;
                }
                if (job->object) {
                    k->elements.push_back(makeShapeElement(job->stroke, true));
                    return true;
                }
                const ShapeElement* cur = k->elements.empty() ? nullptr : asShape(k->elements.front());
                const ShapeGraphPtr current = cur && !cur->isObject ? cur->graph : nullptr;
                // Merged on the worker against the shape that is still there; if the
                // layer changed meanwhile, merge again.
                if (job->merged && current == job->base) setKeyframeMergeShape(*k, std::move(*job->merged));
                else mergeIntoKeyframe(*k, job->stroke, job->options());
                return true;
            });
        }
    }
    if (m_jobs.empty() && !m_active) m_overlay = QImage();
    update();
    startNextJob();
}

void PaintBrushTool::hover(const ToolEvent& e)
{
    m_hover = e.pos;
    m_hasHover = true;
    update();
}

void PaintBrushTool::paint(QPainter& p)
{
    if (!m_overlay.isNull() && (m_active || !m_jobs.empty()) && overlayTransform() == m_overlayXf)
        p.drawImage(QPointF(0, 0), m_overlay);
    if (m_active && (!m_pieces.empty() || !m_tail.isEmpty())) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setTransform(toQTransform(view->timelineToWidget()), true);
        p.setPen(Qt::NoPen);
        for (const PreviewPiece& piece : m_pieces) {
            p.setBrush(piece.color);
            p.drawPath(piece.path);
        }
        if (!m_tail.isEmpty()) {
            QColor c = m_pieces.empty() ? toQColor(paintStyle().mainColor()) : m_pieces.back().color;
            if (!m_chunked) c.setAlphaF(c.alphaF() * 0.55);
            if (ed->settings().paintErase) {
                c = ui::Theme::p().text;
                c.setAlpha(110);
            }
            p.setBrush(c);
            p.drawPath(m_tail);
        }
        p.restore();
    }
    if (m_hasHover) {
        const double size = m_active ? m_preset.size : ed->settings().paint.size;
        drawBrushCursor(p, toWidget(m_hover), size / unitsPerPixel(), ui::Theme::p().text);
    }
}

void PaintBrushTool::cancel()
{
    FreehandTool::cancel();
    ++m_previewGen;
    m_pieces.clear();
    m_tail = QPainterPath();
    if (m_jobs.empty()) m_overlay = QImage();
}

QCursor PaintBrushTool::cursor() const { return Qt::BlankCursor; }

} // namespace vx::app
