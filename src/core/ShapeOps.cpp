// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShapeOps.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <set>

namespace vx {

namespace {

constexpr int kBaseLayer = 0;
constexpr int kTopLayer = 1;
constexpr int kMaskLayer0 = 2;

// Builds an arrangement from a base shape, an optional top shape, winding
// masks and extra curves, then emits new shapes from per-face / per-edge
// decisions. All style ids are remapped into one table.
class Engine {
public:
    explicit Engine(int masks = 0)
    {
        std::vector<LayerKind> kinds{LayerKind::Label, LayerKind::Label};
        for (int i = 0; i < masks; ++i) kinds.push_back(LayerKind::Winding);
        m_own.setLayers(kinds);
    }

    void addBase(const ShapeGraph& g)
    {
        m_base = &g;
        m_baseFill = mapFills(g);
        m_baseStroke = mapStrokes(g);
        m_nBase = int(g.edges.size());
        for (int i = 0; i < m_nBase; ++i) {
            ArrInput in;
            in.curve = g.edges[i].c;
            in.layer = kBaseLayer;
            in.labelLeft = m_baseFill[g.edges[i].fillL];
            in.labelRight = m_baseFill[g.edges[i].fillR];
            in.tag = i;
            m_own.add(in);
        }
    }
    void addTop(const ShapeGraph& g)
    {
        m_top = &g;
        m_topFill = mapFills(g);
        m_topStroke = mapStrokes(g);
        m_nTop = int(g.edges.size());
        for (int i = 0; i < m_nTop; ++i) {
            ArrInput in;
            in.curve = g.edges[i].c;
            in.layer = kTopLayer;
            in.labelLeft = m_topFill[g.edges[i].fillL];
            in.labelRight = m_topFill[g.edges[i].fillR];
            in.tag = i;
            m_own.add(in);
        }
    }
    void addMask(const Region& r, int k)
    {
        for (const Contour& c : r.contours)
            for (const Cubic& cu : c) {
                ArrInput in;
                in.curve = cu;
                in.layer = kMaskLayer0 + k;
                const int idx = m_own.add(in);
                m_extraStroke[idx] = 0;
            }
    }
    /// Extra curve that is neither a fill boundary nor a mask (gap closers,
    /// stroke paths). `stroke` is a result stroke id or 0.
    void addExtra(const Cubic& c, int stroke)
    {
        ArrInput in;
        in.curve = c;
        in.layer = -1;
        const int idx = m_own.add(in);
        m_extraStroke[idx] = stroke;
    }
    int fillId(const FillStyle& f) { return m_styles.addFill(f); }
    int strokeId(const StrokeStyle& s) { return m_styles.addStroke(s); }

    void build() { m_own.build(); }
    const Arrangement& arr() const { return m_own; }

    int baseLabel(int f) const { return m_own.value(f, kBaseLayer); }
    int topLabel(int f) const { return m_own.value(f, kTopLayer); }
    bool inMask(int f, int k) const { return m_own.value(f, kMaskLayer0 + k) != 0; }
    int leftFace(int e) const { return m_own.face(2 * e); }
    int rightFace(int e) const { return m_own.face(2 * e + 1); }

    int baseStroke(int e) const
    {
        int s = 0;
        for (const ArrEdgeSource& src : m_own.edges()[e].sources)
            if (src.input < m_nBase) s = std::max(s, m_baseStroke[m_base->edges[src.input].stroke]);
        return s;
    }
    int topStroke(int e) const
    {
        int s = 0;
        for (const ArrEdgeSource& src : m_own.edges()[e].sources)
            if (src.input >= m_nBase && src.input < m_nBase + m_nTop)
                s = std::max(s, m_topStroke[m_top->edges[src.input - m_nBase].stroke]);
        return s;
    }
    int extraStroke(int e) const
    {
        int s = 0;
        for (const ArrEdgeSource& src : m_own.edges()[e].sources) {
            auto it = m_extraStroke.find(src.input);
            if (it != m_extraStroke.end()) s = std::max(s, it->second);
        }
        return s;
    }

