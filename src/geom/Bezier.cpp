// SPDX-License-Identifier: GPL-3.0-or-later
#include "Bezier.h"
#include "Polynomial.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

// de Casteljau blossom P(a, b, c). Symmetric in its arguments; the control
// points of the segment on [u, v] are P(u,u,u), P(u,u,v), P(u,v,v), P(v,v,v).
Vec2 blossom(const Cubic& q, double a, double b, double c)
{
    const Vec2 q0 = lerp(q.p0, q.p1, a), q1 = lerp(q.p1, q.p2, a), q2 = lerp(q.p2, q.p3, a);
    const Vec2 r0 = lerp(q0, q1, b), r1 = lerp(q1, q2, b);
    return lerp(r0, r1, c);
}

double distToSegment(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2 ab = b - a;
    const double l2 = ab.lengthSq();
    if (l2 <= 0.0) return distance(p, a);
    const double t = std::clamp(dot(p - a, ab) / l2, 0.0, 1.0);
    return distance(p, a + ab * t);
}

// 16-point Gauss-Legendre nodes/weights on [-1, 1].
constexpr double kGLx[16] = {
    -0.9894009349916499, -0.9445750230732326, -0.8656312023878318, -0.7554044083550030,
    -0.6178762444026438, -0.4580167776572274, -0.2816035507792589, -0.0950125098376374,
     0.0950125098376374,  0.2816035507792589,  0.4580167776572274,  0.6178762444026438,
     0.7554044083550030,  0.8656312023878318,  0.9445750230732326,  0.9894009349916499};
constexpr double kGLw[16] = {
    0.0271524594117541, 0.0622535239386479, 0.0951585116824928, 0.1246289712555339,
    0.1495959888165767, 0.1691565193950025, 0.1826034150449236, 0.1894506104550685,
    0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
    0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541};

double speedIntegral(const Cubic& c, double a, double b)
{
    const double h = 0.5 * (b - a), m = 0.5 * (a + b);
    double s = 0.0;
    for (int i = 0; i < 16; ++i) s += kGLw[i] * c.d1(m + h * kGLx[i]).length();
    return s * h;
}

double adaptiveLength(const Cubic& c, double a, double b, double whole, int depth)
{
    const double m = 0.5 * (a + b);
    const double left = speedIntegral(c, a, m), right = speedIntegral(c, m, b);
    if (depth >= 10 || std::abs(left + right - whole) <= 1e-12 * std::max(1.0, whole)) return left + right;
    return adaptiveLength(c, a, m, left, depth + 1) + adaptiveLength(c, m, b, right, depth + 1);
}

} // namespace

Vec2 Cubic::tangent(double t) const
{
    const Vec2 d = d1(t);
    const double scale = std::max({std::abs(p0.x), std::abs(p0.y), std::abs(p3.x), std::abs(p3.y), 1.0});
    if (d.length() > 1e-12 * scale) return d.normalized();
    if (t < 0.5) return startTangent();
    return endTangent();
}

Vec2 Cubic::startTangent() const
{
    const double tiny = 1e-12;
    if (distanceSq(p1, p0) > tiny * tiny) return (p1 - p0).normalized();
    if (distanceSq(p2, p0) > tiny * tiny) return (p2 - p0).normalized();
    return (p3 - p0).normalized();
}

Vec2 Cubic::endTangent() const
{
    const double tiny = 1e-12;
    if (distanceSq(p3, p2) > tiny * tiny) return (p3 - p2).normalized();
    if (distanceSq(p3, p1) > tiny * tiny) return (p3 - p1).normalized();
    return (p3 - p0).normalized();
}

double Cubic::curvature(double t) const
{
    const Vec2 v = d1(t), a = d2(t);
    const double s = v.length();
    if (s < 1e-300) return 0.0;
    return cross(v, a) / (s * s * s);
}

std::pair<Cubic, Cubic> Cubic::split(double t) const
{
    const Vec2 a = lerp(p0, p1, t), b = lerp(p1, p2, t), c = lerp(p2, p3, t);
    const Vec2 d = lerp(a, b, t), e = lerp(b, c, t);
    const Vec2 f = lerp(d, e, t);
    return {Cubic{p0, a, d, f}, Cubic{f, e, c, p3}};
}

Cubic Cubic::sub(double t0, double t1) const
{
    if (t0 == 0.0 && t1 == 1.0) return *this;
    if (t0 == 1.0 && t1 == 0.0) return reversed();
    Cubic r{blossom(*this, t0, t0, t0), blossom(*this, t0, t0, t1), blossom(*this, t0, t1, t1),
            blossom(*this, t1, t1, t1)};
    // Pin exact end points where they coincide with the original ones.
    if (t0 == 0.0) r.p0 = p0;
    if (t0 == 1.0) r.p0 = p3;
    if (t1 == 0.0) r.p3 = p0;
    if (t1 == 1.0) r.p3 = p3;
    return r;
}

