// SPDX-License-Identifier: GPL-3.0-or-later
#include "BasicTools.h"
#include "DrawTools.h"
#include "../StageView.h"
#include "../Theme.h"

#include "core/DocumentOps.h"
#include "render/QtConvert.h"

#include <QKeyEvent>
#include <QPainter>

#include <cmath>

namespace vx::app {

namespace {

FillStyle fitted(FillStyle f, const Rect& b)
{
    if (!f.isGradient() || b.isEmpty()) return f;
    f.gradient.matrix = Affine::translate(b.center()) * Affine::scale(std::max(1e-6, b.width() * 0.5), std::max(1e-6, b.height() * 0.5));
    return f;
}

Vec2 snap45(Vec2 a, Vec2 b)
{
    const Vec2 d = b - a;
    const double step = kPi / 4.0;
    const double ang = std::round(d.angle() / step) * step;
    return a + fromAngle(ang, d.length());
}

void strokePreview(QPainter& p, const QPainterPath& path, const ToolSettings& s, double zoom, bool withFill)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (withFill && s.fillEnabled) {
        QColor c = toQColor(s.fill.mainColor());
        p.fillPath(path, c);
    }
    if (s.strokeEnabled) {
        QPen pen(toQColor(s.stroke.paint.mainColor()), std::max(1.0, s.stroke.width * zoom));
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    } else if (!withFill || !s.fillEnabled) {
        p.setPen(QPen(ui::Theme::p().selection, 1.0, Qt::DashLine));
        p.drawPath(path);
    }
    p.restore();
}

} // namespace

// --- ShapeDragTool -----------------------------------------------------------------

bool ShapeDragTool::geometry(Region& area, std::vector<Cubic>& open) const
{
    const ToolSettings& s = ed->settings();
    Vec2 a = m_start, b = m_end;
    const bool shift = m_mods & Qt::ShiftModifier, alt = m_mods & Qt::AltModifier;
    const double minSize = unitsPerPixel() * 1.5;
    if (m_id == ToolId::Line) {
        if (shift) b = snap45(a, b);
        if (distance(a, b) < minSize) return false;
        open = {Cubic::line(a, b)};
        return true;
    }
    if (m_id == ToolId::PolyStar) {
        const double R = distance(a, b);
        if (R < minSize) return false;
        const int n = std::clamp(s.polySides, 3, 32);
        double rot = (b - a).angle() + kPi / 2.0;
        if (shift) rot = std::round(rot / (kPi / 12)) * (kPi / 12);
        std::vector<Vec2> pts;
        const int count = s.polyStar ? 2 * n : n;
        for (int i = 0; i < count; ++i) {
            const double ang = rot - kPi / 2.0 + 2.0 * kPi * i / count;
            const double r = (s.polyStar && (i % 2)) ? R * std::clamp(s.starDepth, 0.05, 1.0) : R;
            pts.push_back(a + fromAngle(ang, r));
        }
        area = Region::polygon(pts);
        return !area.isEmpty();
    }
    Vec2 d = b - a;
    if (shift) {
        const double m = std::max(std::abs(d.x), std::abs(d.y));
        d = {std::copysign(m, d.x == 0 ? 1.0 : d.x), std::copysign(m, d.y == 0 ? 1.0 : d.y)};
    }
    Rect r = alt ? Rect::fromPoints(a - d, a + d) : Rect::fromPoints(a, a + d);
    if (r.width() < minSize || r.height() < minSize) return false;
    if (m_id == ToolId::Rectangle) area = Region::roundedRect(r, s.rectRadius);
    else area = Region::ellipse(r.center(), r.width() * 0.5, r.height() * 0.5);
    return true;
}

void ShapeDragTool::press(const ToolEvent& e)
{
    m_layer = ed->layerIndex();
    QString why;
    if (!ed->canEdit(m_layer, &why)) {
        ed->notify(why);
        return;
    }
    m_active = true;
    m_start = m_end = e.pos;
    m_mods = e.mods;
}

void ShapeDragTool::move(const ToolEvent& e)
{
    if (!m_active) return;
    m_end = e.pos;
    m_mods = e.mods;
    update();
}

