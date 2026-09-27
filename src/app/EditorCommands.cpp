// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — editor commands (menus, hotkeys, panels).
#include "Editor.h"

#include "core/DocumentOps.h"
#include "core/Evaluate.h"
#include "core/Serialize.h"
#include "core/TimelineOps.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>

#include <map>
#include <set>

namespace vx::app {

namespace {

const char* kElementsMime = "application/x-vertexa-elements";

// Remove elements by index (descending) from keyframes; returns them in
// selection order.
struct Removed {
    std::vector<ElementPtr> elements;
    int layerIndex = -1; ///< layer of the first removed element
    int insertIndex = 0; ///< index where a replacement should go
};

// A point strictly inside a face of an arrangement (for re-selecting pieces
// after they moved).
bool interiorPoint(const Arrangement& a, int face, Vec2& out)
{
    const auto& f = a.faces()[face];
    if (f.outer < 0) return false;
    int h = a.cycles()[f.outer].first;
    double bestLen = -1;
    int best = h;
    for (int i = 0; i < a.cycles()[f.outer].length; ++i) {
        const double l = distance(a.curve(h).p0, a.curve(h).p3);
        if (l > bestLen) {
            bestLen = l;
            best = h;
        }
        h = a.next(h);
    }
    const Cubic c = a.curve(best);
    const Vec2 mid = c.eval(0.5);
    const Vec2 n = c.tangent(0.5).perp();
    const double off = std::min(0.05, std::max(1e-4, bestLen * 0.01));
    out = mid + n * off;
    return true;
}

} // namespace

// --- helpers ---------------------------------------------------------------------

static Removed removeSelected(Editor& ed, Document& d, const std::vector<ElementRef>& sel, int frame)
{
    Removed r;
    Timeline& tl = ed.mutableTimeline(d);
    std::map<int, std::vector<int>> byLayer;
    for (const ElementRef& ref : sel) {
        const int li = tl.layerIndex(ref.layerId);
        if (li < 0) continue;
        byLayer[li].push_back(ref.index);
    }
    for (const ElementRef& ref : sel) {
        const int li = tl.layerIndex(ref.layerId);
        if (li < 0) continue;
        const Keyframe* k = tl.layers[li].keyAt(frame);
        if (k && ref.index >= 0 && ref.index < int(k->elements.size())) r.elements.push_back(k->elements[ref.index]);
        if (r.layerIndex < 0) {
            r.layerIndex = li;
            r.insertIndex = ref.index;
        }
    }
    for (auto& [li, idx] : byLayer) {
        Keyframe* k = tl.layers[li].keyAt(frame);
        if (!k) continue;
        std::sort(idx.begin(), idx.end(), std::greater<int>());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
        for (int i : idx)
            if (i >= 0 && i < int(k->elements.size())) k->elements.erase(k->elements.begin() + i);
        if (li == r.layerIndex) {
            int below = 0;
            for (int i : idx)
                if (i < r.insertIndex) ++below;
            r.insertIndex = std::max(0, r.insertIndex - below);
        }
    }
    return r;
}

// Lifted part of the current shape pick (and the remaining shape).
static bool liftPick(const ShapePick& pick, ShapeGraph& rest, ShapeGraph& lifted)
{
    if (!pick.valid()) return false;
    if (pick.region) cutByRegion(*pick.graph, *pick.region, rest, lifted);
    else liftSelection(*pick.graph, pick.sel, rest, lifted);
    return true;
}

// Places non-object shapes into the merge shape, other elements at `index`.
static void insertElements(Keyframe& k, int index, const std::vector<ElementPtr>& els, std::vector<int>* placed = nullptr)
{
    std::vector<ElementPtr> others;
    for (const ElementPtr& e : els) {
        if (const ShapeElement* s = asShape(e); s && !s->isObject) {
            const bool hadMerge = !keyframeMergeShape(k).isEmpty();
            mergeIntoKeyframe(k, s->matrix.isIdentity() ? *s->graph : s->graph->transformed(s->matrix));
            if (!hadMerge) ++index; // a merge shape was inserted at 0
            continue;
        }
        others.push_back(e);
    }
    const bool hasMerge = !keyframeMergeShape(k).isEmpty();
    index = std::clamp(index, hasMerge ? 1 : 0, int(k.elements.size()));
    for (size_t i = 0; i < others.size(); ++i) {
        k.elements.insert(k.elements.begin() + index + int(i), others[i]);
        if (placed) placed->push_back(index + int(i));
    }
}

void Editor::deleteSelection()
{
    if (!hasSelection()) return;
    const auto sel = m_selection;
    const ShapePick pick = m_shapePick;
    edit(tr("Delete"), [&](Document& d) {
        removeSelected(*this, d, sel, m_frame);
        if (pick.valid()) {
            Timeline& tl = mutableTimeline(d);
            const int li = tl.layerIndex(pick.layerId);
            Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            ShapeGraph rest, lifted;
            if (k && liftPick(pick, rest, lifted)) setKeyframeMergeShape(*k, rest);
        }
        return true;
    });
    clearSelection();
}

void Editor::selectAll()
{
    std::vector<ElementRef> sel;
    const Timeline& tl = timeline();
    for (int li = 0; li < int(tl.layers.size()); ++li) {
        const Layer& l = tl.layers[li];
        if (l.locked || !l.visible || l.type == LayerType::Folder) continue;
        const Keyframe* k = l.keyAt(m_frame);
        if (!k) continue;
        for (int i = 0; i < int(k->elements.size()); ++i) sel.push_back({l.id, i});
    }
    m_shapePick = {};
    setSelection(sel);
}

void Editor::copySelection()
{
    std::vector<ElementPtr> els = selectedElements();
    ShapeGraph rest, lifted;
    if (liftPick(m_shapePick, rest, lifted) && !lifted.isEmpty()) els.insert(els.begin(), makeShapeElement(lifted, false));
    if (els.empty()) return;
    auto* mime = new QMimeData();
    mime->setData(kElementsMime, serializeClipboard(m_doc, els));
    QApplication::clipboard()->setMimeData(mime);
    m_pasteCount = 0;
}

void Editor::cutSelection()
{
    copySelection();
    deleteSelection();
}

void Editor::paste(bool inPlace)
{
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(kElementsMime)) return;
    const QByteArray data = mime->data(kElementsMime);
    const uint32_t layerId = currentLayer() ? currentLayer()->id : 0;
    std::vector<int> placed;
    const bool ok = edit(inPlace ? tr("Paste in Place") : tr("Paste"), [&](Document& d) {
        std::vector<ElementPtr> els = deserializeClipboard(data, d);
        if (els.empty()) return false;
        // Refuse pasting a symbol into itself.
        if (inSymbol())
            for (const ElementPtr& e : els)
                if (const InstanceElement* in = asInstance(e); in && d.symbolContains(in->symbolId, m_stack.back().symbolId)) {
                    notify(tr("A symbol can't contain itself"));
                    return false;
                }
        QString why;
        Keyframe* k = editableKey(d, m_layer, &why);
        if (!k) {
            notify(why);
            return false;
        }
        if (!inPlace) {
            Rect b;
            for (const ElementPtr& e : els) b.include(elementBounds(d, *e));
            Vec2 target;
            if (m_pasteCount == 0) {
                target = contextMatrix().inverted().map({d.width * 0.5, d.height * 0.5});
            } else {
                target = b.center() + Vec2{10.0 * m_pasteCount, 10.0 * m_pasteCount};
            }
            const Affine shift = Affine::translate(target - b.center());
            for (ElementPtr& e : els) {
                if (const ShapeElement* s = asShape(e); s && !s->isObject)
                    e = makeShapeElement(s->graph->transformed(shift), false);
                else e = e->withMatrix(shift * e->matrix);
            }
        }
        insertElements(*k, int(k->elements.size()), els, &placed);
        return true;
    });
    if (ok) {
        ++m_pasteCount;
        std::vector<ElementRef> sel;
        for (int i : placed) sel.push_back({layerId, i});
        m_shapePick = {};
        setSelection(sel);
    }
}

