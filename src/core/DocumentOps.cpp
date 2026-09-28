// SPDX-License-Identifier: GPL-3.0-or-later
#include "DocumentOps.h"
#include "Evaluate.h"
#include "ShapeTween.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace vx {

ShapeGraph keyframeMergeShape(const Keyframe& k)
{
    if (!k.elements.empty())
        if (const ShapeElement* s = asShape(k.elements.front()); s && !s->isObject && s->graph) return *s->graph;
    return {};
}

void setKeyframeMergeShape(Keyframe& k, ShapeGraph g)
{
    const bool has = !k.elements.empty() && asShape(k.elements.front()) && !asShape(k.elements.front())->isObject;
    if (g.isEmpty()) {
        if (has) k.elements.erase(k.elements.begin());
        return;
    }
    auto e = makeShapeElement(std::move(g), false);
    if (has) k.elements.front() = e;
    else k.elements.insert(k.elements.begin(), e);
}

void mergeIntoKeyframe(Keyframe& k, const ShapeGraph& g, const OverlayOptions& opt)
{
    setKeyframeMergeShape(k, overlay(keyframeMergeShape(k), g, opt));
}

Vec2 registrationPoint(const Rect& b, int grid)
{
    if (b.isEmpty()) return {};
    grid = std::clamp(grid, 0, 8);
    const int row = grid / 3, col = grid % 3;
    return {b.x0 + b.width() * col * 0.5, b.y0 + b.height() * row * 0.5};
}

