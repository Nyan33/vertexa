// SPDX-License-Identifier: GPL-3.0-or-later
#include "SelectTools.h"
#include "../StageView.h"
#include "../Theme.h"

#include "core/DocumentOps.h"
#include "core/Evaluate.h"
#include "render/QtConvert.h"

#include <QPainter>

#include <cmath>

namespace vx::app {

// --- hit testing ---------------------------------------------------------------------

bool hitElement(const Document& d, const Element& e, Vec2 p, double tol, int localFrame, int depth)
{
    if (depth > 24) return false;
    const Affine inv = e.matrix.inverted();
    const Vec2 q = inv.map(p);
    const double ltol = tol / std::max(1e-9, e.matrix.meanScale());
    switch (e.type()) {
    case ElementType::Shape: {
        const auto& s = static_cast<const ShapeElement&>(e);
        return s.graph && hitTest(*s.graph, q, ltol).kind != ShapeHit::Kind::None;
    }
    case ElementType::Morph: {
        const auto& m = static_cast<const MorphElement&>(e);
        return m.data && m.data->bounds.inflated(ltol).contains(q);
    }
    case ElementType::Group: {
        for (const ElementPtr& c : static_cast<const GroupElement&>(e).children)
            if (hitElement(d, *c, q, ltol, localFrame, depth + 1)) return true;
        return false;
    }
    case ElementType::Instance: {
        const auto& in = static_cast<const InstanceElement&>(e);
        const Symbol* sym = d.symbol(in.symbolId);
        if (!sym || !in.visible) return false;
        const int f = instanceSymbolFrame(d, in, localFrame);
        const Timeline& tl = sym->timeline;
        for (int li = 0; li < int(tl.layers.size()); ++li) {
            const Layer& l = tl.layers[li];
            if (l.type == LayerType::Guide || l.type == LayerType::Folder || !l.visible) continue;
            const auto items = evaluateLayer(d, tl, li, f);
            for (auto it = items.rbegin(); it != items.rend(); ++it)
                if (hitElement(d, *it->element, q, ltol, it->localFrame, depth + 1)) return true;
        }
        // Empty symbols are clickable around their registration point.
        return timelineBounds(d, tl, f).isEmpty() && distance(q, {0, 0}) <= 6 * ltol;
    }
    }
    return false;
}

StageHit hitStage(const Editor* ed, Vec2 pos, double tol)
{
    const Document& d = ed->doc();
    const Timeline& tl = ed->timeline();
    for (int li = 0; li < int(tl.layers.size()); ++li) {
        const Layer& l = tl.layers[li];
        if (!l.visible || l.locked || l.type == LayerType::Folder) continue;
        const auto items = evaluateLayer(d, tl, li, ed->frame());
        for (int i = int(items.size()) - 1; i >= 0; --i) {
            const ElementPtr& e = items[i].element;
            if (const ShapeElement* s = asShape(e); s && !s->isObject && i == 0) {
                const ShapeHit h = hitTest(*s->graph, pos, tol);
                if (h.kind != ShapeHit::Kind::None) {
                    StageHit r;
                    r.kind = StageHit::Kind::Shape;
                    r.layer = li;
                    r.index = 0;
                    r.shape = h;
                    return r;
                }
                continue;
            }
            if (e->type() == ElementType::Morph) continue;
            if (hitElement(d, *e, pos, tol, items[i].localFrame)) {
                StageHit r;
                r.kind = StageHit::Kind::Element;
                r.layer = li;
                r.index = i;
                return r;
            }
        }
    }
    return {};
}

Rect localBoundsOf(const Document& d, const Element& e, int localFrame)
{
    switch (e.type()) {
    case ElementType::Shape: return static_cast<const ShapeElement&>(e).graph->bounds(true);
    case ElementType::Instance: {
        const auto& in = static_cast<const InstanceElement&>(e);
        const Symbol* s = d.symbol(in.symbolId);
        if (!s) return {};
        Rect r = timelineBounds(d, s->timeline, instanceSymbolFrame(d, in, localFrame));
        return r.isEmpty() ? Rect(-4, -4, 4, 4) : r;
    }
    case ElementType::Group: {
        Rect r;
        for (const ElementPtr& c : static_cast<const GroupElement&>(e).children) r.include(elementBounds(d, *c, localFrame));
        return r;
    }
    case ElementType::Morph: {
        const auto& m = static_cast<const MorphElement&>(e);
        return m.data ? m.data->bounds : Rect{};
    }
    }
    return {};
}

namespace {

QPolygonF boxPolygon(const Rect& r, const Affine& toWidget)
{
    QPolygonF poly;
    for (Vec2 c : {Vec2{r.x0, r.y0}, Vec2{r.x1, r.y0}, Vec2{r.x1, r.y1}, Vec2{r.x0, r.y1}}) poly << toQPoint(toWidget.map(c));
    return poly;
}

// Move edge end points meeting at `v` (no re-normalisation; for previews).
ShapeGraph moveVertexRaw(const ShapeGraph& g, Vec2 v, Vec2 target, double tol)
{
    ShapeGraph h = g;
    for (GEdge& e : h.edges) {
        const bool straight = e.c.isStraight(1e-9);
        const bool s = distance(e.c.p0, v) <= tol, t = distance(e.c.p3, v) <= tol;
        if (s) {
            e.c.p1 += target - e.c.p0;
            e.c.p0 = target;
        }
        if (t) {
            e.c.p2 += target - e.c.p3;
            e.c.p3 = target;
        }
        if ((s || t) && straight) e.c = Cubic::line(e.c.p0, e.c.p3);
    }
    return h;
}

// Replaces the shape element (merge shape or drawing object) in a doc copy.
bool replaceShape(Editor* ed, Document& d, int layer, int element, ShapeGraph g)
{
    Timeline& tl = ed->mutableTimeline(d);
    if (layer < 0 || layer >= int(tl.layers.size())) return false;
    Keyframe* k = tl.layers[layer].keyAt(ed->frame());
    if (!k || element < 0 || element >= int(k->elements.size())) return false;
    const ShapeElement* s = asShape(k->elements[element]);
    if (!s) return false;
    if (!s->isObject) {
        setKeyframeMergeShape(*k, std::move(g));
        return true;
    }
    auto c = s->cloneAs<ShapeElement>();
    c->graph = std::make_shared<ShapeGraph>(std::move(g));
    k->elements[element] = c;
    return true;
}


} // namespace

// --- SelectionTool --------------------------------------------------------------------

Vec2 SelectionTool::constrained(Vec2 d, Qt::KeyboardModifiers mods) const
{
    if (!(mods & Qt::ShiftModifier)) return d;
    return std::abs(d.x) >= std::abs(d.y) ? Vec2{d.x, 0} : Vec2{0, d.y};
}

bool SelectionTool::onPick(Vec2 p, int layer, const ShapeHit& h) const
{
    const ShapePick& pick = ed->shapePick();
    if (!pick.valid()) return false;
    const Timeline& tl = ed->timeline();
    if (layer < 0 || tl.layers[layer].id != pick.layerId) return false;
    if (pick.region) return pick.region->contains(p);
    if (h.kind == ShapeHit::Kind::Fill) return pick.sel.hasFace(h.face);
    if (h.kind == ShapeHit::Kind::Stroke) return pick.sel.hasEdge(h.arrEdge);
    return false;
}

std::vector<std::pair<Vec2, SelectionTool::HintRef>> SelectionTool::visibleHints() const
{
    std::vector<std::pair<Vec2, HintRef>> out;
    const Layer* l = ed->currentLayer();
    if (!l) return out;
    const int ki = l->keyIndexAt(ed->frame());
    if (ki < 0) return out;
    const Keyframe& k = l->keys[ki];
    if (k.start != ed->frame()) return out;
    if (k.tween == TweenType::Shape)
        for (int i = 0; i < int(k.hints.size()); ++i) out.push_back({k.hints[i].start, {k.start, i, false}});
    if (ki > 0 && l->keys[ki - 1].tween == TweenType::Shape) {
        const Keyframe& prev = l->keys[ki - 1];
        for (int i = 0; i < int(prev.hints.size()); ++i) out.push_back({prev.hints[i].end, {prev.start, i, true}});
    }
    return out;
}

void SelectionTool::press(const ToolEvent& e)
{
    m_start = m_cur = e.pos;
    m_moved = false;
    const double tol = 4.0 * unitsPerPixel();

    // Shape hints first.
    for (const auto& [pos, ref] : visibleHints())
        if (distance(pos, e.pos) <= 8.0 * unitsPerPixel()) {
            m_mode = Mode::Hint;
            m_hint = ref;
            return;
        }

    const StageHit hit = hitStage(ed, e.pos, tol);
    const bool shift = e.mods & Qt::ShiftModifier;
    if (hit.kind == StageHit::Kind::Element) {
        const ElementRef ref{ed->timeline().layers[hit.layer].id, hit.index};
        std::vector<ElementRef> sel = ed->selection();
        const bool selected = std::find(sel.begin(), sel.end(), ref) != sel.end();
        if (shift) {
            if (selected) sel.erase(std::remove(sel.begin(), sel.end(), ref), sel.end());
            else sel.push_back(ref);
            ed->setSelection(sel);
            m_mode = Mode::None;
            return;
        }
        if (!selected) {
            ed->setShapePick({});
            ed->setSelection({ref});
        }
        ed->setLayerIndex(hit.layer);
        m_mode = Mode::MoveElements;
        return;
    }
    if (hit.kind == StageHit::Kind::Shape) {
        const Timeline& tl = ed->timeline();
        const ShapeGraphPtr g = ed->mergeShape(hit.layer);
        ed->setLayerIndex(hit.layer);
        // Whole merge shape selected as an element: move it.
        const ElementRef whole{tl.layers[hit.layer].id, 0};
        const auto& sel = ed->selection();
        if (std::find(sel.begin(), sel.end(), whole) != sel.end()) {
            m_mode = Mode::MoveElements;
            return;
        }
        if (onPick(e.pos, hit.layer, hit.shape)) {
            m_mode = Mode::MoveShape;
            ShapePick pick = ed->shapePick();
            if (pick.region) cutByRegion(*pick.graph, *pick.region, m_rest, m_lifted);
            else liftSelection(*pick.graph, pick.sel, m_rest, m_lifted);
            return;
        }
        if (!shift) {
            // Corner drag / bend of an unselected edge (Animate behaviour).
            Vec2 v;
            if (nearestVertex(*g, e.pos, 5.0 * unitsPerPixel(), v)) {
                m_mode = Mode::Vertex;
                m_layer = hit.layer;
                m_element = 0;
                m_graph = g;
                m_matrix = {};
                m_vertex = v;
                ed->clearSelection();
                return;
            }
            const ShapeHit near = nearestEdge(*g, e.pos, 3.0 * unitsPerPixel());
            if (near.edge >= 0) {
                m_mode = Mode::Bend;
                m_layer = hit.layer;
                m_element = 0;
                m_graph = g;
                m_matrix = {};
                m_edge = near.edge;
                m_t = near.t;
                ed->clearSelection();
                return;
            }
        }
        // Select a fill or a line segment.
        ShapeSelection s = hit.shape.kind == ShapeHit::Kind::Fill ? selectFace(*g, hit.shape.face)
                                                                     : selectStrokeRun(*g, hit.shape.arrEdge);
        ShapePick pick;
        if (shift && ed->shapePick().valid() && ed->shapePick().graph == g && !ed->shapePick().region) pick = ed->shapePick();
        else if (!shift) ed->setSelection({});
        pick.layerId = tl.layers[hit.layer].id;
        pick.graph = g;
        pick.region.reset();
        pick.sel.add(s);
        ed->setShapePick(pick);
        m_mode = Mode::MoveShape;
        liftSelection(*g, pick.sel, m_rest, m_lifted);
        return;
    }
    // Empty space: marquee.
    if (!shift) ed->clearSelection();
    m_mode = Mode::Marquee;
}

void SelectionTool::previewMove(Vec2 delta, bool copy)
{
    Document d = ed->doc();
    Timeline& tl = ed->mutableTimeline(d);
    const Affine t = Affine::translate(delta);
    for (const ElementRef& r : ed->selection()) {
        const int li = tl.layerIndex(r.layerId);
        if (li < 0 || tl.layers[li].locked) continue;
        Keyframe* k = ed->selectionKey(d, li);
        if (!k || r.index < 0 || r.index >= int(k->elements.size())) continue;
        const ElementPtr e = k->elements[r.index];
        ElementPtr moved;
        if (const ShapeElement* s = asShape(e); s && !s->isObject) {
            auto c = s->cloneAs<ShapeElement>();
            c->graph = std::make_shared<ShapeGraph>(s->graph->transformed(t));
            c->isObject = copy; // a copy floats above until dropped
            moved = c;
        } else {
            moved = e->withMatrix(t * e->matrix);
        }
        if (copy) k->elements.push_back(moved);
        else k->elements[r.index] = moved;
    }
    ed->setPreview(std::move(d));
}

void SelectionTool::move(const ToolEvent& e)
{
    m_cur = e.pos;
    if (m_mode == Mode::None) return;
    if (distance(m_cur, m_start) > 1.5 * unitsPerPixel()) m_moved = true;
    const Vec2 delta = constrained(m_cur - m_start, e.mods);
    switch (m_mode) {
    case Mode::Marquee: update(); break;
    case Mode::MoveElements:
        if (m_moved) previewMove(delta, e.mods & Qt::AltModifier);
        break;
    case Mode::MoveShape: {
        if (!m_moved) break;
        Document d = ed->doc();
        Timeline& tl = ed->mutableTimeline(d);
        const int li = tl.layerIndex(ed->shapePick().layerId);
        Keyframe* k = li >= 0 ? tl.layers[li].keyAt(ed->frame()) : nullptr;
        if (k) {
            if (!(e.mods & Qt::AltModifier)) setKeyframeMergeShape(*k, m_rest);
            k->elements.push_back(makeShapeElement(m_lifted.transformed(Affine::translate(delta)), true));
            ed->setPreview(std::move(d));
        }
        break;
    }
    case Mode::Bend: {
        ShapeGraph g = *m_graph;
        g.edges[m_edge].c = m_graph->edges[m_edge].c.bentThrough(m_t, m_matrix.inverted().map(m_cur));
        Document d = ed->doc();
        if (replaceShape(ed, d, m_layer, m_element, std::move(g))) ed->setPreview(std::move(d));
        break;
    }
    case Mode::Vertex: {
        Document d = ed->doc();
        const Vec2 target = m_matrix.inverted().map(m_vertex + delta);
        if (replaceShape(ed, d, m_layer, m_element, moveVertexRaw(*m_graph, m_vertex, target, 1e-6)))
            ed->setPreview(std::move(d));
        break;
    }
    case Mode::Hint: update(); break;
    case Mode::None: break;
    }
}

void SelectionTool::release(const ToolEvent& e)
{
    const Mode mode = m_mode;
    m_mode = Mode::None;
    m_cur = e.pos;
    const Vec2 delta = constrained(m_cur - m_start, e.mods);
    switch (mode) {
    case Mode::Marquee: {
        const Rect r = Rect::fromPoints(m_start, m_cur);
        update();
        if (r.width() < unitsPerPixel() * 2 && r.height() < unitsPerPixel() * 2) break;
        const Region region = Region::rect(r);
        std::vector<ElementRef> sel = (e.mods & Qt::ShiftModifier) ? ed->selection() : std::vector<ElementRef>{};
        ShapePick pick;
        const Timeline& tl = ed->timeline();
        for (int li = 0; li < int(tl.layers.size()); ++li) {
            const Layer& l = tl.layers[li];
            if (!l.visible || l.locked || l.type == LayerType::Folder) continue;
            const Keyframe* k = l.keyAt(ed->frame());
            if (!k) continue;
            for (int i = 0; i < int(k->elements.size()); ++i) {
                const ElementPtr& el = k->elements[i];
                const Rect b = elementBounds(ed->doc(), *el);
                if (!b.intersects(r)) continue;
                if (const ShapeElement* s = asShape(el); s && !s->isObject && i == 0) {
                    if (r.contains(b)) {
                        sel.push_back({l.id, 0});
                    } else if (!pick.valid() || li == ed->layerIndex()) {
                        ShapePick p;
                        p.layerId = l.id;
                        p.graph = s->graph;
                        p.region = region;
                        ShapeGraph rest, lifted;
                        cutByRegion(*s->graph, region, rest, lifted);
                        if (!lifted.isEmpty()) pick = p;
                    }
                    continue;
                }
                const ElementRef ref{l.id, i};
                if (std::find(sel.begin(), sel.end(), ref) == sel.end()) sel.push_back(ref);
            }
        }
        ed->setShapePick(pick);
        ed->setSelection(sel);
        break;
    }
    case Mode::MoveElements: {
        if (!m_moved) {
            ed->setPreview(std::nullopt);
            break;
        }
        if (e.mods & Qt::AltModifier) {
            ed->setPreview(std::nullopt);
            ed->copySelection();
            // Paste the copies in place, then move them.
            ed->paste(true);
            ed->transformSelection(Affine::translate(delta), QObject::tr("Duplicate"));
        } else {
            ed->transformSelection(Affine::translate(delta), QObject::tr("Move"));
        }
        break;
    }
    case Mode::MoveShape: {
        if (!m_moved) {
            ed->setPreview(std::nullopt);
            break;
        }
        if (e.mods & Qt::AltModifier) {
            const ShapePick pick = ed->shapePick();
            const ShapeGraph copy = m_lifted.transformed(Affine::translate(delta));
            ed->edit(QObject::tr("Duplicate"), [&](Document& d) {
                Timeline& tl = ed->mutableTimeline(d);
                const int li = tl.layerIndex(pick.layerId);
                Keyframe* k = li >= 0 ? tl.layers[li].keyAt(ed->frame()) : nullptr;
                if (!k) return false;
                mergeIntoKeyframe(*k, copy);
                return true;
            });
            ed->clearSelection();
        } else {
            ed->transformSelection(Affine::translate(delta), QObject::tr("Move"));
        }
        break;
    }
    case Mode::Bend: {
        if (!m_moved) {
            ed->setPreview(std::nullopt);
            break;
        }
        const ShapeGraph g = bendEdge(*m_graph, m_edge, m_t, m_matrix.inverted().map(m_cur));
        ed->edit(QObject::tr("Reshape"), [&](Document& d) { return replaceShape(ed, d, m_layer, m_element, g); });
        break;
    }
    case Mode::Vertex: {
        if (!m_moved) {
            ed->setPreview(std::nullopt);
            break;
        }
        const ShapeGraph g = moveVertex(*m_graph, m_vertex, m_matrix.inverted().map(m_vertex + delta), 1e-6);
        ed->edit(QObject::tr("Move Corner"), [&](Document& d) { return replaceShape(ed, d, m_layer, m_element, g); });
        break;
    }
    case Mode::Hint: {
        const HintRef h = m_hint;
        const int li = ed->layerIndex();
        const Vec2 p = m_cur;
        ed->edit(QObject::tr("Move Shape Hint"), [&](Document& d) {
            Keyframe* k = ed->mutableTimeline(d).layers[li].keyAt(h.keyStart);
            if (!k || h.index >= int(k->hints.size())) return false;
            (h.end ? k->hints[h.index].end : k->hints[h.index].start) = p;
            return true;
        });
        break;
    }
    case Mode::None: break;
    }
    m_graph.reset();
    m_rest = {};
    m_lifted = {};
    update();
}

void SelectionTool::hover(const ToolEvent& e)
{
    const double tol = 4.0 * unitsPerPixel();
    HoverKind k = HoverKind::None;
    for (const auto& [pos, ref] : visibleHints())
        if (distance(pos, e.pos) <= 8.0 * unitsPerPixel()) k = HoverKind::Hint;
    if (k == HoverKind::None) {
        const StageHit hit = hitStage(ed, e.pos, tol);
        if (hit.kind == StageHit::Kind::Element) {
            k = HoverKind::Move;
        } else if (hit.kind == StageHit::Kind::Shape) {
            const ShapeGraphPtr g = ed->mergeShape(hit.layer);
            Vec2 v;
            if (onPick(e.pos, hit.layer, hit.shape)) k = HoverKind::Move;
            else if (g && nearestVertex(*g, e.pos, 5.0 * unitsPerPixel(), v)) k = HoverKind::Corner;
            else if (g && nearestEdge(*g, e.pos, 3.0 * unitsPerPixel()).edge >= 0) k = HoverKind::Bend;
            else k = HoverKind::Move;
        }
    }
    if (k != m_hover) {
        m_hover = k;
        view->refreshCursor();
    }
}

void SelectionTool::doubleClick(const ToolEvent& e)
{
    const double tol = 4.0 * unitsPerPixel();
    const StageHit hit = hitStage(ed, e.pos, tol);
    if (hit.kind == StageHit::Kind::Element) {
        const Timeline& tl = ed->timeline();
        const Keyframe* k = tl.layers[hit.layer].keyAt(ed->frame());
        const ElementPtr el = k && hit.index < int(k->elements.size()) ? k->elements[hit.index] : nullptr;
        if (el && el->type() == ElementType::Instance) {
            ed->enterInstance(hit.layer, hit.index);
        } else if (el) {
            ed->notify(QObject::tr("Ungroup (Ctrl+Shift+G) or Break Apart (Ctrl+B) to edit its shapes"));
        }
        return;
    }
    if (hit.kind == StageHit::Kind::Shape) {
        const ShapeGraphPtr g = ed->mergeShape(hit.layer);
        ShapePick pick;
        pick.layerId = ed->timeline().layers[hit.layer].id;
        pick.graph = g;
        pick.sel = selectConnected(*g, hit.shape);
        ed->setSelection({});
        ed->setShapePick(pick);
        return;
    }
    if (ed->inSymbol()) ed->exitContext();
}

void SelectionTool::cancel()
{
    if (m_mode != Mode::None) ed->setPreview(std::nullopt);
    m_mode = Mode::None;
    update();
}

void SelectionTool::paint(QPainter& p)
{
    const ui::Palette& pal = ui::Theme::p();
    if (m_mode == Mode::Marquee) {
        p.save();
        const QRectF r = QRectF(toWidget(m_start), toWidget(m_cur)).normalized();
        p.setPen(QPen(pal.selection, 1.0, Qt::DashLine));
        p.setBrush(ui::withAlpha(pal.selection, 24));
        p.drawRect(r);
        p.restore();
    }
    // Shape hints (a, b, c ...).
    const auto hints = visibleHints();
    if (!hints.empty()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(ui::Theme::ui(11, QFont::Bold));
        for (const auto& [pos, ref] : hints) {
            Vec2 at = pos;
            if (m_mode == Mode::Hint && ref.index == m_hint.index && ref.end == m_hint.end) at = m_cur;
            const QPointF w = toWidget(at);
            p.setPen(QPen(QColor(0, 0, 0, 90), 1));
            p.setBrush(ref.end ? pal.mint : pal.yellow);
            p.drawEllipse(w, 8, 8);
            p.setPen(QColor(20, 20, 20));
            p.drawText(QRectF(w.x() - 8, w.y() - 8, 16, 16), Qt::AlignCenter, QString(QChar('a' + ref.index)));
        }
        p.restore();
    }
}

QCursor SelectionTool::cursor() const
{
    switch (m_hover) {
    case HoverKind::Move: return Qt::SizeAllCursor;
    case HoverKind::Bend: return view->toolCursor("bend");
    case HoverKind::Corner: return view->toolCursor("corner");
    case HoverKind::Hint: return Qt::PointingHandCursor;
    case HoverKind::None: break;
    }
    return Qt::ArrowCursor;
}

// --- SubselectTool --------------------------------------------------------------------------

void SubselectTool::activate()
{
    m_layer = -1;
    m_element = -1;
    m_hasVertex = false;
}

ShapeGraphPtr SubselectTool::targetGraph() const
{
    const Timeline& tl = ed->timeline();
    if (m_layer < 0 || m_layer >= int(tl.layers.size())) return nullptr;
    const Keyframe* k = tl.layers[m_layer].keyAt(ed->frame());
    if (!k || m_element < 0 || m_element >= int(k->elements.size())) return nullptr;
    const ShapeElement* s = asShape(k->elements[m_element]);
    return s ? s->graph : nullptr;
}

bool SubselectTool::pickTarget(Vec2 p)
{
    const StageHit hit = hitStage(ed, p, 4.0 * unitsPerPixel());
    if (hit.kind == StageHit::Kind::None) return false;
    const Keyframe* k = ed->timeline().layers[hit.layer].keyAt(ed->frame());
    if (!k || hit.index >= int(k->elements.size())) return false;
    const ShapeElement* s = asShape(k->elements[hit.index]);
    if (!s) {
        ed->setShapePick({});
        ed->setSelection({{ed->timeline().layers[hit.layer].id, hit.index}});
        m_layer = -1;
        return false;
    }
    m_layer = hit.layer;
    m_element = hit.index;
    m_matrix = s->matrix;
    ed->setLayerIndex(hit.layer);
    ed->clearSelection();
    return true;
}

void SubselectTool::press(const ToolEvent& e)
{
    const double tol = 5.0 * unitsPerPixel();
    ShapeGraphPtr g = targetGraph();
    if (g) {
        const Vec2 local = m_matrix.inverted().map(e.pos);
        const double ltol = tol / std::max(1e-9, m_matrix.meanScale());
        // Handles of the edges around the selected vertex.
        if (m_hasVertex) {
            for (int i = 0; i < int(g->edges.size()); ++i) {
                const Cubic& c = g->edges[i].c;
                if (c.isStraight(1e-9)) continue;
                if (distance(c.p0, m_vertex) < 1e-9 && distance(c.p1, local) <= ltol) {
                    m_drag = Drag::Handle;
                    m_edge = i;
                    m_handle = 1;
                    m_work = *g;
                    return;
                }
                if (distance(c.p3, m_vertex) < 1e-9 && distance(c.p2, local) <= ltol) {
                    m_drag = Drag::Handle;
                    m_edge = i;
                    m_handle = 2;
                    m_work = *g;
                    return;
                }
            }
        }
        Vec2 v;
        if (nearestVertex(*g, local, ltol, v)) {
            m_vertex = v;
            m_hasVertex = true;
            m_drag = Drag::Vertex;
            m_dragPos = v;
            m_work = *g;
            update();
            return;
        }
    }
    m_hasVertex = false;
    if (!pickTarget(e.pos)) {
        m_layer = -1;
        ed->clearSelection();
    } else if (ShapeGraphPtr ng = targetGraph()) {
        Vec2 v;
        const Vec2 local = m_matrix.inverted().map(e.pos);
        if (nearestVertex(*ng, local, tol / std::max(1e-9, m_matrix.meanScale()), v)) {
            m_vertex = v;
            m_hasVertex = true;
            m_drag = Drag::Vertex;
            m_dragPos = v;
            m_work = *ng;
        }
    }
    update();
}

void SubselectTool::preview(const ShapeGraph& g)
{
    Document d = ed->doc();
    if (replaceShape(ed, d, m_layer, m_element, g)) ed->setPreview(std::move(d));
}

void SubselectTool::move(const ToolEvent& e)
{
    if (m_drag == Drag::None) return;
    ShapeGraphPtr g = targetGraph();
    if (!g) return;
    const Vec2 local = m_matrix.inverted().map(e.pos);
    if (m_drag == Drag::Vertex) {
        m_work = moveVertexRaw(*g, m_vertex, local, 1e-9);
        m_dragPos = local;
    } else {
        m_work = *g;
        Cubic& c = m_work.edges[m_edge].c;
        const Vec2 anchor = m_handle == 1 ? c.p0 : c.p3;
        const Vec2 oldDir = (m_handle == 1 ? c.p1 : c.p2) - anchor;
        if (m_handle == 1) c.p1 = local;
        else c.p2 = local;
        // Keep smooth points smooth (Alt breaks the tangent).
        if (!(e.mods & Qt::AltModifier) && oldDir.lengthSq() > 0) {
            const Vec2 newDir = local - anchor;
            for (int i = 0; i < int(m_work.edges.size()); ++i) {
                if (i == m_edge) continue;
                Cubic& o = m_work.edges[i].c;
                Vec2* h = nullptr;
                if (distance(o.p0, anchor) < 1e-9) h = &o.p1;
                else if (distance(o.p3, anchor) < 1e-9) h = &o.p2;
                if (!h) continue;
                const Vec2 od = *h - anchor;
                if (od.lengthSq() == 0) continue;
                if (dot(od.normalized(), oldDir.normalized()) < -0.995) {
                    *h = anchor - newDir.normalized() * od.length();
                }
            }
        }
    }
    preview(m_work);
    update();
}

void SubselectTool::commit(const ShapeGraph& g, const QString& label)
{
    ed->edit(label, [&](Document& d) { return replaceShape(ed, d, m_layer, m_element, normalizeGraph(g)); });
}

void SubselectTool::release(const ToolEvent&)
{
    if (m_drag == Drag::None) return;
    const Drag drag = m_drag;
    m_drag = Drag::None;
    commit(m_work, drag == Drag::Vertex ? QObject::tr("Move Point") : QObject::tr("Move Handle"));
    if (drag == Drag::Vertex) m_vertex = m_dragPos;
    update();
}

void SubselectTool::cancel()
{
    if (m_drag != Drag::None) ed->setPreview(std::nullopt);
    m_drag = Drag::None;
}

void SubselectTool::paint(QPainter& p)
{
    ShapeGraphPtr g = targetGraph();
    if (!g) return;
    const ShapeGraph& shown = m_drag != Drag::None ? m_work : *g;
    const ui::Palette& pal = ui::Theme::p();
    const Affine toW = view->timelineToWidget() * m_matrix;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    for (const GEdge& e : shown.edges) appendChain(path, {e.c}, false);
    p.setPen(QPen(pal.selection, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(toQTransform(toW).map(path));
    const Vec2 sel = m_drag == Drag::Vertex ? m_dragPos : m_vertex;
    for (const GEdge& e : shown.edges) {
        for (Vec2 v : {e.c.p0, e.c.p3}) {
            const QPointF w = toQPoint(toW.map(v));
            const bool isSel = m_hasVertex && distance(v, sel) < 1e-9;
            p.setPen(QPen(pal.selection, 1.2));
            p.setBrush(isSel ? pal.selection : pal.bg2);
            p.drawRect(QRectF(w.x() - 3, w.y() - 3, 6, 6));
        }
        if (!m_hasVertex || e.c.isStraight(1e-9)) continue;
        auto handle = [&](Vec2 anchor, Vec2 h) {
            const QPointF a = toQPoint(toW.map(anchor)), b = toQPoint(toW.map(h));
            p.setPen(QPen(pal.accent, 1.0));
            p.drawLine(a, b);
            p.setBrush(pal.accent);
            p.drawEllipse(b, 3, 3);
        };
        if (distance(e.c.p0, sel) < 1e-9) handle(e.c.p0, e.c.p1);
        if (distance(e.c.p3, sel) < 1e-9) handle(e.c.p3, e.c.p2);
    }
    p.restore();
}

// --- FreeTransformTool -------------------------------------------------------------------------

FreeTransformTool::Box FreeTransformTool::computeBox() const
{
    Box b;
    const auto els = ed->selectedElements();
    if (els.size() == 1 && !ed->shapePick().valid()) {
        const ElementPtr& e = els.front();
        const ShapeElement* s = asShape(e);
        if (s && !s->isObject) {
            b.local = s->graph->bounds(true);
            b.toTimeline = {};
            b.pivot = b.local.center();
        } else {
            b.local = localBoundsOf(ed->doc(), *e);
            b.toTimeline = e->matrix;
            b.pivot = e->matrix.map(e->pivot);
        }
        b.valid = !b.local.isEmpty();
        return b;
    }
    b.local = ed->selectionBounds();
    b.toTimeline = {};
    b.pivot = b.local.center();
    b.valid = !b.local.isEmpty();
    return b;
}

FreeTransformTool::Handle FreeTransformTool::handleAt(QPointF w, const Box& b) const
{
    if (!b.valid) return Handle::None;
    const Affine toW = view->timelineToWidget() * b.toTimeline;
    const Rect& r = b.local;
    const Vec2 pts[8] = {{r.x0, r.y0}, {(r.x0 + r.x1) / 2, r.y0}, {r.x1, r.y0}, {r.x1, (r.y0 + r.y1) / 2},
                         {r.x1, r.y1}, {(r.x0 + r.x1) / 2, r.y1}, {r.x0, r.y1}, {r.x0, (r.y0 + r.y1) / 2}};
    const Handle hs[8] = {Handle::TL, Handle::T, Handle::TR, Handle::R, Handle::BR, Handle::B, Handle::BL, Handle::L};
    const QPointF pivot = toQPoint(view->timelineToWidget().map(b.pivot));
    if (QLineF(pivot, w).length() <= 7) return Handle::Pivot;
    for (int i = 0; i < 8; ++i)
        if (QLineF(toQPoint(toW.map(pts[i])), w).length() <= 6) return hs[i];
    // Rotation zone just outside the corners.
    for (int i : {0, 2, 4, 6})
        if (QLineF(toQPoint(toW.map(pts[i])), w).length() <= 18) return Handle::Rotate;
    // Skew along the edges.
    const Vec2 local = toW.inverted().map(fromQPoint(w));
    const double tolx = 5.0 / std::max(1e-9, toW.meanScale());
    if (local.x > r.x0 && local.x < r.x1) {
        if (std::abs(local.y - r.y0) <= tolx) return Handle::SkewT;
        if (std::abs(local.y - r.y1) <= tolx) return Handle::SkewB;
    }
    if (local.y > r.y0 && local.y < r.y1) {
        if (std::abs(local.x - r.x0) <= tolx) return Handle::SkewL;
        if (std::abs(local.x - r.x1) <= tolx) return Handle::SkewR;
        if (local.x > r.x0 && local.x < r.x1) return Handle::Move;
    }
    return Handle::None;
}

Affine FreeTransformTool::currentTransform(Vec2 pos, Qt::KeyboardModifiers mods) const
{
    const Box& b = m_box;
    const Affine B = b.toTimeline, Bi = B.inverted();
    const Rect& r = b.local;
    const Vec2 ml = Bi.map(pos), sl = Bi.map(m_start);
    const bool alt = mods & Qt::AltModifier, shift = mods & Qt::ShiftModifier;
    const Vec2 pivotL = Bi.map(b.pivot);
    auto scaleAbout = [&](Vec2 anchor, Vec2 handle, bool sx, bool sy) {
        double kx = 1, ky = 1;
        if (sx && std::abs(handle.x - anchor.x) > 1e-12) kx = (ml.x - anchor.x) / (handle.x - anchor.x);
        if (sy && std::abs(handle.y - anchor.y) > 1e-12) ky = (ml.y - anchor.y) / (handle.y - anchor.y);
        if (shift && sx && sy) {
            const double m = std::max(std::abs(kx), std::abs(ky));
            kx = std::copysign(m, kx);
            ky = std::copysign(m, ky);
        }
        return B * Affine::about(anchor, Affine::scale(kx, ky)) * Bi;
    };
    switch (m_handle) {
    case Handle::Move: {
        Vec2 d = pos - m_start;
        if (shift) d = std::abs(d.x) >= std::abs(d.y) ? Vec2{d.x, 0} : Vec2{0, d.y};
        return Affine::translate(d);
    }
    case Handle::Rotate: {
        double a = (pos - b.pivot).angle() - (m_start - b.pivot).angle();
        if (shift) a = std::round(a / (kPi / 12)) * (kPi / 12);
        return Affine::about(b.pivot, Affine::rotate(a));
    }
    case Handle::TL: return scaleAbout(alt ? pivotL : Vec2{r.x1, r.y1}, {r.x0, r.y0}, true, true);
    case Handle::TR: return scaleAbout(alt ? pivotL : Vec2{r.x0, r.y1}, {r.x1, r.y0}, true, true);
    case Handle::BR: return scaleAbout(alt ? pivotL : Vec2{r.x0, r.y0}, {r.x1, r.y1}, true, true);
    case Handle::BL: return scaleAbout(alt ? pivotL : Vec2{r.x1, r.y0}, {r.x0, r.y1}, true, true);
    case Handle::T: return scaleAbout(alt ? pivotL : Vec2{r.x0, r.y1}, {r.x0, r.y0}, false, true);
    case Handle::B: return scaleAbout(alt ? pivotL : Vec2{r.x0, r.y0}, {r.x0, r.y1}, false, true);
    case Handle::L: return scaleAbout(alt ? pivotL : Vec2{r.x1, r.y0}, {r.x0, r.y0}, true, false);
    case Handle::R: return scaleAbout(alt ? pivotL : Vec2{r.x0, r.y0}, {r.x1, r.y0}, true, false);
    case Handle::SkewT:
    case Handle::SkewB: {
        const double h = std::max(1e-9, r.height());
        const double k = (ml.x - sl.x) / h * (m_handle == Handle::SkewT ? -1.0 : 1.0);
        const Vec2 anchor = alt ? pivotL : Vec2{r.x0, m_handle == Handle::SkewT ? r.y1 : r.y0};
        return B * Affine::about(anchor, Affine(1, 0, k, 1, 0, 0)) * Bi;
    }
    case Handle::SkewL:
    case Handle::SkewR: {
        const double w = std::max(1e-9, r.width());
        const double k = (ml.y - sl.y) / w * (m_handle == Handle::SkewL ? -1.0 : 1.0);
        const Vec2 anchor = alt ? pivotL : Vec2{m_handle == Handle::SkewL ? r.x1 : r.x0, r.y0};
        return B * Affine::about(anchor, Affine(1, k, 0, 1, 0, 0)) * Bi;
    }
    default: break;
    }
    return {};
}

void FreeTransformTool::preview(const Affine& t)
{
    Document d = ed->doc();
    Timeline& tl = ed->mutableTimeline(d);
    for (const ElementRef& r : ed->selection()) {
        const int li = tl.layerIndex(r.layerId);
        Keyframe* k = ed->selectionKey(d, li);
        if (!k || r.index < 0 || r.index >= int(k->elements.size())) continue;
        ElementPtr& e = k->elements[r.index];
        if (const ShapeElement* s = asShape(e); s && !s->isObject) {
            auto c = s->cloneAs<ShapeElement>();
            c->graph = std::make_shared<ShapeGraph>(s->graph->transformed(t));
            e = c;
        } else {
            e = e->withMatrix(t * e->matrix);
        }
    }
    const ShapePick& pick = ed->shapePick();
    if (pick.valid()) {
        const int li = tl.layerIndex(pick.layerId);
        Keyframe* k = li >= 0 ? tl.layers[li].keyAt(ed->frame()) : nullptr;
        ShapeGraph rest, lifted;
        if (k) {
            if (pick.region) cutByRegion(*pick.graph, *pick.region, rest, lifted);
            else liftSelection(*pick.graph, pick.sel, rest, lifted);
            setKeyframeMergeShape(*k, rest);
            k->elements.push_back(makeShapeElement(lifted.transformed(t), true));
        }
    }
    ed->setPreview(std::move(d));
}

void FreeTransformTool::press(const ToolEvent& e)
{
    m_box = computeBox();
    m_handle = handleAt(e.widget, m_box);
    m_start = e.pos;
    m_current = {};
    if (m_handle == Handle::None) {
        // Click to select like the Selection tool.
        const StageHit hit = hitStage(ed, e.pos, 4.0 * unitsPerPixel());
        if (hit.kind == StageHit::Kind::Element) {
            ed->setShapePick({});
            ed->setSelection({{ed->timeline().layers[hit.layer].id, hit.index}});
        } else if (hit.kind == StageHit::Kind::Shape) {
            ed->setShapePick({});
            ed->setSelection({{ed->timeline().layers[hit.layer].id, 0}});
        } else {
            ed->clearSelection();
            return;
        }
        m_box = computeBox();
        m_handle = m_box.valid ? Handle::Move : Handle::None;
    }
    if (m_handle == Handle::Pivot) m_pivotDrag = m_box.pivot;
    update();
}

void FreeTransformTool::move(const ToolEvent& e)
{
    if (m_handle == Handle::None) return;
    if (m_handle == Handle::Pivot) {
        m_pivotDrag = e.pos;
        update();
        return;
    }
    m_current = currentTransform(e.pos, e.mods);
    preview(m_current);
    update();
}

void FreeTransformTool::release(const ToolEvent& e)
{
    const Handle h = m_handle;
    if (h == Handle::None) return;
    // The final transform depends on the handle being dragged: compute it
    // before the drag state is cleared.
    const Affine t = h == Handle::Pivot ? Affine{} : currentTransform(e.pos, e.mods);
    m_handle = Handle::None;
    if (h == Handle::Pivot) {
        const auto els = ed->selectedElements();
        if (els.size() == 1) {
            const Vec2 local = els.front()->matrix.inverted().map(m_pivotDrag);
            const ElementRef ref = ed->selection().front();
            ed->edit(QObject::tr("Move Transformation Point"), [&](Document& d) {
                Timeline& tl = ed->mutableTimeline(d);
                const int li = tl.layerIndex(ref.layerId);
                Keyframe* k = ed->selectionKey(d, li);
                if (!k || ref.index >= int(k->elements.size())) return false;
                auto c = k->elements[ref.index]->clone();
                c->pivot = local;
                k->elements[ref.index] = c;
                return true;
            });
            ed->setSelection({ref});
        }
        update();
        return;
    }
    ed->setPreview(std::nullopt);
    if (!(t == Affine{})) ed->transformSelection(t, QObject::tr("Free Transform"));
    update();
}

void FreeTransformTool::hover(const ToolEvent& e)
{
    const Handle h = handleAt(e.widget, computeBox());
    if (h != m_hoverHandle) {
        m_hoverHandle = h;
        view->refreshCursor();
    }
}

void FreeTransformTool::cancel()
{
    if (m_handle != Handle::None) ed->setPreview(std::nullopt);
    m_handle = Handle::None;
}

QCursor FreeTransformTool::cursor() const
{
    switch (m_hoverHandle) {
    case Handle::TL:
    case Handle::BR: return Qt::SizeFDiagCursor;
    case Handle::TR:
    case Handle::BL: return Qt::SizeBDiagCursor;
    case Handle::T:
    case Handle::B: return Qt::SizeVerCursor;
    case Handle::L:
    case Handle::R: return Qt::SizeHorCursor;
    case Handle::Rotate: return view->toolCursor("rotate");
    case Handle::SkewT:
    case Handle::SkewB: return Qt::SplitHCursor;
    case Handle::SkewL:
    case Handle::SkewR: return Qt::SplitVCursor;
    case Handle::Move: return Qt::SizeAllCursor;
    case Handle::Pivot: return Qt::PointingHandCursor;
    case Handle::None: break;
    }
    return Qt::ArrowCursor;
}

void FreeTransformTool::paint(QPainter& p)
{
    Box b = m_handle != Handle::None ? m_box : computeBox();
    if (!b.valid) return;
    const ui::Palette& pal = ui::Theme::p();
    Affine toW = view->timelineToWidget() * (m_handle != Handle::None && m_handle != Handle::Pivot ? m_current : Affine{}) * b.toTimeline;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QPolygonF poly = boxPolygon(b.local, toW);
    p.setPen(QPen(pal.selection, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(poly);
    const Rect& r = b.local;
    const Vec2 pts[8] = {{r.x0, r.y0}, {(r.x0 + r.x1) / 2, r.y0}, {r.x1, r.y0}, {r.x1, (r.y0 + r.y1) / 2},
                         {r.x1, r.y1}, {(r.x0 + r.x1) / 2, r.y1}, {r.x0, r.y1}, {r.x0, (r.y0 + r.y1) / 2}};
    for (const Vec2& v : pts) {
        const QPointF w = toQPoint(toW.map(v));
        p.setPen(QPen(pal.selection, 1.2));
        p.setBrush(pal.bg2);
        p.drawRect(QRectF(w.x() - 3.5, w.y() - 3.5, 7, 7));
    }
    const bool transforming = m_handle != Handle::None && m_handle != Handle::Pivot;
    const Vec2 pivot = m_handle == Handle::Pivot ? m_pivotDrag : (transforming ? m_current.map(b.pivot) : b.pivot);
    const QPointF pw = toQPoint(view->timelineToWidget().map(pivot));
    p.setPen(QPen(pal.selection, 1.5));
    p.setBrush(QColor(255, 255, 255));
    p.drawEllipse(pw, 4.5, 4.5);
    p.restore();
}

// --- LassoTool -----------------------------------------------------------------------------------

void LassoTool::press(const ToolEvent& e)
{
    m_points = {e.pos};
    if (!(e.mods & Qt::ShiftModifier)) ed->clearSelection();
}

void LassoTool::move(const ToolEvent& e)
{
    if (m_points.empty()) return;
    if (distance(m_points.back(), e.pos) > unitsPerPixel()) m_points.push_back(e.pos);
    update();
}

void LassoTool::release(const ToolEvent&)
{
    if (m_points.size() < 3) {
        m_points.clear();
        update();
        return;
    }
    const Region region = normalizeRegion(Region::polygon(m_points));
    m_points.clear();
    update();
    if (region.isEmpty()) return;
    const Rect rb = region.bounds();
    std::vector<ElementRef> sel;
    ShapePick pick;
    const Timeline& tl = ed->timeline();
    for (int li = 0; li < int(tl.layers.size()); ++li) {
        const Layer& l = tl.layers[li];
        if (!l.visible || l.locked || l.type == LayerType::Folder) continue;
        const Keyframe* k = l.keyAt(ed->frame());
        if (!k) continue;
        for (int i = 0; i < int(k->elements.size()); ++i) {
            const ElementPtr& el = k->elements[i];
            const Rect b = elementBounds(ed->doc(), *el);
            if (!b.intersects(rb)) continue;
            if (const ShapeElement* s = asShape(el); s && !s->isObject && i == 0) {
                if (!pick.valid() || li == ed->layerIndex()) {
                    ShapeGraph rest, lifted;
                    cutByRegion(*s->graph, region, rest, lifted);
                    if (!lifted.isEmpty()) {
                        pick.layerId = l.id;
                        pick.graph = s->graph;
                        pick.region = region;
                    }
                }
                continue;
            }
            if (region.contains(b.center())) sel.push_back({l.id, i});
        }
    }
    ed->setShapePick(pick);
    ed->setSelection(sel);
}

void LassoTool::paint(QPainter& p)
{
    if (m_points.size() < 2) return;
    QPolygonF poly;
    for (const Vec2& v : m_points) poly << toWidget(v);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(ui::Theme::p().selection, 1.0, Qt::DashLine));
    p.setBrush(ui::withAlpha(ui::Theme::p().selection, 24));
    p.drawPolygon(poly);
    p.restore();
}

} // namespace vx::app
