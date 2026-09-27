// SPDX-License-Identifier: GPL-3.0-or-later
#include "VectorBrush.h"
#include "ShapeOps.h"

#include "geom/Fit.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace vx {

// --- response curve ----------------------------------------------------------------

double ResponseCurve::eval(double x) const
{
    x = std::clamp(x, 0.0, 1.0);
    if (points.size() < 2) return x;
    if (x <= points.front().x) return std::clamp(points.front().y, 0.0, 1.0);
    if (x >= points.back().x) return std::clamp(points.back().y, 0.0, 1.0);
    // Monotone cubic Hermite interpolation (Fritsch-Carlson).
    const size_t n = points.size();
    size_t k = 0;
    while (k + 1 < n && points[k + 1].x < x) ++k;
    auto slope = [&](size_t i) {
        const double dx = points[i + 1].x - points[i].x;
        return dx > 0 ? (points[i + 1].y - points[i].y) / dx : 0.0;
    };
    auto tangent = [&](size_t i) {
        if (i == 0) return slope(0);
        if (i == n - 1) return slope(n - 2);
        const double a = slope(i - 1), b = slope(i);
        if (a * b <= 0) return 0.0;
        return 2.0 / (1.0 / a + 1.0 / b);
    };
    const Vec2 p0 = points[k], p1 = points[k + 1];
    const double h = p1.x - p0.x;
    if (h <= 0) return std::clamp(p1.y, 0.0, 1.0);
    const double t = (x - p0.x) / h;
    const double t2 = t * t, t3 = t2 * t;
    const double y = (2 * t3 - 3 * t2 + 1) * p0.y + (t3 - 2 * t2 + t) * h * tangent(k) + (-2 * t3 + 3 * t2) * p1.y +
                     (t3 - t2) * h * tangent(k + 1);
    return std::clamp(y, 0.0, 1.0);
}

bool ResponseCurve::isLinear() const
{
    return points.size() == 2 && points[0] == Vec2{0, 0} && points[1] == Vec2{1, 1};
}

