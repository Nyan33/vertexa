// SPDX-License-Identifier: GPL-3.0-or-later
#include "Intersect.h"
#include "Polynomial.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

bool refineNewton(const Cubic& a, const Cubic& b, double& s, double& t, double accept)
{
    Vec2 F = a.eval(s) - b.eval(t);
    for (int iter = 0; iter < 40; ++iter) {
        if (F.lengthSq() < 1e-28) break;
        const Vec2 da = a.d1(s), db = b.d1(t);
        const double det = cross(db, da);
        if (std::abs(det) < 1e-300) break;
        const double ds = (F.x * db.y - db.x * F.y) / det;
        const double dt = (F.x * da.y - da.x * F.y) / det;
        const double ns = std::clamp(s + ds, 0.0, 1.0), nt = std::clamp(t + dt, 0.0, 1.0);
        const Vec2 NF = a.eval(ns) - b.eval(nt);
        if (NF.lengthSq() > F.lengthSq() && iter > 3) break; // diverging — keep the best
        s = ns;
        t = nt;
        F = NF;
        if (std::abs(ds) < 1e-16 && std::abs(dt) < 1e-16) break;
    }
    return F.length() <= accept;
}

/// Robust fallback for crossings Newton cannot resolve (curves meeting at a
/// very small angle, often right next to a piece end): bisection on the
/// signed distance from b(t) to curve a around the candidate.
bool refineBisect(const Cubic& a, const Cubic& b, double& s, double& t, double accept)
{
    auto g = [&](double tt, double& ss) {
        const Vec2 q = b.eval(tt);
        ss = a.nearest(q);
        Vec2 d = a.d1(ss);
        if (d.lengthSq() < 1e-30) d = a.eval(std::min(1.0, ss + 1e-6)) - a.eval(std::max(0.0, ss - 1e-6));
        return cross(d.normalized(), q - a.eval(ss));
    };
    for (double w : {0.02, 0.1, 0.3}) {
        double lo = std::max(0.0, t - w), hi = std::min(1.0, t + w);
        double sl, sh;
        double gl = g(lo, sl), gh = g(hi, sh);
        if (gl == 0.0) { t = lo; s = sl; return distance(a.eval(s), b.eval(t)) <= accept; }
        if (gh == 0.0) { t = hi; s = sh; return distance(a.eval(s), b.eval(t)) <= accept; }
        if ((gl > 0) == (gh > 0)) continue;
        for (int it = 0; it < 90; ++it) {
            const double mid = 0.5 * (lo + hi);
            double sm;
            const double gm = g(mid, sm);
            if ((gm > 0) == (gl > 0)) {
                lo = mid;
                gl = gm;
            } else {
                hi = mid;
            }
        }
        const double tt = 0.5 * (lo + hi);
        double ss;
        g(tt, ss);
        if (distance(a.eval(ss), b.eval(tt)) <= accept) {
            s = ss;
            t = tt;
            return true;
        }
    }
    return false;
}

void snapParam(double& t)
{
    if (t < 1e-12) t = 0.0;
    if (t > 1.0 - 1e-12) t = 1.0;
}

