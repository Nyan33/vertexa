// SPDX-License-Identifier: GPL-3.0-or-later
#include "Outline.h"
#include "Fit.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

double signedAngle(Vec2 a, Vec2 b) { return std::atan2(cross(a, b), dot(a, b)); }

double normalizeNegative(double a)
{
    while (a > 0.0) a -= 2.0 * kPi;
    while (a <= -2.0 * kPi) a += 2.0 * kPi;
    return a;
}

void addLine(std::vector<Cubic>& c, Vec2 a, Vec2 b)
{
    if (distance(a, b) > 1e-12) c.push_back(Cubic::line(a, b));
}

void addArc(std::vector<Cubic>& c, Vec2 center, double r, Vec2 from, Vec2 to, double sweep)
{
    if (std::abs(sweep) < 1e-12 || r <= 0.0) {
        addLine(c, from, to);
        return;
    }
    const size_t start = c.size();
    appendArc(c, center, r, (from - center).angle(), sweep);
    if (c.size() > start) {
        c[start].p1 += from - c[start].p0;
        c[start].p0 = from;
        c.back().p2 += to - c.back().p3;
        c.back().p3 = to;
    }
}

// Removes circles that are swallowed by their neighbours: the union is
// unchanged, and every remaining segment has well-defined tangent lines.
std::vector<BrushPoint> pruneSwallowed(std::vector<BrushPoint> pts)
{
    bool changed = true;
    while (changed && pts.size() > 1) {
        changed = false;
        std::vector<BrushPoint> out;
        out.reserve(pts.size());
        for (const BrushPoint& q : pts) {
            if (!out.empty()) {
                const BrushPoint& prev = out.back();
                const double L = distance(prev.p, q.p);
                if (L + q.r <= prev.r * (1.0 + 1e-9) + 1e-9) {
                    changed = true;
                    continue; // q inside prev
                }
                if (L + prev.r <= q.r * (1.0 + 1e-9) + 1e-9) {
                    out.back() = q; // prev inside q
                    changed = true;
                    continue;
                }
            }
            out.push_back(q);
        }
        pts.swap(out);
    }
    return pts;
}

} // namespace

Region roundSweepOutline(const std::vector<BrushPoint>& input)
{
    std::vector<BrushPoint> pts;
    for (const BrushPoint& q : input)
        if (q.r > 0.0 && q.p.isFinite() && (pts.empty() || distance(pts.back().p, q.p) > 1e-9)) pts.push_back(q);
    pts = pruneSwallowed(pts);
    if (pts.empty()) return {};
    if (pts.size() == 1) return Region::circle(pts[0].p, pts[0].r);

    const size_t n = pts.size();
    std::vector<Vec2> u(n - 1), mL(n - 1), mR(n - 1);
    for (size_t i = 0; i + 1 < n; ++i) {
        const Vec2 d = pts[i + 1].p - pts[i].p;
        const double L = d.length();
        u[i] = d / L;
        const Vec2 N = u[i].perp();
        const double s = std::clamp((pts[i].r - pts[i + 1].r) / L, -1.0, 1.0);
        const double c = std::sqrt(std::max(0.0, 1.0 - s * s));
        mL[i] = u[i] * s + N * c;
        mR[i] = u[i] * s - N * c;
    }

    std::vector<Cubic> left, right;
    for (size_t i = 0; i + 1 < n; ++i) {
        const Vec2 pi = pts[i].p, pj = pts[i + 1].p;
        const double ri = pts[i].r, rj = pts[i + 1].r;
        if (i > 0) {
            const double turn = cross(u[i - 1], u[i]);
            const Vec2 prevL = pi + mL[i - 1] * ri, curL = pi + mL[i] * ri;
            const Vec2 prevR = pi + mR[i - 1] * ri, curR = pi + mR[i] * ri;
            if (turn > 0.0) {
                // Left turn: left side is the inner side -> pivot join.
                addLine(left, prevL, pi);
                addLine(left, pi, curL);
                addArc(right, pi, ri, prevR, curR, signedAngle(mR[i - 1], mR[i]));
            } else {
                addArc(left, pi, ri, prevL, curL, signedAngle(mL[i - 1], mL[i]));
                addLine(right, prevR, pi);
                addLine(right, pi, curR);
            }
        }
        addLine(left, pi + mL[i] * ri, pj + mL[i] * rj);
        addLine(right, pi + mR[i] * ri, pj + mR[i] * rj);
    }

    Contour c;
    const BrushPoint& a = pts.front();
    const BrushPoint& b = pts.back();
    const Vec2 startR = a.p + mR.front() * a.r, startL = a.p + mL.front() * a.r;
    addArc(c, a.p, a.r, startR, startL, normalizeNegative(mL.front().angle() - mR.front().angle()));
    c.insert(c.end(), left.begin(), left.end());
    const Vec2 endL = b.p + mL.back() * b.r, endR = b.p + mR.back() * b.r;
    addArc(c, b.p, b.r, endL, endR, normalizeNegative(mR.back().angle() - mL.back().angle()));
    for (auto it = right.rbegin(); it != right.rend(); ++it) c.push_back(it->reversed());
    // Exact continuity.
    for (size_t i = 0; i < c.size(); ++i) {
        Cubic& next = c[(i + 1) % c.size()];
        const Vec2 d = c[i].p3 - next.p0;
        next.p0 = c[i].p3;
        next.p1 += d;
    }
    Region r;
    r.contours.push_back(std::move(c));
    return r;
}

