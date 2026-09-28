// SPDX-License-Identifier: GPL-3.0-or-later
#include "Arrangement.h"
#include "Intersect.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace vx {

namespace {

struct DisjointSet {
    std::vector<int> parent;
    explicit DisjointSet(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x)
    {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
};

uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t cellKey(int64_t gx, int64_t gy) { return mix64(uint64_t(gx)) ^ (mix64(uint64_t(gy)) << 1); }

// Crossing contribution of a y-monotone curve for the ray p -> +x, using the
// half-open rule [ylo, yhi) so vertices are counted exactly once.
int monotoneCrossing(Vec2 p, const Cubic& c)
{
    const double y0 = c.p0.y, y3 = c.p3.y;
    if (y0 == y3) return 0;
    const bool up = y3 > y0;
    const double ylo = up ? y0 : y3, yhi = up ? y3 : y0;
    if (!(p.y >= ylo && p.y < yhi)) return 0;
    const double xmin = std::min({c.p0.x, c.p1.x, c.p2.x, c.p3.x});
    const double xmax = std::max({c.p0.x, c.p1.x, c.p2.x, c.p3.x});
    bool right;
    if (p.x < xmin) {
        right = true;
    } else if (p.x >= xmax) {
        right = false;
    } else {
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 64; ++i) {
            const double mid = 0.5 * (lo + hi);
            const double y = c.eval(mid).y;
            if ((y < p.y) == up) lo = mid;
            else hi = mid;
        }
        right = c.eval(0.5 * (lo + hi)).x > p.x;
    }
    return right ? (up ? 1 : -1) : 0;
}

int curveCrossing(Vec2 p, const Cubic& c)
{
    double ts[2];
    const int n = c.yExtrema(ts);
    if (n == 0) return monotoneCrossing(p, c);
    int w = 0;
    double prev = 0.0;
    for (int i = 0; i <= n; ++i) {
        const double t = i < n ? ts[i] : 1.0;
        w += monotoneCrossing(p, c.sub(prev, t));
        prev = t;
    }
    return w;
}

bool sameGeometry(const Cubic& a, const Cubic& b, double tol)
{
    if (a.isStraight(tol * 0.1) && b.isStraight(tol * 0.1)) return true;
    if (distance(a.p1, b.p1) <= tol && distance(a.p2, b.p2) <= tol) return true;
    for (double u : {0.1, 0.3, 0.5, 0.7, 0.9})
        if (b.distanceTo(a.eval(u)) > tol || a.distanceTo(b.eval(u)) > tol) return false;
    return true;
}

} // namespace

int windingNumber(Vec2 p, const std::vector<Cubic>& curves)
{
    int w = 0;
    for (const Cubic& c : curves) {
        const Rect r = c.controlBounds();
        if (p.y < r.y0 || p.y > r.y1 || p.x > r.x1) continue;
        w += curveCrossing(p, c);
    }
    return w;
}

Arrangement::Arrangement(double eps) : m_eps(eps) {}

int Arrangement::add(const ArrInput& in)
{
    m_inputs.push_back(in);
    return int(m_inputs.size()) - 1;
}

void Arrangement::addCurve(const Cubic& c, int layer, int tag)
{
    ArrInput in;
    in.curve = c;
    in.layer = layer;
    in.tag = tag;
    add(in);
}

void Arrangement::addContour(const std::vector<Cubic>& contour, int layer, int tag)
{
    for (const Cubic& c : contour) addCurve(c, layer, tag);
}

