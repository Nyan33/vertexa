// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — 2D affine transform using the Flash/SVG matrix convention:
//   x' = a*x + c*y + tx
//   y' = b*x + d*y + ty
#pragma once

#include "Vec2.h"

namespace vx {

struct Affine {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, tx = 0.0, ty = 0.0;

    constexpr Affine() = default;
    constexpr Affine(double a_, double b_, double c_, double d_, double tx_, double ty_)
        : a(a_), b(b_), c(c_), d(d_), tx(tx_), ty(ty_) {}

    static constexpr Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static constexpr Affine translate(Vec2 v) { return {1, 0, 0, 1, v.x, v.y}; }
    static constexpr Affine scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
    static constexpr Affine scale(double s) { return {s, 0, 0, s, 0, 0}; }
    static Affine rotate(double radians)
    {
        const double cs = std::cos(radians), sn = std::sin(radians);
        return {cs, sn, -sn, cs, 0, 0};
    }
    /// Flash style skew: skewX rotates the y axis, skewY rotates the x axis.
    static Affine skew(double skewX, double skewY)
    {
        return {std::cos(skewY), std::sin(skewY), -std::sin(skewX), std::cos(skewX), 0, 0};
    }
    /// Transform about a pivot: translate(p) * m * translate(-p).
    static Affine about(Vec2 pivot, const Affine& m)
    {
        return translate(pivot) * m * translate(-pivot);
    }

    constexpr Vec2 map(Vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    constexpr Vec2 mapVector(Vec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }

    /// Composition: (A * B).map(p) == A.map(B.map(p)).
    constexpr Affine operator*(const Affine& o) const
    {
        return {a * o.a + c * o.b,     b * o.a + d * o.b,
                a * o.c + c * o.d,     b * o.c + d * o.d,
                a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
    }
    constexpr double det() const { return a * d - b * c; }
    Affine inverted() const
    {
        const double dt = det();
        if (std::abs(dt) < 1e-300) return {};
        const double id = 1.0 / dt;
        return {d * id, -b * id, -c * id, a * id, (c * ty - d * tx) * id, (b * tx - a * ty) * id};
    }
    constexpr bool isIdentity() const { return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0; }
    constexpr bool operator==(const Affine&) const = default;
    constexpr Vec2 translation() const { return {tx, ty}; }
    Rect mapRect(const Rect& r) const
    {
        if (r.isEmpty()) return r;
        Rect o;
        o.include(map({r.x0, r.y0}));
        o.include(map({r.x1, r.y0}));
        o.include(map({r.x0, r.y1}));
        o.include(map({r.x1, r.y1}));
        return o;
    }
    /// Average linear scale factor, used for stroke widths and tolerances.
    double meanScale() const { return std::sqrt(std::abs(det())); }
};

/// Flash-compatible matrix decomposition. Interpolating these components
/// (and not the raw matrix) is what makes classic tweens rotate correctly.
struct AffineParts {
    double scaleX = 1.0, scaleY = 1.0;
    double skewX = 0.0, skewY = 0.0; // radians; rotation == skewY when skewX == skewY
    double tx = 0.0, ty = 0.0;

    static AffineParts decompose(const Affine& m)
    {
        AffineParts p;
        p.tx = m.tx;
        p.ty = m.ty;
        p.scaleX = std::hypot(m.a, m.b);
        p.scaleY = std::hypot(m.c, m.d);
        p.skewY = std::atan2(m.b, m.a);
        p.skewX = std::atan2(-m.c, m.d);
        // A mirrored matrix: keep the rotation continuous and flip scaleY.
        if (m.det() < 0.0) {
            p.scaleY = -p.scaleY;
            p.skewX = std::atan2(m.c, -m.d);
        }
        return p;
    }
    Affine compose() const
    {
        return {scaleX * std::cos(skewY), scaleX * std::sin(skewY),
                -scaleY * std::sin(skewX), scaleY * std::cos(skewX), tx, ty};
    }
    double rotation() const { return skewY; }
};

} // namespace vx