void Editor::duplicateSelection()
{
    copySelection();
    m_pasteCount = 1;
    paste(false);
}

void Editor::nudge(double dx, double dy) { transformSelection(Affine::translate(dx, dy), tr("Move")); }

void Editor::transformSelection(const Affine& m, const QString& label)
{
    if (!hasSelection()) return;
    const auto sel = m_selection;
    const ShapePick pick = m_shapePick;
    ShapeGraph liftedMoved;
    ShapeGraphPtr newGraph;
    const bool ok = edit(label, [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        for (const ElementRef& r : sel) {
            const int li = tl.layerIndex(r.layerId);
            if (li < 0 || tl.layers[li].locked) continue;
            Keyframe* k = tl.layers[li].keyAt(m_frame);
            if (!k || r.index < 0 || r.index >= int(k->elements.size())) continue;
            ElementPtr& e = k->elements[r.index];
            if (const ShapeElement* s = asShape(e); s && !s->isObject) {
                auto c = s->cloneAs<ShapeElement>();
                c->graph = std::make_shared<ShapeGraph>(s->graph->transformed(m));
                c->pivot = m.map(s->pivot);
                e = c;
            } else {
                auto c = e->clone();
                c->matrix = m * e->matrix;
                e = c;
            }
        }
        if (pick.valid()) {
            const int li = tl.layerIndex(pick.layerId);
            Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            ShapeGraph rest, lifted;
            if (k && liftPick(pick, rest, lifted)) {
                liftedMoved = lifted.transformed(m);
                ShapeGraph merged = overlay(rest, liftedMoved);
                setKeyframeMergeShape(*k, merged);
                if (!k->elements.empty())
                    if (const ShapeElement* s = asShape(k->elements.front()); s && !s->isObject) newGraph = s->graph;
            }
        }
        return true;
    });
    if (!ok) return;
    if (pick.valid() && newGraph) {
        // Keep the moved pieces selected.
        ShapePick np;
        np.layerId = pick.layerId;
        np.graph = newGraph;
        const Arrangement& la = liftedMoved.topology();
        const Arrangement& na = newGraph->topology();
        for (int f = 1; f < la.faceCount(); ++f) {
            if (!la.value(f, 0)) continue;
            Vec2 p;
            if (!interiorPoint(la, f, p)) continue;
            const int nf = na.locate(p);
            if (nf > 0 && !np.sel.hasFace(nf)) np.sel.faces.push_back(nf);
        }
        for (int e = 0; e < int(na.edges().size()); ++e) {
            if (!newGraph->topologyStroke(e)) continue;
            const Vec2 mid = na.edges()[e].curve.eval(0.5);
            for (const GEdge& ge : liftedMoved.edges)
                if (ge.stroke && ge.c.distanceTo(mid) < 1e-6) {
                    np.sel.edges.push_back(e);
                    break;
                }
        }
        m_shapePick = np.valid() ? np : ShapePick{};
    }
    emit selectionChanged();
}