void Arrangement::build()
{
    struct Piece {
        Cubic c;
        int input;
        bool straight;
    };
    std::vector<Piece> pieces;
    pieces.reserve(m_inputs.size() * 2);
    std::vector<Cubic> mono;
    for (int i = 0; i < int(m_inputs.size()); ++i) {
        const Cubic& c = m_inputs[i].curve;
        if (c.isDegenerate(m_eps)) continue;
        if (c.isStraight(m_eps * 0.01)) {
            if (distance(c.p0, c.p3) <= m_eps) continue;
            pieces.push_back({Cubic::line(c.p0, c.p3), i, true});
            continue;
        }
        mono.clear();
        c.monotonePieces(mono);
        for (const Cubic& m : mono)
            if (!m.isDegenerate(m_eps)) pieces.push_back({m, i, m.isStraight(m_eps * 0.01)});
    }
    const int P = int(pieces.size());

    // Point events: piece end points and intersections.
    struct Split {
        double t;
        int ev;
    };
    std::vector<Vec2> ev;
    std::vector<char> evIsEnd;
    std::vector<std::vector<Split>> splits(P);
    for (int i = 0; i < P; ++i) {
        splits[i].push_back({0.0, int(ev.size())});
        ev.push_back(pieces[i].c.p0);
        evIsEnd.push_back(1);
        splits[i].push_back({1.0, int(ev.size())});
        ev.push_back(pieces[i].c.p3);
        evIsEnd.push_back(1);
    }

    // Sweep over x to find candidate pairs.
    std::vector<Rect> bb(P);
    for (int i = 0; i < P; ++i) bb[i] = pieces[i].c.controlBounds();
    std::vector<int> order(P);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return bb[a].x0 < bb[b].x0; });
    std::vector<int> active;
    std::vector<CurveHit> hits;
    for (int oi = 0; oi < P; ++oi) {
        const int j = order[oi];
        for (size_t k = 0; k < active.size();) {
            if (bb[active[k]].x1 < bb[j].x0 - m_eps) {
                active[k] = active.back();
                active.pop_back();
            } else {
                ++k;
            }
        }
        const int gj = m_inputs[pieces[j].input].group;
        for (int i : active) {
            if (!bb[i].intersects(bb[j], m_eps)) continue;
            if (gj >= 0 && m_inputs[pieces[i].input].group == gj) continue;
            hits.clear();
            intersectCurves(pieces[i].c, pieces[j].c, hits, m_eps);
            for (const CurveHit& h : hits) {
                const int e = int(ev.size());
                ev.push_back(h.p);
                evIsEnd.push_back(0);
                splits[i].push_back({h.t1, e});
                splits[j].push_back({h.t2, e});
            }
        }
        active.push_back(j);
    }

    // Cluster coincident events into vertices.
    DisjointSet dsu(ev.size());
    {
        const double mergeDist = 2.0 * m_eps;
        const double cell = 4.0 * mergeDist;
        std::unordered_map<uint64_t, std::vector<int>> grid;
        grid.reserve(ev.size());
        for (int e = 0; e < int(ev.size()); ++e) {
            const int64_t gx = int64_t(std::floor(ev[e].x / cell)), gy = int64_t(std::floor(ev[e].y / cell));
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    auto it = grid.find(cellKey(gx + dx, gy + dy));
                    if (it == grid.end()) continue;
                    for (int f : it->second)
                        if (distance(ev[e], ev[f]) <= mergeDist) dsu.unite(e, f);
                }
            grid[cellKey(gx, gy)].push_back(e);
        }
    }
    std::vector<int> vertexOfRoot(ev.size(), -1);
    std::vector<int> vertexOf(ev.size(), -1);
    m_vertices.clear();
    // First pass: roots with an end-point event take the exact end point.
    for (int e = 0; e < int(ev.size()); ++e) {
        const int r = dsu.find(e);
        if (vertexOfRoot[r] < 0 && evIsEnd[e]) {
            vertexOfRoot[r] = int(m_vertices.size());
            m_vertices.push_back(ev[e]);
        }
    }
    for (int e = 0; e < int(ev.size()); ++e) {
        const int r = dsu.find(e);
        if (vertexOfRoot[r] < 0) {
            vertexOfRoot[r] = int(m_vertices.size());
            m_vertices.push_back(ev[e]);
        }
        vertexOf[e] = vertexOfRoot[r];
    }

    // Split pieces into edges.
    std::vector<ArrEdge> raw;
    raw.reserve(P * 2);
    for (int i = 0; i < P; ++i) {
        auto& sp = splits[i];
        std::sort(sp.begin(), sp.end(), [](const Split& a, const Split& b) { return a.t < b.t; });
        struct Stop {
            double t;
            int v;
        };
        std::vector<Stop> stops;
        for (const Split& s : sp) {
            const int v = vertexOf[s.ev];
            if (!stops.empty() && (stops.back().v == v || s.t - stops.back().t < 1e-13)) {
                // Same vertex (or same parameter): keep the extreme parameters
                // at the ends of the piece.
                if (s.t == 1.0 && stops.back().v == v && stops.size() > 1) stops.back().t = 1.0;
                continue;
            }
            stops.push_back({s.t, v});
        }
        const Piece& pc = pieces[i];
        for (size_t k = 0; k + 1 < stops.size(); ++k) {
            const Stop a = stops[k], b = stops[k + 1];
            const Vec2 va = m_vertices[a.v], vb = m_vertices[b.v];
            Cubic c;
            if (pc.straight) {
                c = Cubic::line(va, vb);
            } else {
                c = pc.c.sub(a.t, b.t);
                c.p1 += va - c.p0;
                c.p0 = va;
                c.p2 += vb - c.p3;
                c.p3 = vb;
            }
            ArrEdge e;
            e.curve = c;
            e.v0 = a.v;
            e.v1 = b.v;
            e.sources.push_back({pc.input, false});
            raw.push_back(std::move(e));
        }
    }

    // Merge coincident edges (overlapping input curves).
    m_edges.clear();
    std::unordered_map<uint64_t, std::vector<int>> byKey;
    const double dupTol = m_eps * 10.0;
    for (ArrEdge& e : raw) {
        if (e.v0 == e.v1) {
            if (e.curve.length() <= m_eps * 4.0) continue;
            m_edges.push_back(std::move(e));
            continue;
        }
        const uint64_t key = (uint64_t(uint32_t(std::min(e.v0, e.v1))) << 32) | uint32_t(std::max(e.v0, e.v1));
        auto& list = byKey[key];
        bool merged = false;
        for (int f : list) {
            ArrEdge& g = m_edges[f];
            const bool same = g.v0 == e.v0;
            const Cubic ec = same ? e.curve : e.curve.reversed();
            if (sameGeometry(g.curve, ec, dupTol)) {
                for (const ArrEdgeSource& s : e.sources) g.sources.push_back({s.input, same ? s.reversed : !s.reversed});
                merged = true;
                break;
            }
        }
        if (!merged) {
            list.push_back(int(m_edges.size()));
            m_edges.push_back(std::move(e));
        }
    }

    computeFans();
    computeCycles();
    computeFaces();
    propagate();
}

