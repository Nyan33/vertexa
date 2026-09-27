// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShapeTween.h"
#include "ShapeOps.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

struct Blob {
    FillStyle style;
    Contour outer;
    std::vector<Contour> holes;
    double area = 0.0;
    Vec2 centroid;
};

struct Line {
    StrokeStyle style;
    std::vector<Cubic> chain;
    bool closed = false;
    double length = 0.0;
};

double contourArea(const Contour& c)
{
    double a = 0.0;
    for (const Cubic& cu : c) a += cu.areaContribution();
    return a;
}

Vec2 contourCentroid(const Contour& c)
{
    Vec2 acc;
    int n = 0;
    for (const Cubic& cu : c)
        for (double t : {0.0, 0.25, 0.5, 0.75}) {
            acc += cu.eval(t);
            ++n;
        }
    return n ? acc / double(n) : Vec2{};
}

double approxLength(const Cubic& c)
{
    return distance(c.p0, c.p1) + distance(c.p1, c.p2) + distance(c.p2, c.p3);
}

void subdivideTo(std::vector<Cubic>& c, size_t n)
{
    if (c.empty()) return;
    while (c.size() < n) {
        size_t best = 0;
        double bestL = -1.0;
        for (size_t i = 0; i < c.size(); ++i) {
            const double l = approxLength(c[i]);
            if (l > bestL) {
                bestL = l;
                best = i;
            }
        }
        auto [l, r] = c[best].split(0.5);
        c[best] = l;
        c.insert(c.begin() + long(best) + 1, r);
    }
}

std::vector<Blob> blobsOf(const ShapeRenderData& rd)
{
    std::vector<Blob> blobs;
    for (const auto& fp : rd.fills) {
        std::vector<int> outers, holes;
        for (int i = 0; i < int(fp.contours.size()); ++i)
            (contourArea(fp.contours[i]) >= 0 ? outers : holes).push_back(i);
        const size_t first = blobs.size();
        for (int i : outers) {
            Blob b;
            b.style = fp.style;
            b.outer = fp.contours[i];
            b.area = contourArea(b.outer);
            b.centroid = contourCentroid(b.outer);
            blobs.push_back(std::move(b));
        }
        for (int h : holes) {
            const Vec2 p = fp.contours[h].front().p0;
            int bestBlob = -1;
            double bestArea = 1e300;
            for (size_t k = first; k < blobs.size(); ++k)
                if (blobs[k].area < bestArea && windingNumber(p, blobs[k].outer) != 0) {
                    bestArea = blobs[k].area;
                    bestBlob = int(k);
                }
            if (bestBlob >= 0) blobs[bestBlob].holes.push_back(fp.contours[h]);
        }
    }
    std::stable_sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) { return a.area > b.area; });
    for (Blob& b : blobs)
        std::stable_sort(b.holes.begin(), b.holes.end(),
                         [](const Contour& x, const Contour& y) { return contourArea(x) < contourArea(y); });
    return blobs;
}

std::vector<Line> linesOf(const ShapeRenderData& rd)
{
    std::vector<Line> lines;
    for (const auto& sp : rd.strokes)
        for (size_t i = 0; i < sp.chains.size(); ++i) {
            Line l;
            l.style = sp.style;
            l.chain = sp.chains[i];
            l.closed = sp.closed[i];
            for (const Cubic& c : l.chain) l.length += approxLength(c);
            lines.push_back(std::move(l));
        }
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.length > b.length; });
    return lines;
}

double polarLerpAngle(double a, double b, double t)
{
    double d = std::fmod(b - a, 2.0 * kPi);
    if (d > kPi) d -= 2.0 * kPi;
    if (d < -kPi) d += 2.0 * kPi;
    return a + d * t;
}

Vec2 lerpHandle(Vec2 a, Vec2 b, double t)
{
    const double la = a.length(), lb = b.length();
    if (la < 1e-12 || lb < 1e-12) return lerp(a, b, t);
    return fromAngle(polarLerpAngle(a.angle(), b.angle(), t), la + (lb - la) * t);
}

Cubic lerpCubic(const Cubic& a, const Cubic& b, double t, bool angular)
{
    if (!angular) return {lerp(a.p0, b.p0, t), lerp(a.p1, b.p1, t), lerp(a.p2, b.p2, t), lerp(a.p3, b.p3, t)};
    const Vec2 p0 = lerp(a.p0, b.p0, t), p3 = lerp(a.p3, b.p3, t);
    return {p0, p0 + lerpHandle(a.p1 - a.p0, b.p1 - b.p0, t), p3 + lerpHandle(a.p2 - a.p3, b.p2 - b.p3, t), p3};
}