void ShapeDragTool::release(const ToolEvent& e)
{
    if (!m_active) return;
    m_end = e.pos;
    m_mods = e.mods;
    m_active = false;
    update();
    Region area;
    std::vector<Cubic> open;
    if (!geometry(area, open)) return;
    const ToolSettings& s = ed->settings();
    ShapeGraph g;
    if (!open.empty()) {
        if (!s.strokeEnabled) {
            ed->notify(QObject::tr("The stroke colour is set to none"));
            return;
        }
        g = graphFromPaths({open}, s.stroke);
    } else {
        const FillStyle f = fitted(s.fill, area.bounds());
        g = graphFromShape(area, s.fillEnabled ? &f : nullptr, s.strokeEnabled ? &s.stroke : nullptr);
    }
    commitShape(ed, m_layer, g, Editor::toolName(m_id));
}

void ShapeDragTool::paint(QPainter& p)
{
    if (!m_active) return;
    Region area;
    std::vector<Cubic> open;
    if (!geometry(area, open)) return;
    const QPainterPath path = open.empty() ? toQPath(area) : toQPath(open, false);
    const QPainterPath wpath = toQTransform(view->timelineToWidget()).map(path);
    strokePreview(p, wpath, ed->settings(), 1.0 / unitsPerPixel(), open.empty());
}

// --- PenTool -------------------------------------------------------------------------

std::vector<Cubic> PenTool::chain(bool close) const
{
    std::vector<Cubic> out;
    const size_t n = m_nodes.size();
    auto seg = [&](const Node& a, const Node& b) {
        const Vec2 c1 = a.smooth ? a.out : a.p;
        const Vec2 c2 = b.smooth ? b.p * 2.0 - b.out : b.p;
        if (!a.smooth && !b.smooth) out.push_back(Cubic::line(a.p, b.p));
        else out.push_back({a.p, c1, c2, b.p});
    };
    for (size_t i = 0; i + 1 < n; ++i) seg(m_nodes[i], m_nodes[i + 1]);
    if (close && n > 2) seg(m_nodes.back(), m_nodes.front());
    return out;
}

void PenTool::press(const ToolEvent& e)
{
    if (m_nodes.empty()) {
        m_layer = ed->layerIndex();
        QString why;
        if (!ed->canEdit(m_layer, &why)) {
            ed->notify(why);
            return;
        }
    } else if (m_nodes.size() >= 2 && distance(e.pos, m_nodes.front().p) < 6.0 * unitsPerPixel()) {
        finish(true);
        return;
    }
    m_nodes.push_back({e.pos, e.pos, false});
    m_dragging = true;
    update();
}

void PenTool::move(const ToolEvent& e)
{
    m_hover = e.pos;
    if (m_dragging && !m_nodes.empty()) {
        Node& n = m_nodes.back();
        n.out = e.pos;
        n.smooth = distance(n.p, n.out) > 1.5 * unitsPerPixel();
    }
    update();
}

void PenTool::release(const ToolEvent&) { m_dragging = false; }

void PenTool::hover(const ToolEvent& e)
{
    m_hover = e.pos;
    update();
}

void PenTool::doubleClick(const ToolEvent&)
{
    // The double click added a duplicate node: drop it.
    if (m_nodes.size() >= 2 && distance(m_nodes.back().p, m_nodes[m_nodes.size() - 2].p) < 2.0 * unitsPerPixel())
        m_nodes.pop_back();
    finish(false);
}

bool PenTool::keyPress(QKeyEvent* e)
{
    if (m_nodes.empty()) return false;
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        finish(false);
        return true;
    }
    if (e->key() == Qt::Key_Escape) {
        cancel();
        return true;
    }
    if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) {
        m_nodes.pop_back();
        update();
        return true;
    }
    return false;
}

void PenTool::finish(bool close)
{
    if (m_nodes.size() >= 2) {
        const std::vector<Cubic> c = chain(close);
        const ToolSettings& s = ed->settings();
        ShapeGraph g;
        if (close) {
            Region r;
            r.contours.push_back(c);
            const FillStyle f = fitted(s.fill, r.bounds());
            g = graphFromShape(r, s.fillEnabled ? &f : nullptr, s.strokeEnabled ? &s.stroke : nullptr);
        } else if (s.strokeEnabled) {
            g = graphFromPaths({c}, s.stroke);
        }
        commitShape(ed, m_layer, g, QObject::tr("Pen"));
    }
    m_nodes.clear();
    m_dragging = false;
    update();
}