namespace {

ElementPtr transformElement(const ElementPtr& e, const Affine& m)
{
    if (const ShapeElement* s = asShape(e); s && !s->isObject) {
        auto c = s->cloneAs<ShapeElement>();
        c->graph = std::make_shared<ShapeGraph>(s->graph->transformed(m));
        c->pivot = m.map(s->pivot);
        return c;
    }
    auto c = e->clone();
    c->matrix = m * e->matrix;
    return c;
}

ElementPtr colorElement(const ElementPtr& e, const ColorTransform& ct)
{
    if (ct.isIdentity()) return e;
    switch (e->type()) {
    case ElementType::Shape: {
        auto c = e->cloneAs<ShapeElement>();
        c->graph = std::make_shared<ShapeGraph>(c->graph->withColorTransform(ct));
        return c;
    }
    case ElementType::Instance: {
        auto c = e->cloneAs<InstanceElement>();
        c->color.advanced = ct * c->color.toTransform();
        c->color.kind = ColorEffect::Kind::Advanced;
        return c;
    }
    case ElementType::Group: {
        auto c = e->cloneAs<GroupElement>();
        for (ElementPtr& ch : c->children) ch = colorElement(ch, ct);
        return c;
    }
    case ElementType::Morph: return e;
    }
    return e;
}

Rect boundsOf(const Document& doc, const std::vector<ElementPtr>& elements)
{
    Rect r;
    for (const ElementPtr& e : elements) r.include(elementBounds(doc, *e));
    return r;
}

void forEachTimeline(Document& doc, const std::function<void(Timeline&)>& fn)
{
    for (Timeline& t : doc.scenes) fn(t);
    for (Symbol& s : doc.symbols) fn(s.timeline);
}

bool containsShape(const Keyframe& k)
{
    for (const ElementPtr& e : k.elements)
        if (e->type() == ElementType::Shape) return true;
    return false;
}

int tweenSymbolCounter(const Document& doc)
{
    int n = 1;
    while (doc.symbolByName("Tween " + std::to_string(n))) ++n;
    return n;
}

/// The one drawing of a keyframe (its merge shape or a single drawing
/// object), in timeline space.
std::optional<ShapeGraph> soleDrawing(const Keyframe& k)
{
    if (k.elements.size() != 1) return std::nullopt;
    const ShapeElement* s = asShape(k.elements.front());
    if (!s || !s->graph || s->graph->isEmpty()) return std::nullopt;
    return s->matrix.isIdentity() ? *s->graph : s->graph->transformed(s->matrix);
}

/// Affine map through three point pairs (none if the points are collinear).
std::optional<Affine> mapThrough(const Vec2 p[3], const Vec2 q[3])
{
    const Affine from(p[1].x - p[0].x, p[1].y - p[0].y, p[2].x - p[0].x, p[2].y - p[0].y, p[0].x, p[0].y);
    const Affine to(q[1].x - q[0].x, q[1].y - q[0].y, q[2].x - q[0].x, q[2].y - q[0].y, q[0].x, q[0].y);
    if (std::abs(from.det()) < 1e-9) return std::nullopt;
    return to * from.inverted();
}

bool sameLooks(const ShapeGraph& a, const ShapeGraph& b)
{
    if (a.fills.size() != b.fills.size() || a.strokes.size() != b.strokes.size()) return false;
    for (size_t i = 0; i < a.fills.size(); ++i)
        if (a.fills[i].kind != b.fills[i].kind || !(a.fills[i].mainColor() == b.fills[i].mainColor())) return false;
    for (size_t i = 0; i < a.strokes.size(); ++i)
        if (!(a.strokes[i].paint.mainColor() == b.strokes[i].paint.mainColor())) return false;
    return true;
}

/// The affine map taking drawing `a` onto drawing `b` when `b` is `a` moved,
/// scaled, rotated or skewed (as the Free Transform tool leaves it).
std::optional<Affine> drawingMap(const ShapeGraph& a, const ShapeGraph& b)
{
    if (a.edges.empty() || a.edges.size() != b.edges.size() || !sameLooks(a, b)) return std::nullopt;
    const double tol = 1e-4 * std::max({1.0, a.bounds(false).width(), a.bounds(false).height()});
    std::vector<Vec2> pa, pb;
    for (size_t i = 0; i < a.edges.size(); ++i) {
        const GEdge &ea = a.edges[i], &eb = b.edges[i];
        if (ea.fillL != eb.fillL || ea.fillR != eb.fillR || ea.stroke != eb.stroke) return std::nullopt;
        for (Vec2 v : {ea.c.p0, ea.c.p1, ea.c.p2, ea.c.p3}) pa.push_back(v);
        for (Vec2 v : {eb.c.p0, eb.c.p1, eb.c.p2, eb.c.p3}) pb.push_back(v);
    }
    // Three well spread points: the first, the farthest from it, the
    // farthest from the line through both.
    size_t i1 = 0, i2 = 0;
    double best = 0;
    for (size_t i = 0; i < pa.size(); ++i)
        if (const double d = distance(pa[i], pa[0]); d > best) {
            best = d;
            i1 = i;
        }
    best = 0;
    for (size_t i = 0; i < pa.size(); ++i)
        if (const double d = std::abs(cross(pa[i1] - pa[0], pa[i] - pa[0])); d > best) {
            best = d;
            i2 = i;
        }
    const Vec2 p[3] = {pa[0], pa[i1], pa[i2]}, q[3] = {pb[0], pb[i1], pb[i2]};
    const std::optional<Affine> m = mapThrough(p, q);
    if (!m) return std::nullopt;
    for (size_t i = 0; i < pa.size(); ++i)
        if (distance(m->map(pa[i]), pb[i]) > tol * std::max(1.0, m->meanScale())) return std::nullopt;
    return m;
}

} // namespace

