// SPDX-License-Identifier: GPL-3.0-or-later
#include "Evaluate.h"
#include "ShapeTween.h"
#include "Tween.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

bool tweenable(const ElementPtr& a, const ElementPtr& b)
{
    if (!a || !b || a->type() != b->type()) return false;
    switch (a->type()) {
    case ElementType::Instance:
        return static_cast<const InstanceElement&>(*a).symbolId == static_cast<const InstanceElement&>(*b).symbolId;
    case ElementType::Shape:
        return static_cast<const ShapeElement&>(*a).isObject && static_cast<const ShapeElement&>(*b).isObject;
    case ElementType::Group:
    case ElementType::Paint: return true;
    case ElementType::Morph: return false;
    }
    return false;
}

} // namespace

std::vector<std::vector<Cubic>> guideChains(const Timeline& tl, int guideLayerIndex, int frame)
{
    std::vector<std::vector<Cubic>> chains;
    if (guideLayerIndex < 0 || guideLayerIndex >= int(tl.layers.size())) return chains;
    const Keyframe* k = tl.layers[guideLayerIndex].keyAt(frame);
    if (!k) return chains;
    for (const ElementPtr& e : k->elements) {
        const ShapeElement* s = asShape(e);
        if (!s || !s->graph) continue;
        for (auto& ch : strokeChains(*s->graph)) {
            if (!s->matrix.isIdentity())
                for (Cubic& c : ch) c = c.transformed(s->matrix);
            chains.push_back(std::move(ch));
        }
    }
    return chains;
}

std::vector<EvalItem> evaluateLayer(const Document& doc, const Timeline& tl, int layerIndex, int frame)
{
    std::vector<EvalItem> out;
    if (layerIndex < 0 || layerIndex >= int(tl.layers.size())) return out;
    const Layer& layer = tl.layers[layerIndex];
    const int ki = layer.keyIndexAt(frame);
    if (ki < 0) return out;
    const Keyframe& k = layer.keys[ki];
    const int local = frame - k.start;
    const Keyframe* next = (ki + 1 < int(layer.keys.size())) ? &layer.keys[ki + 1] : nullptr;
    const bool canTween = next && local > 0 && !next->elements.empty();

    if (canTween && k.tween == TweenType::Classic) {
        const double t = k.classic.ease.apply(double(local) / double(k.duration));
        // Motion guide.
        GuidePath guide;
        const Layer* guideLayer = tl.guideOf(layerIndex);
        const int guideIndex = guideLayer ? tl.layerIndex(guideLayer->id) : -1;
        for (size_t i = 0; i < k.elements.size(); ++i) {
            const ElementPtr& a = k.elements[i];
            const ElementPtr b = i < next->elements.size() ? next->elements[i] : nullptr;
            if (!tweenable(a, b)) {
                out.push_back({a, local});
                continue;
            }
            const Vec2 pa = a->matrix.map(a->pivot), pb = b->matrix.map(b->pivot);
            Affine m;
            if (guideIndex >= 0) {
                if (guide.isEmpty()) guide = chooseGuide(guideChains(tl, guideIndex, k.start), pa, pb);
            }
            if (!guide.isEmpty()) {
                const double s0 = guide.project(pa), s1 = guide.project(pb);
                const double s = s0 + (s1 - s0) * t;
                const Vec2 pos = guide.pointAt(s);
                double extra = 0.0;
                if (k.classic.orientToPath) {
                    const Vec2 t0 = guide.tangentAt(s0), tc = guide.tangentAt(s);
                    extra = std::atan2(cross(t0, tc), dot(t0, tc));
                }
                m = tweenMatrix(a->matrix, b->matrix, a->pivot, b->pivot, t, k.classic.rotate, k.classic.rotations,
                                k.classic.scale, &pos, extra);
            } else {
                m = tweenMatrix(a->matrix, b->matrix, a->pivot, b->pivot, t, k.classic.rotate, k.classic.rotations,
                                k.classic.scale);
            }
            auto c = a->clone();
            c->matrix = m;
            c->pivot = lerp(a->pivot, b->pivot, t);
            if (c->type() == ElementType::Instance) {
                auto* ci = static_cast<InstanceElement*>(c.get());
                const auto& ia = static_cast<const InstanceElement&>(*a);
                const auto& ib = static_cast<const InstanceElement&>(*b);
                ci->color = ColorEffect::lerp(ia.color, ib.color, t);
                ci->filters = lerpFilters(ia.filters, ib.filters, t);
            }
            out.push_back({c, local});
        }
        return out;
    }

    if (canTween && k.tween == TweenType::Shape) {
        const ShapeRenderData a = collectShapeData(k.elements);
        const ShapeRenderData b = collectShapeData(next->elements);
        const double t = k.shape.ease.apply(double(local) / double(k.duration));
        auto morph = std::make_shared<MorphElement>();
        morph->data = std::make_shared<ShapeRenderData>(morphShapes(a, b, t, k.shape.angular, k.hints));
        out.push_back({morph, local});
        for (const ElementPtr& e : k.elements)
            if (!asShape(e)) out.push_back({e, local});
        return out;
    }

    for (const ElementPtr& e : k.elements) out.push_back({e, local});
    return out;
}