Rect Cubic::controlBounds() const
{
    Rect r;
    r.include(p0); r.include(p1); r.include(p2); r.include(p3);
    return r;
}

Rect Cubic::bounds() const
{
    Rect r;
    r.include(p0);
    r.include(p3);
    double ts[4];
    const int n = extrema(ts);
    for (int i = 0; i < n; ++i) r.include(eval(ts[i]));
    return r;
}

double Cubic::flatness() const
{
    return std::max(distToSegment(p1, p0, p3), distToSegment(p2, p0, p3));
}

bool Cubic::isStraight(double eps) const { return flatness() <= eps; }

bool Cubic::isDegenerate(double eps) const
{
    return distanceSq(p0, p1) <= eps * eps && distanceSq(p0, p2) <= eps * eps && distanceSq(p0, p3) <= eps * eps;
}

double Cubic::length(double t0, double t1) const
{
    if (t1 < t0) std::swap(t0, t1);
    if (t1 - t0 <= 0.0) return 0.0;
    if (isStraight(0.0) && t0 == 0.0 && t1 == 1.0 && p1 == lerp(p0, p3, 1.0 / 3.0) && p2 == lerp(p0, p3, 2.0 / 3.0))
        return distance(p0, p3);
    const double whole = speedIntegral(*this, t0, t1);
    return adaptiveLength(*this, t0, t1, whole, 0);
}

double Cubic::paramAtLength(double s) const
{
    const double total = length();
    if (s <= 0.0) return 0.0;
    if (s >= total) return 1.0;
    double lo = 0.0, hi = 1.0, t = s / total;
    for (int i = 0; i < 50; ++i) {
        const double f = length(0.0, t) - s;
        if (std::abs(f) < 1e-10 * std::max(1.0, total)) break;
        if (f > 0) hi = t; else lo = t;
        const double sp = d1(t).length();
        double next = sp > 1e-300 ? t - f / sp : 0.5 * (lo + hi);
        if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
        t = next;
    }
    return t;
}

double Cubic::areaContribution() const
{
    // (x y' - y x') is a polynomial of degree 5: 3-point Gauss-Legendre is exact.
    static constexpr double xs[3] = {0.5 - 0.3872983346207417, 0.5, 0.5 + 0.3872983346207417};
    static constexpr double ws[3] = {5.0 / 18.0, 8.0 / 18.0, 5.0 / 18.0};
    double s = 0.0;
    for (int i = 0; i < 3; ++i) {
        const Vec2 p = eval(xs[i]), d = d1(xs[i]);
        s += ws[i] * (p.x * d.y - p.y * d.x);
    }
    return 0.5 * s;
}

int Cubic::extrema(double out[4]) const
{
    int n = 0;
    const Vec2 a = p1 - p0, b = p2 - p1, c = p3 - p2;
    const Vec2 A = a - b * 2.0 + c, B = (b - a) * 2.0, C = a;
    double r[2];
    int k = solveQuadratic(A.x, B.x, C.x, r);
    for (int i = 0; i < k; ++i)
        if (r[i] > 1e-12 && r[i] < 1.0 - 1e-12) out[n++] = r[i];
    k = solveQuadratic(A.y, B.y, C.y, r);
    for (int i = 0; i < k; ++i)
        if (r[i] > 1e-12 && r[i] < 1.0 - 1e-12) out[n++] = r[i];
    std::sort(out, out + n);
    int m = 0;
    for (int i = 0; i < n; ++i)
        if (m == 0 || out[i] - out[m - 1] > 1e-12) out[m++] = out[i];
    return m;
}

int Cubic::yExtrema(double out[2]) const
{
    const double a = p1.y - p0.y, b = p2.y - p1.y, c = p3.y - p2.y;
    double r[2];
    const int k = solveQuadratic(a - 2.0 * b + c, 2.0 * (b - a), a, r);
    int n = 0;
    for (int i = 0; i < k; ++i)
        if (r[i] > 1e-12 && r[i] < 1.0 - 1e-12) out[n++] = r[i];
    return n;
}

int Cubic::inflections(double out[2]) const
{
    const Vec2 a = p1 - p0, b = p2 - p1, c = p3 - p2;
    const Vec2 A = a - b * 2.0 + c, B = (b - a) * 2.0, C = a;
    double r[2];
    const int k = solveQuadratic(-cross(A, B), 2.0 * cross(C, A), cross(C, B), r);
    int n = 0;
    for (int i = 0; i < k; ++i)
        if (r[i] > 1e-9 && r[i] < 1.0 - 1e-9) out[n++] = r[i];
    return n;
}