std::shared_ptr<InstanceElement> convertToSymbol(Document& doc, const std::vector<ElementPtr>& elements,
                                                 const std::string& name, SymbolType type, Vec2 registration)
{
    const Rect bounds = boundsOf(doc, elements);
    Symbol s;
    s.id = doc.newSymbolId();
    s.name = doc.uniqueSymbolName(name.empty() ? "Symbol 1" : name);
    s.type = type;
    s.timeline.name = s.name;
    Layer layer = doc.makeLayer("Layer 1");
    const Affine toLocal = Affine::translate(-registration);
    ShapeGraph merged;
    for (const ElementPtr& e : elements) {
        if (const ShapeElement* sh = asShape(e); sh && !sh->isObject) {
            merged = merged.isEmpty() ? sh->graph->transformed(toLocal) : overlay(merged, sh->graph->transformed(toLocal));
            continue;
        }
        layer.keys[0].elements.push_back(transformElement(e, toLocal));
    }
    if (!merged.isEmpty()) setKeyframeMergeShape(layer.keys[0], std::move(merged));
    s.timeline.layers.push_back(std::move(layer));
    doc.symbols.push_back(std::move(s));

    auto inst = std::make_shared<InstanceElement>();
    inst->symbolId = doc.symbols.back().id;
    inst->behavior = doc.symbols.back().type;
    inst->matrix = Affine::translate(registration);
    inst->pivot = bounds.isEmpty() ? Vec2{} : bounds.center() - registration;
    return inst;
}

std::vector<ElementPtr> breakApart(const Document& doc, const ElementPtr& e, int localFrame)
{
    std::vector<ElementPtr> out;
    switch (e->type()) {
    case ElementType::Instance: {
        const auto& in = static_cast<const InstanceElement&>(*e);
        const Symbol* s = doc.symbol(in.symbolId);
        if (!s) return out;
        const int f = instanceSymbolFrame(doc, in, localFrame);
        const ColorTransform ct = in.color.toTransform();
        // Bottom layer first.
        for (int li = int(s->timeline.layers.size()) - 1; li >= 0; --li) {
            const Layer& l = s->timeline.layers[li];
            if (l.type == LayerType::Guide || l.type == LayerType::Folder || l.type == LayerType::Mask) continue;
            for (const EvalItem& it : evaluateLayer(doc, s->timeline, li, f)) {
                ElementPtr el = it.element;
                if (el->type() == ElementType::Morph) {
                    const auto& m = static_cast<const MorphElement&>(*el);
                    if (!m.data) continue;
                    el = makeShapeElement(graphFromRenderData(*m.data), false);
                }
                out.push_back(colorElement(transformElement(el, in.matrix), ct));
            }
        }
        return out;
    }
    case ElementType::Group: return ungroup(static_cast<const GroupElement&>(*e));
    case ElementType::Shape: {
        const auto& sh = static_cast<const ShapeElement&>(*e);
        if (!sh.isObject) return {e};
        out.push_back(makeShapeElement(sh.graph->transformed(sh.matrix), false));
        return out;
    }
    case ElementType::Morph: {
        const auto& m = static_cast<const MorphElement&>(*e);
        if (m.data) out.push_back(makeShapeElement(graphFromRenderData(*m.data).transformed(m.matrix), false));
        return out;
    }
    }
    return out;
}

std::shared_ptr<GroupElement> groupElements(const Document& doc, const std::vector<ElementPtr>& elements)
{
    auto g = std::make_shared<GroupElement>();
    g->children = elements;
    const Rect b = boundsOf(doc, elements);
    if (!b.isEmpty()) g->pivot = b.center();
    return g;
}

std::vector<ElementPtr> ungroup(const GroupElement& g)
{
    std::vector<ElementPtr> out;
    for (const ElementPtr& c : g.children) out.push_back(transformElement(c, g.matrix));
    return out;
}

ShapeGraph flattenToShape(const Document& doc, const std::vector<ElementPtr>& elements, int localFrame)
{
    ShapeGraph acc;
    std::function<void(const ElementPtr&, int)> add = [&](const ElementPtr& e, int depth) {
        if (depth > 16) return;
        if (const ShapeElement* s = asShape(e)) {
            const ShapeGraph g = s->matrix.isIdentity() ? *s->graph : s->graph->transformed(s->matrix);
            acc = acc.isEmpty() ? g : overlay(acc, g);
            return;
        }
        for (const ElementPtr& c : breakApart(doc, e, localFrame))
            if (c != e) add(c, depth + 1);
    };
    for (const ElementPtr& e : elements) add(e, 0);
    return acc;
}