std::vector<Cubic> rotateChain(const std::vector<Cubic>& c, size_t k)
{
    std::vector<Cubic> out;
    out.reserve(c.size());
    for (size_t i = 0; i < c.size(); ++i) out.push_back(c[(i + k) % c.size()]);
    return out;
}

size_t bestOffset(const Contour& a, const Contour& b)
{
    const size_t n = a.size();
    size_t best = 0;
    double bestCost = 1e300;
    for (size_t k = 0; k < n; ++k) {
        double cost = 0.0;
        for (size_t i = 0; i < n && cost < bestCost; ++i) cost += distanceSq(a[i].p0, b[(i + k) % n].p0);
        if (cost < bestCost) {
            bestCost = cost;
            best = k;
        }
    }
    return best;
}

// Inserts a node at the point of the contour closest to p; returns its index.
size_t insertNode(Contour& c, Vec2 p)
{
    size_t bestI = 0;
    double bestD = 1e300, bestT = 0.0;
    for (size_t i = 0; i < c.size(); ++i) {
        double d;
        const double t = c[i].nearest(p, &d);
        if (d < bestD) {
            bestD = d;
            bestI = i;
            bestT = t;
        }
    }
    if (bestT < 1e-6) return bestI;
    if (bestT > 1.0 - 1e-6) return (bestI + 1) % c.size();
    auto [l, r] = c[bestI].split(bestT);
    c[bestI] = l;
    c.insert(c.begin() + long(bestI) + 1, r);
    return bestI + 1;
}

// Align two closed contours so node i of a corresponds to node i of b.
void alignContours(Contour& a, Contour& b, const std::vector<ShapeHint>& hints)
{
    if (a.empty() || b.empty()) return;
    if (hints.size() >= 1) {
        // Insert hint nodes, then resample each section between hints.
        std::vector<size_t> ia, ib;
        std::vector<Vec2> pa, pb;
        for (const ShapeHint& h : hints) {
            pa.push_back(h.start);
            pb.push_back(h.end);
        }
        for (const Vec2& p : pa) insertNode(a, p);
        for (const Vec2& p : pb) insertNode(b, p);
        auto nodeIndex = [](const Contour& c, Vec2 p) {
            size_t best = 0;
            double bd = 1e300;
            for (size_t i = 0; i < c.size(); ++i)
                if (distanceSq(c[i].p0, p) < bd) {
                    bd = distanceSq(c[i].p0, p);
                    best = i;
                }
            return best;
        };
        for (size_t k = 0; k < hints.size(); ++k) {
            ia.push_back(nodeIndex(a, pa[k]));
            ib.push_back(nodeIndex(b, pb[k]));
        }
        // Hints must appear in the same cyclic order in both contours.
        auto cyclicOrder = [](const std::vector<size_t>& idx, size_t n) {
            std::vector<size_t> rel;
            for (size_t v : idx) rel.push_back((v + n - idx[0]) % n);
            return std::is_sorted(rel.begin(), rel.end()) &&
                   std::adjacent_find(rel.begin(), rel.end()) == rel.end();
        };
        if (cyclicOrder(ia, a.size()) && cyclicOrder(ib, b.size())) {
            Contour na, nb;
            const size_t H = hints.size();
            for (size_t k = 0; k < H; ++k) {
                std::vector<Cubic> sa, sb;
                const size_t a0 = ia[k], a1 = ia[(k + 1) % H], b0 = ib[k], b1 = ib[(k + 1) % H];
                for (size_t i = a0;; i = (i + 1) % a.size()) {
                    if (!sa.empty() && i == a1) break;
                    sa.push_back(a[i]);
                    if (H == 1 && sa.size() == a.size()) break;
                }
                for (size_t i = b0;; i = (i + 1) % b.size()) {
                    if (!sb.empty() && i == b1) break;
                    sb.push_back(b[i]);
                    if (H == 1 && sb.size() == b.size()) break;
                }
                const size_t n = std::max(sa.size(), sb.size());
                subdivideTo(sa, n);
                subdivideTo(sb, n);
                na.insert(na.end(), sa.begin(), sa.end());
                nb.insert(nb.end(), sb.begin(), sb.end());
            }
            a.swap(na);
            b.swap(nb);
            return;
        }
    }
    const size_t n = std::max({a.size(), b.size(), size_t(4)});
    subdivideTo(a, n);
    subdivideTo(b, n);
    b = rotateChain(b, bestOffset(a, b));
}

