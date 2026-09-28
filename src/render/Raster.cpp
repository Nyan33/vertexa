// SPDX-License-Identifier: GPL-3.0-or-later
#include "Raster.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vx {

namespace {

struct Seg {
    double x0, y0, x1, y1; // device space relative to the raster origin
    int style;
};

struct Paint {
    bool solid = true;
    float c[4] = {0, 0, 0, 0}; // premultiplied r,g,b,a in 0..255
    FillStyle::Kind kind = FillStyle::Kind::Solid;
    Affine inv;                 // device -> gradient space
    SpreadMode spread = SpreadMode::Pad;
    double focal = 0.0;
    std::array<std::array<float, 4>, 256> lut{};

    void init(const FillStyle& f, const Affine& toDevice)
    {
        kind = f.kind;
        if (f.kind == FillStyle::Kind::Solid) {
            solid = true;
            const float a = f.color.a / 255.0f;
            c[0] = f.color.r * a;
            c[1] = f.color.g * a;
            c[2] = f.color.b * a;
            c[3] = f.color.a;
            return;
        }
        solid = false;
        inv = (toDevice * f.gradient.matrix).inverted();
        spread = f.gradient.spread;
        focal = std::clamp(f.gradient.focal, -0.98, 0.98);
        for (int i = 0; i < 256; ++i) {
            const Color col = f.gradient.colorAt(i / 255.0);
            const float a = col.a / 255.0f;
            lut[i] = {col.r * a, col.g * a, col.b * a, float(col.a)};
        }
    }

    double applySpread(double t) const
    {
        switch (spread) {
        case SpreadMode::Pad: return std::clamp(t, 0.0, 1.0);
        case SpreadMode::Repeat: return t - std::floor(t);
        case SpreadMode::Reflect: {
            double m = std::fmod(std::abs(t), 2.0);
            return m > 1.0 ? 2.0 - m : m;
        }
        }
        return t;
    }

    const float* at(double x, double y) const
    {
        if (solid) return c;
        const Vec2 g = inv.map({x, y});
        double t;
        if (kind == FillStyle::Kind::Linear) {
            t = (g.x + 1.0) * 0.5;
        } else if (focal == 0.0) {
            t = g.length();
        } else {
            const Vec2 F{focal, 0.0};
            const Vec2 d = g - F;
            const double dd = d.lengthSq();
            if (dd < 1e-18) {
                t = 0.0;
            } else {
                const double fd = dot(F, d);
                const double disc = fd * fd - dd * (F.lengthSq() - 1.0);
                const double s = (-fd + std::sqrt(std::max(0.0, disc))) / dd;
                t = s > 0 ? 1.0 / s : 1.0;
            }
        }
        t = applySpread(t);
        return lut[std::clamp(int(t * 255.0 + 0.5), 0, 255)].data();
    }
};

// Accumulates the signed area of one line piece inside row `row` (font-rs).
inline void accumulate(float* acc, int width, double x0, double y0, double x1, double y1, int row)
{
    double dir = 1.0;
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        dir = -1.0;
    }
    const double ya = std::max(y0, double(row)), yb = std::min(y1, double(row + 1));
    if (yb <= ya) return;
    const double dxdy = (x1 - x0) / (y1 - y0);
    const double x = x0 + (ya - y0) * dxdy;
    const double xnext = x0 + (yb - y0) * dxdy;
    const float d = float((yb - ya) * dir);
    const double xa = std::min(x, xnext), xb = std::max(x, xnext);
    const double x0floor = std::floor(xa);
    const int x0i = int(x0floor);
    const double x1ceil = std::ceil(xb);
    const int x1i = int(x1ceil);
    auto add = [&](int i, float v) {
        if (i < 0) i = 0;
        if (i > width + 1) i = width + 1;
        acc[i] += v;
    };
    if (x1i <= x0i + 1) {
        const float xmf = float(0.5 * (x + xnext) - x0floor);
        add(x0i, d - d * xmf);
        add(x0i + 1, d * xmf);
    } else {
        const double s = 1.0 / (xb - xa);
        const double x0f = xa - x0floor;
        const double a0 = 0.5 * s * (1.0 - x0f) * (1.0 - x0f);
        const double x1f = xb - x1ceil + 1.0;
        const double am = 0.5 * s * x1f * x1f;
        add(x0i, float(d * a0));
        if (x1i == x0i + 2) {
            add(x0i + 1, float(d * (1.0 - a0 - am)));
        } else {
            const double a1 = s * (1.5 - x0f);
            add(x0i + 1, float(d * (a1 - a0)));
            for (int xi = x0i + 2; xi < x1i - 1; ++xi) add(xi, float(d * s));
            const double a2 = a1 + (x1i - x0i - 3) * s;
            add(x1i - 1, float(d * (1.0 - a2 - am)));
        }
        add(x1i, float(d * am));
    }
}