bool createClassicTween(Document& doc, Timeline& tl, int layerIndex, int frame)
{
    Layer& l = tl.layers[layerIndex];
    const int ki = l.keyIndexAt(frame);
    if (ki < 0) return false;
    auto symbolize = [&](Keyframe& k) {
        if (k.elements.empty() || !containsShape(k)) return;
        const Rect b = boundsOf(doc, k.elements);
        auto inst = convertToSymbol(doc, k.elements, "Tween " + std::to_string(tweenSymbolCounter(doc)),
                                    SymbolType::Graphic, b.isEmpty() ? Vec2{} : b.center());
        k.elements = {inst};
    };
    Keyframe* next = ki + 1 < int(l.keys.size()) ? &l.keys[ki + 1] : nullptr;
    // The same drawing moved, scaled or rotated in the next keyframe: both
    // keyframes show one symbol, so the tween interpolates the transform.
    std::optional<Affine> map;
    if (next)
        if (const auto a = soleDrawing(l.keys[ki]))
            if (const auto b = soleDrawing(*next)) map = drawingMap(*a, *b);
    symbolize(l.keys[ki]);
    if (next) {
        const Keyframe& k = l.keys[ki];
        if (map && k.elements.size() == 1 && k.elements.front()->type() == ElementType::Instance) {
            auto inst = k.elements.front()->cloneAs<InstanceElement>();
            inst->matrix = *map * inst->matrix;
            next->elements = {inst};
        } else {
            symbolize(*next);
        }
    }
    l.keys[ki].tween = TweenType::Classic;
    return next != nullptr;
}

bool createShapeTween(Document& doc, Timeline& tl, int layerIndex, int frame)
{
    Layer& l = tl.layers[layerIndex];
    Keyframe* k = l.keyAt(frame);
    if (!k) return false;
    k->tween = TweenType::Shape;
    const int ki = l.keyIndexAt(frame);
    return ki + 1 < int(l.keys.size());
}

std::string duplicateSymbol(Document& doc, const std::string& symbolId, const std::string& newName)
{
    const Symbol* s = doc.symbol(symbolId);
    if (!s) return {};
    Symbol c = *s;
    c.id = doc.newSymbolId();
    c.name = doc.uniqueSymbolName(newName.empty() ? s->name + " copy" : newName);
    doc.symbols.push_back(std::move(c));
    return doc.symbols.back().id;
}

void deleteSymbol(Document& doc, const std::string& symbolId)
{
    std::function<void(std::vector<ElementPtr>&)> strip = [&](std::vector<ElementPtr>& els) {
        std::vector<ElementPtr> kept;
        for (const ElementPtr& e : els) {
            if (const InstanceElement* in = asInstance(e); in && in->symbolId == symbolId) continue;
            if (const GroupElement* g = asGroup(e)) {
                auto c = g->cloneAs<GroupElement>();
                strip(c->children);
                kept.push_back(c);
                continue;
            }
            kept.push_back(e);
        }
        els.swap(kept);
    };
    doc.symbols.erase(std::remove_if(doc.symbols.begin(), doc.symbols.end(),
                                     [&](const Symbol& s) { return s.id == symbolId; }),
                      doc.symbols.end());
    forEachTimeline(doc, [&](Timeline& t) {
        for (Layer& l : t.layers)
            for (Keyframe& k : l.keys) strip(k.elements);
    });
}

ElementPtr swapSymbol(const ElementPtr& instance, const std::string& newSymbolId)
{
    const InstanceElement* in = asInstance(instance);
    if (!in) return instance;
    auto c = in->cloneAs<InstanceElement>();
    c->symbolId = newSymbolId;
    return c;
}

} // namespace vx