namespace {

// --- noise ---------------------------------------------------------------------------

uint32_t hash3(int32_t x, int32_t y, uint32_t salt)
{
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ salt * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

double unit(uint32_t h) { return (h & 0xffffff) / double(0x1000000); }

double lattice(Vec2 p, uint32_t salt)
{
    const double fx = std::floor(p.x), fy = std::floor(p.y);
    const int32_t ix = int32_t(fx), iy = int32_t(fy);
    auto smooth = [](double t) { return t * t * (3 - 2 * t); };
    const double tx = smooth(p.x - fx), ty = smooth(p.y - fy);
    auto v = [&](int32_t dx, int32_t dy) { return unit(hash3(ix + dx, iy + dy, salt)) * 2.0 - 1.0; };
    const double a = v(0, 0) + (v(1, 0) - v(0, 0)) * tx;
    const double b = v(0, 1) + (v(1, 1) - v(0, 1)) * tx;
    return a + (b - a) * ty;
}

// --- the centre line as a moving frame ----------------------------------------------

struct Frame {
    std::vector<Vec2> pos, nrm;
    std::vector<double> s, r;
    double length = 0.0;
    double rmax = 0.0, rmean = 0.0;

    bool valid() const { return pos.size() >= 2 && length > 1e-9; }

    /// Point at arc length `at`, offset by `v` half widths along the normal.
    Vec2 map(double at, double v) const
    {
        at = std::clamp(at, 0.0, length);
        size_t i = size_t(std::upper_bound(s.begin(), s.end(), at) - s.begin());
        i = std::clamp<size_t>(i, 1, s.size() - 1);
        const double seg = s[i] - s[i - 1];
        const double t = seg > 1e-12 ? (at - s[i - 1]) / seg : 0.0;
        const Vec2 p = lerp(pos[i - 1], pos[i], t);
        Vec2 n = lerp(nrm[i - 1], nrm[i], t);
        const double len = n.length();
        n = len > 1e-12 ? n / len : nrm[i];
        const double rr = r[i - 1] + (r[i] - r[i - 1]) * t;
        return p + n * (v * rr);
    }
};

Frame buildFrame(const std::vector<BrushPoint>& path, double spacing)
{
    Frame f;
    const std::vector<BrushPoint> pts = resampleStroke(path, spacing);
    for (const BrushPoint& b : pts) {
        if (!f.pos.empty() && distance(f.pos.back(), b.p) < 1e-9) continue;
        f.pos.push_back(b.p);
        f.r.push_back(b.r);
    }
    const size_t n = f.pos.size();
    if (n < 2) return f;
    f.s.assign(n, 0.0);
    for (size_t i = 1; i < n; ++i) f.s[i] = f.s[i - 1] + distance(f.pos[i - 1], f.pos[i]);
    f.length = f.s.back();
    std::vector<Vec2> tan(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec2 d = f.pos[std::min(i + 1, n - 1)] - f.pos[i == 0 ? 0 : i - 1];
        const double len = d.length();
        tan[i] = len > 1e-12 ? d / len : (i > 0 ? tan[i - 1] : Vec2{1, 0});
    }
    // Smooth the tangents a little so the frame does not flip at jitter.
    std::vector<Vec2> sm(n);
    for (size_t i = 0; i < n; ++i) {
        Vec2 acc;
        for (int k = -2; k <= 2; ++k) acc += tan[size_t(std::clamp<long>(long(i) + k, 0, long(n) - 1))];
        const double len = acc.length();
        sm[i] = len > 1e-12 ? acc / len : tan[i];
    }
    f.nrm.resize(n);
    for (size_t i = 0; i < n; ++i) f.nrm[i] = {-sm[i].y, sm[i].x};
    double sum = 0;
    for (double rr : f.r) {
        f.rmax = std::max(f.rmax, rr);
        sum += rr;
    }
    f.rmean = sum / double(n);
    return f;
}

std::vector<Vec2> flatten(const Contour& c, double step)
{
    std::vector<Vec2> out;
    for (const Cubic& cu : c) {
        const int n = std::clamp(int(std::ceil(cu.length() / std::max(step, 1e-6))), 2, 4000);
        for (int k = 0; k < n; ++k) out.push_back(cu.eval(double(k) / n));
    }
    return out;
}

Contour fitClosed(std::vector<Vec2> pts, double tol)
{
    pts = dedupePoints(pts, tol * 0.25);
    if (pts.size() > 2 && distance(pts.front(), pts.back()) < tol * 0.25) pts.pop_back();
    if (pts.size() < 3) return {};
    FitOptions opt;
    opt.tolerance = tol;
    opt.cornerAngle = 0.7;
    opt.closed = true;
    return fitCurves(pts, opt);
}

/// Artwork box: x along the path, y across (centred on the box).
struct ArtBox {
    double x0, w, cy, hh;
};

ArtBox artBox(const Rect& b)
{
    return {b.x0, std::max(b.width(), 1e-9), 0.5 * (b.y0 + b.y1), std::max(0.5 * b.height(), 1e-9)};
}

/// Maps artwork contours onto [a0, a1] (arc length) of the frame.
void warpInto(const Region& art, const ArtBox& box, const Frame& f, double a0, double a1, double tol, Region& out)
{
    const double span = std::max(a1 - a0, 1e-9);
    // Flatten finely enough in both directions of the artwork.
    const double stepAlong = 1.2 * tol * 8.0 * box.w / span;
    const double stepAcross = 1.2 * tol * 8.0 * box.hh / std::max(f.rmax, 1e-9);
    const double step = std::max(std::min(stepAlong, stepAcross), 1e-6);
    for (const Contour& c : art.contours) {
        std::vector<Vec2> mapped;
        for (const Vec2 q : flatten(c, step)) {
            const double u = (q.x - box.x0) / box.w;
            const double v = (q.y - box.cy) / box.hh;
            mapped.push_back(f.map(a0 + u * span, v));
        }
        Contour fitted = fitClosed(std::move(mapped), tol);
        if (!fitted.empty()) out.contours.push_back(std::move(fitted));
    }
}

struct ArtPieces {
    std::vector<BrushPiece> pieces;
    Rect box;
};

ArtPieces artPieces(const VectorBrushPreset& p, const FillStyle& paint)
{
    ArtPieces out;
    if (!p.art || p.art->isEmpty()) return out;
    const ShapeGraph& g = *p.art;
    if (p.colorize) {
        Region all = g.fillRegion(0);
        if (!all.isEmpty()) out.pieces.push_back({paint, all});
    } else {
        for (size_t i = 1; i <= g.fills.size(); ++i) {
            Region r = g.fillRegion(int(i));
            if (!r.isEmpty()) out.pieces.push_back({g.fills[i - 1], r});
        }
    }
    for (const BrushPiece& piece : out.pieces) out.box.include(piece.region.bounds());
    return out;
}

std::vector<BrushPiece> artStroke(const VectorBrushPreset& p, const Frame& f, const FillStyle& paint, double tol)
{
    std::vector<BrushPiece> result;
    const ArtPieces art = artPieces(p, paint);
    if (art.pieces.empty() || art.box.isEmpty()) return result;
    const ArtBox box = artBox(art.box);
    std::vector<std::pair<double, double>> spans;
    if (p.kind == VectorBrushKind::Art) {
        spans.push_back({0.0, f.length});
    } else {
        // Tile length follows the artwork's aspect at the mean width.
        const double tile = box.w / (2.0 * box.hh) * 2.0 * f.rmean * (1.0 + p.patternGap);
        double count = f.length / std::max(tile, 1e-6);
        const int n = std::max(1, int(p.stretchToFit ? std::lround(count) : std::floor(count)));
        const double step = p.stretchToFit ? f.length / n : tile;
        const double fill = step / (1.0 + p.patternGap);
        for (int i = 0; i < n; ++i) spans.push_back({i * step, i * step + fill});
    }
    for (const BrushPiece& piece : art.pieces) {
        Region warped;
        for (const auto& [a0, a1] : spans) warpInto(piece.region, box, f, a0, a1, tol, warped);
        if (warped.isEmpty()) continue;
        Region norm = normalizeRegion(warped);
        if (!norm.isEmpty()) result.push_back({piece.fill, std::move(norm)});
    }
    return result;
}

/// Boundary of a region as short segments bucketed on a grid, for fast
/// "near the edge" and point-in-region queries.
class EdgeGrid {
public:
    EdgeGrid(const Region& r, double cell) : m_cell(cell), m_bounds(r.bounds())
    {
        for (const Contour& c : r.contours) {
            const std::vector<Vec2> pts = flatten(c, cell * 0.25);
            for (size_t i = 0; i < pts.size(); ++i) add(pts[i], pts[(i + 1) % pts.size()]);
        }
    }

    /// True when a boundary segment passes within `d` of `c`.
    bool near(Vec2 c, double d) const
    {
        const int32_t x0 = key(c.x - d), x1 = key(c.x + d), y0 = key(c.y - d), y1 = key(c.y + d);
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t x = x0; x <= x1; ++x) {
                const auto it = m_cells.find(pack(x, y));
                if (it == m_cells.end()) continue;
                for (const auto& [a, b] : it->second)
                    if (segDistance(c, a, b) <= d) return true;
            }
        return false;
    }

    /// Non-zero winding at `c` (a horizontal ray to the right, counted in the
    /// cell holding each crossing so shared segments count once).
    int winding(Vec2 c) const
    {
        int w = 0;
        const int32_t y = key(c.y), xEnd = key(m_bounds.x1) + 1;
        for (int32_t x = key(c.x); x <= xEnd; ++x) {
            const auto it = m_cells.find(pack(x, y));
            if (it == m_cells.end()) continue;
            for (const auto& [a, b] : it->second) {
                if ((a.y <= c.y) == (b.y <= c.y)) continue;
                const double t = (c.y - a.y) / (b.y - a.y);
                const double cx = a.x + (b.x - a.x) * t;
                if (cx <= c.x || key(cx) != x) continue;
                w += b.y > a.y ? 1 : -1;
            }
        }
        return w;
    }

private:
    int32_t key(double v) const { return int32_t(std::floor(v / m_cell)); }
    static int64_t pack(int32_t x, int32_t y) { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); }
    static double segDistance(Vec2 p, Vec2 a, Vec2 b)
    {
        const Vec2 ab = b - a;
        const double l2 = ab.lengthSq();
        const double t = l2 > 0 ? std::clamp(dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
        return distance(p, a + ab * t);
    }
    void add(Vec2 a, Vec2 b)
    {
        const int32_t x0 = key(std::min(a.x, b.x)), x1 = key(std::max(a.x, b.x));
        const int32_t y0 = key(std::min(a.y, b.y)), y1 = key(std::max(a.y, b.y));
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t x = x0; x <= x1; ++x) m_cells[pack(x, y)].push_back({a, b});
    }

