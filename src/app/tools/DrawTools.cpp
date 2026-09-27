// SPDX-License-Identifier: GPL-3.0-or-later
#include "DrawTools.h"
#include "../StageView.h"
#include "../Theme.h"

#include "core/DocumentOps.h"
#include "core/Evaluate.h"
#include "geom/Fit.h"
#include "render/DabEngine.h"
#include "render/QtConvert.h"

#include <QPainter>
#include <QRandomGenerator>

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
    m_insideMask.reset();
    m_insideEmpty = false;
    if (s.brushMode == PaintMode::Inside && !s.objectDrawing) {
        if (ShapeGraphPtr g = ed->mergeShape(m_layer)) {
            const Arrangement& a = g->topology();
            const int f = a.locate(e.pos);
            if (f > 0 && a.value(f, 0)) {
                Region r;
                r.contours = a.faceContours(f);
                m_insideMask = r;
            } else {
                m_insideEmpty = true;
            }
        } else {
            m_insideEmpty = true;
        }
    }
    if (s.brushMode == PaintMode::Selection && !s.objectDrawing) {
        Region r = maskFromPick(ed, m_layer);
        if (r.isEmpty()) {
            ed->notify(QObject::tr("Paint Selection: select a fill first"));
            return;
        }
        m_insideMask = r;
    }
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
    const EraseMode mode = s.eraseMode;
    const Region* mask = m_mask ? &*m_mask : nullptr;
    const int li = m_layer;
    ed->edit(QObject::tr("Erase"), [&](Document& d) {
        QString why;
        if (!ed->canEdit(li, &why)) return false;
        Timeline& tl = ed->mutableTimeline(d);
        Keyframe* k = tl.layers[li].keyAt(ed->frame());
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

BrushPresetPtr PaintBrushTool::currentPreset() const
{
    BrushPreset p = ed->settings().paint;
    p.size = std::max(0.2, p.size * ed->settings().paintSizeScale);
    return std::make_shared<BrushPreset>(p);
}

void PaintBrushTool::press(const ToolEvent& e)
{
    m_layer = ed->layerIndex();
    QString why;
    if (!ed->canEdit(m_layer, &why)) {
        ed->notify(why);
        return;
    }
    m_preset = currentPreset();
    m_seed = QRandomGenerator::global()->generate() | 1u;
    m_samples.clear();
    m_rendered = 0;
    const qreal dpr = view->devicePixelRatioF();
    m_overlay = QImage(view->size() * dpr, QImage::Format_ARGB32_Premultiplied);
    m_overlay.setDevicePixelRatio(dpr);
    m_overlay.fill(0);
    beginStroke(e, m_preset->smoothing);
    for (const InputSample& s : m_points)
        m_samples.push_back({s.pos, float(s.pressure), float(s.tiltX), float(s.tiltY), float(s.rotation)});
    renderIncrement();
    update();
}

void PaintBrushTool::renderIncrement()
{
    if (m_samples.empty() || m_overlay.isNull()) return;
    PaintStroke s;
    s.brush = m_preset;
    s.color = ed->settings().paintErase ? Color(128, 128, 128, 160) : ed->settings().stroke.paint.mainColor();
    s.seed = m_seed + uint32_t(m_rendered);
    const size_t from = m_rendered > 0 ? m_rendered - 1 : 0;
    s.samples.assign(m_samples.begin() + long(from), m_samples.end());
    if (s.samples.size() < 2 && m_rendered > 0) return;
    DabContext ctx;
    ctx.toDevice = Affine::scale(m_overlay.devicePixelRatio()) * view->timelineToWidget();
    ctx.doc = &ed->doc();
    paintStroke(m_overlay, s, ctx);
    m_rendered = m_samples.size();
}

void PaintBrushTool::move(const ToolEvent& e)
{
    m_hover = e.pos;
    m_hasHover = true;
    if (!m_active) {
        update();
        return;
    }
    for (const InputSample& s : addSample(e))
        m_samples.push_back({s.pos, float(s.pressure), float(s.tiltX), float(s.tiltY), float(s.rotation)});
    renderIncrement();
    update();
}

void PaintBrushTool::release(const ToolEvent&)
{
    if (!m_active) return;
    auto tail = m_stab.finish();
    for (const InputSample& s : tail) m_samples.push_back({s.pos, float(s.pressure), float(s.tiltX), float(s.tiltY), float(s.rotation)});
    m_active = false;
    m_points.clear();
    std::vector<PaintSample> samples = std::move(m_samples);
    m_samples.clear();
    m_overlay = QImage();
    update();
    if (samples.empty()) return;
    PaintStroke stroke;
    stroke.brush = m_preset;
    stroke.color = ed->settings().stroke.paint.mainColor();
    stroke.seed = m_seed;
    stroke.erase = ed->settings().paintErase;
    const int li = m_layer;
    ed->edit(stroke.erase ? QObject::tr("Paint Eraser") : QObject::tr("Paint Brush"), [&](Document& d) {
        QString why;
        Keyframe* k = ed->editableKey(d, li, &why);
        if (!k) {
            ed->notify(why);
            return false;
        }
        const PaintElement* top = k->elements.empty() ? nullptr : asPaint(k->elements.back());
        if (!top && stroke.erase) {
            ed->notify(QObject::tr("Nothing painted here to erase"));
            return false;
        }
        std::shared_ptr<PaintElement> pe = top ? top->cloneAs<PaintElement>() : std::make_shared<PaintElement>();
        PaintStroke st = stroke;
        st.samples = samples;
        if (top && !top->matrix.isIdentity()) {
            const Affine inv = top->matrix.inverted();
            for (PaintSample& s : st.samples) s.pos = inv.map(s.pos);
        }
        pe->strokes.push_back(std::move(st));
        if (top) {
            k->elements.back() = pe;
        } else {
            const Rect b = pe->localBounds();
            if (!b.isEmpty()) pe->pivot = b.center();
            k->elements.push_back(pe);
        }
        return true;
    });
}

void PaintBrushTool::hover(const ToolEvent& e)
{
    m_hover = e.pos;
    m_hasHover = true;
    update();
}

void PaintBrushTool::paint(QPainter& p)
{
    if (!m_overlay.isNull()) p.drawImage(QPointF(0, 0), m_overlay);
    if (m_hasHover) {
        const BrushPresetPtr pr = m_preset && m_active ? m_preset : currentPreset();
        drawBrushCursor(p, toWidget(m_hover), pr->size / unitsPerPixel(), ui::Theme::p().text);
    }
}

void PaintBrushTool::cancel()
{
    FreehandTool::cancel();
    m_samples.clear();
    m_overlay = QImage();
}

QCursor PaintBrushTool::cursor() const { return Qt::BlankCursor; }

} // namespace vx::app