void Arrangement::computeFans()
{
    const int H = halfEdgeCount();
    m_fans.assign(m_vertices.size(), {});
    m_fanIndex.assign(H, -1);
    for (int h = 0; h < H; ++h) m_fans[origin(h)].push_back(h);

    static constexpr double kLevels[] = {0.0, 1e-7, 1e-5, 1e-3, 1e-2, 0.05, 0.2, 0.5};
    auto dirAt = [&](int h, int level) {
        const Cubic c = curve(h);
        if (level == 0) return c.startTangent();
        return (c.eval(kLevels[level]) - c.p0).normalized();
    };

    for (auto& fan : m_fans) {
        const int n = int(fan.size());
        if (n >= 2) {
            std::vector<double> ang(n);
            for (int i = 0; i < n; ++i) ang[i] = dirAt(fan[i], 0).angle();
            // Cut the circle in the middle of the largest gap so that nearly
            // equal angles never straddle the +-pi discontinuity.
            std::vector<double> sorted = ang;
            std::sort(sorted.begin(), sorted.end());
            double cut = -kPi, bestGap = -1.0;
            for (int i = 0; i < n; ++i) {
                const double a = sorted[i];
                const double b = (i + 1 < n) ? sorted[i + 1] : sorted[0] + 2.0 * kPi;
                if (b - a > bestGap) {
                    bestGap = b - a;
                    cut = a + 0.5 * (b - a);
                }
            }
            auto rel = [&](double a) {
                double r = std::fmod(a - cut, 2.0 * kPi);
                if (r < 0) r += 2.0 * kPi;
                return r;
            };
            std::vector<std::pair<double, int>> keyed(n);
            for (int i = 0; i < n; ++i) keyed[i] = {rel(ang[i]), fan[i]};
            std::sort(keyed.begin(), keyed.end());
            // Resolve runs of (nearly) identical tangents using points
            // further along the curves.
            for (int i = 0; i < n;) {
                int j = i + 1;
                while (j < n && keyed[j].first - keyed[j - 1].first < 1e-9) ++j;
                if (j - i > 1) {
                    for (int level = 1; level < int(std::size(kLevels)); ++level) {
                        std::vector<std::pair<double, int>> sub;
                        for (int k = i; k < j; ++k) sub.push_back({rel(dirAt(keyed[k].second, level).angle()), keyed[k].second});
                        std::sort(sub.begin(), sub.end());
                        bool distinct = true;
                        for (size_t k = 1; k < sub.size(); ++k)
                            if (sub[k].first - sub[k - 1].first < 1e-12) distinct = false;
                        if (distinct || level + 1 == int(std::size(kLevels))) {
                            for (int k = i; k < j; ++k) keyed[k].second = sub[k - i].second;
                            break;
                        }
                    }
                }
                i = j;
            }
            for (int i = 0; i < n; ++i) fan[i] = keyed[i].second;
        }
        for (int i = 0; i < n; ++i) m_fanIndex[fan[i]] = i;
    }
}