void PenTool::cancel()
{
    m_nodes.clear();
    m_dragging = false;
    update();
}

void PenTool::paint(QPainter& p)
{
    if (m_nodes.empty()) return;
    const ui::Palette& pal = ui::Theme::p();
    const QTransform t = toQTransform(view->timelineToWidget());
    std::vector<Cubic> c = chain(false);
    if (!m_dragging) {
        const Node& last = m_nodes.back();
        c.push_back(last.smooth ? Cubic{last.p, last.out, m_hover, m_hover} : Cubic::line(last.p, m_hover));
    }
    strokePreview(p, t.map(toQPath(c, false)), ed->settings(), 1.0 / unitsPerPixel(), false);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        const Node& n = m_nodes[i];
        const QPointF w = toWidget(n.p);
        if (n.smooth) {
            const QPointF h1 = toWidget(n.out), h2 = toWidget(n.p * 2.0 - n.out);
            p.setPen(QPen(pal.selection, 1.0));
            p.drawLine(h1, h2);
            p.setBrush(pal.selection);
            p.drawEllipse(h1, 2.5, 2.5);
            p.drawEllipse(h2, 2.5, 2.5);
        }
        p.setPen(QPen(pal.selection, 1.2));
        p.setBrush(i == 0 ? pal.selection : pal.bg2);
        p.drawRect(QRectF(w.x() - 3, w.y() - 3, 6, 6));
    }
    p.restore();
}

// --- PaintBucketTool ------------------------------------------------------------------

void PaintBucketTool::press(const ToolEvent& e)
{
    const int li = ed->layerIndex();
    QString why;
    if (!ed->canEdit(li, &why)) {
        ed->notify(why);
        return;
    }
    const ToolSettings& s = ed->settings();
    if (!s.fillEnabled) {
        ed->notify(QObject::tr("The fill colour is set to none"));
        return;
    }
    const double gap = s.gapPixels() * unitsPerPixel();
    bool filled = false;
    ed->edit(QObject::tr("Paint Bucket"), [&](Document& d) {
        Timeline& tl = ed->mutableTimeline(d);
        Keyframe* k = tl.layers[li].keyAt(ed->frame());
        if (!k) return false;
        for (int i = int(k->elements.size()) - 1; i >= 0; --i) {
            const ShapeElement* sh = asShape(k->elements[i]);
            if (!sh || !sh->graph) continue;
            const Vec2 local = sh->matrix.inverted().map(e.pos);
            const Region face = bucketRegion(*sh->graph, local, gap);
            if (face.isEmpty()) continue;
            ShapeGraph out;
            if (!paintBucket(*sh->graph, local, fitted(s.fill, face.bounds()), gap, out)) continue;
            auto c = sh->cloneAs<ShapeElement>();
            c->graph = std::make_shared<ShapeGraph>(std::move(out));
            k->elements[i] = c;
            filled = true;
            return true;
        }
        return false;
    });
    if (!filled) ed->notify(QObject::tr("No closed area here — try a larger gap size"));
    m_lastHover = {1e300, 1e300};
}

void PaintBucketTool::hover(const ToolEvent& e)
{
    // Highlight the face under the cursor (cached topology, no gap closing).
    m_previewPath = QPainterPath();
    ShapeGraphPtr g = ed->mergeShape(ed->layerIndex());
    if (g && !g->isEmpty()) {
        const Arrangement& a = g->topology();
        const int f = a.locate(e.pos);
        if (f > 0) {
            Region r;
            r.contours = a.faceContours(f);
            m_previewPath = toQPath(r);
        }
    }
    update();
}