Contour lerpContour(const Contour& a, const Contour& b, double t, bool angular)
{
    Contour out;
    out.reserve(a.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) out.push_back(lerpCubic(a[i], b[i], t, angular));
    for (size_t i = 0; i + 1 < out.size(); ++i) out[i + 1].p0 = out[i].p3;
    if (!out.empty()) out.back().p3 = out.front().p0;
    return out;
}

Contour scaledContour(const Contour& c, Vec2 center, double s)
{
    Contour out;
    const Affine m = Affine::about(center, Affine::scale(s));
    for (const Cubic& cu : c) out.push_back(cu.transformed(m));
    return out;
}

void morphOpen(std::vector<Cubic> a, std::vector<Cubic> b, double t, bool angular, std::vector<Cubic>& out)
{
    const size_t n = std::max(a.size(), b.size());
    subdivideTo(a, n);
    subdivideTo(b, n);
    // Pick the direction of b that matches a best.
    std::vector<Cubic> rb;
    for (auto it = b.rbegin(); it != b.rend(); ++it) rb.push_back(it->reversed());
    subdivideTo(rb, n);
    double fwd = distanceSq(a.front().p0, b.front().p0) + distanceSq(a.back().p3, b.back().p3);
    double rev = distanceSq(a.front().p0, rb.front().p0) + distanceSq(a.back().p3, rb.back().p3);
    const std::vector<Cubic>& bb = rev < fwd ? rb : b;
    out.clear();
    for (size_t i = 0; i < n; ++i) out.push_back(lerpCubic(a[i], bb[i], t, angular));
    for (size_t i = 0; i + 1 < out.size(); ++i) out[i + 1].p0 = out[i].p3;
}

void includeBounds(Rect& r, const std::vector<Cubic>& c, double inflate)
{
    for (const Cubic& cu : c) r.include(cu.bounds().inflated(inflate));
}

} // namespace

FillStyle lerpFill(const FillStyle& a, const FillStyle& b, double t)
{
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    if (a.kind == FillStyle::Kind::Solid && b.kind == FillStyle::Kind::Solid)
        return FillStyle::solid(lerpColor(a.color, b.color, t));
    // Promote solids to gradients of the other's kind.
    auto asGradient = [](const FillStyle& f, const FillStyle& like) {
        if (f.isGradient()) return f;
        FillStyle g = like;
        for (GradientStop& s : g.gradient.stops) s.color = f.color;
        return g;
    };
    FillStyle ga = asGradient(a, b), gb = asGradient(b, a);
    if (ga.kind != gb.kind || ga.gradient.stops.size() != gb.gradient.stops.size()) return t < 0.5 ? a : b;
    FillStyle o = ga;
    for (size_t i = 0; i < o.gradient.stops.size(); ++i) {
        o.gradient.stops[i].pos = ga.gradient.stops[i].pos + (gb.gradient.stops[i].pos - ga.gradient.stops[i].pos) * t;
        o.gradient.stops[i].color = lerpColor(ga.gradient.stops[i].color, gb.gradient.stops[i].color, t);
    }
    const Affine& ma = ga.gradient.matrix;
    const Affine& mb = gb.gradient.matrix;
    auto l = [t](double x, double y) { return x + (y - x) * t; };
    o.gradient.matrix = {l(ma.a, mb.a), l(ma.b, mb.b), l(ma.c, mb.c), l(ma.d, mb.d), l(ma.tx, mb.tx), l(ma.ty, mb.ty)};
    o.gradient.focal = l(ga.gradient.focal, gb.gradient.focal);
    return o;
}

StrokeStyle lerpStroke(const StrokeStyle& a, const StrokeStyle& b, double t)
{
    StrokeStyle s = t < 0.5 ? a : b;
    s.paint = lerpFill(a.paint, b.paint, t);
    s.width = a.width + (b.width - a.width) * t;
    return s;
}

ShapeRenderData collectShapeData(const std::vector<ElementPtr>& elements)
{
    ShapeGraph merged;
    bool any = false;
    for (const ElementPtr& e : elements) {
        const ShapeElement* s = asShape(e);
        if (!s || !s->graph || s->graph->isEmpty()) continue;
        const ShapeGraph g = s->matrix.isIdentity() ? *s->graph : s->graph->transformed(s->matrix);
        merged = any ? overlay(merged, g) : g;
        any = true;
    }
    return any ? buildRenderData(merged) : ShapeRenderData{};
}