    double m_cell;
    Rect m_bounds;
    std::unordered_map<int64_t, std::vector<std::pair<Vec2, Vec2>>> m_cells;
};

/// Punches grain holes into a normalised region. At most one hole per cell
/// of a canvas-anchored grid, placed so holes never overlap: holes clear of
/// the edge are added directly as reversed contours, only the ones crossing
/// the edge need a boolean subtraction.
Region addGrain(const Region& base, double grain, double grainSize)
{
    const double cell = grainSize * 3.0;
    const Rect b = base.bounds();
    const int32_t x0 = int32_t(std::floor(b.x0 / cell)), x1 = int32_t(std::floor(b.x1 / cell));
    const int32_t y0 = int32_t(std::floor(b.y0 / cell)), y1 = int32_t(std::floor(b.y1 / cell));
    if (double(x1 - x0 + 1) * double(y1 - y0 + 1) >= 400000) return base;
    const EdgeGrid edges(base, cell);
    Region inner, crossing;
    for (int32_t cy = y0; cy <= y1; ++cy) {
        for (int32_t cx = x0; cx <= x1; ++cx) {
            if (unit(hash3(cx, cy, 0x9e37u)) >= grain) continue;
            const Vec2 c((cx + 0.35 + 0.3 * unit(hash3(cx, cy, 1))) * cell, (cy + 0.35 + 0.3 * unit(hash3(cx, cy, 2))) * cell);
            const double rad = grainSize * (0.45 + 0.55 * unit(hash3(cx, cy, 3)));
            const double aspect = 0.55 + 0.45 * unit(hash3(cx, cy, 4));
            const double rot = unit(hash3(cx, cy, 5)) * kPi;
            const bool touches = edges.near(c, rad * 1.05);
            if (!touches && edges.winding(c) == 0) continue;
            Region e = Region::ellipse({0, 0}, rad, rad * aspect).transformed(Affine::translate(c) * Affine::rotate(rot));
            Region& into = touches ? crossing : inner;
            for (Contour& ct : e.contours) into.contours.push_back(std::move(ct));
        }
    }
    Region out = crossing.isEmpty() ? base : booleanOp(base, crossing, BoolOp::Subtract);
    if (inner.isEmpty() || out.isEmpty()) return out;
    // A hole must bring the winding at its centre to zero: orient the inner
    // holes against the filled side (all holes share one orientation).
    Region first;
    first.contours.push_back(inner.contours.front());
    const Vec2 c0 = first.bounds().center();
    const int filled = out.winding(c0), own = first.winding(c0);
    if (filled != 0 && own == filled) inner = inner.reversed();
    for (Contour& ct : inner.contours) out.contours.push_back(std::move(ct));
    return out;
}