// Straight/straight intersection including collinear overlaps.
void lineLine(const Cubic& a, const Cubic& b, std::vector<CurveHit>& out, double eps)
{
    const Vec2 a0 = a.p0, a1 = a.p3, b0 = b.p0, b1 = b.p3;
    const Vec2 da = a1 - a0, db = b1 - b0;
    const double la = da.length(), lb = db.length();
    if (la <= eps || lb <= eps) return;
    const double denom = cross(da, db);
    const double distB0 = std::abs(cross(da, b0 - a0)) / la;
    const double distB1 = std::abs(cross(da, b1 - a0)) / la;
    if (distB0 <= eps && distB1 <= eps) {
        // Collinear: overlap interval in a's parameter space.
        const double u0 = dot(b0 - a0, da) / (la * la), u1 = dot(b1 - a0, da) / (la * la);
        const double lo = std::max(0.0, std::min(u0, u1)), hi = std::min(1.0, std::max(u0, u1));
        const double tolU = eps / la;
        if (hi < lo - tolU) return;
        auto paramOnB = [&](Vec2 p) { return std::clamp(dot(p - b0, db) / (lb * lb), 0.0, 1.0); };
        if (hi - lo <= tolU) {
            // Touching at a single point.
            double s = std::clamp(0.5 * (lo + hi), 0.0, 1.0);
            const Vec2 p = a.eval(s);
            double t = paramOnB(p);
            snapParam(s); snapParam(t);
            out.push_back({s, t, p, false});
            return;
        }
        for (double s : {lo, hi}) {
            const Vec2 p = a0 + da * s;
            double t = paramOnB(p);
            double ss = s;
            snapParam(ss); snapParam(t);
            out.push_back({ss, t, (ss == 0.0 ? a0 : ss == 1.0 ? a1 : p), true});
        }
        return;
    }
    if (std::abs(denom) <= 1e-300) return;
    const double s = cross(b0 - a0, db) / denom;
    const double t = cross(b0 - a0, da) / denom;
    const double tolS = eps / la, tolT = eps / lb;
    if (s < -tolS || s > 1.0 + tolS || t < -tolT || t > 1.0 + tolT) return;
    double ss = std::clamp(s, 0.0, 1.0), tt = std::clamp(t, 0.0, 1.0);
    snapParam(ss); snapParam(tt);
    out.push_back({ss, tt, a.eval(ss), false});
}

// Straight a against curved b: roots of the signed distance polynomial.
void lineCurve(const Cubic& line, const Cubic& curve, bool swapped, std::vector<CurveHit>& out, double eps)
{
    const Vec2 o = line.p0, d = line.p3 - line.p0;
    const double len = d.length();
    if (len <= eps) return;
    const Vec2 dir = d / len;
    double y[4];
    for (int i = 0; i < 4; ++i) y[i] = cross(dir, curve[i] - o);
    // Bernstein -> power basis
    const double A = -y[0] + 3 * y[1] - 3 * y[2] + y[3];
    const double B = 3 * y[0] - 6 * y[1] + 3 * y[2];
    const double C = -3 * y[0] + 3 * y[1];
    const double D = y[0];
    double roots[3];
    const int n = solveCubicInRange(A, B, C, D, 0.0, 1.0, roots);
    for (int i = 0; i < n; ++i) {
        double t = roots[i];
        const Vec2 p = curve.eval(t);
        double s = dot(p - o, dir) / len;
        const double tolS = eps / len;
        if (s < -tolS || s > 1.0 + tolS) continue;
        s = std::clamp(s, 0.0, 1.0);
        snapParam(s); snapParam(t);
        if (swapped) out.push_back({t, s, p, false});
        else out.push_back({s, t, p, false});
    }
}

struct Candidate {
    double s, t;
};

