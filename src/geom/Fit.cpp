// SPDX-License-Identifier: GPL-3.0-or-later
#include "Fit.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

struct Fitter {
    const std::vector<Vec2>& p;
    double tol;
    std::vector<Cubic>& out;

    std::vector<double> chordParams(int first, int last) const
    {
        std::vector<double> u(last - first + 1, 0.0);
        for (int i = first + 1; i <= last; ++i) u[i - first] = u[i - first - 1] + distance(p[i], p[i - 1]);
        const double total = u.back();
        if (total > 0)
            for (double& v : u) v /= total;
        return u;
    }

    Cubic generate(int first, int last, const std::vector<double>& u, Vec2 t1, Vec2 t2) const
    {
        const Vec2 p0 = p[first], p3 = p[last];
        double C[2][2] = {{0, 0}, {0, 0}}, X[2] = {0, 0};
        for (int i = first; i <= last; ++i) {
            const double t = u[i - first], mt = 1.0 - t;
            const double b0 = mt * mt * mt, b1 = 3 * mt * mt * t, b2 = 3 * mt * t * t, b3 = t * t * t;
            const Vec2 a1 = t1 * b1, a2 = t2 * b2;
            C[0][0] += dot(a1, a1);
            C[0][1] += dot(a1, a2);
            C[1][1] += dot(a2, a2);
            const Vec2 tmp = p[i] - (p0 * (b0 + b1) + p3 * (b2 + b3));
            X[0] += dot(a1, tmp);
            X[1] += dot(a2, tmp);
        }
        C[1][0] = C[0][1];
        const double detC = C[0][0] * C[1][1] - C[1][0] * C[0][1];
        const double detX1 = C[0][0] * X[1] - C[1][0] * X[0];
        const double detX2 = X[0] * C[1][1] - X[1] * C[0][1];
        double alpha1 = detC != 0 ? detX2 / detC : 0.0;
        double alpha2 = detC != 0 ? detX1 / detC : 0.0;
        const double seg = distance(p0, p3);
        const double epsilon = 1e-12 * seg;
        if (alpha1 < epsilon || alpha2 < epsilon || alpha1 > seg * 4 || alpha2 > seg * 4) {
            // Wu/Barsky heuristic fallback.
            alpha1 = alpha2 = seg / 3.0;
        }
        return {p0, p0 + t1 * alpha1, p3 + t2 * alpha2, p3};
    }

    double maxError(int first, int last, const Cubic& c, const std::vector<double>& u, int& split) const
    {
        double maxD = 0.0;
        split = (first + last) / 2;
        for (int i = first + 1; i < last; ++i) {
            const double d = distance(c.eval(u[i - first]), p[i]);
            if (d >= maxD) {
                maxD = d;
                split = i;
            }
        }
        return maxD;
    }

    void reparameterize(int first, int last, std::vector<double>& u, const Cubic& c) const
    {
        for (int i = first; i <= last; ++i) {
            double& t = u[i - first];
            const Vec2 d = c.eval(t) - p[i], d1 = c.d1(t), d2 = c.d2(t);
            const double num = dot(d, d1), den = dot(d1, d1) + dot(d, d2);
            if (std::abs(den) > 1e-300) t = std::clamp(t - num / den, 0.0, 1.0);
        }
    }

    Vec2 centerTangent(int i) const
    {
        const int n = int(p.size());
        const Vec2 a = p[std::max(0, i - 1)], b = p[std::min(n - 1, i + 1)];
        Vec2 t = (a - b).normalized();
        if (t.lengthSq() == 0) t = (p[i] - b).normalized();
        return t;
    }