    ShapeGraph emit(const std::function<int(int face)>& faceFill,
                    const std::function<int(int edge, int left, int right)>& edgeStroke) const
    {
        ShapeGraph g;
        g.fills = m_styles.fills;
        g.strokes = m_styles.strokes;
        const Arrangement& a = m_own;
        std::vector<int> ff(a.faceCount());
        for (int f = 0; f < a.faceCount(); ++f) ff[f] = faceFill(f);
        for (int e = 0; e < int(a.edges().size()); ++e) {
            const int lf = a.face(2 * e), rf = a.face(2 * e + 1);
            const int L = ff[lf], R = ff[rf];
            const int S = edgeStroke(e, lf, rf);
            if (S != 0 || L != R) g.edges.push_back({a.edges()[e].curve, L, R, S});
        }
        g.compact();
        return g;
    }

private:
    std::vector<int> mapFills(const ShapeGraph& g)
    {
        std::vector<int> m(g.fills.size() + 1, 0);
        for (size_t i = 0; i < g.fills.size(); ++i) m[i + 1] = m_styles.addFill(g.fills[i]);
        return m;
    }
    std::vector<int> mapStrokes(const ShapeGraph& g)
    {
        std::vector<int> m(g.strokes.size() + 1, 0);
        for (size_t i = 0; i < g.strokes.size(); ++i) m[i + 1] = m_styles.addStroke(g.strokes[i]);
        return m;
    }