void subdivide(const Cubic& a, double a0, double a1, const Cubic& b, double b0, double b1, int depth,
               std::vector<Candidate>& cands, double flatTol, double eps, int& budget)
{
    if (--budget < 0) return;
    if (!a.controlBounds().intersects(b.controlBounds(), eps)) return;
    const double fa = a.flatness(), fb = b.flatness();
    if ((fa <= flatTol && fb <= flatTol) || depth >= 48) {
        double s, t;
        const double slack = 1e-3;
        if (intersectSegments(a.p0, a.p3, b.p0, b.p3, s, t) && s >= -slack && s <= 1.0 + slack && t >= -slack &&
            t <= 1.0 + slack) {
            cands.push_back({a0 + std::clamp(s, 0.0, 1.0) * (a1 - a0), b0 + std::clamp(t, 0.0, 1.0) * (b1 - b0)});
            return;
        }
        // Nearly parallel pieces closer than the flatness tolerance: the chords
        // say nothing about a crossing, so let the refinement decide.
        auto segDist = [](Vec2 p, Vec2 q0, Vec2 q1) {
            const Vec2 d = q1 - q0;
            const double l2 = d.lengthSq();
            const double u = l2 > 0 ? std::clamp(dot(p - q0, d) / l2, 0.0, 1.0) : 0.0;
            return distance(p, q0 + d * u);
        };
        const double gap = std::min({segDist(a.p0, b.p0, b.p3), segDist(a.p3, b.p0, b.p3), segDist(b.p0, a.p0, a.p3),
                                     segDist(b.p3, a.p0, a.p3)});
        if (depth >= 48 || gap <= 4.0 * flatTol + eps) cands.push_back({0.5 * (a0 + a1), 0.5 * (b0 + b1)});
        return;
    }
    const double am = 0.5 * (a0 + a1), bm = 0.5 * (b0 + b1);
    const double sa = a.controlBounds().diagonal(), sb = b.controlBounds().diagonal();
    if (fa > flatTol && (sa >= sb || fb <= flatTol)) {
        auto [al, ar] = a.split(0.5);
        subdivide(al, a0, am, b, b0, b1, depth + 1, cands, flatTol, eps, budget);
        subdivide(ar, am, a1, b, b0, b1, depth + 1, cands, flatTol, eps, budget);
    } else {
        auto [bl, br] = b.split(0.5);
        subdivide(a, a0, a1, bl, b0, bm, depth + 1, cands, flatTol, eps, budget);
        subdivide(a, a0, a1, br, bm, b1, depth + 1, cands, flatTol, eps, budget);
    }
}

// Coincident curved segments: both must share a stretch whose ends are end
// points of one of the curves.
bool curveOverlap(const Cubic& a, const Cubic& b, std::vector<CurveHit>& out, double eps)
{
    struct Pair { double s, t; };
    Pair pairs[4];
    int n = 0;
    auto addPair = [&](double s, double t) {
        for (int i = 0; i < n; ++i)
            if (std::abs(pairs[i].s - s) < 1e-9 && std::abs(pairs[i].t - t) < 1e-9) return;
        pairs[n++] = {s, t};
    };
    const double tol = eps * 10.0;
    double d;
    double t = b.nearest(a.p0, &d);
    if (d <= tol) addPair(0.0, t);
    t = b.nearest(a.p3, &d);
    if (d <= tol) addPair(1.0, t);
    double s = a.nearest(b.p0, &d);
    if (d <= tol) addPair(s, 0.0);
    s = a.nearest(b.p3, &d);
    if (d <= tol) addPair(s, 1.0);
    if (n < 2) return false;
    // Pick the pair with the largest separation along a.
    int bi = 0, bj = 1;
    double best = -1;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double sep = std::abs(pairs[i].s - pairs[j].s);
            if (sep > best) { best = sep; bi = i; bj = j; }
        }
    if (best < 1e-9) return false;
    Pair p = pairs[bi], q = pairs[bj];
    if (p.s > q.s) std::swap(p, q);
    if (std::abs(p.t - q.t) < 1e-9) return false;
    const Cubic sa = a.sub(p.s, q.s);
    const Cubic sb = b.sub(p.t, q.t);
    const double ctol = tol * 10.0;
    if (distance(sa.p1, sb.p1) > ctol || distance(sa.p2, sb.p2) > ctol) {
        // Control polygons differ: only accept when the geometry still matches
        // (e.g. nearly straight segments parametrised differently).
        for (double u : {0.1, 0.25, 0.5, 0.75, 0.9})
            if (sb.distanceTo(sa.eval(u)) > ctol || sa.distanceTo(sb.eval(u)) > ctol) return false;
    }
    for (const Pair& pr : {p, q}) {
        double ss = pr.s, tt = pr.t;
        snapParam(ss); snapParam(tt);
        out.push_back({ss, tt, a.eval(ss), true});
    }
    return true;
}

void endpointTouches(const Cubic& a, const Cubic& b, std::vector<CurveHit>& out, double eps)
{
    double d;
    double t = b.nearest(a.p0, &d);
    if (d <= eps) { snapParam(t); out.push_back({0.0, t, a.p0, false}); }
    t = b.nearest(a.p3, &d);
    if (d <= eps) { snapParam(t); out.push_back({1.0, t, a.p3, false}); }
    double s = a.nearest(b.p0, &d);
    if (d <= eps) { snapParam(s); out.push_back({s, 0.0, b.p0, false}); }
    s = a.nearest(b.p3, &d);
    if (d <= eps) { snapParam(s); out.push_back({s, 1.0, b.p3, false}); }
}

} // namespace