void Editor::convertSelectionToSymbol(const QString& name, SymbolType type, int registration)
{
    if (!hasSelection()) return;
    const auto sel = m_selection;
    const ShapePick pick = m_shapePick;
    int placedLayer = -1, placedIndex = -1;
    const bool ok = edit(tr("Convert to Symbol"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        std::vector<ElementPtr> els;
        int targetLayer = m_layer, insertAt = -1;
        if (pick.valid()) {
            const int li = tl.layerIndex(pick.layerId);
            Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            ShapeGraph rest, lifted;
            if (k && liftPick(pick, rest, lifted)) {
                setKeyframeMergeShape(*k, rest);
                els.push_back(makeShapeElement(lifted, false));
                targetLayer = li;
                insertAt = int(k->elements.size());
            }
        }
        Removed r = removeSelected(*this, d, sel, m_frame);
        if (!r.elements.empty() && insertAt < 0) {
            targetLayer = r.layerIndex;
            insertAt = r.insertIndex;
        }
        els.insert(els.end(), r.elements.begin(), r.elements.end());
        if (els.empty()) return false;
        Rect b;
        for (const ElementPtr& e : els) b.include(elementBounds(d, *e));
        auto inst = vx::convertToSymbol(d, els, name.toStdString(), type, registrationPoint(b, registration));
        Timeline& tl2 = mutableTimeline(d); // symbols vector may have reallocated
        Keyframe* k = tl2.layers[targetLayer].keyAt(m_frame);
        if (!k) return false;
        const bool hasMerge = !keyframeMergeShape(*k).isEmpty();
        insertAt = std::clamp(insertAt < 0 ? int(k->elements.size()) : insertAt, hasMerge ? 1 : 0, int(k->elements.size()));
        k->elements.insert(k->elements.begin() + insertAt, inst);
        placedLayer = targetLayer;
        placedIndex = insertAt;
        return true;
    });
    if (ok && placedLayer >= 0) {
        m_shapePick = {};
        setSelection({{timeline().layers[placedLayer].id, placedIndex}});
    }
}

void Editor::breakApart()
{
    if (m_selection.empty()) return;
    const auto sel = m_selection;
    edit(tr("Break Apart"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        std::map<int, std::vector<int>> byLayer;
        for (const ElementRef& r : sel) {
            const int li = tl.layerIndex(r.layerId);
            if (li >= 0 && !tl.layers[li].locked) byLayer[li].push_back(r.index);
        }
        bool changed = false;
        for (auto& [li, idx] : byLayer) {
            std::sort(idx.begin(), idx.end(), std::greater<int>());
            for (int i : idx) {
                Keyframe* k = tl.layers[li].keyAt(m_frame);
                if (!k || i < 0 || i >= int(k->elements.size())) continue;
                const ElementPtr e = k->elements[i];
                if (const ShapeElement* s = asShape(e); s && !s->isObject) continue;
                const int local = m_frame - k->start;
                std::vector<ElementPtr> parts = vx::breakApart(d, e, local);
                if (parts.size() == 1 && parts[0] == e) continue;
                k->elements.erase(k->elements.begin() + i);
                insertElements(*k, i, parts);
                changed = true;
            }
        }
        return changed;
    });
    clearSelection();
}

void Editor::groupSelection()
{
    if (!hasSelection()) return;
    const auto sel = m_selection;
    const ShapePick pick = m_shapePick;
    int placedLayer = -1, placedIndex = -1;
    const bool ok = edit(tr("Group"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        std::vector<ElementPtr> els;
        int targetLayer = m_layer;
        if (pick.valid()) {
            const int li = tl.layerIndex(pick.layerId);
            Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            ShapeGraph rest, lifted;
            if (k && liftPick(pick, rest, lifted)) {
                setKeyframeMergeShape(*k, rest);
                els.push_back(makeShapeElement(lifted, false));
                targetLayer = li;
            }
        }
        Removed r = removeSelected(*this, d, sel, m_frame);
        if (!r.elements.empty() && els.empty()) targetLayer = r.layerIndex;
        els.insert(els.end(), r.elements.begin(), r.elements.end());
        if (els.empty()) return false;
        auto g = groupElements(d, els);
        Keyframe* k = tl.layers[targetLayer].keyAt(m_frame);
        if (!k) return false;
        k->elements.push_back(g);
        placedLayer = targetLayer;
        placedIndex = int(k->elements.size()) - 1;
        return true;
    });
    if (ok) {
        m_shapePick = {};
        setSelection({{timeline().layers[placedLayer].id, placedIndex}});
    }
}