void Arrangement::computeCycles()
{
    const int H = halfEdgeCount();
    m_next.assign(H, -1);
    for (int h = 0; h < H; ++h) {
        const auto& fan = m_fans[dest(h)];
        const int n = int(fan.size());
        const int k = m_fanIndex[twin(h)];
        m_next[h] = fan[(k - 1 + n) % n];
    }
    m_cycleOf.assign(H, -1);
    m_cycles.clear();
    for (int h = 0; h < H; ++h) {
        if (m_cycleOf[h] >= 0) continue;
        Cycle cy;
        cy.first = h;
        const int id = int(m_cycles.size());
        int cur = h;
        do {
            m_cycleOf[cur] = id;
            const Cubic c = curve(cur);
            cy.area += c.areaContribution();
            cy.bounds.include(c.controlBounds());
            ++cy.length;
            cur = m_next[cur];
        } while (cur != h && m_cycleOf[cur] < 0 && cy.length <= H);
        m_cycles.push_back(cy);
    }
}

int Arrangement::cycleWinding(Vec2 p, int cycle) const
{
    const Cycle& cy = m_cycles[cycle];
    if (!cy.bounds.contains(p)) return 0;
    int w = 0;
    int cur = cy.first;
    for (int i = 0; i < cy.length; ++i) {
        const Cubic c = curve(cur);
        const Rect r = c.controlBounds();
        if (!(p.y < r.y0 || p.y > r.y1 || p.x > r.x1)) w += curveCrossing(p, c);
        cur = m_next[cur];
    }
    return w;
}

void Arrangement::computeFaces()
{
    DisjointSet comp(m_vertices.size());
    for (const ArrEdge& e : m_edges) comp.unite(e.v0, e.v1);
    m_faces.clear();
    m_faces.push_back({}); // unbounded
    std::vector<int> positive, other;
    for (int c = 0; c < int(m_cycles.size()); ++c) {
        Cycle& cy = m_cycles[c];
        cy.component = comp.find(origin(cy.first));
        const double d = std::max(1.0, cy.bounds.diagonal());
        if (cy.area > 1e-10 * d * d) {
            cy.face = int(m_faces.size());
            Face f;
            f.outer = c;
            m_faces.push_back(f);
            positive.push_back(c);
        } else {
            other.push_back(c);
        }
    }
    for (int c : other) {
        Cycle& cy = m_cycles[c];
        const Vec2 p = m_vertices[origin(cy.first)];
        int best = -1;
        double bestArea = 0.0;
        for (int q : positive) {
            const Cycle& qc = m_cycles[q];
            if (qc.component == cy.component) continue;
            if (best >= 0 && qc.area >= bestArea) continue;
            if (!qc.bounds.contains(p)) continue;
            if (cycleWinding(p, q) != 0) {
                best = q;
                bestArea = qc.area;
            }
        }
        cy.face = best < 0 ? 0 : m_cycles[best].face;
        m_faces[cy.face].holes.push_back(c);
    }
}

void Arrangement::propagate()
{
    const size_t L = m_kinds.size();
    const int F = faceCount();
    m_values.assign(size_t(F) * L, 0);
    if (L == 0) return;
    std::vector<char> seen(F, 0);
    std::queue<int> q;
    q.push(0);
    seen[0] = 1;
    std::vector<int> sum(L);
    std::vector<int> labelSrc(L);
    while (!q.empty()) {
        const int f = q.front();
        q.pop();
        for (int h : faceHalfEdges(f)) {
            const int g = face(twin(h));
            if (seen[g]) continue;
            // Crossing from the left of h (face f) to its right (face g).
            std::fill(sum.begin(), sum.end(), 0);
            std::fill(labelSrc.begin(), labelSrc.end(), -1);
            const ArrEdge& e = m_edges[edgeOf(h)];
            for (size_t si = 0; si < e.sources.size(); ++si) {
                const ArrEdgeSource& s = e.sources[si];
                const int layer = m_inputs[s.input].layer;
                if (layer < 0 || layer >= int(L)) continue;
                if (m_kinds[layer] == LayerKind::Winding) sum[layer] += sourceAlong(h, s) ? 1 : -1;
                else if (labelSrc[layer] < 0) labelSrc[layer] = int(si);
            }
            for (size_t l = 0; l < L; ++l) {
                int& out = m_values[size_t(g) * L + l];
                const int in = m_values[size_t(f) * L + l];
                if (m_kinds[l] == LayerKind::Winding) {
                    out = in - sum[l];
                } else if (labelSrc[l] >= 0) {
                    const ArrEdgeSource& s = e.sources[labelSrc[l]];
                    const ArrInput& inp = m_inputs[s.input];
                    out = sourceAlong(h, s) ? inp.labelRight : inp.labelLeft;
                } else {
                    out = in;
                }
            }
            seen[g] = 1;
            q.push(g);
        }
    }
}