std::vector<BrushPiece> texturedStroke(const VectorBrushPreset& p, const std::vector<BrushPoint>& path, const Frame& f,
                                       const FillStyle& paint, double tol)
{
    Region base = sweptRegion(path, BrushTip{});
    if (base.isEmpty()) return {};
    if (p.roughness <= 0) base = normalizeRegion(refitRegion(base, tol, 0.9)); // refitting may nick neighbours
    const double rmean = f.valid() ? f.rmean : (path.empty() ? 1.0 : path.front().r);
    if (p.roughness > 0 && rmean > 0) {
        Region rough;
        const double amp = p.roughness * rmean;
        const double step = std::max(std::min(p.roughScale / 5.0, rmean * 0.5), tol * 3.0);
        for (const Contour& c : base.contours) {
            std::vector<Vec2> pts = flatten(c, step);
            const size_t n = pts.size();
            if (n < 3) continue;
            std::vector<Vec2> moved(n);
            for (size_t i = 0; i < n; ++i) {
                const Vec2 d = pts[(i + 1) % n] - pts[(i + n - 1) % n];
                const double len = d.length();
                const Vec2 nrm = len > 1e-12 ? Vec2{-d.y / len, d.x / len} : Vec2{};
                moved[i] = pts[i] + nrm * (brushNoise(pts[i], p.roughScale) * amp);
            }
            // The edge is noise anyway: a looser fit keeps the curve count down.
            Contour fitted = fitClosed(std::move(moved), std::max(tol, amp * 0.08));
            if (!fitted.empty()) rough.contours.push_back(std::move(fitted));
        }
        base = normalizeRegion(rough);
    }
    if (p.grain > 0 && p.grainSize > 0 && !base.isEmpty()) base = addGrain(base, p.grain, p.grainSize);
    if (base.isEmpty()) return {};
    return {{paint, std::move(base)}};
}

