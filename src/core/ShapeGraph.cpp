// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShapeGraph.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace vx {

namespace {

struct PointKey {
    uint64_t x, y;
    explicit PointKey(Vec2 p)
    {
        // +0.0 and -0.0 must compare equal.
        const double px = p.x == 0.0 ? 0.0 : p.x, py = p.y == 0.0 ? 0.0 : p.y;
        std::memcpy(&x, &px, 8);
        std::memcpy(&y, &py, 8);
    }
    bool operator==(const PointKey&) const = default;
};

struct PointKeyHash {
    size_t operator()(const PointKey& k) const
    {
        uint64_t h = k.x * 0x9E3779B97F4A7C15ull;
        h ^= (k.y + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2));
        return size_t(h);
    }
};

GEdge oriented(const GEdge& e, bool forward)
{
    if (forward) return e;
    return {e.c.reversed(), e.fillR, e.fillL, e.stroke};
}

// Chains directed curves into closed loops by matching end points exactly.
std::vector<Contour> chainLoops(std::vector<Cubic> curves)
{
    std::vector<Contour> loops;
    std::unordered_multimap<PointKey, int, PointKeyHash> starts;
    for (int i = 0; i < int(curves.size()); ++i) starts.insert({PointKey(curves[i].p0), i});
    std::vector<char> used(curves.size(), 0);
    for (int i = 0; i < int(curves.size()); ++i) {
        if (used[i]) continue;
        Contour loop;
        int cur = i;
        while (cur >= 0) {
            used[cur] = 1;
            loop.push_back(curves[cur]);
            const Vec2 end = curves[cur].p3;
            if (end == loop.front().p0) break;
            int next = -1;
            auto range = starts.equal_range(PointKey(end));
            for (auto it = range.first; it != range.second; ++it)
                if (!used[it->second]) {
                    next = it->second;
                    break;
                }
            cur = next;
        }
        if (loop.back().p3 != loop.front().p0) loop.push_back(Cubic::line(loop.back().p3, loop.front().p0));
        loops.push_back(std::move(loop));
    }
    return loops;
}

} // namespace

int ShapeGraph::addFill(const FillStyle& f)
{
    for (size_t i = 0; i < fills.size(); ++i)
        if (fills[i] == f) return int(i) + 1;
    fills.push_back(f);
    invalidate();
    return int(fills.size());
}

int ShapeGraph::addStroke(const StrokeStyle& s)
{
    for (size_t i = 0; i < strokes.size(); ++i)
        if (strokes[i] == s) return int(i) + 1;
    strokes.push_back(s);
    invalidate();
    return int(strokes.size());
}

Rect ShapeGraph::bounds(bool includeStrokeWidth) const
{
    Rect r;
    for (const GEdge& e : edges) {
        Rect b = e.c.bounds();
        if (includeStrokeWidth && e.stroke > 0) b = b.inflated(stroke(e.stroke).width * 0.5);
        r.include(b);
    }
    return r;
}

ShapeGraph ShapeGraph::transformed(const Affine& m) const
{
    ShapeGraph g;
    g.fills.reserve(fills.size());
    for (const FillStyle& f : fills) g.fills.push_back(f.transformed(m));
    for (const StrokeStyle& s : strokes) g.strokes.push_back(s.transformed(m));
    const bool mirror = m.det() < 0.0;
    g.edges.reserve(edges.size());
    for (const GEdge& e : edges) {
        GEdge t = e;
        t.c = e.c.transformed(m);
        if (mirror) std::swap(t.fillL, t.fillR); // orientation flips with a mirror
        g.edges.push_back(t);
    }
    return g;
}

ShapeGraph ShapeGraph::withColorTransform(const ColorTransform& ct) const
{
    ShapeGraph g = *this;
    for (FillStyle& f : g.fills) f = f.withColorTransform(ct);
    for (StrokeStyle& s : g.strokes) s.paint = s.paint.withColorTransform(ct);
    return g;
}