std::vector<Vec2> convexHull(std::vector<Vec2> pts)
{
    std::sort(pts.begin(), pts.end(), [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return pts;
    std::vector<Vec2> h(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 1] - h[k - 2], pts[i] - h[k - 2]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 1] - h[k - 2], pts[i - 1] - h[k - 2]) <= 0) --k;
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    return h;
}

std::vector<Vec2> tipPolygon(const BrushTip& tip, double r, double angle)
{
    std::vector<Vec2> poly;
    const double thin = std::max(0.35, r * 0.12);
    double hw = r, hh = r, rot = angle;
    switch (tip.shape) {
    case TipShape::Round:
    case TipShape::Ellipse:
    case TipShape::Square: break;
    case TipShape::Rectangle: hh = r * std::clamp(tip.aspect, 0.01, 1.0); break;
    case TipShape::Slash: hw = r * std::sqrt(2.0); hh = thin; rot += kPi / 4.0; break;
    case TipShape::Backslash: hw = r * std::sqrt(2.0); hh = thin; rot -= kPi / 4.0; break;
    case TipShape::Horizontal: hh = thin; break;
    case TipShape::Vertical: hw = thin; break;
    }
    for (Vec2 c : {Vec2{-hw, -hh}, Vec2{hw, -hh}, Vec2{hw, hh}, Vec2{-hw, hh}}) poly.push_back(rotated(c, rot));
    return poly;
}

Region sweptRegion(const std::vector<BrushPoint>& pts, const BrushTip& tip, double eps)
{
    if (pts.empty()) return {};
    const bool roundLike = tip.shape == TipShape::Round || tip.shape == TipShape::Ellipse;
    if (roundLike && (!tip.rotates || tip.shape == TipShape::Round || tip.aspect >= 1.0)) {
        // An ellipse with a fixed orientation is an affine image of a circle,
        // so the swept area is the affine image of a round sweep (exact).
        const double aspect = tip.shape == TipShape::Ellipse ? std::clamp(tip.aspect, 0.02, 1.0) : 1.0;
        const Affine toEllipse = Affine::rotate(tip.angle) * Affine::scale(1.0, aspect) * Affine::rotate(-tip.angle);
        const Affine inv = toEllipse.inverted();
        std::vector<BrushPoint> local = pts;
        if (aspect != 1.0)
            for (BrushPoint& q : local) q.p = inv.map(q.p);
        Region r = normalizeRegion(roundSweepOutline(local), FillRule::NonZero, eps);
        return aspect != 1.0 ? r.transformed(toEllipse) : r;
    }
    // Polygonal tips (and rotating ellipses): union of the convex hulls of
    // consecutive tip placements — the exact Minkowski sweep of each step.
    std::vector<Region> hulls;
    auto polyAt = [&](const BrushPoint& q) {
        if (roundLike) {
            std::vector<Vec2> poly;
            const int k = 48;
            const double aspect = tip.shape == TipShape::Ellipse ? tip.aspect : 1.0;
            for (int i = 0; i < k; ++i) {
                const double a = 2.0 * kPi * i / k;
                poly.push_back(q.p + rotated({std::cos(a) * q.r, std::sin(a) * q.r * aspect}, tip.angle + q.angle));
            }
            return poly;
        }
        std::vector<Vec2> poly = tipPolygon(tip, q.r, tip.angle + q.angle);
        for (Vec2& v : poly) v += q.p;
        return poly;
    };
    if (pts.size() == 1) return Region::polygon(convexHull(polyAt(pts[0])));
    std::vector<Vec2> prev = polyAt(pts[0]);
    for (size_t i = 1; i < pts.size(); ++i) {
        std::vector<Vec2> cur = polyAt(pts[i]);
        std::vector<Vec2> both = prev;
        both.insert(both.end(), cur.begin(), cur.end());
        Region h = Region::polygon(convexHull(both));
        if (!h.isEmpty() && std::abs(h.area()) > 1e-12) hulls.push_back(std::move(h));
        prev = std::move(cur);
    }
    return uniteAll(hulls, eps);
}

std::vector<BrushPoint> resampleStroke(const std::vector<BrushPoint>& pts, double spacing)
{
    if (pts.size() < 2 || spacing <= 0.0) return pts;
    std::vector<BrushPoint> out;
    out.push_back(pts.front());
    double carry = 0.0;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const BrushPoint& a = pts[i];
        const BrushPoint& b = pts[i + 1];
        const double L = distance(a.p, b.p);
        if (L <= 0.0) continue;
        double s = spacing - carry;
        while (s <= L) {
            const double t = s / L;
            out.push_back({lerp(a.p, b.p, t), a.r + (b.r - a.r) * t, a.angle + (b.angle - a.angle) * t});
            s += spacing;
        }
        carry = L - (s - spacing);
    }
    if (distance(out.back().p, pts.back().p) > spacing * 0.25) out.push_back(pts.back());
    else out.back() = pts.back();
    return out;
}

Region refitRegion(const Region& r, double tolerance, double cornerAngle)
{
    Region out;
    for (const Contour& c : r.contours) {
        std::vector<Vec2> pts;
        sampleChain(c, std::max(tolerance * 1.5, 1e-4), pts);
        if (pts.size() > 1 && distance(pts.front(), pts.back()) < 1e-9) pts.pop_back();
        if (pts.size() < 3) continue;
        FitOptions opt;
        opt.tolerance = tolerance;
        opt.closed = true;
        opt.cornerAngle = cornerAngle;
        opt.cornerRadius = tolerance * 4.0;
        Contour f = fitCurves(pts, opt);
        if (f.size() >= 2 || (f.size() == 1 && distance(f[0].p0, f[0].p3) < 1e-9)) out.contours.push_back(std::move(f));
    }
    return out;
}

} // namespace vx