std::vector<BrushPiece> scatterStroke(const VectorBrushPreset& p, const std::vector<BrushPoint>& path, const Frame& f,
                                      const FillStyle& paint, uint32_t seed)
{
    Region dabs;
    auto dab = [&](Vec2 c, double rad, uint32_t h) {
        if (rad <= 1e-6) return;
        const double aspect = 0.7 + 0.3 * unit(hash3(int32_t(h), 7, seed));
        const double rot = unit(hash3(int32_t(h), 8, seed)) * kPi;
        Region e = Region::ellipse({0, 0}, rad, rad * aspect).transformed(Affine::translate(c) * Affine::rotate(rot));
        for (Contour& ct : e.contours) dabs.contours.push_back(std::move(ct));
    };
    if (!f.valid()) {
        if (!path.empty()) {
            const double r = path.front().r;
            const int n = std::max(3, int(p.density * 4));
            for (int i = 0; i < n; ++i) {
                const double a = unit(hash3(i, 1, seed)) * 2 * kPi, d = std::sqrt(unit(hash3(i, 2, seed))) * r * p.scatter;
                dab(path.front().p + Vec2{std::cos(a), std::sin(a)} * d, p.dabSize * 2 * r * (0.6 + 0.4 * unit(hash3(i, 3, seed))), uint32_t(i));
            }
        }
    } else {
        const double step = std::max(2.0 * f.rmean / std::max(p.density, 0.05), 1e-3);
        int i = 0;
        for (double s = 0; s <= f.length && i < 20000; s += step, ++i) {
            const double along = (unit(hash3(i, 4, seed)) - 0.5) * step;
            const double v = (unit(hash3(i, 5, seed)) * 2.0 - 1.0) * p.scatter;
            const Vec2 c = f.map(s + along, v);
            const double r = distance(f.map(s, 1.0), f.map(s, 0.0)); // half width here
            dab(c, p.dabSize * 2.0 * r * (0.6 + 0.4 * unit(hash3(i, 6, seed))), uint32_t(i));
        }
    }
    if (dabs.isEmpty()) return {};
    Region u = normalizeRegion(dabs);
    if (u.isEmpty()) return {};
    return {{paint, std::move(u)}};
}