std::vector<int> Arrangement::faceHalfEdges(int f) const
{
    std::vector<int> out;
    auto addCycle = [&](int c) {
        const Cycle& cy = m_cycles[c];
        int cur = cy.first;
        for (int i = 0; i < cy.length; ++i) {
            out.push_back(cur);
            cur = m_next[cur];
        }
    };
    const Face& face = m_faces[f];
    if (face.outer >= 0) addCycle(face.outer);
    for (int c : face.holes) addCycle(c);
    return out;
}

double Arrangement::faceArea(int f) const
{
    const Face& face = m_faces[f];
    double a = face.outer >= 0 ? m_cycles[face.outer].area : 0.0;
    for (int c : face.holes) a += m_cycles[c].area;
    return a;
}

std::vector<std::vector<Cubic>> Arrangement::faceContours(int f) const
{
    std::vector<std::vector<Cubic>> out;
    auto addCycle = [&](int c) {
        const Cycle& cy = m_cycles[c];
        std::vector<int> loop;
        int cur = cy.first;
        for (int i = 0; i < cy.length; ++i) {
            loop.push_back(cur);
            cur = m_next[cur];
        }
        out.push_back(loopCurves(loop));
    };
    const Face& face = m_faces[f];
    if (face.outer >= 0) addCycle(face.outer);
    for (int c : face.holes) addCycle(c);
    return out;
}

int Arrangement::locate(Vec2 p) const
{
    int best = -1;
    double bestArea = 0.0;
    for (int c = 0; c < int(m_cycles.size()); ++c) {
        const Cycle& cy = m_cycles[c];
        if (cy.face == 0 || m_faces[cy.face].outer != c) continue;
        if (best >= 0 && cy.area >= bestArea) continue;
        if (!cy.bounds.contains(p)) continue;
        if (cycleWinding(p, c) != 0) {
            best = c;
            bestArea = cy.area;
        }
    }
    return best < 0 ? 0 : m_cycles[best].face;
}

std::vector<std::vector<int>> Arrangement::traceBoundary(const std::function<bool(int)>& inside) const
{
    const int H = halfEdgeCount();
    std::vector<char> in(m_faces.size());
    for (int f = 0; f < faceCount(); ++f) in[f] = inside(f) ? 1 : 0;
    auto isBoundary = [&](int h) { return in[face(h)] && !in[face(twin(h))]; };
    std::vector<char> used(H, 0);
    std::vector<std::vector<int>> loops;
    for (int h = 0; h < H; ++h) {
        if (used[h] || !isBoundary(h)) continue;
        std::vector<int> loop;
        int cur = h;
        while (true) {
            used[cur] = 1;
            loop.push_back(cur);
            const auto& fan = m_fans[dest(cur)];
            const int n = int(fan.size());
            const int k = m_fanIndex[twin(cur)];
            int nxt = -1;
            for (int j = 1; j <= n; ++j) {
                const int o = fan[((k - j) % n + n) % n];
                if (isBoundary(o)) {
                    nxt = o;
                    break;
                }
            }
            if (nxt < 0 || nxt == h || used[nxt]) break;
            cur = nxt;
        }
        loops.push_back(std::move(loop));
    }
    return loops;
}

std::vector<Cubic> Arrangement::loopCurves(const std::vector<int>& loop, bool join) const
{
    std::vector<Cubic> out;
    out.reserve(loop.size());
    for (int h : loop) {
        const Cubic c = curve(h);
        Cubic joined;
        if (join && !out.empty() && tryJoinCubics(out.back(), c, joined, m_eps * 10.0)) out.back() = joined;
        else out.push_back(c);
    }
    if (join && out.size() > 1) {
        Cubic joined;
        if (tryJoinCubics(out.back(), out.front(), joined, m_eps * 10.0)) {
            out.front() = joined;
            out.pop_back();
        }
    }
    return out;
}

} // namespace vx