void Editor::ungroupSelection()
{
    if (m_selection.empty()) return;
    const auto sel = m_selection;
    edit(tr("Ungroup"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool changed = false;
        std::vector<ElementRef> sorted = sel;
        std::sort(sorted.begin(), sorted.end(), [](const ElementRef& a, const ElementRef& b) { return a.index > b.index; });
        for (const ElementRef& r : sorted) {
            const int li = tl.layerIndex(r.layerId);
            if (li < 0) continue;
            Keyframe* k = tl.layers[li].keyAt(m_frame);
            if (!k || r.index < 0 || r.index >= int(k->elements.size())) continue;
            const GroupElement* g = asGroup(k->elements[r.index]);
            if (!g) continue;
            const std::vector<ElementPtr> parts = ungroup(*g);
            k->elements.erase(k->elements.begin() + r.index);
            insertElements(*k, r.index, parts);
            changed = true;
        }
        return changed;
    });
    clearSelection();
}

void Editor::arrange(Arrange a)
{
    if (m_selection.empty()) return;
    const auto sel = m_selection;
    std::vector<ElementRef> newSel;
    edit(tr("Arrange"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        std::map<int, std::vector<int>> byLayer;
        for (const ElementRef& r : sel) {
            const int li = tl.layerIndex(r.layerId);
            if (li >= 0) byLayer[li].push_back(r.index);
        }
        for (auto& [li, idx] : byLayer) {
            Keyframe* k = tl.layers[li].keyAt(m_frame);
            if (!k) continue;
            const int floor = keyframeMergeShape(*k).isEmpty() ? 0 : 1;
            std::set<int> chosen(idx.begin(), idx.end());
            if (floor) chosen.erase(0);
            std::vector<ElementPtr> picked, others;
            for (int i = 0; i < int(k->elements.size()); ++i)
                (chosen.count(i) && i >= floor ? picked : others).push_back(k->elements[i]);
            std::vector<ElementPtr> out;
            switch (a) {
            case Arrange::Front:
                out = others;
                out.insert(out.end(), picked.begin(), picked.end());
                break;
            case Arrange::Back:
                out.assign(others.begin(), others.begin() + std::min<int>(floor, int(others.size())));
                out.insert(out.end(), picked.begin(), picked.end());
                out.insert(out.end(), others.begin() + std::min<int>(floor, int(others.size())), others.end());
                break;
            case Arrange::Forward:
            case Arrange::Backward: {
                out = k->elements;
                const int dir = a == Arrange::Forward ? 1 : -1;
                std::vector<int> order(chosen.begin(), chosen.end());
                if (dir > 0) std::reverse(order.begin(), order.end());
                for (int i : order) {
                    const int j = i + dir;
                    if (i < floor || j < floor || j >= int(out.size()) || chosen.count(j)) continue;
                    std::swap(out[i], out[j]);
                }
                break;
            }
            }
            k->elements = out;
            for (int i = 0; i < int(out.size()); ++i)
                for (const ElementPtr& p : picked)
                    if (out[i] == p) newSel.push_back({tl.layers[li].id, i});
        }
        return true;
    });
    setSelection(newSel);
}

void Editor::combineObjects(Combine c)
{
    const auto sel = m_selection;
    int placedLayer = -1, placedIndex = -1;
    const bool ok = edit(tr("Combine Objects"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        std::vector<std::pair<ElementRef, ShapeGraph>> objs;
        for (const ElementRef& r : sel) {
            const int li = tl.layerIndex(r.layerId);
            const Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            if (!k || r.index < 0 || r.index >= int(k->elements.size())) continue;
            const ShapeElement* s = asShape(k->elements[r.index]);
            if (!s || !s->isObject) continue;
            objs.push_back({r, s->graph->transformed(s->matrix)});
        }
        if (objs.size() < 2) {
            notify(tr("Select two or more drawing objects"));
            return false;
        }
        std::sort(objs.begin(), objs.end(), [](const auto& a, const auto& b) { return a.first.index < b.first.index; });
        ShapeGraph result;
        const ShapeGraph& top = objs.back().second;
        std::vector<ShapeGraph> rest;
        for (size_t i = 0; i + 1 < objs.size(); ++i) rest.push_back(objs[i].second);
        const ShapeGraph below = combineUnion(rest);
        switch (c) {
        case Combine::Union: {
            std::vector<ShapeGraph> all = rest;
            all.push_back(top);
            result = combineUnion(all);
            break;
        }
        case Combine::Intersect: result = combineIntersect(below, top); break;
        case Combine::Punch: result = combinePunch(below, top); break;
        case Combine::Crop: result = combineCrop(below, top); break;
        }
        std::vector<ElementRef> refs;
        for (auto& o : objs) refs.push_back(o.first);
        Removed r = removeSelected(*this, d, refs, m_frame);
        Keyframe* k = tl.layers[r.layerIndex].keyAt(m_frame);
        if (!k || result.isEmpty()) return !r.elements.empty();
        const int at = std::clamp(r.insertIndex, 0, int(k->elements.size()));
        k->elements.insert(k->elements.begin() + at, makeShapeElement(result, true));
        placedLayer = r.layerIndex;
        placedIndex = at;
        return true;
    });
    if (ok && placedLayer >= 0) setSelection({{timeline().layers[placedLayer].id, placedIndex}});
    else if (ok) clearSelection();
}