// --- built-in artwork ------------------------------------------------------------------

Contour lens(Vec2 a, Vec2 b, double bulgeLeft, double bulgeRight)
{
    // A pointed lens from a to b, bulging to both sides of the chord.
    const Vec2 d = b - a;
    const double len = d.length();
    const Vec2 t = d / len, n = {-t.y, t.x};
    const Vec2 q1 = a + d * 0.25, q2 = a + d * 0.75;
    return {Cubic(a, q1 + n * bulgeLeft, q2 + n * bulgeLeft, b), Cubic(b, q2 - n * bulgeRight, q1 - n * bulgeRight, a)};
}

std::shared_ptr<const ShapeGraph> artFrom(const std::vector<std::pair<Color, Region>>& pieces)
{
    ShapeGraph g;
    for (const auto& [c, r] : pieces) {
        ShapeGraph pg = graphFromRegion(normalizeRegion(r), FillStyle::solid(c));
        g = g.isEmpty() ? pg : overlay(g, pg);
    }
    return std::make_shared<const ShapeGraph>(std::move(g));
}

Region regionOf(std::vector<Contour> cs)
{
    Region r;
    r.contours = std::move(cs);
    return r;
}

std::vector<VectorBrushPreset> makeBuiltins()
{
    std::vector<VectorBrushPreset> out;
    auto add = [&](VectorBrushPreset p) {
        p.builtin = true;
        out.push_back(std::move(p));
    };
    const Color ink(0x1b, 0x1a, 0x22);
    {
        VectorBrushPreset p;
        p.id = "ink-taper";
        p.name = "Ink Taper";
        p.kind = VectorBrushKind::Art;
        p.size = 14;
        p.minSize = 0.35;
        p.art = artFrom({{ink, regionOf({lens({0, 0}, {100, 0}, 10, 10)})}});
        add(p);
    }
    {
        VectorBrushPreset p;
        p.id = "dry-brush";
        p.name = "Dry Brush";
        p.kind = VectorBrushKind::Art;
        p.size = 22;
        p.minSize = 0.4;
        std::vector<Contour> streaks;
        const double ys[] = {-8.5, -5.8, -3.0, -0.4, 2.4, 5.2, 8.2};
        const double starts[] = {6, 0, 3, 1, 9, 2, 12}, ends[] = {88, 97, 100, 94, 99, 91, 84};
        const double half[] = {1.3, 1.9, 1.6, 2.0, 1.8, 1.5, 1.2};
        for (int i = 0; i < 7; ++i) {
            if (i == 2 || i == 5) { // broken streaks
                const double mid = starts[i] + (ends[i] - starts[i]) * (i == 2 ? 0.45 : 0.6);
                streaks.push_back(lens({starts[i], ys[i]}, {mid - 4, ys[i] + 0.3}, half[i], half[i]));
                streaks.push_back(lens({mid + 3, ys[i] + 0.2}, {ends[i], ys[i] - 0.2}, half[i], half[i]));
            } else {
                streaks.push_back(lens({starts[i], ys[i]}, {ends[i], ys[i] + (i % 2 ? 0.4 : -0.4)}, half[i], half[i]));
            }
        }
        p.art = artFrom({{ink, regionOf(std::move(streaks))}});
        add(p);
    }
    {
        VectorBrushPreset p;
        p.id = "chalk";
        p.name = "Chalk";
        p.kind = VectorBrushKind::Textured;
        p.size = 16;
        p.minSize = 0.5;
        p.roughness = 0.22;
        p.roughScale = 5;
        p.grain = 0.4;
        p.grainSize = 1.7;
        add(p);
    }
    {
        VectorBrushPreset p;
        p.id = "charcoal";
        p.name = "Charcoal";
        p.kind = VectorBrushKind::Textured;
        p.size = 24;
        p.minSize = 0.35;
        p.roughness = 0.35;
        p.roughScale = 9;
        p.grain = 0.35;
        p.grainSize = 2.4;
        add(p);
    }
    {
        VectorBrushPreset p;
        p.id = "pencil";
        p.name = "Pencil";
        p.kind = VectorBrushKind::Textured;
        p.size = 4;
        p.minSize = 0.4;
        p.roughness = 0.25;
        p.roughScale = 3;
        p.grain = 0.4;
        p.grainSize = 0.9;
        p.smoothing = 20;
        add(p);
    }
    {
        VectorBrushPreset p;
        p.id = "spray";
        p.name = "Spray";
        p.kind = VectorBrushKind::Scatter;
        p.size = 40;
        p.minSize = 0.5;
        p.dabSize = 0.05;
        p.density = 9;
        p.scatter = 1.0;
        add(p);
    }
    {
        VectorBrushPreset p;
        p.id = "stipple";
        p.name = "Stipple";
        p.kind = VectorBrushKind::Scatter;
        p.size = 24;
        p.dabSize = 0.16;
        p.density = 2.5;
        p.scatter = 0.8;
        add(p);
    }
    {
        VectorBrushPreset p;
        p.id = "rope";
        p.name = "Rope";
        p.kind = VectorBrushKind::Pattern;
        p.size = 14;
        p.pressureSize = false;
        // One twisted strand per tile; neighbouring strands touch.
        p.art = artFrom({{ink, regionOf({lens({0, 10}, {9, -10}, 2.6, 2.6)})}});
        p.patternGap = 0.0;
        add(p);
    }
    {
        VectorBrushPreset p;
        p.id = "dashes";
        p.name = "Dashes";
        p.kind = VectorBrushKind::Pattern;
        p.size = 8;
        p.pressureSize = false;
        p.art = artFrom({{ink, Region::roundedRect({0, -10, 60, 10}, 10)}});
        p.patternGap = 0.6;
        add(p);
    }
    {
        VectorBrushPreset p;
        p.id = "vine";
        p.name = "Vine";
        p.kind = VectorBrushKind::Pattern;
        p.size = 26;
        p.colorize = false;
        p.pressureSize = false;
        const Color stem(0x2e, 0x6b, 0x3a), leaf(0x4f, 0xb8, 0x5a);
        p.art = artFrom({{stem, Region::rect({0, -1.2, 40, 1.2})},
                         {leaf, regionOf({lens({6, -0.5}, {19, -10}, 3.2, 2.2), lens({26, 0.5}, {39, 10}, 2.2, 3.2)})}});
        add(p);
    }
    return out;
}

} // namespace