    void fit(int first, int last, Vec2 t1, Vec2 t2, int depth)
    {
        const int n = last - first + 1;
        if (n == 2 || depth > 40) {
            const double d = distance(p[first], p[last]) / 3.0;
            if (n == 2) {
                // A straight chunk: keep it an exact line when the tangents agree.
                out.push_back(Cubic::line(p[first], p[last]));
            } else {
                out.push_back({p[first], p[first] + t1 * d, p[last] + t2 * d, p[last]});
            }
            return;
        }
        std::vector<double> u = chordParams(first, last);
        Cubic c = generate(first, last, u, t1, t2);
        int split = 0;
        double err = maxError(first, last, c, u, split);
        if (err <= tol) {
            out.push_back(c);
            return;
        }
        // Newton reparameterisation while it keeps improving (as in Paper.js).
        for (int it = 0; it < 16; ++it) {
            std::vector<double> u2 = u;
            reparameterize(first, last, u2, c);
            const Cubic c2 = generate(first, last, u2, t1, t2);
            int split2 = 0;
            const double err2 = maxError(first, last, c2, u2, split2);
            if (err2 >= err) break;
            u.swap(u2);
            c = c2;
            err = err2;
            split = split2;
            if (err <= tol) {
                out.push_back(c);
                return;
            }
        }
        split = std::clamp(split, first + 1, last - 1);
        const Vec2 tc = centerTangent(split);
        fit(first, split, t1, tc, depth + 1);
        fit(split, last, -tc, t2, depth + 1);
    }
};

Vec2 leftTangent(const std::vector<Vec2>& p, int first, int last, double reach)
{
    Vec2 acc;
    for (int i = first + 1; i <= last; ++i) {
        const Vec2 d = p[i] - p[first];
        acc = d;
        if (d.length() >= reach) break;
    }
    return acc.normalized();
}

Vec2 rightTangent(const std::vector<Vec2>& p, int first, int last, double reach)
{
    Vec2 acc;
    for (int i = last - 1; i >= first; --i) {
        const Vec2 d = p[i] - p[last];
        acc = d;
        if (d.length() >= reach) break;
    }
    return acc.normalized();
}

} // namespace

std::vector<Vec2> dedupePoints(const std::vector<Vec2>& pts, double minDist)
{
    std::vector<Vec2> out;
    out.reserve(pts.size());
    for (const Vec2& q : pts)
        if (out.empty() || distance(out.back(), q) > minDist) out.push_back(q);
    return out;
}

std::vector<Vec2> simplifyPolyline(const std::vector<Vec2>& pts, double tolerance)
{
    if (pts.size() < 3) return pts;
    std::vector<char> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<int, int>> stack{{0, int(pts.size()) - 1}};
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        double maxD = -1;
        int idx = -1;
        const Vec2 ab = pts[b] - pts[a];
        const double l2 = ab.lengthSq();
        for (int i = a + 1; i < b; ++i) {
            double d;
            if (l2 <= 0) d = distance(pts[i], pts[a]);
            else {
                const double t = std::clamp(dot(pts[i] - pts[a], ab) / l2, 0.0, 1.0);
                d = distance(pts[i], pts[a] + ab * t);
            }
            if (d > maxD) {
                maxD = d;
                idx = i;
            }
        }
        if (idx >= 0 && maxD > tolerance) {
            keep[idx] = 1;
            stack.push_back({a, idx});
            stack.push_back({idx, b});
        }
    }
    std::vector<Vec2> out;
    for (size_t i = 0; i < pts.size(); ++i)
        if (keep[i]) out.push_back(pts[i]);
    return out;
}

void sampleChain(const std::vector<Cubic>& chain, double spacing, std::vector<Vec2>& out, std::vector<char>* isVertex)
{
    for (size_t k = 0; k < chain.size(); ++k) {
        const Cubic& c = chain[k];
        if (k == 0) {
            out.push_back(c.p0);
            if (isVertex) isVertex->push_back(1);
        }
        if (c.isStraight(1e-9)) {
            out.push_back(c.p3);
            if (isVertex) isVertex->push_back(1);
            continue;
        }
        const double len = c.length();
        const int n = std::max(2, int(std::ceil(len / std::max(spacing, 1e-9))));
        for (int i = 1; i < n; ++i) {
            out.push_back(c.eval(double(i) / n));
            if (isVertex) isVertex->push_back(0);
        }
        out.push_back(c.p3);
        if (isVertex) isVertex->push_back(1);
    }
}