// Clip a segment horizontally to [0, W]; parts outside become vertical lines
// on the boundary so the winding to the right is preserved.
void clipAndPush(std::vector<Seg>& out, double x0, double y0, double x1, double y1, int style, double W)
{
    if (y0 == y1) return;
    auto push = [&](double ax, double ay, double bx, double by) {
        if (ay != by) out.push_back({std::clamp(ax, 0.0, W), ay, std::clamp(bx, 0.0, W), by, style});
    };
    double ts[2];
    int n = 0;
    for (double edge : {0.0, W}) {
        if ((x0 < edge && x1 > edge) || (x0 > edge && x1 < edge)) ts[n++] = (edge - x0) / (x1 - x0);
    }
    if (n == 2 && ts[0] > ts[1]) std::swap(ts[0], ts[1]);
    double px = x0, py = y0, prevT = 0.0;
    for (int i = 0; i <= n; ++i) {
        const double t = i < n ? ts[i] : 1.0;
        const double qx = x0 + (x1 - x0) * t, qy = y0 + (y1 - y0) * t;
        if (t > prevT) push(px, py, qx, qy);
        px = qx;
        py = qy;
        prevT = t;
    }
}

} // namespace

void flattenContour(const Contour& c, const Affine& m, double tol, std::vector<Vec2>& out, const Rect* cull)
{
    for (size_t i = 0; i < c.size(); ++i) {
        const Cubic d = c[i].transformed(m);
        if (i == 0) out.push_back(d.p0);
        if (cull) {
            const double x0 = std::min({d.p0.x, d.p1.x, d.p2.x, d.p3.x}), x1 = std::max({d.p0.x, d.p1.x, d.p2.x, d.p3.x});
            const double y0 = std::min({d.p0.y, d.p1.y, d.p2.y, d.p3.y}), y1 = std::max({d.p0.y, d.p1.y, d.p2.y, d.p3.y});
            if (x1 < cull->x0 || x0 > cull->x1 || y1 < cull->y0 || y0 > cull->y1) {
                out.push_back(d.p3);
                continue;
            }
        }
        if (d.isStraight(tol * 0.05)) {
            out.push_back(d.p3);
            continue;
        }
        const double dd = std::max((d.p0 - d.p1 * 2.0 + d.p2).length(), (d.p1 - d.p2 * 2.0 + d.p3).length());
        const int n = std::clamp(int(std::ceil(std::sqrt(0.75 * dd / tol))), 1, 1000);
        for (int k = 1; k < n; ++k) out.push_back(d.eval(double(k) / n));
        out.push_back(d.p3);
    }
}

