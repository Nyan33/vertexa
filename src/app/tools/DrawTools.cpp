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
#include <cstring>

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

// --- StrokeQueue -----------------------------------------------------------------------

void StrokeQueue::push(Task task)
{
    m_tasks.push_back(std::move(task));
    next();
}

void StrokeQueue::next()
{
    if (m_running || m_tasks.empty()) return;
    m_running = true;
    Task& t = m_tasks.front();
    if (t.start) t.start();
    std::weak_ptr<bool> alive = m_alive;
    QThreadPool::globalInstance()->start([this, work = t.work, alive]() {
        if (work) work();
        if (QCoreApplication* app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [this, alive]() {
                if (const auto a = alive.lock(); a && *a) done();
            }, Qt::QueuedConnection);
    });
}

void StrokeQueue::done()
{
    m_running = false;
    if (m_tasks.empty()) return;
    Task t = std::move(m_tasks.front());
    m_tasks.pop_front();
    if (t.commit) t.commit();
    next();
}

// --- Ink -----------------------------------------------------------------------------------

QImage Ink::takeImage(QSize px, qreal dpr)
{
    QImage img;
    for (QImage& f : m_free)
        if (f.size() == px) {
            img = std::move(f);
            break;
        }
    m_free.clear(); // the rest are the wrong size, or none are left
    if (img.isNull()) {
        img = QImage(px, QImage::Format_ARGB32_Premultiplied);
        img.fill(0);
    }
    img.setDevicePixelRatio(dpr);
    return img;
}

void Ink::recycle(Sheet& s)
{
    // Only the stroke's own area needs clearing for the next one.
    const QRect used = s.used & s.image.rect();
    for (int y = used.top(); y <= used.bottom(); ++y)
        std::memset(s.image.scanLine(y) + used.left() * 4, 0, size_t(used.width()) * 4);
    if (m_free.size() < 2) m_free.push_back(std::move(s.image));
}

void Ink::begin(const StageView* view, const QColor& color)
{
    cancel();
    m_view = view;
    const qreal dpr = view->devicePixelRatioF();
    Sheet s;
    s.image = takeImage((QSizeF(view->size()) * dpr).toSize(), dpr);
    s.opacity = color.alphaF();
    s.xf = Affine::scale(dpr) * view->timelineToWidget();
    m_active = std::move(s);
    m_color = color;
    m_color.setAlpha(255);
}