void ShapeGraph::compact()
{
    invalidate();
    // 1. Drop invisible and degenerate edges.
    edges.erase(std::remove_if(edges.begin(), edges.end(),
                               [](const GEdge& e) {
                                   return (e.stroke == 0 && e.fillL == e.fillR) || e.c.isDegenerate(1e-12);
                               }),
                edges.end());
    // 2. Merge edges that continue each other through a degree-2 vertex.
    bool changed = true;
    while (changed) {
        changed = false;
        std::unordered_map<PointKey, std::vector<std::pair<int, int>>, PointKeyHash> inc;
        for (int i = 0; i < int(edges.size()); ++i) {
            inc[PointKey(edges[i].c.p0)].push_back({i, 0});
            inc[PointKey(edges[i].c.p3)].push_back({i, 1});
        }
        std::vector<char> dead(edges.size(), 0), touched(edges.size(), 0);
        for (auto& [key, list] : inc) {
            if (list.size() != 2) continue;
            auto [i, ei] = list[0];
            auto [j, ej] = list[1];
            if (i == j || dead[i] || dead[j] || touched[i] || touched[j]) continue;
            const GEdge a = oriented(edges[i], ei == 1); // ends at the vertex
            const GEdge b = oriented(edges[j], ej == 0); // starts at the vertex
            if (a.fillL != b.fillL || a.fillR != b.fillR || a.stroke != b.stroke) continue;
            Cubic joined;
            if (!tryJoinCubics(a.c, b.c, joined, 1e-7)) continue;
            edges[i] = {joined, a.fillL, a.fillR, a.stroke};
            dead[j] = 1;
            touched[i] = touched[j] = 1;
            changed = true;
        }
        if (changed) {
            std::vector<GEdge> kept;
            kept.reserve(edges.size());
            for (size_t i = 0; i < edges.size(); ++i)
                if (!dead[i]) kept.push_back(edges[i]);
            edges.swap(kept);
        }
    }
    // 3. Remove unused styles.
    std::vector<int> fillMap(fills.size() + 1, 0), strokeMap(strokes.size() + 1, 0);
    for (const GEdge& e : edges) {
        if (e.fillL) fillMap[e.fillL] = 1;
        if (e.fillR) fillMap[e.fillR] = 1;
        if (e.stroke) strokeMap[e.stroke] = 1;
    }
    std::vector<FillStyle> nf;
    for (size_t i = 1; i < fillMap.size(); ++i)
        if (fillMap[i]) {
            nf.push_back(fills[i - 1]);
            fillMap[i] = int(nf.size());
        }
    std::vector<StrokeStyle> ns;
    for (size_t i = 1; i < strokeMap.size(); ++i)
        if (strokeMap[i]) {
            ns.push_back(strokes[i - 1]);
            strokeMap[i] = int(ns.size());
        }
    for (GEdge& e : edges) {
        e.fillL = fillMap[e.fillL];
        e.fillR = fillMap[e.fillR];
        e.stroke = strokeMap[e.stroke];
    }
    fills.swap(nf);
    strokes.swap(ns);
}

std::vector<std::vector<Cubic>> strokeChains(const ShapeGraph& g, std::vector<int>* styles, std::vector<char>* closed)
{
    std::vector<std::vector<Cubic>> chains;
    for (int s = 1; s <= int(g.strokes.size()); ++s) {
        std::vector<int> ids;
        for (int i = 0; i < int(g.edges.size()); ++i)
            if (g.edges[i].stroke == s) ids.push_back(i);
        if (ids.empty()) continue;
        std::unordered_map<PointKey, std::vector<std::pair<int, int>>, PointKeyHash> inc;
        for (int k = 0; k < int(ids.size()); ++k) {
            inc[PointKey(g.edges[ids[k]].c.p0)].push_back({k, 0});
            inc[PointKey(g.edges[ids[k]].c.p3)].push_back({k, 1});
        }
        std::vector<char> used(ids.size(), 0);
        auto walk = [&](int k, bool forward) {
            std::vector<Cubic> chain;
            int cur = k;
            bool fwd = forward;
            const Vec2 start = fwd ? g.edges[ids[k]].c.p0 : g.edges[ids[k]].c.p3;
            bool isClosed = false;
            while (cur >= 0) {
                used[cur] = 1;
                const Cubic c = fwd ? g.edges[ids[cur]].c : g.edges[ids[cur]].c.reversed();
                chain.push_back(c);
                if (c.p3 == start) {
                    isClosed = true;
                    break;
                }
                const auto& list = inc[PointKey(c.p3)];
                if (list.size() != 2) break;
                int next = -1;
                bool nextFwd = true;
                for (auto [kk, end] : list)
                    if (kk != cur && !used[kk]) {
                        next = kk;
                        nextFwd = end == 0;
                    }
                cur = next;
                fwd = nextFwd;
            }
            chains.push_back(std::move(chain));
            if (styles) styles->push_back(s);
            if (closed) closed->push_back(isClosed ? 1 : 0);
        };
        // Open chains start at vertices whose degree is not 2.
        for (auto& [key, list] : inc) {
            if (list.size() == 2) continue;
            for (auto [k, end] : list)
                if (!used[k]) walk(k, end == 0);
        }
        for (int k = 0; k < int(ids.size()); ++k)
            if (!used[k]) walk(k, true);
    }
    return chains;
}

