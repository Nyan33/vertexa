// SPDX-License-Identifier: GPL-3.0-or-later
#include "Polynomial.h"

#include <algorithm>
#include <cmath>

namespace vx {

int solveQuadratic(double a, double b, double c, double roots[2])
{
    const double scale = std::max({std::abs(a), std::abs(b), std::abs(c)});
    if (scale == 0.0) return 0;
    a /= scale; b /= scale; c /= scale;
    if (std::abs(a) < 1e-14) {
        if (std::abs(b) < 1e-14) return 0;
        roots[0] = -c / b;
        return 1;
    }
    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) {
        // Treat a numerically tiny negative discriminant as a double root.
        if (disc > -1e-14) {
            roots[0] = -b / (2.0 * a);
            return 1;
        }
        return 0;
    }
    if (disc == 0.0) {
        roots[0] = -b / (2.0 * a);
        return 1;
    }
    const double sq = std::sqrt(disc);
    const double q = -0.5 * (b + (b >= 0.0 ? sq : -sq));
    double r0 = q / a;
    double r1 = (q != 0.0) ? c / q : -r0;
    if (r0 > r1) std::swap(r0, r1);
    roots[0] = r0;
    roots[1] = r1;
    return 2;
}

namespace {

// Safeguarded Newton/bisection on a bracket where f(lo) and f(hi) differ in sign.
double polishRoot(double a, double b, double c, double d, double lo, double hi, double flo)
{
    double t = 0.5 * (lo + hi);
    for (int i = 0; i < 100; ++i) {
        const double f = evalCubicPoly(a, b, c, d, t);
        if (f == 0.0) return t;
        if ((f < 0.0) == (flo < 0.0)) {
            lo = t;
            flo = f;
        } else {
            hi = t;
        }
        const double df = (3.0 * a * t + 2.0 * b) * t + c;
        double next = (df != 0.0) ? t - f / df : 0.5 * (lo + hi);
        if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
        if (std::abs(next - t) <= 1e-16 * std::max(1.0, std::abs(t))) return next;
        t = next;
        if (hi - lo <= 1e-16 * std::max(1.0, std::abs(t))) break;
    }
    return t;
}

} // namespace

int solveCubicInRange(double a, double b, double c, double d, double lo, double hi, double roots[3])
{
    if (!(hi >= lo)) return 0;
    const double scale = std::max({std::abs(a), std::abs(b), std::abs(c), std::abs(d)});
    if (scale == 0.0) return 0;
    a /= scale; b /= scale; c /= scale; d /= scale;
    const double zeroTol = 1e-13;

    // Break points: interval ends and the critical points inside it.
    double pts[4];
    int np = 0;
    pts[np++] = lo;
    double crit[2];
    const int nc = solveQuadratic(3.0 * a, 2.0 * b, c, crit);
    for (int i = 0; i < nc; ++i)
        if (crit[i] > lo && crit[i] < hi) pts[np++] = crit[i];
    pts[np++] = hi;
    std::sort(pts, pts + np);

    int n = 0;
    auto push = [&](double r) {
        for (int i = 0; i < n; ++i)
            if (std::abs(roots[i] - r) <= 1e-12 * std::max(1.0, std::abs(r))) return;
        if (n < 3) roots[n++] = r;
    };

    for (int i = 0; i + 1 < np; ++i) {
        const double x0 = pts[i], x1 = pts[i + 1];
        const double f0 = evalCubicPoly(a, b, c, d, x0);
        const double f1 = evalCubicPoly(a, b, c, d, x1);
        if (std::abs(f0) <= zeroTol) { push(x0); continue; }
        if (std::abs(f1) <= zeroTol) { push(x1); continue; }
        if ((f0 < 0.0) != (f1 < 0.0)) push(polishRoot(a, b, c, d, x0, x1, f0));
    }
    // Endpoint of the last interval.
    if (std::abs(evalCubicPoly(a, b, c, d, pts[np - 1])) <= zeroTol) push(pts[np - 1]);
    std::sort(roots, roots + n);
    return n;
}

} // namespace vx