QRectF Ink::fill(const QPainterPath& piece)
{
    if (!m_active || piece.isEmpty()) return {};
    QPainter p(&m_active->image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setTransform(toQTransform(m_view->timelineToWidget()));
    p.setPen(Qt::NoPen);
    p.setBrush(m_color);
    p.drawPath(piece);
    const QRectF r = p.transform().mapRect(piece.boundingRect()).adjusted(-1, -1, 1, 1);
    const qreal dpr = m_active->image.devicePixelRatio();
    m_active->used |= QRectF(r.topLeft() * dpr, r.size() * dpr).toAlignedRect();
    return r;
}

QRectF Ink::line(Vec2 a, Vec2 b, double widthPx)
{
    if (!m_active) return {};
    const Affine T = m_view->timelineToWidget();
    const QPointF pa = toQPoint(T.map(a)), pb = toQPoint(T.map(b));
    QPainter p(&m_active->image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(m_color, widthPx, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(pa, pb);
    const double m = widthPx / 2 + 2;
    const QRectF r = QRectF(pa, pb).normalized().adjusted(-m, -m, m, m);
    const qreal dpr = m_active->image.devicePixelRatio();
    m_active->used |= QRectF(r.topLeft() * dpr, r.size() * dpr).toAlignedRect();
    return r;
}

void Ink::end()
{
    if (!m_active) return;
    m_pending.push_back(std::move(*m_active));
    m_active.reset();
}

void Ink::dropOldest()
{
    if (m_pending.empty()) return;
    recycle(m_pending.front());
    m_pending.pop_front();
}

void Ink::cancel()
{
    if (!m_active) return;
    recycle(*m_active);
    m_active.reset();
}

void Ink::paint(QPainter& p) const
{
    if (!m_view) return;
    // Sheets drawn before the view moved no longer line up: the committed
    // strokes show soon enough.
    const Affine xf = Affine::scale(m_view->devicePixelRatioF()) * m_view->timelineToWidget();
    auto draw = [&](const Sheet& s) {
        if (!(s.xf == xf)) return;
        p.save();
        p.setOpacity(p.opacity() * s.opacity);
        p.drawImage(QPointF(0, 0), s.image);
        p.restore();
    };
    for (const Sheet& s : m_pending) draw(s);
    if (m_active) draw(*m_active);
}

namespace {

/// Queues a finished stroke whose shape `build` makes on a worker thread,
/// where it is also merged into the layer's drawing (or becomes a drawing
/// object); the commit adds it to the frame it was drawn on. `after` runs
/// once it is committed.
void queueShape(Editor* ed, StrokeQueue& queue, Ink& ink, int layer, std::function<ShapeGraph()> build,
                const QString& label, PaintMode mode, std::optional<Region> mask, bool insideEmpty,
                std::function<void()> after)
{
    const Timeline& tl = ed->timeline();
    if (layer < 0 || layer >= int(tl.layers.size())) {
        ink.dropOldest();
        after();
        return;
    }
    struct State {
        std::optional<Region> mask;
        ShapeGraphPtr base; ///< merge shape the stroke was merged into on the worker
        ShapeGraph shape;
        std::optional<ShapeGraph> merged;
    };
    auto st = std::make_shared<State>();
    st->mask = std::move(mask);
    const uint32_t layerId = tl.layers[layer].id;
    const int frame = ed->frame();
    const bool object = ed->settings().objectDrawing;
    auto options = [st, mode, insideEmpty]() {
        OverlayOptions o;
        o.mode = mode;
        if (st->mask) o.mask = &*st->mask;
        o.insideEmpty = insideEmpty;
        return o;
    };
    StrokeQueue::Task t;
    t.start = [ed, st, layerId, frame, object]() {
        const int li = ed->timeline().layerIndex(layerId);
        if (!object && li >= 0 && ed->frame() == frame) st->base = ed->mergeShape(li);
    };
    t.work = [st, build = std::move(build), object, options]() {
        st->shape = build();
        if (!object && !st->shape.isEmpty()) st->merged = overlay(st->base ? *st->base : ShapeGraph{}, st->shape, options());
    };
    t.commit = [ed, &ink, st, layerId, frame, object, label, options, after = std::move(after)]() {
        ink.dropOldest();
        const int li = ed->timeline().layerIndex(layerId);
        if (li >= 0 && !st->shape.isEmpty())
            ed->edit(label, [&](Document& d) {
                QString why;
                Keyframe* k = ed->editableKey(d, li, &why, frame);
                if (!k) {
                    ed->notify(why);
                    return false;
                }
                if (object) {
                    k->elements.push_back(makeShapeElement(st->shape, true));
                    return true;
                }
                const ShapeElement* cur = k->elements.empty() ? nullptr : asShape(k->elements.front());
                const ShapeGraphPtr current = cur && !cur->isObject ? cur->graph : nullptr;
                // Merged on the worker into the drawing that is still there;
                // if the layer changed meanwhile, merge again.
                if (st->merged && current == st->base) setKeyframeMergeShape(*k, std::move(*st->merged));
                else mergeIntoKeyframe(*k, st->shape, options());
                return true;
            });
        after();
    };
    queue.push(std::move(t));
}

/// Piece of a brush stroke between two samples (one sample: the tip alone).
QPainterPath brushPiece(const BrushTip& tip, const BrushPoint* prev, const BrushPoint& cur)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    if (tip.shape == TipShape::Round) {
        std::vector<BrushPoint> pair;
        if (prev) pair.push_back(*prev);
        pair.push_back(cur);
        return toQPath(roundSweepOutline(pair));
    }
    std::vector<Vec2> pts = tipPolygon(tip, cur.r, tip.angle + cur.angle);
    for (Vec2& v : pts) v += cur.p;
    if (prev) {
        std::vector<Vec2> before = tipPolygon(tip, prev->r, tip.angle + prev->angle);
        for (Vec2& v : before) v += prev->p;
        pts.insert(pts.end(), before.begin(), before.end());
    }
    const std::vector<Vec2> hull = convexHull(pts);
    if (hull.size() < 3) return path;
    QPolygonF poly;
    for (const Vec2& v : hull) poly << toQPoint(v);
    path.addPolygon(poly);
    path.closeSubpath();
    return path;
}

/// Curves of a pencil stroke (runs on a worker: everything it needs is passed in).
std::vector<Cubic> pencilChain(std::vector<Vec2> pts, PencilMode mode, double smoothing, double upp, bool& closed)
{
    pts = dedupePoints(pts, upp * 0.25);
    std::vector<Cubic> chain;
    if (pts.size() < 2) return chain;
    closed = pts.size() > 3 && distance(pts.front(), pts.back()) < 6.0 * upp;
    if (mode == PencilMode::Straighten) {
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
    opt.tolerance = mode == PencilMode::Ink ? 0.35 * upp : (0.6 + smoothing / 100.0 * 3.0) * upp;
    opt.cornerAngle = mode == PencilMode::Ink ? 0.8 : 1.3;
    opt.cornerRadius = 4.0 * upp;
    if (closed) {
        pts.back() = pts.front();
        pts.pop_back();
        opt.closed = true;
    }
    return fitCurves(pts, opt);
}

} // namespace

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

QRectF FreehandTool::moveCursor(Vec2 pos, double diameterPx)
{
    const double r = diameterPx / 2 + 4;
    auto around = [r](QPointF c) { return QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r); };
    QRectF out;
    if (m_hasHover) out = around(toWidget(m_hover));
    m_hover = pos;
    m_hasHover = true;
    return out | around(toWidget(pos));
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

QRectF BrushTool::inkFrom(size_t from)
{
    if (from >= m_points.size()) return {};
    const ToolSettings& s = ed->settings();
    BrushTip tip;
    tip.shape = s.brushShape;
    tip.angle = s.brushAngle * kPi / 180.0;
    tip.aspect = 0.45;
    const size_t first = from > 0 ? from - 1 : 0;
    const std::vector<BrushPoint> bp = brushPoints(std::vector<InputSample>(m_points.begin() + long(first), m_points.end()));
    QRectF touched;
    for (size_t i = from - first; i < bp.size(); ++i) touched |= m_ink.fill(brushPiece(tip, i > 0 ? &bp[i - 1] : nullptr, bp[i]));
    return touched;
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
    m_ink.begin(view, toQColor(s.fill.mainColor()));
    update(inkFrom(0) | moveCursor(e.pos, s.brushSize));
}

void BrushTool::move(const ToolEvent& e)
{
    const size_t before = m_points.size();
    QRectF dirty = moveCursor(e.pos, ed->settings().brushSize);
    if (m_active && !addSample(e).empty()) dirty |= inkFrom(before);
    update(dirty);
}

void BrushTool::release(const ToolEvent&)
{
    if (!m_active) return;
    const ToolSettings& s = ed->settings();
    const std::vector<InputSample> pts = endStroke(s.brushSmoothing);
    m_ink.end();
    if (pts.empty()) {
        m_ink.dropOldest();
        update();
        return;
    }
    // The shape is built from the smoothed samples on a worker thread; the
    // ink stays up until it is merged.
    BrushTip tip;
    tip.shape = s.brushShape;
    tip.angle = s.brushAngle * kPi / 180.0;
    tip.aspect = 0.45;
    tip.rotates = s.brushTilt;
    const double upp = unitsPerPixel();
    auto build = [bp = brushPoints(pts), tip, upp, size = s.brushSize, fill = s.fill]() mutable {
        if (tip.shape != TipShape::Round) bp = resampleStroke(bp, std::clamp(size * upp * 0.15, upp * 0.75, upp * 4.0));
        Region r = sweptRegion(bp, tip);
        if (r.isEmpty()) return ShapeGraph{};
        r = refitRegion(r, 0.08 * upp, tip.shape == TipShape::Round ? 0.9 : 0.5);
        return graphFromRegion(r, fitGradient(fill, r.bounds()));
    };
    queueShape(ed, m_queue, m_ink, m_layer, std::move(build), QObject::tr("Brush"), s.brushMode, m_insideMask, m_insideEmpty,
               [this]() { update(); });
}

void BrushTool::hover(const ToolEvent& e) { update(moveCursor(e.pos, ed->settings().brushSize)); }

void BrushTool::cancel()
{
    m_ink.cancel();
    FreehandTool::cancel();
}

void BrushTool::paint(QPainter& p)
{
    m_ink.paint(p);
    if (m_hasHover) drawBrushCursor(p, toWidget(m_hover), ed->settings().brushSize, ui::Theme::p().text);
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

QRectF EraserTool::inkFrom(size_t from)
{
    if (from >= m_points.size()) return {};
    BrushTip round;
    const size_t first = from > 0 ? from - 1 : 0;
    const std::vector<BrushPoint> bp = brushPoints(std::vector<InputSample>(m_points.begin() + long(first), m_points.end()));
    QRectF touched;
    for (size_t i = from - first; i < bp.size(); ++i) touched |= m_ink.fill(brushPiece(round, i > 0 ? &bp[i - 1] : nullptr, bp[i]));
    return touched;
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
    m_ink.begin(view, ui::withAlpha(ui::Theme::p().bg0, 200));
    update(inkFrom(0) | moveCursor(e.pos, s.eraserSize * 1.5));
}

void EraserTool::move(const ToolEvent& e)
{
    const size_t before = m_points.size();
    QRectF dirty = moveCursor(e.pos, ed->settings().eraserSize * 1.5);
    if (m_active && !addSample(e).empty()) dirty |= inkFrom(before);
    update(dirty);
}

void EraserTool::release(const ToolEvent&)
{
    if (!m_active) return;
    const ToolSettings& s = ed->settings();
    const std::vector<InputSample> pts = endStroke(12.0);
    m_ink.end();
    if (pts.empty()) {
        m_ink.dropOldest();
        update();
        return;
    }
    BrushTip tip;
    tip.shape = s.eraserShape;
    const double upp = unitsPerPixel();
    struct State {
        Region area;
    };
    auto st = std::make_shared<State>();
    const int layer = m_layer;
    const EraseMode mode = s.eraseMode;
    const std::optional<Region> mask = m_mask;
    StrokeQueue::Task t;
    t.work = [st, bp = brushPoints(pts), tip, upp, size = s.eraserSize]() mutable {
        if (tip.shape != TipShape::Round) bp = resampleStroke(bp, std::clamp(size * upp * 0.15, upp * 0.75, upp * 4.0));
        const Region r = sweptRegion(bp, tip);
        if (!r.isEmpty()) st->area = refitRegion(r, 0.08 * upp, 0.9);
    };
    t.commit = [this, st, layer, mode, mask]() {
        m_ink.dropOldest();
        if (!st->area.isEmpty()) eraseArea(ed, layer, st->area, mode, mask ? &*mask : nullptr, QObject::tr("Erase"));
        update();
    };
    m_queue.push(std::move(t));
}

void EraserTool::hover(const ToolEvent& e) { update(moveCursor(e.pos, ed->settings().eraserSize * 1.5)); }

void EraserTool::cancel()
{
    m_ink.cancel();
    FreehandTool::cancel();
}

void EraserTool::paint(QPainter& p)
{
    const ui::Palette& pal = ui::Theme::p();
    m_ink.paint(p);
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

QRectF PencilTool::inkFrom(size_t from)
{
    const double width = std::max(1.0, ed->settings().stroke.width / unitsPerPixel());
    QRectF touched;
    for (size_t i = std::max<size_t>(from, 1); i < m_points.size(); ++i)
        touched |= m_ink.line(m_points[i - 1].pos, m_points[i].pos, width);
    if (from == 0 && m_points.size() == 1) touched |= m_ink.line(m_points[0].pos, m_points[0].pos, width);
    return touched;
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
    m_ink.begin(view, toQColor(s.stroke.paint.mainColor()));
    update(inkFrom(0));
}

void PencilTool::move(const ToolEvent& e)
{
    if (!m_active) return;
    const size_t before = m_points.size();
    if (!addSample(e).empty()) update(inkFrom(before));
}

void PencilTool::release(const ToolEvent&)
{
    if (!m_active) return;
    const ToolSettings& s = ed->settings();
    const auto pts = endStroke(s.pencilMode == PencilMode::Smooth ? s.pencilSmoothing : 0.0);
    m_ink.end();
    std::vector<Vec2> pos;
    for (const InputSample& q : pts) pos.push_back(q.pos);
    auto build = [pos = std::move(pos), mode = s.pencilMode, smoothing = s.pencilSmoothing, upp = unitsPerPixel(),
                  stroke = s.stroke]() {
        bool closed = false;
        const std::vector<Cubic> chain = pencilChain(pos, mode, smoothing, upp, closed);
        return chain.empty() ? ShapeGraph{} : graphFromPaths({chain}, stroke);
    };
    queueShape(ed, m_queue, m_ink, m_layer, std::move(build), QObject::tr("Pencil"), PaintMode::Normal, {}, false,
               [this]() { update(); });
}

void PencilTool::cancel()
{
    m_ink.cancel();
    FreehandTool::cancel();
}

void PencilTool::paint(QPainter& p) { m_ink.paint(p); }

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