ShapeRenderData buildRenderData(const ShapeGraph& g)
{
    ShapeRenderData rd;
    for (int s = 1; s <= int(g.fills.size()); ++s) {
        std::vector<Cubic> directed;
        for (const GEdge& e : g.edges) {
            if (e.fillL == e.fillR) continue;
            if (e.fillL == s) directed.push_back(e.c);
            else if (e.fillR == s) directed.push_back(e.c.reversed());
        }
        if (directed.empty()) continue;
        ShapeRenderData::FillPath fp;
        fp.style = g.fills[s - 1];
        fp.contours = chainLoops(std::move(directed));
        rd.fills.push_back(std::move(fp));
    }
    std::vector<int> styles;
    std::vector<char> closed;
    auto chains = strokeChains(g, &styles, &closed);
    for (int s = 1; s <= int(g.strokes.size()); ++s) {
        ShapeRenderData::StrokePath sp;
        sp.style = g.strokes[s - 1];
        for (size_t i = 0; i < chains.size(); ++i)
            if (styles[i] == s) {
                sp.chains.push_back(chains[i]);
                sp.closed.push_back(closed[i]);
            }
        if (!sp.chains.empty()) rd.strokes.push_back(std::move(sp));
    }
    rd.bounds = g.bounds(true);
    return rd;
}

const ShapeRenderData& ShapeGraph::renderData() const
{
    if (!m_cache.render) m_cache.render = std::make_shared<ShapeRenderData>(buildRenderData(*this));
    return *m_cache.render;
}

std::shared_ptr<const ShapeRenderData> ShapeGraph::renderDataPtr() const
{
    (void)renderData();
    return m_cache.render;
}

const Arrangement& ShapeGraph::topology() const
{
    if (!m_cache.topology) {
        auto arr = std::make_shared<Arrangement>();
        arr->setLayers({LayerKind::Label});
        for (int i = 0; i < int(edges.size()); ++i) {
            ArrInput in;
            in.curve = edges[i].c;
            in.layer = 0;
            in.labelLeft = edges[i].fillL;
            in.labelRight = edges[i].fillR;
            in.tag = i;
            arr->add(in);
        }
        arr->build();
        m_cache.topology = arr;
    }
    return *m_cache.topology;
}

int ShapeGraph::topologySource(int arrEdge) const
{
    const Arrangement& arr = topology();
    for (const ArrEdgeSource& s : arr.edges()[arrEdge].sources) {
        const int tag = arr.inputs()[s.input].tag;
        if (tag >= 0) return tag;
    }
    return -1;
}

int ShapeGraph::topologyStroke(int arrEdge) const
{
    const Arrangement& arr = topology();
    int st = 0;
    for (const ArrEdgeSource& s : arr.edges()[arrEdge].sources) {
        const int tag = arr.inputs()[s.input].tag;
        if (tag >= 0 && tag < int(edges.size())) st = std::max(st, edges[tag].stroke);
    }
    return st;
}

int ShapeGraph::fillAt(Vec2 p) const
{
    if (edges.empty()) return 0;
    const Arrangement& arr = topology();
    return arr.value(arr.locate(p), 0);
}

Region ShapeGraph::fillRegion(int fillId) const
{
    Region r;
    if (edges.empty()) return r;
    const Arrangement& arr = topology();
    for (const auto& loop : arr.traceBoundary([&](int f) {
             const int v = arr.value(f, 0);
             return fillId == 0 ? v != 0 : v == fillId;
         }))
        r.contours.push_back(arr.loopCurves(loop));
    return r;
}

} // namespace vx