void Editor::applyFillToSelection(const FillStyle& f)
{
    m_settings.fill = f;
    emit settingsChanged();
    if (!hasSelection()) return;
    const auto sel = m_selection;
    const ShapePick pick = m_shapePick;
    edit(tr("Fill Color"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool changed = false;
        for (const ElementRef& r : sel) {
            const int li = tl.layerIndex(r.layerId);
            Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            if (!k || r.index < 0 || r.index >= int(k->elements.size())) continue;
            const ShapeElement* s = asShape(k->elements[r.index]);
            if (!s) continue;
            auto c = s->cloneAs<ShapeElement>();
            ShapeGraph g = *s->graph;
            for (FillStyle& fs : g.fills) fs = f;
            g.compact();
            c->graph = std::make_shared<ShapeGraph>(normalizeGraph(g));
            k->elements[r.index] = c;
            changed = true;
        }
        if (pick.valid()) {
            const int li = tl.layerIndex(pick.layerId);
            Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            if (k) {
                if (pick.region) {
                    ShapeGraph rest, lifted;
                    cutByRegion(*pick.graph, *pick.region, rest, lifted);
                    for (FillStyle& fs : lifted.fills) fs = f;
                    setKeyframeMergeShape(*k, overlay(rest, lifted));
                } else {
                    setKeyframeMergeShape(*k, recolorFaces(*pick.graph, pick.sel.faces, f));
                }
                changed = true;
            }
        }
        return changed;
    });
    if (pick.valid()) m_shapePick = {};
    emit selectionChanged();
}

void Editor::applyStrokeToSelection(const StrokeStyle& st)
{
    m_settings.stroke = st;
    emit settingsChanged();
    if (!hasSelection()) return;
    const auto sel = m_selection;
    const ShapePick pick = m_shapePick;
    edit(tr("Stroke"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool changed = false;
        for (const ElementRef& r : sel) {
            const int li = tl.layerIndex(r.layerId);
            Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            if (!k || r.index < 0 || r.index >= int(k->elements.size())) continue;
            const ShapeElement* s = asShape(k->elements[r.index]);
            if (!s || s->graph->strokes.empty()) continue;
            auto c = s->cloneAs<ShapeElement>();
            ShapeGraph g = *s->graph;
            for (StrokeStyle& ss : g.strokes) ss = st;
            g.compact();
            c->graph = std::make_shared<ShapeGraph>(normalizeGraph(g));
            k->elements[r.index] = c;
            changed = true;
        }
        if (pick.valid() && !pick.region && !pick.sel.edges.empty()) {
            const int li = tl.layerIndex(pick.layerId);
            Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            if (k) {
                setKeyframeMergeShape(*k, restrokeEdges(*pick.graph, pick.sel.edges, st));
                changed = true;
            }
        }
        return changed;
    });
    if (pick.valid()) m_shapePick = {};
    emit selectionChanged();
}

void Editor::setInstanceProperty(const std::function<void(InstanceElement&)>& fn, const QString& label)
{
    const auto sel = m_selection;
    edit(label, [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool changed = false;
        for (const ElementRef& r : sel) {
            const int li = tl.layerIndex(r.layerId);
            Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
            if (!k || r.index < 0 || r.index >= int(k->elements.size())) continue;
            const InstanceElement* in = asInstance(k->elements[r.index]);
            if (!in) continue;
            auto c = in->cloneAs<InstanceElement>();
            fn(*c);
            k->elements[r.index] = c;
            changed = true;
        }
        return changed;
    });
    emit selectionChanged();
}

void Editor::setElementMatrix(int selIndex, const Affine& m)
{
    if (selIndex < 0 || selIndex >= int(m_selection.size())) return;
    const ElementRef r = m_selection[selIndex];
    edit(tr("Transform"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        const int li = tl.layerIndex(r.layerId);
        Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_frame) : nullptr;
        if (!k || r.index < 0 || r.index >= int(k->elements.size())) return false;
        ElementPtr& e = k->elements[r.index];
        if (const ShapeElement* s = asShape(e); s && !s->isObject) {
            // Merge shapes stay in layer space: bake the relative transform.
            const Affine rel = m * e->matrix.inverted();
            auto c = s->cloneAs<ShapeElement>();
            c->graph = std::make_shared<ShapeGraph>(s->graph->transformed(rel));
            e = c;
        } else {
            e = e->withMatrix(m);
        }
        return true;
    });
    emit selectionChanged();
}

// --- frames -----------------------------------------------------------------------

namespace {

struct Range {
    int layerFrom, layerTo, frameFrom, frameTo;
};

} // namespace

static Range frameRange(const Editor& ed)
{
    const FrameSelection& s = ed.frameSelection();
    if (s.valid()) return {s.layerFrom, s.layerTo, s.frameFrom, s.frameTo};
    return {ed.layerIndex(), ed.layerIndex(), ed.frame(), ed.frame()};
}

void Editor::insertFrames()
{
    const Range r = frameRange(*this);
    const int count = r.frameTo - r.frameFrom + 1;
    edit(tr("Insert Frame"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li) {
            if (tl.layers[li].type == LayerType::Folder) continue;
            vx::insertFrames(tl.layers[li], r.frameFrom, count);
        }
        return true;
    });
}

void Editor::removeFrames()
{
    const Range r = frameRange(*this);
    const int count = r.frameTo - r.frameFrom + 1;
    edit(tr("Remove Frames"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li)
            vx::removeFrames(tl.layers[li], r.frameFrom, count);
        return true;
    });
    setFrameSelection({});
}