double brushNoise(Vec2 p, double scale)
{
    const double s = std::max(scale, 1e-6);
    const Vec2 q = p / s;
    return std::clamp(0.7 * lattice(q, 11) + 0.3 * lattice(q * 2.7 + Vec2{17.3, 4.1}, 23), -1.0, 1.0);
}

std::vector<BrushPiece> vectorBrushStroke(const VectorBrushPreset& p, const std::vector<BrushPoint>& path,
                                          const FillStyle& paint, uint32_t seed, double tolerance)
{
    if (path.empty()) return {};
    const double tol = std::max(tolerance, 1e-4);
    double rmax = 0;
    for (const BrushPoint& b : path) rmax = std::max(rmax, b.r);
    const Frame f = buildFrame(path, std::clamp(rmax * 0.25, tol * 2.0, std::max(rmax, tol * 2.0)));
    switch (p.kind) {
    case VectorBrushKind::Art:
    case VectorBrushKind::Pattern:
        if (!f.valid() || f.length < tol * 4) return {};
        return artStroke(p, f, paint, tol);
    case VectorBrushKind::Textured: return texturedStroke(p, path, f, paint, tol);
    case VectorBrushKind::Scatter: return scatterStroke(p, path, f, paint, seed);
    }
    return {};
}

ShapeGraph vectorBrushGraph(const std::vector<BrushPiece>& pieces)
{
    ShapeGraph g;
    for (const BrushPiece& piece : pieces) {
        if (piece.region.isEmpty()) continue;
        ShapeGraph pg = graphFromCleanRegion(piece.region, piece.fill);
        g = g.isEmpty() ? pg : overlay(g, pg);
    }
    return g;
}