    Arrangement m_own;
    ShapeGraph m_styles;
    const ShapeGraph* m_base = nullptr;
    const ShapeGraph* m_top = nullptr;
    int m_nBase = 0, m_nTop = 0;
    std::vector<int> m_baseFill{0}, m_topFill{0}, m_baseStroke{0}, m_topStroke{0};
    std::map<int, int> m_extraStroke;
};

// Emits a new shape directly from the cached topology of g.
ShapeGraph emitFromTopology(const ShapeGraph& g, const std::function<int(int face)>& faceFill,
                            const std::function<int(int edge)>& edgeStroke)
{
    const Arrangement& a = g.topology();
    ShapeGraph out;
    out.fills = g.fills;
    out.strokes = g.strokes;
    std::vector<int> ff(a.faceCount());
    for (int f = 0; f < a.faceCount(); ++f) ff[f] = faceFill(f);
    for (int e = 0; e < int(a.edges().size()); ++e) {
        const int L = ff[a.face(2 * e)], R = ff[a.face(2 * e + 1)];
        const int S = edgeStroke(e);
        if (S != 0 || L != R) out.edges.push_back({a.edges()[e].curve, L, R, S});
    }
    out.compact();
    return out;
}

std::vector<Cubic> gapClosers(const ShapeGraph& g, double gap)
{
    std::vector<Cubic> out;
    if (gap <= 0.0 || g.edges.empty()) return out;
    const Arrangement& a = g.topology();
    const auto& V = a.vertices();
    std::vector<int> dangling;
    for (int v = 0; v < int(V.size()); ++v)
        if (a.fan(v).size() == 1) dangling.push_back(v);
    std::set<std::pair<int, int>> done;
    for (int v : dangling) {
        int best = -1;
        double bestD = gap;
        for (int w : dangling) {
            if (w == v) continue;
            const double d = distance(V[v], V[w]);
            if (d <= bestD) {
                bestD = d;
                best = w;
            }
        }
        if (best >= 0) {
            const auto key = std::minmax(v, best);
            if (done.insert(key).second) out.push_back(Cubic::line(V[v], V[best]));
            continue;
        }
        // Otherwise close towards the nearest other edge.
        const int ownEdge = Arrangement::edgeOf(a.fan(v)[0]);
        Vec2 target;
        double bestE = gap;
        bool found = false;
        for (int e = 0; e < int(a.edges().size()); ++e) {
            if (e == ownEdge) continue;
            const Cubic& c = a.edges()[e].curve;
            if (!c.controlBounds().inflated(gap).contains(V[v])) continue;
            double d;
            const double t = c.nearest(V[v], &d);
            if (d <= bestE && d > 1e-9) {
                bestE = d;
                target = c.eval(t);
                found = true;
            }
        }
        if (found) out.push_back(Cubic::line(V[v], target));
    }
    return out;
}

} // namespace

// --- construction ----------------------------------------------------------

ShapeGraph graphFromRegion(const Region& r, const FillStyle& fill)
{
    Engine e(1);
    const int fid = e.fillId(fill);
    e.addMask(r, 0);
    e.build();
    return e.emit([&](int f) { return e.inMask(f, 0) ? fid : 0; }, [](int, int, int) { return 0; });
}

ShapeGraph graphFromPaths(const std::vector<std::vector<Cubic>>& chains, const StrokeStyle& stroke)
{
    Engine e(0);
    const int sid = e.strokeId(stroke);
    for (const auto& ch : chains)
        for (const Cubic& c : ch) e.addExtra(c, sid);
    e.build();
    return e.emit([](int) { return 0; }, [&](int edge, int, int) { return e.extraStroke(edge); });
}

ShapeGraph graphFromShape(const Region& area, const FillStyle* fill, const StrokeStyle* stroke)
{
    Engine e(1);
    const int fid = fill ? e.fillId(*fill) : 0;
    const int sid = stroke ? e.strokeId(*stroke) : 0;
    e.addMask(area, 0);
    if (sid)
        for (const Contour& c : area.contours)
            for (const Cubic& cu : c) e.addExtra(cu, sid);
    e.build();
    return e.emit([&](int f) { return e.inMask(f, 0) ? fid : 0; },
                  [&](int edge, int, int) { return e.extraStroke(edge); });
}

// --- merging ------------------------------------------------------------------

ShapeGraph overlay(const ShapeGraph& base, const ShapeGraph& top, const OverlayOptions& opt)
{
    if (top.isEmpty()) return base;
    const bool useMask = opt.mask && (opt.mode == PaintMode::Selection || opt.mode == PaintMode::Inside);
    Engine e(useMask ? 1 : 0);
    e.addBase(base);
    e.addTop(top);
    if (useMask) e.addMask(*opt.mask, 0);
    e.build();
    auto faceFill = [&](int f) {
        const int b = e.baseLabel(f), t = e.topLabel(f);
        switch (opt.mode) {
        case PaintMode::Normal:
        case PaintMode::Fills: return t ? t : b;
        case PaintMode::Behind: return b ? b : t;
        case PaintMode::Selection: return (t && useMask && e.inMask(f, 0)) ? t : b;
        case PaintMode::Inside:
            if (opt.insideEmpty) return (t && !b) ? t : b;
            return (t && useMask && e.inMask(f, 0)) ? t : b;
        }
        return b;
    };
    auto edgeStroke = [&](int edge, int lf, int rf) {
        const int ts = e.topStroke(edge);
        if (ts) return ts;
        const int bs = e.baseStroke(edge);
        if (!bs) return 0;
        if (opt.mode == PaintMode::Normal && e.topLabel(lf) && e.topLabel(rf)) return 0; // painted over
        return bs;
    };
    return e.emit(faceFill, edgeStroke);
}

ShapeGraph erase(const ShapeGraph& base, const Region& eraser, EraseMode mode, const Region* mask)
{
    if (base.isEmpty() || eraser.isEmpty()) return base;
    Engine e(mask ? 2 : 1);
    e.addBase(base);
    e.addMask(eraser, 0);
    if (mask) e.addMask(*mask, 1);
    e.build();
    auto faceFill = [&](int f) {
        const int b = e.baseLabel(f);
        const bool in = e.inMask(f, 0);
        switch (mode) {
        case EraseMode::Normal:
        case EraseMode::Fills: return in ? 0 : b;
        case EraseMode::Lines: return b;
        case EraseMode::SelectedFills:
        case EraseMode::Inside: return (in && mask && e.inMask(f, 1)) ? 0 : b;
        }
        return b;
    };
    auto edgeStroke = [&](int edge, int lf, int rf) {
        const int bs = e.baseStroke(edge);
        if (!bs) return 0;
        if ((mode == EraseMode::Normal || mode == EraseMode::Lines) && e.inMask(lf, 0) && e.inMask(rf, 0)) return 0;
        return bs;
    };
    return e.emit(faceFill, edgeStroke);
}

// --- fills ---------------------------------------------------------------------

bool paintBucket(const ShapeGraph& base, Vec2 p, const FillStyle& fill, double gap, ShapeGraph& out)
{
    Engine e(0);
    const int fid = e.fillId(fill);
    e.addBase(base);
    for (const Cubic& c : gapClosers(base, gap)) e.addExtra(c, 0);
    e.build();
    const int face = e.arr().locate(p);
    if (face == 0) return false;
    if (e.baseLabel(face) == fid) return false;
    out = e.emit([&](int f) { return f == face ? fid : e.baseLabel(f); },
                 [&](int edge, int, int) { return e.baseStroke(edge); });
    return true;
}

Region bucketRegion(const ShapeGraph& base, Vec2 p, double gap)
{
    Engine e(0);
    e.addBase(base);
    for (const Cubic& c : gapClosers(base, gap)) e.addExtra(c, 0);
    e.build();
    Region r;
    const int face = e.arr().locate(p);
    if (face == 0) return r;
    r.contours = e.arr().faceContours(face);
    return r;
}

bool inkBottle(const ShapeGraph& base, Vec2 p, const StrokeStyle& stroke, double tol, ShapeGraph& out)
{
    const ShapeHit hit = hitTest(base, p, tol);
    if (hit.kind == ShapeHit::Kind::None) return false;
    ShapeGraph g = base;
    const int sid = g.addStroke(stroke);
    const Arrangement& a = base.topology();
    std::vector<char> mark(a.edges().size(), 0);
    if (hit.kind == ShapeHit::Kind::Stroke) {
        for (int e : selectStrokeRun(base, hit.arrEdge).edges) mark[e] = 1;
    } else {
        for (int h : a.faceHalfEdges(hit.face)) mark[Arrangement::edgeOf(h)] = 1;
    }
    out = emitFromTopology(
        g, [&](int f) { return a.value(f, 0); },
        [&](int e) { return mark[e] ? sid : base.topologyStroke(e); });
    return true;
}

ShapeGraph recolorFaces(const ShapeGraph& g, const std::vector<int>& faces, const FillStyle& fill)
{
    ShapeGraph h = g;
    const int fid = h.addFill(fill);
    const Arrangement& a = g.topology();
    std::set<int> fs(faces.begin(), faces.end());
    return emitFromTopology(
        h, [&](int f) { return fs.count(f) ? fid : a.value(f, 0); }, [&](int e) { return g.topologyStroke(e); });
}

ShapeGraph restrokeEdges(const ShapeGraph& g, const std::vector<int>& arrEdges, const StrokeStyle& stroke)
{
    ShapeGraph h = g;
    const int sid = h.addStroke(stroke);
    const Arrangement& a = g.topology();
    std::set<int> es(arrEdges.begin(), arrEdges.end());
    return emitFromTopology(
        h, [&](int f) { return a.value(f, 0); },
        [&](int e) {
            const int s = g.topologyStroke(e);
            return (es.count(e) && s) ? sid : s;
        });
}

// --- hit testing & selection -----------------------------------------------------

ShapeHit nearestEdge(const ShapeGraph& g, Vec2 p, double tol)
{
    ShapeHit best;
    double bestD = tol;
    for (int i = 0; i < int(g.edges.size()); ++i) {
        const Cubic& c = g.edges[i].c;
        if (!c.controlBounds().inflated(tol).contains(p)) continue;
        double d;
        const double t = c.nearest(p, &d);
        if (d <= bestD) {
            bestD = d;
            best.kind = g.edges[i].stroke ? ShapeHit::Kind::Stroke : ShapeHit::Kind::Fill;
            best.edge = i;
            best.t = t;
            best.distance = d;
        }
    }
    if (best.edge >= 0) {
        const Arrangement& a = g.topology();
        double bd = 1e300;
        for (int e = 0; e < int(a.edges().size()); ++e) {
            if (g.topologySource(e) != best.edge) continue;
            const double d = a.edges()[e].curve.distanceTo(p);
            if (d < bd) {
                bd = d;
                best.arrEdge = e;
            }
        }
    }
    return best;
}

ShapeHit hitTest(const ShapeGraph& g, Vec2 p, double tol)
{
    ShapeHit hit;
    if (g.edges.empty()) return hit;
    double bestD = 1e300;
    int bestEdge = -1;
    double bestT = 0;
    for (int i = 0; i < int(g.edges.size()); ++i) {
        const GEdge& e = g.edges[i];
        if (!e.stroke) continue;
        const double reach = std::max(tol, g.stroke(e.stroke).width * 0.5);
        if (!e.c.controlBounds().inflated(reach).contains(p)) continue;
        double d;
        const double t = e.c.nearest(p, &d);
        if (d <= reach && d < bestD) {
            bestD = d;
            bestEdge = i;
            bestT = t;
        }
    }
    const Arrangement& a = g.topology();
    if (bestEdge >= 0) {
        hit.kind = ShapeHit::Kind::Stroke;
        hit.edge = bestEdge;
        hit.t = bestT;
        hit.distance = bestD;
        double bd = 1e300;
        for (int e = 0; e < int(a.edges().size()); ++e) {
            if (g.topologySource(e) != bestEdge) continue;
            const double d = a.edges()[e].curve.distanceTo(p);
            if (d < bd) {
                bd = d;
                hit.arrEdge = e;
            }
        }
        return hit;
    }
    const int f = a.locate(p);
    if (f != 0 && a.value(f, 0) != 0) {
        hit.kind = ShapeHit::Kind::Fill;
        hit.face = f;
    }
    return hit;
}

void ShapeSelection::add(const ShapeSelection& o)
{
    for (int f : o.faces)
        if (!hasFace(f)) faces.push_back(f);
    for (int e : o.edges)
        if (!hasEdge(e)) edges.push_back(e);
}

bool ShapeSelection::hasFace(int f) const { return std::find(faces.begin(), faces.end(), f) != faces.end(); }
bool ShapeSelection::hasEdge(int e) const { return std::find(edges.begin(), edges.end(), e) != edges.end(); }

ShapeSelection selectStrokeRun(const ShapeGraph& g, int arrEdge)
{
    ShapeSelection sel;
    if (arrEdge < 0) return sel;
    const Arrangement& a = g.topology();
    const int style = g.topologyStroke(arrEdge);
    sel.edges.push_back(arrEdge);
    std::set<int> in{arrEdge};
    for (int startH : {2 * arrEdge, 2 * arrEdge + 1}) {
        int h = startH;
        while (true) {
            const int v = a.dest(h);
            int count = 0, cand = -1;
            for (int o : a.fan(v)) {
                const int e = Arrangement::edgeOf(o);
                if (g.topologyStroke(e) == 0) continue;
                ++count;
                if (e != Arrangement::edgeOf(h)) cand = o;
            }
            if (count != 2 || cand < 0) break;
            const int ce = Arrangement::edgeOf(cand);
            if (in.count(ce) || g.topologyStroke(ce) != style) break;
            // Stop at corners (tangent discontinuity), like Flash does.
            const Vec2 t1 = a.curve(h).endTangent(), t2 = a.curve(cand).startTangent();
            if (dot(t1, t2) < std::cos(12.0 * kPi / 180.0)) break;
            in.insert(ce);
            sel.edges.push_back(ce);
            h = cand;
        }
    }
    return sel;
}

ShapeSelection selectConnected(const ShapeGraph& g, const ShapeHit& hit)
{
    ShapeSelection sel;
    const Arrangement& a = g.topology();
    if (hit.kind == ShapeHit::Kind::Stroke && hit.arrEdge >= 0) {
        std::vector<char> seen(a.edges().size(), 0);
        std::queue<int> q;
        q.push(hit.arrEdge);
        seen[hit.arrEdge] = 1;
        while (!q.empty()) {
            const int e = q.front();
            q.pop();
            sel.edges.push_back(e);
            for (int v : {a.edges()[e].v0, a.edges()[e].v1})
                for (int o : a.fan(v)) {
                    const int ne = Arrangement::edgeOf(o);
                    if (!seen[ne] && g.topologyStroke(ne)) {
                        seen[ne] = 1;
                        q.push(ne);
                    }
                }
        }
    } else if (hit.kind == ShapeHit::Kind::Fill && hit.face >= 0) {
        sel.faces.push_back(hit.face);
        for (int h : a.faceHalfEdges(hit.face)) {
            const int e = Arrangement::edgeOf(h);
            if (g.topologyStroke(e) && !sel.hasEdge(e)) sel.edges.push_back(e);
        }
    }
    return sel;
}

ShapeSelection selectFace(const ShapeGraph& g, int face)
{
    ShapeSelection s;
    if (face > 0 && face < g.topology().faceCount()) s.faces.push_back(face);
    return s;
}

ShapeSelection selectAll(const ShapeGraph& g)
{
    ShapeSelection s;
    const Arrangement& a = g.topology();
    for (int f = 1; f < a.faceCount(); ++f)
        if (a.value(f, 0)) s.faces.push_back(f);
    for (int e = 0; e < int(a.edges().size()); ++e)
        if (g.topologyStroke(e)) s.edges.push_back(e);
    return s;
}

ShapeSelection selectInside(const ShapeGraph& g, const Region& r)
{
    ShapeSelection s;
    const Arrangement& a = g.topology();
    auto curveInside = [&](const Cubic& c) {
        for (double t : {0.0, 0.25, 0.5, 0.75, 1.0})
            if (!r.contains(c.eval(t))) return false;
        return true;
    };
    for (int f = 1; f < a.faceCount(); ++f) {
        if (!a.value(f, 0)) continue;
        bool all = true;
        for (int h : a.faceHalfEdges(f))
            if (!curveInside(a.curve(h))) {
                all = false;
                break;
            }
        if (all) s.faces.push_back(f);
    }
    for (int e = 0; e < int(a.edges().size()); ++e)
        if (g.topologyStroke(e) && curveInside(a.edges()[e].curve)) s.edges.push_back(e);
    return s;
}

Region selectionRegion(const ShapeGraph& g, const ShapeSelection& sel)
{
    Region r;
    if (sel.faces.empty()) return r;
    const Arrangement& a = g.topology();
    std::set<int> fs(sel.faces.begin(), sel.faces.end());
    for (const auto& loop : a.traceBoundary([&](int f) { return fs.count(f) > 0; }))
        r.contours.push_back(a.loopCurves(loop));
    return r;
}

std::vector<Cubic> selectionCurves(const ShapeGraph& g, const ShapeSelection& sel)
{
    std::vector<Cubic> out;
    const Arrangement& a = g.topology();
    for (int e : sel.edges)
        if (e >= 0 && e < int(a.edges().size())) out.push_back(a.edges()[e].curve);
    return out;
}

void liftSelection(const ShapeGraph& g, const ShapeSelection& sel, ShapeGraph& rest, ShapeGraph& lifted)
{
    const Arrangement& a = g.topology();
    std::vector<char> fsel(a.faceCount(), 0), esel(a.edges().size(), 0);
    for (int f : sel.faces)
        if (f >= 0 && f < a.faceCount()) fsel[f] = 1;
    for (int e : sel.edges)
        if (e >= 0 && e < int(esel.size())) esel[e] = 1;
    rest = emitFromTopology(
        g, [&](int f) { return fsel[f] ? 0 : a.value(f, 0); }, [&](int e) { return esel[e] ? 0 : g.topologyStroke(e); });
    lifted = emitFromTopology(
        g, [&](int f) { return fsel[f] ? a.value(f, 0) : 0; }, [&](int e) { return esel[e] ? g.topologyStroke(e) : 0; });
}

void cutByRegion(const ShapeGraph& g, const Region& r, ShapeGraph& rest, ShapeGraph& lifted)
{
    Engine e(1);
    e.addBase(g);
    e.addMask(r, 0);
    e.build();
    rest = e.emit([&](int f) { return e.inMask(f, 0) ? 0 : e.baseLabel(f); },
                  [&](int edge, int lf, int rf) {
                      const bool inside = e.inMask(lf, 0) && e.inMask(rf, 0);
                      return inside ? 0 : e.baseStroke(edge);
                  });
    lifted = e.emit([&](int f) { return e.inMask(f, 0) ? e.baseLabel(f) : 0; },
                    [&](int edge, int lf, int rf) {
                        const bool inside = e.inMask(lf, 0) && e.inMask(rf, 0);
                        return inside ? e.baseStroke(edge) : 0;
                    });
}

// --- direct editing -------------------------------------------------------

ShapeGraph normalizeGraph(const ShapeGraph& g)
{
    Engine e(0);
    e.addBase(g);
    e.build();
    return e.emit([&](int f) { return e.baseLabel(f); }, [&](int edge, int, int) { return e.baseStroke(edge); });
}

ShapeGraph bendEdge(const ShapeGraph& g, int edge, double t, Vec2 target)
{
    if (edge < 0 || edge >= int(g.edges.size())) return g;
    ShapeGraph h = g;
    h.edges[edge].c = g.edges[edge].c.bentThrough(t, target);
    return normalizeGraph(h);
}

ShapeGraph moveVertex(const ShapeGraph& g, Vec2 vertex, Vec2 target, double tol)
{
    ShapeGraph h = g;
    for (GEdge& e : h.edges) {
        const bool straight = e.c.isStraight(1e-9);
        const bool s = distance(e.c.p0, vertex) <= tol, t = distance(e.c.p3, vertex) <= tol;
        if (!s && !t) continue;
        if (s) {
            const Vec2 d = target - e.c.p0;
            e.c.p0 = target;
            e.c.p1 += d;
        }
        if (t) {
            const Vec2 d = target - e.c.p3;
            e.c.p3 = target;
            e.c.p2 += d;
        }
        if (straight) e.c = Cubic::line(e.c.p0, e.c.p3);
    }
    return normalizeGraph(h);
}

ShapeGraph moveHandle(const ShapeGraph& g, int edge, int handle, Vec2 target)
{
    if (edge < 0 || edge >= int(g.edges.size())) return g;
    ShapeGraph h = g;
    if (handle == 1) h.edges[edge].c.p1 = target;
    else h.edges[edge].c.p2 = target;
    return normalizeGraph(h);
}

bool nearestVertex(const ShapeGraph& g, Vec2 p, double tol, Vec2& vertex)
{
    double best = tol;
    bool found = false;
    for (const GEdge& e : g.edges)
        for (Vec2 v : {e.c.p0, e.c.p3}) {
            const double d = distance(v, p);
            if (d <= best) {
                best = d;
                vertex = v;
                found = true;
            }
        }
    return found;
}

ShapeGraph combineUnion(const std::vector<ShapeGraph>& graphs)
{
    ShapeGraph acc;
    for (const ShapeGraph& g : graphs) acc = acc.isEmpty() ? g : overlay(acc, g);
    return acc;
}

ShapeGraph combineIntersect(const ShapeGraph& a, const ShapeGraph& top)
{
    ShapeGraph rest, lifted;
    cutByRegion(top, a.fillRegion(0), rest, lifted);
    return lifted;
}

ShapeGraph combinePunch(const ShapeGraph& a, const ShapeGraph& top)
{
    return erase(a, top.fillRegion(0), EraseMode::Normal);
}

ShapeGraph combineCrop(const ShapeGraph& a, const ShapeGraph& top)
{
    ShapeGraph rest, lifted;
    cutByRegion(a, top.fillRegion(0), rest, lifted);
    return lifted;
}

} // namespace vx