void Editor::insertKeyframe(bool blank)
{
    const Range r = frameRange(*this);
    int landed = -1;
    edit(blank ? tr("Insert Blank Keyframe") : tr("Insert Keyframe"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool changed = false;
        for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li) {
            if (tl.layers[li].type == LayerType::Folder) continue;
            if (r.frameTo > r.frameFrom) {
                vx::convertToKeyframes(d, tl, li, r.frameFrom, r.frameTo, blank);
                changed = true;
            } else {
                const int f = vx::insertKeyframe(d, tl, li, r.frameFrom, blank);
                if (f >= 0) {
                    changed = true;
                    if (li == m_layer) landed = f;
                }
            }
        }
        return changed;
    });
    if (landed >= 0 && landed != m_frame) setFrame(landed);
}

void Editor::clearKeyframe()
{
    const Range r = frameRange(*this);
    edit(tr("Clear Keyframe"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool changed = false;
        for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li)
            for (int f = r.frameTo; f >= r.frameFrom; --f) changed |= vx::clearKeyframe(tl.layers[li], f);
        return changed;
    });
}

void Editor::convertToKeyframes(bool blank)
{
    const Range r = frameRange(*this);
    edit(blank ? tr("Convert to Blank Keyframes") : tr("Convert to Keyframes"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li)
            vx::convertToKeyframes(d, tl, li, r.frameFrom, r.frameTo, blank);
        return true;
    });
}

void Editor::clearFrames()
{
    const Range r = frameRange(*this);
    edit(tr("Clear Frames"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li)
            vx::clearFrames(tl.layers[li], r.frameFrom, r.frameTo);
        return true;
    });
}

void Editor::createTween(TweenType t)
{
    const Range r = frameRange(*this);
    bool missingNext = false;
    edit(t == TweenType::Classic ? tr("Create Classic Tween") : tr("Create Shape Tween"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool changed = false;
        for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li) {
            if (tl.layers[li].type == LayerType::Folder) continue;
            // Every keyframe span touched by the selection gets the tween.
            std::set<int> starts;
            for (int f = r.frameFrom; f <= r.frameTo; ++f)
                if (const Keyframe* k = tl.layers[li].keyAt(f)) starts.insert(k->start);
            for (int s : starts) {
                const bool ok = t == TweenType::Classic ? vx::createClassicTween(d, tl, li, s) : vx::createShapeTween(d, tl, li, s);
                missingNext |= !ok;
                changed = true;
            }
        }
        return changed;
    });
    if (missingNext) notify(tr("Tween created — add a keyframe at its end (F6) to complete it"));
}

void Editor::removeTween()
{
    setKeyframeProperty([](Keyframe& k) { k.tween = TweenType::None; }, tr("Remove Tween"));
}

void Editor::reverseFrames()
{
    const Range r = frameRange(*this);
    if (r.frameTo <= r.frameFrom) return;
    edit(tr("Reverse Frames"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li)
            vx::reverseFrames(tl.layers[li], r.frameFrom, r.frameTo);
        return true;
    });
}

void Editor::copyFrames(bool cut)
{
    const Range r = frameRange(*this);
    const Timeline& tl = timeline();
    m_frameClipboard.clear();
    // Frames of several layers are stored back to back, separated by an
    // empty marker keyframe with duration 0 (layer boundaries).
    std::vector<std::vector<Keyframe>> perLayer;
    for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li)
        perLayer.push_back(vx::copyFrames(tl.layers[li], r.frameFrom, r.frameTo));
    for (size_t i = 0; i < perLayer.size(); ++i) {
        if (i > 0) {
            Keyframe sep;
            sep.duration = 0;
            m_frameClipboard.push_back(sep);
        }
        m_frameClipboard.insert(m_frameClipboard.end(), perLayer[i].begin(), perLayer[i].end());
    }
    if (cut) {
        edit(tr("Cut Frames"), [&](Document& d) {
            Timeline& mt = mutableTimeline(d);
            for (int li = r.layerFrom; li <= r.layerTo && li < int(mt.layers.size()); ++li)
                vx::removeFrames(mt.layers[li], r.frameFrom, r.frameTo - r.frameFrom + 1);
            return true;
        });
    }
}

void Editor::pasteFrames()
{
    if (m_frameClipboard.empty()) return;
    const Range r = frameRange(*this);
    std::vector<std::vector<Keyframe>> perLayer(1);
    for (const Keyframe& k : m_frameClipboard) {
        if (k.duration == 0) perLayer.emplace_back();
        else perLayer.back().push_back(k);
    }
    edit(tr("Paste Frames"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        for (size_t i = 0; i < perLayer.size(); ++i) {
            const int li = r.layerFrom + int(i);
            if (li >= int(tl.layers.size())) break;
            vx::pasteFrames(tl.layers[li], r.frameFrom, perLayer[i], false);
        }
        return true;
    });
}

void Editor::moveFrames(int layerIndex, int from, int to, int delta)
{
    if (delta == 0) return;
    edit(tr("Move Frames"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        if (layerIndex < 0 || layerIndex >= int(tl.layers.size())) return false;
        vx::moveFrames(tl.layers[layerIndex], from, to, delta);
        return true;
    });
}

