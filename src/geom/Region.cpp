// SPDX-License-Identifier: GPL-3.0-or-later
#include "Region.h"
#include "Arrangement.h"

#include <algorithm>
#include <cmath>

namespace vx {

Rect Region::bounds() const
{
    Rect r;
    for (const Contour& c : contours)
        for (const Cubic& cu : c) r.include(cu.bounds());
    return r;
}

double Region::area() const
{
    double a = 0.0;
    for (const Contour& c : contours)
        for (const Cubic& cu : c) a += cu.areaContribution();
    return a;
}

int Region::winding(Vec2 p) const
{
    int w = 0;
    for (const Contour& c : contours) w += windingNumber(p, c);
    return w;
}

Region Region::transformed(const Affine& m) const
{
    Region r;
    r.contours.reserve(contours.size());
    const bool flip = m.det() < 0.0;
    for (const Contour& c : contours) {
        Contour t;
        t.reserve(c.size());
        for (const Cubic& cu : c) t.push_back(cu.transformed(m));
        if (flip) {
            // Keep outer contours positive after a mirror transform.
            std::reverse(t.begin(), t.end());
            for (Cubic& cu : t) cu = cu.reversed();
        }
        r.contours.push_back(std::move(t));
    }
    return r;
}

std::vector<Cubic> Region::curves() const
{
    std::vector<Cubic> out;
    for (const Contour& c : contours) out.insert(out.end(), c.begin(), c.end());
    return out;
}

int Region::curveCount() const
{
    int n = 0;
    for (const Contour& c : contours) n += int(c.size());
    return n;
}

Region Region::reversed() const
{
    Region r = *this;
    for (Contour& c : r.contours) {
        std::reverse(c.begin(), c.end());
        for (Cubic& cu : c) cu = cu.reversed();
    }
    return r;
}

Region Region::rect(const Rect& r)
{
    Region g;
    if (r.isEmpty()) return g;
    const Vec2 a{r.x0, r.y0}, b{r.x1, r.y0}, c{r.x1, r.y1}, d{r.x0, r.y1};
    g.contours.push_back({Cubic::line(a, b), Cubic::line(b, c), Cubic::line(c, d), Cubic::line(d, a)});
    return g;
}

Region Region::roundedRect(const Rect& r, double radius)
{
    radius = std::min({radius, r.width() * 0.5, r.height() * 0.5});
    if (radius <= 0.0) return rect(r);
    Region g;
    Contour c;
    const double x0 = r.x0, y0 = r.y0, x1 = r.x1, y1 = r.y1, R = radius;
    // Counter-clockwise in math orientation (y axis pointing down on screen
    // this is visually clockwise, but it is what gives a positive area).
    auto line = [&](Vec2 a, Vec2 b) {
        if (distance(a, b) > 1e-12) c.push_back(Cubic::line(a, b));
    };
    line({x0 + R, y0}, {x1 - R, y0});
    appendArc(c, {x1 - R, y0 + R}, R, -kPi / 2, kPi / 2);
    line({x1, y0 + R}, {x1, y1 - R});
    appendArc(c, {x1 - R, y1 - R}, R, 0, kPi / 2);
    line({x1 - R, y1}, {x0 + R, y1});
    appendArc(c, {x0 + R, y1 - R}, R, kPi / 2, kPi / 2);
    line({x0, y1 - R}, {x0, y0 + R});
    appendArc(c, {x0 + R, y0 + R}, R, kPi, kPi / 2);
    // Snap arc end points onto the neighbouring lines exactly.
    for (size_t i = 0; i < c.size(); ++i) c[(i + 1) % c.size()].p0 = c[i].p3;
    g.contours.push_back(std::move(c));
    return g;
}

Region Region::ellipse(Vec2 center, double rx, double ry)
{
    Region g;
    if (rx <= 0.0 || ry <= 0.0) return g;
    Contour c;
    appendEllipseArc(c, Affine(rx, 0, 0, ry, center.x, center.y), 0.0, 2.0 * kPi);
    c.back().p3 = c.front().p0;
    g.contours.push_back(std::move(c));
    return g;
}

Region Region::polygon(const std::vector<Vec2>& pts)
{
    Region g;
    if (pts.size() < 3) return g;
    Contour c;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vec2 a = pts[i], b = pts[(i + 1) % pts.size()];
        if (distance(a, b) > 1e-12) c.push_back(Cubic::line(a, b));
    }
    if (c.size() >= 2) g.contours.push_back(std::move(c));
    return g;
}

namespace {

Region collect(const Arrangement& arr, const std::function<bool(int)>& inside)
{
    Region r;
    for (const auto& loop : arr.traceBoundary(inside)) {
        Contour c = arr.loopCurves(loop);
        if (!c.empty()) r.contours.push_back(std::move(c));
    }
    return r;
}

} // namespace

Region booleanOp(const Region& a, const Region& b, BoolOp op, double eps)
{
    Arrangement arr(eps);
    arr.setLayers({LayerKind::Winding, LayerKind::Winding});
    for (const Contour& c : a.contours) arr.addContour(c, 0);
    for (const Contour& c : b.contours) arr.addContour(c, 1);
    arr.build();
    return collect(arr, [&](int f) {
        const bool ia = arr.value(f, 0) != 0, ib = arr.value(f, 1) != 0;
        switch (op) {
        case BoolOp::Union: return ia || ib;
        case BoolOp::Intersect: return ia && ib;
        case BoolOp::Subtract: return ia && !ib;
        case BoolOp::Xor: return ia != ib;
        }
        return false;
    });
}

Region normalizeRegion(const Region& a, FillRule rule, double eps)
{
    Arrangement arr(eps);
    for (const Contour& c : a.contours) arr.addContour(c, 0);
    arr.build();
    return collect(arr, [&](int f) {
        const int w = arr.value(f, 0);
        return rule == FillRule::NonZero ? w != 0 : (w & 1) != 0;
    });
}

Region uniteAll(const std::vector<Region>& regions, double eps)
{
    Arrangement arr(eps);
    for (const Region& r : regions) {
        // Orient every region positively so that windings only add up.
        const Region& src = r;
        const bool neg = src.area() < 0.0;
        for (const Contour& c : src.contours) {
            if (!neg) {
                arr.addContour(c, 0);
            } else {
                Contour rc = c;
                std::reverse(rc.begin(), rc.end());
                for (Cubic& cu : rc) cu = cu.reversed();
                arr.addContour(rc, 0);
            }
        }
    }
    arr.build();
    return collect(arr, [&](int f) { return arr.value(f, 0) != 0; });
}

Contour closeChain(const std::vector<Cubic>& chain, double eps)
{
    Contour c = chain;
    if (c.empty()) return c;
    if (distance(c.back().p3, c.front().p0) > eps) c.push_back(Cubic::line(c.back().p3, c.front().p0));
    else c.back().p3 = c.front().p0;
    return c;
}

} // namespace vx