void rasterizeFills(QImage& target, const std::vector<RasterFill>& fills, const Affine& toDevice, const QRect& clipIn)
{
    if (fills.empty() || target.isNull()) return;
    const QRect clip = clipIn.intersected(target.rect());
    if (clip.isEmpty()) return;

    // Flatten everything and find the device bounds.
    std::vector<std::vector<Vec2>> polys;
    std::vector<int> polyStyle;
    Rect bb;
    std::vector<Vec2> pts;
    // Curves away from the clip only contribute their end points.
    const Rect cull{double(clip.left()) - 1, double(clip.top()) - 1, double(clip.right()) + 2, double(clip.bottom()) + 2};
    for (int s = 0; s < int(fills.size()); ++s) {
        for (const std::vector<std::vector<Vec2>>* set : fills[s].polygons)
            for (const std::vector<Vec2>& poly : *set) {
                if (poly.size() < 3) continue;
                pts.clear();
                for (const Vec2& v : poly) {
                    pts.push_back(toDevice.map(v));
                    bb.include(pts.back());
                }
                polys.push_back(pts);
                polyStyle.push_back(s);
            }
        if (!fills[s].contours) continue;
        for (const Contour& c : *fills[s].contours) {
            pts.clear();
            flattenContour(c, toDevice, 0.12, pts, &cull);
            if (pts.size() < 3) continue;
            for (const Vec2& p : pts) bb.include(p);
            polys.push_back(pts);
            polyStyle.push_back(s);
        }
    }
    if (bb.isEmpty()) return;
    const int rx0 = std::max(clip.left(), int(std::floor(bb.x0)));
    const int ry0 = std::max(clip.top(), int(std::floor(bb.y0)));
    const int rx1 = std::min(clip.right() + 1, int(std::ceil(bb.x1)) + 1);
    const int ry1 = std::min(clip.bottom() + 1, int(std::ceil(bb.y1)) + 1);
    if (rx1 <= rx0 || ry1 <= ry0) return;
    const int W = rx1 - rx0;
    const double rowsEnd = ry1 - ry0;

    std::vector<Seg> segs;
    for (size_t p = 0; p < polys.size(); ++p) {
        const auto& poly = polys[p];
        for (size_t i = 0; i < poly.size(); ++i) {
            const Vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
            const double ay = a.y - ry0, by = b.y - ry0;
            // Rows only accumulate the pieces crossing them.
            if (std::max(ay, by) <= 0.0 || std::min(ay, by) >= rowsEnd) continue;
            clipAndPush(segs, a.x - rx0, ay, b.x - rx0, by, polyStyle[p], double(W));
        }
    }
    if (segs.empty()) return;
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return std::min(a.y0, a.y1) < std::min(b.y0, b.y1); });

    const int S = int(fills.size());
    std::vector<Paint> paints(S);
    for (int s = 0; s < S; ++s) paints[s].init(fills[s].style, toDevice);

    std::vector<std::vector<float>> acc(S);
    std::vector<int> minX(S, W + 2), maxX(S, -1);
    std::vector<int> touched;
    std::vector<int> active;
    std::vector<float> running(S, 0.0f);
    size_t nextSeg = 0;
    const int rows = ry1 - ry0;
    for (int row = 0; row < rows; ++row) {
        while (nextSeg < segs.size() && std::min(segs[nextSeg].y0, segs[nextSeg].y1) < row + 1) active.push_back(int(nextSeg++));
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](int i) { return std::max(segs[i].y0, segs[i].y1) <= row; }),
                     active.end());
        if (active.empty()) continue;
        touched.clear();
        for (int i : active) {
            const Seg& sg = segs[i];
            auto& buf = acc[sg.style];
            if (buf.empty()) buf.assign(W + 3, 0.0f);
            if (maxX[sg.style] < 0) touched.push_back(sg.style);
            accumulate(buf.data(), W, sg.x0, sg.y0, sg.x1, sg.y1, row);
            minX[sg.style] = std::min(minX[sg.style], std::max(0, int(std::floor(std::min(sg.x0, sg.x1)))));
            maxX[sg.style] = std::max(maxX[sg.style], std::min(W + 1, int(std::ceil(std::max(sg.x0, sg.x1))) + 1));
        }
        if (touched.empty()) continue;
        int from = W, to = 0;
        for (int s : touched) {
            from = std::min(from, minX[s]);
            to = std::max(to, maxX[s]);
            running[s] = 0.0f;
        }
        to = std::min(to, W);
        auto* line = reinterpret_cast<uint32_t*>(target.scanLine(ry0 + row)) + rx0;
        const double py = ry0 + row + 0.5;
        for (int x = from; x < to; ++x) {
            float r = 0, g = 0, b = 0, a = 0, total = 0;
            for (int s : touched) {
                running[s] += acc[s][x];
                const float cov = std::min(1.0f, std::abs(running[s]));
                if (cov <= 1e-4f) continue;
                const float* col = paints[s].at(rx0 + x + 0.5, py);
                r += col[0] * cov;
                g += col[1] * cov;
                b += col[2] * cov;
                a += col[3] * cov;
                total += cov;
            }
            if (a <= 0.0f) continue;
            if (total > 1.0f) {
                const float k = 1.0f / total;
                r *= k;
                g *= k;
                b *= k;
                a *= k;
            }
            const uint32_t d = line[x];
            const float inv = 1.0f - a / 255.0f;
            auto ch = [&](int shift, float src) {
                const float v = src + float((d >> shift) & 0xff) * inv;
                return uint32_t(std::clamp(int(v + 0.5f), 0, 255)) << shift;
            };
            line[x] = ch(24, a) | ch(16, r) | ch(8, g) | ch(0, b);
        }
        for (int s : touched) {
            std::fill(acc[s].begin() + minX[s], acc[s].begin() + std::min(int(acc[s].size()), maxX[s] + 2), 0.0f);
            minX[s] = W + 2;
            maxX[s] = -1;
        }
    }
}

} // namespace vx