double Cubic::nearest(Vec2 p, double* dist) const
{
    constexpr int kSamples = 24;
    double bestT = 0.0, bestD = distanceSq(p, p0);
    for (int i = 1; i <= kSamples; ++i) {
        const double t = double(i) / kSamples;
        const double d = distanceSq(p, eval(t));
        if (d < bestD) { bestD = d; bestT = t; }
    }
    // Newton refinement on f(t) = (B(t) - p) . B'(t)
    auto refine = [&](double t) {
        for (int it = 0; it < 12; ++it) {
            const Vec2 q = eval(t) - p, d = d1(t), dd = d2(t);
            const double f = dot(q, d), df = dot(d, d) + dot(q, dd);
            if (df == 0.0) break;
            const double nt = std::clamp(t - f / df, 0.0, 1.0);
            if (std::abs(nt - t) < 1e-15) { t = nt; break; }
            t = nt;
        }
        return t;
    };
    for (double start : {bestT, std::max(0.0, bestT - 0.5 / kSamples), std::min(1.0, bestT + 0.5 / kSamples)}) {
        const double t = refine(start);
        const double d = distanceSq(p, eval(t));
        if (d < bestD) { bestD = d; bestT = t; }
    }
    if (dist) *dist = std::sqrt(bestD);
    return bestT;
}

void Cubic::monotonePieces(std::vector<Cubic>& out) const
{
    double ts[4];
    const int n = extrema(ts);
    double prev = 0.0;
    for (int i = 0; i <= n; ++i) {
        const double t = (i < n) ? ts[i] : 1.0;
        if (t - prev > 1e-12 || i == n) {
            Cubic piece = sub(prev, t);
            if (!piece.isDegenerate(1e-12)) out.push_back(piece);
            prev = t;
        }
    }
}

Cubic Cubic::bentThrough(double t, Vec2 target) const
{
    t = std::clamp(t, 0.05, 0.95);
    const double mt = 1.0 - t;
    const double b1 = 3.0 * mt * mt * t, b2 = 3.0 * mt * t * t;
    const Vec2 delta = target - eval(t);
    const double denom = b1 * b1 + b2 * b2;
    Cubic r = *this;
    r.p1 += delta * (b1 / denom);
    r.p2 += delta * (b2 / denom);
    return r;
}

bool tryJoinCubics(const Cubic& a, const Cubic& b, Cubic& out, double eps)
{
    if (distance(a.p3, b.p0) > eps) return false;
    const bool sa = a.isStraight(eps), sb = b.isStraight(eps);
    if (sa != sb) return false;
    if (sa) {
        const Vec2 da = a.p3 - a.p0, db = b.p3 - b.p0;
        const double la = da.length(), lb = db.length();
        if (la <= eps || lb <= eps) return false;
        if (dot(da, db) <= 0.0) return false;
        const Vec2 whole = b.p3 - a.p0;
        const double lw = whole.length();
        if (lw <= eps) return false;
        // The joint must lie on the combined chord.
        if (std::abs(cross(whole, a.p3 - a.p0)) / lw > eps) return false;
        out = Cubic::line(a.p0, b.p3);
        return true;
    }
    const Vec2 ta = a.d1(1.0), tb = b.d1(0.0);
    const double la = ta.length(), lb = tb.length();
    if (la <= 1e-12 || lb <= 1e-12) return false;
    if (dot(ta, tb) <= 0.0 || std::abs(cross(ta / la, tb / lb)) > 1e-6) return false;
    const double u = la / (la + lb);
    Cubic q = (u >= 0.5) ? a.sub(0.0, 1.0 / u) : b.sub(-u / (1.0 - u), 1.0);
    q.p0 = a.p0;
    q.p3 = b.p3;
    const Cubic qa = q.sub(0.0, u), qb = q.sub(u, 1.0);
    const double tol = eps * std::max(1.0, std::max(a.controlBounds().diagonal(), b.controlBounds().diagonal()));
    auto close = [&](const Cubic& x, const Cubic& y) {
        return distance(x.p0, y.p0) <= tol && distance(x.p1, y.p1) <= tol && distance(x.p2, y.p2) <= tol &&
               distance(x.p3, y.p3) <= tol;
    };
    if (!close(qa, a) || !close(qb, b)) return false;
    out = q;
    return true;
}

void appendArc(std::vector<Cubic>& out, Vec2 c, double r, double a0, double sweep)
{
    if (sweep == 0.0 || r <= 0.0) return;
    const int n = std::max(1, int(std::ceil(std::abs(sweep) / (kPi * 0.5) - 1e-9)));
    const double step = sweep / n;
    const double k = 4.0 / 3.0 * std::tan(step / 4.0);
    for (int i = 0; i < n; ++i) {
        const double s = a0 + step * i, e = s + step;
        const Vec2 ds = fromAngle(s), de = fromAngle(e);
        const Vec2 P0 = c + ds * r, P3 = c + de * r;
        out.push_back({P0, P0 + ds.perp() * (k * r), P3 - de.perp() * (k * r), P3});
    }
}

void appendEllipseArc(std::vector<Cubic>& out, const Affine& m, double a0, double sweep)
{
    std::vector<Cubic> unit;
    appendArc(unit, {0, 0}, 1.0, a0, sweep);
    for (const Cubic& c : unit) out.push_back(c.transformed(m));
}

} // namespace vx