bool intersectSegments(Vec2 a0, Vec2 a1, Vec2 b0, Vec2 b1, double& s, double& t)
{
    const Vec2 da = a1 - a0, db = b1 - b0;
    const double denom = cross(da, db);
    if (std::abs(denom) <= 1e-300) return false;
    s = cross(b0 - a0, db) / denom;
    t = cross(b0 - a0, da) / denom;
    return true;
}

int intersectLine(const Cubic& c, Vec2 p, Vec2 dir, double ts[3])
{
    const Vec2 nd = dir.normalized();
    double y[4];
    for (int i = 0; i < 4; ++i) y[i] = cross(nd, c[i] - p);
    const double A = -y[0] + 3 * y[1] - 3 * y[2] + y[3];
    const double B = 3 * y[0] - 6 * y[1] + 3 * y[2];
    const double C = -3 * y[0] + 3 * y[1];
    const double D = y[0];
    return solveCubicInRange(A, B, C, D, 0.0, 1.0, ts);
}

void intersectCurves(const Cubic& a, const Cubic& b, std::vector<CurveHit>& out, double eps)
{
    if (!a.controlBounds().intersects(b.controlBounds(), eps)) return;
    const size_t start = out.size();
    const bool sa = a.isStraight(eps * 0.01), sb = b.isStraight(eps * 0.01);
    if (sa && sb) {
        lineLine(a, b, out, eps);
    } else if (sa) {
        endpointTouches(a, b, out, eps);
        lineCurve(a, b, false, out, eps);
    } else if (sb) {
        endpointTouches(a, b, out, eps);
        lineCurve(b, a, true, out, eps);
    } else {
        if (curveOverlap(a, b, out, eps)) return;
        endpointTouches(a, b, out, eps);
        std::vector<Candidate> cands;
        const double scale = std::max(a.controlBounds().diagonal(), b.controlBounds().diagonal());
        const double flatTol = std::max(1e-9, scale * 1e-5);
        int budget = 20000;
        subdivide(a, 0.0, 1.0, b, 0.0, 1.0, 0, cands, flatTol, eps, budget);
        const double accept = std::max(eps, scale * 1e-10);
        for (Candidate c : cands) {
            double s = c.s, t = c.t;
            if (!refineNewton(a, b, s, t, accept)) {
                double s2 = c.s, t2 = c.t;
                if (refineBisect(a, b, s2, t2, accept)) {
                    s = s2;
                    t = t2;
                } else if (distance(a.eval(s), b.eval(t)) > eps) {
                    continue; // near-tangential touch: keep the estimate only if close
                }
            }
            snapParam(s); snapParam(t);
            out.push_back({s, t, (a.eval(s) + b.eval(t)) * 0.5, false});
        }
    }
    // Deduplicate hits produced by this pair.
    std::sort(out.begin() + start, out.end(), [](const CurveHit& x, const CurveHit& y) { return x.t1 < y.t1; });
    size_t w = start;
    for (size_t i = start; i < out.size(); ++i) {
        bool dup = false;
        for (size_t j = start; j < w; ++j) {
            if ((std::abs(out[j].t1 - out[i].t1) < 1e-9 && std::abs(out[j].t2 - out[i].t2) < 1e-9) ||
                distance(out[j].p, out[i].p) <= eps) {
                out[j].overlap = out[j].overlap || out[i].overlap;
                // Prefer exact end point parameters.
                if (out[i].t1 == 0.0 || out[i].t1 == 1.0) out[j].t1 = out[i].t1;
                if (out[i].t2 == 0.0 || out[i].t2 == 1.0) out[j].t2 = out[i].t2;
                dup = true;
                break;
            }
        }
        if (!dup) out[w++] = out[i];
    }
    out.resize(w);
}

} // namespace vx
