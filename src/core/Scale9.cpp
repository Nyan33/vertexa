// SPDX-License-Identifier: GPL-3.0-or-later
#include "Scale9.h"
#include "Evaluate.h"

#include "geom/Polynomial.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

bool setupAxis(double b0, double b1, double g0, double g1, double s, double& corner, double& middle)
{
    if (!(b1 - b0 > 1e-9) || !(g0 > b0) || !(g1 < b1) || !(g1 > g0) || s <= 1e-12) return false;
    const double ends = (g0 - b0) + (b1 - g1), mid = g1 - g0;
    const double target = s * (b1 - b0); // total size after the instance scale
    if (target >= ends) {
        corner = 1.0 / s;
        middle = (target - ends) / (s * mid);
    } else {
        // Too small for the corners: they shrink together, the middle vanishes.
        corner = target / ends / s;
        middle = 0.0;
    }
    return true;
}

/// Parameters in (0, 1) where one coordinate of `c` crosses `v`.
void crossings(const Cubic& c, bool y, double v, std::vector<double>& ts)
{
    const double p0 = y ? c.p0.y : c.p0.x, p1 = y ? c.p1.y : c.p1.x, p2 = y ? c.p2.y : c.p2.x, p3 = y ? c.p3.y : c.p3.x;
    if (std::min({p0, p1, p2, p3}) >= v || std::max({p0, p1, p2, p3}) <= v) return;
    double roots[3];
    const int n = solveCubicInRange(-p0 + 3 * p1 - 3 * p2 + p3, 3 * p0 - 6 * p1 + 3 * p2, -3 * p0 + 3 * p1, p0 - v, 0.0, 1.0, roots);
    for (int i = 0; i < n; ++i)
        if (roots[i] > 1e-9 && roots[i] < 1 - 1e-9) ts.push_back(roots[i]);
}

} // namespace

double Slice9::Axis::map(double v) const
{
    const double x0 = b0 + (g0 - b0) * corner;
    const double x1 = x0 + (g1 - g0) * middle;
    if (v < g0) return b0 + (v - b0) * corner;
    if (v <= g1) return x0 + (v - g0) * middle;
    return x1 + (v - g1) * corner;
}

std::optional<Slice9> Slice9::make(const Rect& grid, const Rect& bounds, double sx, double sy)
{
    if (std::abs(sx - 1.0) < 1e-9 && std::abs(sy - 1.0) < 1e-9) return std::nullopt;
    Slice9 s;
    s.m_x = {bounds.x0, grid.x0, grid.x1, bounds.x1};
    s.m_y = {bounds.y0, grid.y0, grid.y1, bounds.y1};
    const bool okx = setupAxis(bounds.x0, bounds.x1, grid.x0, grid.x1, sx, s.m_x.corner, s.m_x.middle);
    const bool oky = setupAxis(bounds.y0, bounds.y1, grid.y0, grid.y1, sy, s.m_y.corner, s.m_y.middle);
    if (!okx && !oky) return std::nullopt;
    // An axis whose guides fall outside the content scales normally.
    if (!okx) s.m_x = {bounds.x0, bounds.x1, bounds.x1, bounds.x1, 1.0, 1.0};
    if (!oky) s.m_y = {bounds.y0, bounds.y1, bounds.y1, bounds.y1, 1.0, 1.0};
    return s;
}

ShapeGraph Slice9::apply(const ShapeGraph& src, const Affine& toSymbol) const
{
    ShapeGraph out = src;
    out.edges.clear();
    std::vector<double> ts;
    for (const GEdge& e : src.edges) {
        const Cubic c = toSymbol.isIdentity() ? e.c : e.c.transformed(toSymbol);
        ts.assign({0.0, 1.0});
        for (double v : {m_x.g0, m_x.g1}) crossings(c, false, v, ts);
        for (double v : {m_y.g0, m_y.g1}) crossings(c, true, v, ts);
        std::sort(ts.begin(), ts.end());
        ts.erase(std::unique(ts.begin(), ts.end(), [](double a, double b) { return b - a < 1e-9; }), ts.end());
        for (size_t i = 0; i + 1 < ts.size(); ++i) {
            const Cubic piece = ts.size() == 2 ? c : c.sub(ts[i], ts[i + 1]);
            // Inside one cell the map is affine: scale around the cell's anchor.
            const Vec2 mid = c.eval(0.5 * (ts[i] + ts[i + 1]));
            const double kx = m_x.scaleAt(mid.x), ky = m_y.scaleAt(mid.y);
            const Vec2 o = mid, mo = map(mid);
            auto f = [&](Vec2 p) { return Vec2{mo.x + (p.x - o.x) * kx, mo.y + (p.y - o.y) * ky}; };
            GEdge ne = e;
            ne.c = {f(piece.p0), f(piece.p1), f(piece.p2), f(piece.p3)};
            // Snap the ends onto the exact map so neighbouring pieces meet.
            ne.c.p0 = map(piece.p0);
            ne.c.p3 = map(piece.p3);
            out.edges.push_back(ne);
        }
    }
    return out;
}

std::optional<Slice9> instanceSlice9(const Document& doc, const InstanceElement& in, int symbolFrame)
{
    const Symbol* s = doc.symbol(in.symbolId);
    if (!s || !s->scale9) return std::nullopt;
    const Affine& m = in.matrix;
    const double sx = std::hypot(m.a, m.b), sy = std::hypot(m.c, m.d);
    if (std::abs(sx - 1.0) < 1e-9 && std::abs(sy - 1.0) < 1e-9) return std::nullopt;
    const Rect bounds = timelineBounds(doc, s->timeline, symbolFrame);
    if (bounds.isEmpty()) return std::nullopt;
    return Slice9::make(*s->scale9, bounds, sx, sy);
}

} // namespace vx
