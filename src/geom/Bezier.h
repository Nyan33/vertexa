// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — cubic Bezier segment. Every curve in Vertexa (lines, quadratic
// imports from Flash/XFL, brush outlines) is stored as an exact cubic in
// double precision; there is no flattening in the model.
#pragma once

#include "Affine.h"
#include "Vec2.h"

#include <utility>
#include <vector>

namespace vx {

struct Cubic {
    Vec2 p0, p1, p2, p3;

    constexpr Cubic() = default;
    constexpr Cubic(Vec2 a, Vec2 b, Vec2 c, Vec2 d) : p0(a), p1(b), p2(c), p3(d) {}

    /// Straight segment with uniformly spaced handles (uniform parametrisation).
    static constexpr Cubic line(Vec2 a, Vec2 b)
    {
        return {a, lerp(a, b, 1.0 / 3.0), lerp(a, b, 2.0 / 3.0), b};
    }
    /// Exact degree elevation of a quadratic Bezier (Flash / XFL edges).
    static constexpr Cubic fromQuad(Vec2 a, Vec2 ctrl, Vec2 b)
    {
        return {a, a + (ctrl - a) * (2.0 / 3.0), b + (ctrl - b) * (2.0 / 3.0), b};
    }

    constexpr Vec2 operator[](int i) const { return i == 0 ? p0 : i == 1 ? p1 : i == 2 ? p2 : p3; }
    constexpr bool operator==(const Cubic&) const = default;

    Vec2 eval(double t) const
    {
        const double mt = 1.0 - t;
        const double a = mt * mt * mt, b = 3.0 * mt * mt * t, c = 3.0 * mt * t * t, d = t * t * t;
        return {a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y};
    }
    /// First derivative.
    Vec2 d1(double t) const
    {
        const double mt = 1.0 - t;
        return (p1 - p0) * (3.0 * mt * mt) + (p2 - p1) * (6.0 * mt * t) + (p3 - p2) * (3.0 * t * t);
    }
    /// Second derivative.
    Vec2 d2(double t) const
    {
        return (p2 - p1 * 2.0 + p0) * (6.0 * (1.0 - t)) + (p3 - p2 * 2.0 + p1) * (6.0 * t);
    }
    /// Unit tangent; well defined even when handles coincide with end points.
    Vec2 tangent(double t) const;
    /// Unit tangent direction leaving the start point / arriving at the end point.
    Vec2 startTangent() const;
    Vec2 endTangent() const;
    double curvature(double t) const;

    std::pair<Cubic, Cubic> split(double t) const;
    /// Exact sub-segment on [t0, t1] (t0 > t1 yields a reversed segment).
    Cubic sub(double t0, double t1) const;
    Cubic reversed() const { return {p3, p2, p1, p0}; }
    Cubic transformed(const Affine& m) const { return {m.map(p0), m.map(p1), m.map(p2), m.map(p3)}; }
    Cubic translated(Vec2 v) const { return {p0 + v, p1 + v, p2 + v, p3 + v}; }

    Rect controlBounds() const;
    /// Tight bounds computed from the derivative roots.
    Rect bounds() const;

    /// Maximum distance of the handles from the chord.
    double flatness() const;
    /// True when the curve is geometrically the straight segment p0-p3.
    bool isStraight(double eps = 1e-9) const;
    /// True when all control points coincide (zero length curve).
    bool isDegenerate(double eps = 1e-12) const;

    double length() const { return length(0.0, 1.0); }
    double length(double t0, double t1) const;
    /// Parameter at arc length s (0 <= s <= length()).
    double paramAtLength(double s) const;

    /// Exact contribution to the signed area: integral of (x dy - y dx) / 2.
    /// Summed over a closed contour this is the enclosed signed area.
    double areaContribution() const;

    /// Parameters in (0,1) where dx/dt == 0 or dy/dt == 0, sorted, unique.
    int extrema(double out[4]) const;
    /// Parameters in (0,1) where dy/dt == 0.
    int yExtrema(double out[2]) const;
    /// Parameters in (0,1) of inflection points.
    int inflections(double out[2]) const;

    /// Parameter of the closest point to p; `dist` receives the distance.
    double nearest(Vec2 p, double* dist = nullptr) const;
    double distanceTo(Vec2 p) const { double d; nearest(p, &d); return d; }

    /// Split into pieces monotone in both x and y (no self intersections).
    void monotonePieces(std::vector<Cubic>& out) const;

    /// Moves the curve so that eval(t) lands on `target` while end points stay
    /// fixed — the "bend a line with the Selection tool" behaviour of Flash.
    Cubic bentThrough(double t, Vec2 target) const;
};

/// Try to merge two consecutive cubics (a.p3 == b.p0) into one exact cubic.
/// Succeeds when both are pieces of a single cubic (e.g. produced by split()),
/// or when both are collinear straight segments.
bool tryJoinCubics(const Cubic& a, const Cubic& b, Cubic& out, double eps = 1e-7);

/// Cubic approximation of a circular arc (|sweep| <= pi/2 per segment is
/// performed internally). Center c, radius r, from angle a0 sweeping `sweep`.
void appendArc(std::vector<Cubic>& out, Vec2 c, double r, double a0, double sweep);
/// Elliptical arc with radii rx, ry (axis aligned in the ellipse frame m).
void appendEllipseArc(std::vector<Cubic>& out, const Affine& unitToWorld, double a0, double sweep);

} // namespace vx
