// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — exact curve/curve intersection including coincident (overlapping)
// segments. Results are refined with Newton iterations on the original curves
// so they are accurate to ~1e-12, independent of the subdivision depth.
#pragma once

#include "Bezier.h"

#include <vector>

namespace vx {

struct CurveHit {
    double t1 = 0.0; ///< parameter on the first curve
    double t2 = 0.0; ///< parameter on the second curve
    Vec2 p;          ///< intersection point
    bool overlap = false; ///< end point of a coincident stretch
};

/// Appends all intersections of a and b. `eps` is the geometric tolerance
/// used for touching end points and coincidence tests.
void intersectCurves(const Cubic& a, const Cubic& b, std::vector<CurveHit>& out, double eps = 1e-7);

/// Intersections of a curve with an infinite line through p along dir
/// (parameters on the curve only).
int intersectLine(const Cubic& c, Vec2 p, Vec2 dir, double ts[3]);

/// Intersection of two straight segments; returns false when parallel.
bool intersectSegments(Vec2 a0, Vec2 a1, Vec2 b0, Vec2 b1, double& s, double& t);

} // namespace vx