void Editor::setKeyframeProperty(const std::function<void(Keyframe&)>& fn, const QString& label)
{
    const Range r = frameRange(*this);
    edit(label, [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool changed = false;
        for (int li = r.layerFrom; li <= r.layerTo && li < int(tl.layers.size()); ++li) {
            std::set<int> starts;
            for (int f = r.frameFrom; f <= r.frameTo; ++f)
                if (const Keyframe* k = tl.layers[li].keyAt(f)) starts.insert(k->start);
            for (int s : starts)
                if (Keyframe* k = tl.layers[li].keyAt(s)) {
                    fn(*k);
                    changed = true;
                }
        }
        return changed;
    });
}

void Editor::addShapeHint()
{
    const Layer* l = currentLayer();
    const Keyframe* k = l ? l->keyAt(m_frame) : nullptr;
    if (!k || k->tween != TweenType::Shape) {
        notify(tr("Shape hints need a shape tween on this keyframe"));
        return;
    }
    const int ki = l->keyIndexAt(m_frame);
    const Keyframe* next = ki + 1 < int(l->keys.size()) ? &l->keys[ki + 1] : nullptr;
    Rect a, b;
    for (const ElementPtr& e : k->elements) a.include(elementBounds(m_doc, *e));
    if (next)
        for (const ElementPtr& e : next->elements) b.include(elementBounds(m_doc, *e));
    const Vec2 ca = a.isEmpty() ? Vec2{} : a.center();
    const Vec2 cb = b.isEmpty() ? ca : b.center();
    const int li = m_layer;
    const int start = k->start;
    edit(tr("Add Shape Hint"), [&](Document& d) {
        Keyframe* mk = mutableTimeline(d).layers[li].keyAt(start);
        if (!mk || mk->hints.size() >= 26) return false;
        mk->hints.push_back({ca, cb});
        return true;
    });
}

void Editor::removeShapeHints()
{
    setKeyframeProperty([](Keyframe& k) { k.hints.clear(); }, tr("Remove All Hints"));
}

// --- layers -----------------------------------------------------------------------

void Editor::addLayer(LayerType type)
{
    int newIndex = m_layer;
    edit(type == LayerType::Guide ? tr("Add Motion Guide") : tr("New Layer"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        const int at = std::clamp(m_layer, 0, int(tl.layers.size()));
        Layer l = d.makeLayer(d.uniqueLayerName(tl));
        l.type = type;
        if (type == LayerType::Guide) l.name = "Guide: " + (at < int(tl.layers.size()) ? tl.layers[at].name : std::string("Layer"));
        // Extend the new layer to the timeline length.
        vx::extendTo(l, std::max(0, tl.frameCount() - 1));
        if (at < int(tl.layers.size())) {
            l.parentId = tl.layers[at].type == LayerType::Folder ? tl.layers[at].id : tl.layers[at].parentId;
            if (type == LayerType::Guide) {
                // The current layer becomes guided by the new guide.
                l.parentId = tl.layers[at].parentId;
                tl.layers[at].parentId = l.id;
            } else if (tl.layers[at].type == LayerType::Mask || tl.layers[at].type == LayerType::Guide) {
                l.parentId = tl.layers[at].parentId;
            }
        }
        tl.layers.insert(tl.layers.begin() + at, l);
        newIndex = at;
        return true;
    });
    m_layer = newIndex;
    emit layerChanged(m_layer);
}

void Editor::addFolder()
{
    edit(tr("New Folder"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        const int at = std::clamp(m_layer, 0, int(tl.layers.size()));
        Layer l = d.makeLayer("Folder " + std::to_string(d.nextLayerId - 1));
        l.type = LayerType::Folder;
        l.keys.clear();
        l.keys.push_back(Keyframe{});
        if (at < int(tl.layers.size())) l.parentId = tl.layers[at].parentId;
        tl.layers.insert(tl.layers.begin() + at, l);
        return true;
    });
}

void Editor::deleteLayer()
{
    const Timeline& cur = timeline();
    if (cur.layers.size() <= 1) {
        notify(tr("A timeline needs at least one layer"));
        return;
    }
    const uint32_t id = cur.layers[m_layer].id;
    edit(tr("Delete Layer"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        // Folders take their content with them.
        std::set<uint32_t> doomed{id};
        bool grew = true;
        const Layer* self = tl.layerById(id);
        const bool isFolder = self && self->type == LayerType::Folder;
        while (grew && isFolder) {
            grew = false;
            for (const Layer& l : tl.layers)
                if (doomed.count(l.parentId) && !doomed.count(l.id)) {
                    doomed.insert(l.id);
                    grew = true;
                }
        }
        const uint32_t parentOfDeleted = self ? self->parentId : 0;
        tl.layers.erase(std::remove_if(tl.layers.begin(), tl.layers.end(), [&](const Layer& l) { return doomed.count(l.id) > 0; }),
                        tl.layers.end());
        for (Layer& l : tl.layers)
            if (doomed.count(l.parentId)) l.parentId = parentOfDeleted;
        if (tl.layers.empty()) tl.layers.push_back(d.makeLayer("Layer 1"));
        return true;
    });
    setLayerIndex(std::min(m_layer, int(timeline().layers.size()) - 1));
    emit layerChanged(m_layer);
}

void Editor::renameLayer(int index, const QString& name)
{
    if (name.trimmed().isEmpty()) return;
    setLayerProperty(index, [&](Layer& l) { l.name = name.trimmed().toStdString(); }, tr("Rename Layer"));
}