std::vector<Cubic> fitCurves(const std::vector<Vec2>& input, const FitOptions& opt)
{
    std::vector<Cubic> out;
    std::vector<Vec2> pts = dedupePoints(input, 1e-9);
    if (opt.closed && pts.size() > 2 && distance(pts.front(), pts.back()) <= 1e-9) pts.pop_back();
    const int n = int(pts.size());
    if (n < 2) return out;
    if (n == 2) {
        out.push_back(Cubic::line(pts[0], pts[1]));
        if (opt.closed) out.push_back(Cubic::line(pts[1], pts[0]));
        return out;
    }
    const double reach = opt.cornerRadius > 0 ? opt.cornerRadius : std::max(opt.tolerance * 3.0, 1e-6);

    // Turning angle at each point, measured between neighbours at `reach`.
    auto turnAt = [&](int i) -> double {
        auto walk = [&](int dir) {
            Vec2 d;
            for (int k = 1; k < n; ++k) {
                int j = i + dir * k;
                if (!opt.closed && (j < 0 || j >= n)) break;
                j = ((j % n) + n) % n;
                d = pts[j] - pts[i];
                if (d.length() >= reach) break;
            }
            return d;
        };
        const Vec2 a = walk(-1), b = walk(1);
        if (a.lengthSq() == 0 || b.lengthSq() == 0) return 0.0;
        const Vec2 din = (-a).normalized(), dout = b.normalized();
        return std::acos(std::clamp(dot(din, dout), -1.0, 1.0));
    };

    std::vector<int> corners;
    for (int i = opt.closed ? 0 : 1; i < (opt.closed ? n : n - 1); ++i) {
        const double t = turnAt(i);
        if (t > opt.cornerAngle) {
            // Keep only the local maximum within a run of corner candidates.
            if (!corners.empty() && corners.back() == i - 1) {
                if (turnAt(corners.back()) < t) corners.back() = i;
            } else {
                corners.push_back(i);
            }
        }
    }

    std::vector<Vec2> seq;
    auto fitSection = [&](const std::vector<Vec2>& s, bool smoothClosed) {
        if (s.size() < 2) return;
        Fitter f{s, opt.tolerance, out};
        const int last = int(s.size()) - 1;
        Vec2 t1, t2;
        if (smoothClosed) {
            // Tangent at the seam of a smooth closed curve.
            const Vec2 t = (s[1] - s[last - 1]).normalized();
            t1 = t;
            t2 = -t;
        } else {
            t1 = leftTangent(s, 0, last, reach);
            t2 = rightTangent(s, 0, last, reach);
        }
        f.fit(0, last, t1, t2, 0);
    };

    if (!opt.closed) {
        std::vector<int> cuts{0};
        cuts.insert(cuts.end(), corners.begin(), corners.end());
        cuts.push_back(n - 1);
        for (size_t k = 0; k + 1 < cuts.size(); ++k) {
            seq.assign(pts.begin() + cuts[k], pts.begin() + cuts[k + 1] + 1);
            fitSection(seq, false);
        }
    } else if (corners.empty()) {
        seq = pts;
        seq.push_back(pts.front());
        fitSection(seq, true);
    } else {
        for (size_t k = 0; k < corners.size(); ++k) {
            const int a = corners[k], b = corners[(k + 1) % corners.size()];
            seq.clear();
            int i = a;
            do {
                seq.push_back(pts[i]);
                i = (i + 1) % n;
            } while (i != b);
            seq.push_back(pts[b]);
            fitSection(seq, false);
        }
    }
    // Exact continuity between consecutive pieces.
    for (size_t i = 1; i < out.size(); ++i) out[i].p0 = out[i - 1].p3;
    if (opt.closed && !out.empty()) out.back().p3 = out.front().p0;
    return out;
}

} // namespace vx