const std::vector<VectorBrushPreset>& builtinVectorBrushes()
{
    static const std::vector<VectorBrushPreset> presets = makeBuiltins();
    return presets;
}

const VectorBrushPreset* builtinVectorBrush(const std::string& id)
{
    for (const VectorBrushPreset& p : builtinVectorBrushes())
        if (p.id == id) return &p;
    return nullptr;
}

std::vector<BrushPoint> vectorBrushPath(const VectorBrushPreset& p, const std::vector<InputSample>& samples)
{
    const double base = std::max(p.size, 0.05) * 0.5;
    std::vector<BrushPoint> out;
    out.reserve(samples.size());
    for (const InputSample& q : samples) {
        double f = 1.0;
        if (p.pressureSize) {
            const double m = std::clamp(p.minSize, 0.0, 1.0);
            f = m + (1.0 - m) * p.sizeCurve.eval(std::clamp(q.pressure, 0.0, 1.0));
        }
        out.push_back({q.pos, std::max(base * f, base * 0.02), 0.0});
    }
    return out;
}

ShapeGraph linesToFills(const ShapeGraph& g)
{
    if (g.strokes.empty()) return g;
    ShapeGraph fills = g;
    for (GEdge& e : fills.edges) e.stroke = 0;
    fills.strokes.clear();
    fills.compact();
    std::vector<Region> lines(g.strokes.size());
    for (const GEdge& e : g.edges) {
        if (e.stroke <= 0 || e.stroke > int(g.strokes.size())) continue;
        const double r = std::max(g.strokes[size_t(e.stroke - 1)].width * 0.5, 0.05);
        const int n = std::clamp(int(std::ceil(e.c.length() / std::max(r * 0.25, 0.05))), 1, 2000);
        std::vector<BrushPoint> pts;
        for (int k = 0; k <= n; ++k) pts.push_back({e.c.eval(double(k) / n), r, 0.0});
        Region swept = sweptRegion(pts, BrushTip{});
        for (Contour& c : swept.contours) lines[size_t(e.stroke - 1)].contours.push_back(std::move(c));
    }
    ShapeGraph out = fills;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].isEmpty()) continue;
        const Region u = normalizeRegion(lines[i]);
        if (u.isEmpty()) continue;
        ShapeGraph lg = graphFromRegion(u, g.strokes[i].paint);
        out = out.isEmpty() ? lg : overlay(out, lg);
    }
    return out;
}

VectorBrushPreset makeArtBrush(const ShapeGraph& art, bool pattern, const std::string& name)
{
    VectorBrushPreset p;
    p.name = name;
    p.kind = pattern ? VectorBrushKind::Pattern : VectorBrushKind::Art;
    // Fills only, in the artwork's own coordinates.
    ShapeGraph g = linesToFills(art);
    const Rect b = g.bounds(false);
    p.size = std::clamp(b.height(), 2.0, 200.0);
    p.colorize = g.fills.size() == 1 && !g.fills.front().isGradient() && g.fills.front().color == Color(0, 0, 0);
    p.pressureSize = !pattern;
    p.art = std::make_shared<const ShapeGraph>(std::move(g));
    return p;
}

} // namespace vx