int instanceSymbolFrame(const Document& doc, const InstanceElement& inst, int localFrame, int clipFrame)
{
    const Symbol* s = doc.symbol(inst.symbolId);
    if (!s) return 0;
    const int len = s->timeline.frameCount();
    if (len <= 1) return 0;
    if (s->type != SymbolType::Graphic) return ((clipFrame % len) + len) % len;
    const int first = std::clamp(inst.firstFrame, 0, len - 1);
    const int last = inst.lastFrame < 0 ? len - 1 : std::clamp(inst.lastFrame, first, len - 1);
    const int span = last - first + 1;
    const int off = std::max(0, localFrame);
    switch (inst.loop) {
    case LoopMode::SingleFrame: return first;
    case LoopMode::Loop: return first + off % span;
    case LoopMode::PlayOnce: return std::min(first + off, last);
    case LoopMode::LoopReverse: return last - off % span;
    case LoopMode::PlayOnceReverse: return std::max(last - off, first);
    }
    return first;
}

Rect elementBounds(const Document& doc, const Element& e, int localFrame, int depth)
{
    Rect r;
    switch (e.type()) {
    case ElementType::Shape: {
        const auto& s = static_cast<const ShapeElement&>(e);
        if (!s.graph) break;
        if (s.matrix.isIdentity()) return s.graph->bounds(true);
        for (const GEdge& ge : s.graph->edges) {
            Rect b = ge.c.transformed(s.matrix).bounds();
            if (ge.stroke) b = b.inflated(s.graph->stroke(ge.stroke).width * 0.5 * s.matrix.meanScale());
            r.include(b);
        }
        return r;
    }
    case ElementType::Instance: {
        const auto& in = static_cast<const InstanceElement&>(e);
        const Symbol* sym = doc.symbol(in.symbolId);
        if (!sym || depth > 32) break;
        const Rect inner = timelineBounds(doc, sym->timeline, instanceSymbolFrame(doc, in, localFrame), depth + 1);
        if (inner.isEmpty()) {
            // Empty symbols still need a handle around their registration point.
            return e.matrix.mapRect(Rect(-4, -4, 4, 4));
        }
        return e.matrix.mapRect(inner);
    }
    case ElementType::Group: {
        const auto& g = static_cast<const GroupElement&>(e);
        for (const ElementPtr& c : g.children) r.include(elementBounds(doc, *c, localFrame, depth + 1));
        return e.matrix.mapRect(r);
    }
    case ElementType::Paint: return e.matrix.mapRect(static_cast<const PaintElement&>(e).localBounds());
    case ElementType::Morph: {
        const auto& m = static_cast<const MorphElement&>(e);
        return m.data ? e.matrix.mapRect(m.data->bounds) : r;
    }
    }
    return r;
}

Rect timelineBounds(const Document& doc, const Timeline& tl, int frame, int depth)
{
    Rect r;
    for (int i = 0; i < int(tl.layers.size()); ++i) {
        const Layer& l = tl.layers[i];
        if (l.type == LayerType::Guide || l.type == LayerType::Folder) continue;
        for (const EvalItem& it : evaluateLayer(doc, tl, i, frame))
            r.include(elementBounds(doc, *it.element, it.localFrame, depth));
    }
    return r;
}

std::vector<ElementPtr> bakeFrame(const Document& doc, const Timeline& tl, int layerIndex, int frame)
{
    std::vector<ElementPtr> out;
    const Layer& layer = tl.layers[layerIndex];
    const Keyframe* k = layer.keyAt(frame);
    if (!k) return out;
    const int local = frame - k->start;
    const bool shapeTween = k->tween == TweenType::Shape && local > 0;
    ShapeGraph shapes;
    bool haveShapes = false;
    for (const EvalItem& it : evaluateLayer(doc, tl, layerIndex, frame)) {
        if (it.element->type() == ElementType::Morph) {
            const auto& m = static_cast<const MorphElement&>(*it.element);
            if (m.data) {
                shapes = graphFromRenderData(*m.data);
                haveShapes = true;
            }
            continue;
        }
        if (shapeTween && asShape(it.element)) continue;
        ElementPtr e = it.element;
        // Graphic instances continue from where they were.
        if (const InstanceElement* in = asInstance(e)) {
            const Symbol* s = doc.symbol(in->symbolId);
            if (s && s->type == SymbolType::Graphic && in->loop != LoopMode::SingleFrame && local > 0) {
                auto c = in->cloneAs<InstanceElement>();
                c->firstFrame = instanceSymbolFrame(doc, *in, local);
                e = c;
            }
        }
        out.push_back(e);
    }
    if (haveShapes) out.insert(out.begin(), makeShapeElement(shapes, false));
    return out;
}

std::shared_ptr<ShapeElement> makeShapeElement(ShapeGraph g, bool isObject, const Affine& m)
{
    auto s = std::make_shared<ShapeElement>();
    s->graph = std::make_shared<ShapeGraph>(std::move(g));
    s->isObject = isObject;
    s->matrix = m;
    const Rect b = s->graph->bounds(false);
    if (!b.isEmpty()) s->pivot = b.center();
    return s;
}

Rect PaintElement::localBounds() const
{
    Rect r;
    for (const PaintStroke& s : strokes) {
        const double rad = s.brush ? s.brush->size * 0.5 * (1.0 + s.brush->scatter * 2.0 + s.brush->sizeJitter) : 1.0;
        for (const PaintSample& p : s.samples) r.include(Rect(p.pos.x - rad, p.pos.y - rad, p.pos.x + rad, p.pos.y + rad));
    }
    return r;
}

} // namespace vx
