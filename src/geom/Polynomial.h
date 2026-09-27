// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — robust real root finding for low degree polynomials.
#pragma once

namespace vx {

/// All real roots of a*t^2 + b*t + c = 0 (degenerates gracefully to linear).
/// Returns the number of roots written to `roots` (0..2), sorted ascending.
int solveQuadratic(double a, double b, double c, double roots[2]);

/// Real roots of a*t^3 + b*t^2 + c*t + d inside [lo, hi], sorted ascending.
/// The interval is split at the critical points so every sub-interval is
/// monotone; roots are then bracketed and polished with a safeguarded Newton
/// iteration, so the result is accurate to the last few ulps. Double roots
/// (tangential touches) are detected at the critical points.
int solveCubicInRange(double a, double b, double c, double d, double lo, double hi, double roots[3]);

/// Evaluate a*t^3 + b*t^2 + c*t + d.
inline double evalCubicPoly(double a, double b, double c, double d, double t)
{
    return ((a * t + b) * t + c) * t + d;
}

} // namespace vx