ShapeRenderData morphShapes(const ShapeRenderData& a, const ShapeRenderData& b, double t, bool angular,
                            const std::vector<ShapeHint>& hints)
{
    ShapeRenderData out;
    std::vector<Blob> ba = blobsOf(a), bb = blobsOf(b);
    const size_t nb = std::max(ba.size(), bb.size());
    for (size_t i = 0; i < nb; ++i) {
        ShapeRenderData::FillPath fp;
        if (i < ba.size() && i < bb.size()) {
            Blob x = ba[i], y = bb[i];
            alignContours(x.outer, y.outer, i == 0 ? hints : std::vector<ShapeHint>{});
            fp.style = lerpFill(x.style, y.style, t);
            fp.contours.push_back(lerpContour(x.outer, y.outer, t, angular));
            const size_t nh = std::max(x.holes.size(), y.holes.size());
            for (size_t h = 0; h < nh; ++h) {
                if (h < x.holes.size() && h < y.holes.size()) {
                    Contour hx = x.holes[h], hy = y.holes[h];
                    alignContours(hx, hy, {});
                    fp.contours.push_back(lerpContour(hx, hy, t, angular));
                } else if (h < x.holes.size()) {
                    fp.contours.push_back(scaledContour(x.holes[h], contourCentroid(x.holes[h]), 1.0 - t));
                } else {
                    fp.contours.push_back(scaledContour(y.holes[h], contourCentroid(y.holes[h]), t));
                }
            }
        } else if (i < ba.size()) {
            const Blob& x = ba[i];
            fp.style = x.style;
            fp.contours.push_back(scaledContour(x.outer, x.centroid, 1.0 - t));
            for (const Contour& h : x.holes) fp.contours.push_back(scaledContour(h, x.centroid, 1.0 - t));
        } else {
            const Blob& y = bb[i];
            fp.style = y.style;
            fp.contours.push_back(scaledContour(y.outer, y.centroid, t));
            for (const Contour& h : y.holes) fp.contours.push_back(scaledContour(h, y.centroid, t));
        }
        for (const Contour& c : fp.contours) includeBounds(out.bounds, c, 0.0);
        out.fills.push_back(std::move(fp));
    }

    std::vector<Line> la = linesOf(a), lb = linesOf(b);
    const size_t nl = std::max(la.size(), lb.size());
    for (size_t i = 0; i < nl; ++i) {
        ShapeRenderData::StrokePath sp;
        std::vector<Cubic> chain;
        bool closed = false;
        if (i < la.size() && i < lb.size()) {
            sp.style = lerpStroke(la[i].style, lb[i].style, t);
            if (la[i].closed && lb[i].closed) {
                Contour x = la[i].chain, y = lb[i].chain;
                alignContours(x, y, {});
                chain = lerpContour(x, y, t, angular);
                closed = true;
            } else {
                morphOpen(la[i].chain, lb[i].chain, t, angular, chain);
            }
        } else {
            const Line& l = i < la.size() ? la[i] : lb[i];
            const double s = i < la.size() ? 1.0 - t : t;
            sp.style = l.style;
            Vec2 c;
            for (const Cubic& cu : l.chain) c += cu.eval(0.5);
            c = c / double(std::max<size_t>(1, l.chain.size()));
            for (const Cubic& cu : l.chain) chain.push_back(cu.transformed(Affine::about(c, Affine::scale(s))));
            closed = l.closed;
        }
        includeBounds(out.bounds, chain, sp.style.width * 0.5);
        sp.chains.push_back(std::move(chain));
        sp.closed.push_back(closed ? 1 : 0);
        out.strokes.push_back(std::move(sp));
    }
    return out;
}

ShapeGraph graphFromRenderData(const ShapeRenderData& rd)
{
    ShapeGraph g;
    bool any = false;
    for (const auto& fp : rd.fills) {
        Region r;
        r.contours = fp.contours;
        const ShapeGraph f = graphFromRegion(r, fp.style);
        g = any ? overlay(g, f) : f;
        any = true;
    }
    for (const auto& sp : rd.strokes) {
        const ShapeGraph s = graphFromPaths(sp.chains, sp.style);
        g = any ? overlay(g, s) : s;
        any = true;
    }
    return g;
}

} // namespace vx
