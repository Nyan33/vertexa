// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — filled regions (compound paths) and exact boolean operations.
#pragma once

#include "Affine.h"
#include "Bezier.h"

#include <vector>

namespace vx {

/// A closed chain of cubic segments (c[i].p3 == c[i+1].p0, last.p3 == first.p0).
using Contour = std::vector<Cubic>;

enum class FillRule { NonZero, EvenOdd };
enum class BoolOp { Union, Intersect, Subtract, Xor };

struct Region {
    std::vector<Contour> contours;

    bool isEmpty() const { return contours.empty(); }
    Rect bounds() const;
    /// Signed area (outer contours positive, holes negative for normalised regions).
    double area() const;
    int winding(Vec2 p) const;
    bool contains(Vec2 p, FillRule rule = FillRule::NonZero) const
    {
        const int w = winding(p);
        return rule == FillRule::NonZero ? w != 0 : (w & 1) != 0;
    }
    Region transformed(const Affine& m) const;
    std::vector<Cubic> curves() const;
    int curveCount() const;
    Region reversed() const;

    static Region rect(const Rect& r);
    static Region roundedRect(const Rect& r, double radius);
    static Region ellipse(Vec2 center, double rx, double ry);
    static Region circle(Vec2 center, double r) { return ellipse(center, r, r); }
    static Region polygon(const std::vector<Vec2>& pts);
};

/// Exact boolean operation (non-zero fill rule on both operands). The result
/// is normalised: outer contours are counter-clockwise (positive area), holes
/// are clockwise, and no two contours intersect.
Region booleanOp(const Region& a, const Region& b, BoolOp op, double eps = 1e-7);

/// Resolve self intersections and overlapping contours.
Region normalizeRegion(const Region& a, FillRule rule = FillRule::NonZero, double eps = 1e-7);

/// Union of many regions in one arrangement pass.
Region uniteAll(const std::vector<Region>& regions, double eps = 1e-7);

/// Close an open chain of curves into a contour (adds a straight closing
/// segment when the ends do not meet).
Contour closeChain(const std::vector<Cubic>& chain, double eps = 1e-9);

} // namespace vx