void Editor::setLayerProperty(int index, const std::function<void(Layer&)>& fn, const QString& label)
{
    edit(label, [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        if (index < 0 || index >= int(tl.layers.size())) return false;
        const LayerType before = tl.layers[index].type;
        fn(tl.layers[index]);
        Layer& l = tl.layers[index];
        if (l.type != before) {
            if (before == LayerType::Mask || before == LayerType::Guide)
                for (Layer& o : tl.layers)
                    if (o.parentId == l.id) o.parentId = l.parentId;
            if (l.type == LayerType::Mask && index + 1 < int(tl.layers.size())) {
                // Animate: the layer below becomes masked.
                Layer& below = tl.layers[index + 1];
                if (below.type == LayerType::Normal && below.parentId == l.parentId) below.parentId = l.id;
                l.locked = true;
                below.locked = true;
            }
        }
        return true;
    });
}

void Editor::moveLayer(int from, int before)
{
    int moved = -1;
    edit(tr("Move Layer"), [&](Document& d) {
        moved = vx::moveLayer(mutableTimeline(d), from, before);
        return moved >= 0;
    });
    if (moved < 0) return;
    m_layer = moved;
    emit layerChanged(m_layer);
}

void Editor::toggleOthersLocked(int index)
{
    edit(tr("Lock Others"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool allLocked = true;
        for (int i = 0; i < int(tl.layers.size()); ++i)
            if (i != index && !tl.layers[i].locked) allLocked = false;
        for (int i = 0; i < int(tl.layers.size()); ++i) tl.layers[i].locked = (i == index) ? false : !allLocked;
        return true;
    });
}

void Editor::toggleOthersHidden(int index)
{
    edit(tr("Hide Others"), [&](Document& d) {
        Timeline& tl = mutableTimeline(d);
        bool allHidden = true;
        for (int i = 0; i < int(tl.layers.size()); ++i)
            if (i != index && tl.layers[i].visible) allHidden = false;
        for (int i = 0; i < int(tl.layers.size()); ++i) tl.layers[i].visible = (i == index) ? true : allHidden;
        return true;
    });
}

// --- symbols ------------------------------------------------------------------

void Editor::newSymbol(const QString& name, SymbolType type)
{
    std::string id;
    edit(tr("New Symbol"), [&](Document& d) {
        Symbol s;
        s.id = d.newSymbolId();
        s.name = d.uniqueSymbolName(name.isEmpty() ? "Symbol 1" : name.toStdString());
        s.type = type;
        s.timeline.name = s.name;
        s.timeline.layers.push_back(d.makeLayer("Layer 1"));
        id = s.id;
        d.symbols.push_back(std::move(s));
        return true;
    });
    if (!id.empty()) enterSymbol(id);
}

void Editor::duplicateSymbol(const std::string& id)
{
    edit(tr("Duplicate Symbol"), [&](Document& d) { return !vx::duplicateSymbol(d, id, "").empty(); });
}

void Editor::deleteSymbol(const std::string& id)
{
    for (const ContextEntry& e : m_stack)
        if (e.symbolId == id) {
            exitToScene();
            break;
        }
    edit(tr("Delete Symbol"), [&](Document& d) {
        if (!d.symbol(id)) return false;
        vx::deleteSymbol(d, id);
        return true;
    });
    clearSelection();
}

void Editor::renameSymbol(const std::string& id, const QString& name)
{
    if (name.trimmed().isEmpty()) return;
    edit(tr("Rename Symbol"), [&](Document& d) {
        Symbol* s = d.symbol(id);
        if (!s) return false;
        const std::string n = name.trimmed().toStdString();
        if (n == s->name) return false;
        s->name = d.uniqueSymbolName(n);
        return true;
    });
    emit contextChanged();
}

void Editor::setSymbolType(const std::string& id, SymbolType type)
{
    edit(tr("Symbol Type"), [&](Document& d) {
        Symbol* s = d.symbol(id);
        if (!s || s->type == type) return false;
        s->type = type;
        return true;
    });
}

void Editor::placeSymbol(const std::string& id, Vec2 pos)
{
    if (inSymbol() && m_doc.symbolContains(id, m_stack.back().symbolId)) {
        notify(tr("A symbol can't contain itself"));
        return;
    }
    int placed = -1;
    const uint32_t layerId = currentLayer() ? currentLayer()->id : 0;
    edit(tr("Place Symbol"), [&](Document& d) {
        const Symbol* s = d.symbol(id);
        if (!s) return false;
        QString why;
        Keyframe* k = editableKey(d, m_layer, &why);
        if (!k) {
            notify(why);
            return false;
        }
        auto inst = std::make_shared<InstanceElement>();
        inst->symbolId = id;
        inst->behavior = s->type;
        inst->matrix = Affine::translate(pos);
        const Rect b = timelineBounds(d, s->timeline, 0);
        inst->pivot = b.isEmpty() ? Vec2{} : b.center();
        k->elements.push_back(inst);
        placed = int(k->elements.size()) - 1;
        return true;
    });
    if (placed >= 0) setSelection({{layerId, placed}});
}

void Editor::setStageSettings(double w, double h, double fps, Color bg)
{
    edit(tr("Document Settings"), [&](Document& d) {
        d.width = std::clamp(w, 1.0, 16384.0);
        d.height = std::clamp(h, 1.0, 16384.0);
        d.fps = std::clamp(fps, 0.1, 240.0);
        d.background = bg;
        return true;
    });
}

} // namespace vx::app