void PaintBucketTool::paint(QPainter& p)
{
    if (m_previewPath.isEmpty()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QPainterPath w = toQTransform(view->timelineToWidget()).map(m_previewPath);
    QColor c = toQColor(ed->settings().fill.mainColor());
    c.setAlpha(70);
    p.fillPath(w, c);
    p.setPen(QPen(ui::Theme::p().accent, 1.2, Qt::DashLine));
    p.drawPath(w);
    p.restore();
}

QCursor PaintBucketTool::cursor() const { return Qt::PointingHandCursor; }

// --- InkBottleTool -----------------------------------------------------------------------

void InkBottleTool::press(const ToolEvent& e)
{
    const int li = ed->layerIndex();
    QString why;
    if (!ed->canEdit(li, &why)) {
        ed->notify(why);
        return;
    }
    const ToolSettings& s = ed->settings();
    const double tol = 4.0 * unitsPerPixel();
    ed->edit(QObject::tr("Ink Bottle"), [&](Document& d) {
        Timeline& tl = ed->mutableTimeline(d);
        Keyframe* k = tl.layers[li].keyAt(ed->frame());
        if (!k) return false;
        for (int i = int(k->elements.size()) - 1; i >= 0; --i) {
            const ShapeElement* sh = asShape(k->elements[i]);
            if (!sh || !sh->graph) continue;
            ShapeGraph out;
            if (!inkBottle(*sh->graph, sh->matrix.inverted().map(e.pos), s.stroke, tol, out)) continue;
            auto c = sh->cloneAs<ShapeElement>();
            c->graph = std::make_shared<ShapeGraph>(std::move(out));
            k->elements[i] = c;
            return true;
        }
        return false;
    });
}

QCursor InkBottleTool::cursor() const { return Qt::PointingHandCursor; }

// --- EyedropperTool ------------------------------------------------------------------------

void EyedropperTool::press(const ToolEvent& e)
{
    const Timeline& tl = ed->timeline();
    const double tol = 4.0 * unitsPerPixel();
    for (int li = 0; li < int(tl.layers.size()); ++li) {
        const Layer& l = tl.layers[li];
        if (!l.visible) continue;
        const Keyframe* k = l.keyAt(ed->frame());
        if (!k) continue;
        for (int i = int(k->elements.size()) - 1; i >= 0; --i) {
            const ShapeElement* sh = asShape(k->elements[i]);
            if (!sh || !sh->graph) continue;
            const Vec2 local = sh->matrix.inverted().map(e.pos);
            const ShapeHit hit = hitTest(*sh->graph, local, tol);
            if (hit.kind == ShapeHit::Kind::Stroke && hit.edge >= 0) {
                ed->settings().stroke = sh->graph->stroke(sh->graph->edges[hit.edge].stroke);
                ed->settings().strokeEnabled = true;
                ed->emitSettingsChanged();
                ed->setTool(ToolId::InkBottle);
                return;
            }
            if (hit.kind == ShapeHit::Kind::Fill) {
                const int fid = sh->graph->topology().value(hit.face, 0);
                if (fid > 0) {
                    ed->settings().fill = sh->graph->fill(fid);
                    ed->settings().fillEnabled = true;
                    ed->emitSettingsChanged();
                    ed->setTool(ToolId::PaintBucket);
                    return;
                }
            }
        }
    }
    // Anything else (symbols, texture paint): sample the rendered colour.
    const QColor c = view->colorAt(e.widget);
    if (c.isValid()) {
        ed->settings().fill = FillStyle::solid(fromQColor(c));
        ed->settings().fillEnabled = true;
        ed->emitSettingsChanged();
    }
}

QCursor EyedropperTool::cursor() const { return Qt::CrossCursor; }

// --- ZoomTool ----------------------------------------------------------------------------

void ZoomTool::press(const ToolEvent& e)
{
    m_drag = true;
    m_a = m_b = e.widget;
    m_mods = e.mods;
}

void ZoomTool::move(const ToolEvent& e)
{
    if (!m_drag) return;
    m_b = e.widget;
    update();
}

void ZoomTool::release(const ToolEvent& e)
{
    if (!m_drag) return;
    m_drag = false;
    update();
    const QRectF r = QRectF(m_a, e.widget).normalized();
    if (r.width() > 8 && r.height() > 8) {
        const double f = std::min(view->width() / r.width(), view->height() / r.height());
        view->setZoom(view->zoom() * f, r.center());
        return;
    }
    const bool out = e.mods & Qt::AltModifier;
    view->setZoom(view->zoom() * (out ? 0.5 : 2.0), e.widget);
}

void ZoomTool::paint(QPainter& p)
{
    if (!m_drag) return;
    p.setPen(QPen(ui::Theme::p().accent, 1.0, Qt::DashLine));
    p.setBrush(ui::withAlpha(ui::Theme::p().accent, 30));
    p.drawRect(QRectF(m_a, m_b).normalized());
}

QCursor ZoomTool::cursor() const { return Qt::CrossCursor; }

} // namespace vx::app
