// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — basic 2D vector and rectangle types used by the geometry kernel.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace vx {

constexpr double kPi = 3.14159265358979323846;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    constexpr Vec2() = default;
    constexpr Vec2(double x_, double y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(double s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(double s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2& operator*=(double s) { x *= s; y *= s; return *this; }
    constexpr Vec2& operator/=(double s) { x /= s; y /= s; return *this; }
    constexpr bool operator==(const Vec2&) const = default;

    double length() const { return std::hypot(x, y); }
    constexpr double lengthSq() const { return x * x + y * y; }
    Vec2 normalized() const
    {
        const double l = length();
        return l > 0.0 ? Vec2{x / l, y / l} : Vec2{};
    }
    /// Rotation by +90 degrees in the mathematical sense. For a contour with
    /// positive signed area the interior lies on the perp() side of each edge.
    constexpr Vec2 perp() const { return {-y, x}; }
    double angle() const { return std::atan2(y, x); }
    bool isFinite() const { return std::isfinite(x) && std::isfinite(y); }
};

constexpr Vec2 operator*(double s, Vec2 v) { return {v.x * s, v.y * s}; }
constexpr double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
constexpr Vec2 lerp(Vec2 a, Vec2 b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
inline double distance(Vec2 a, Vec2 b) { return (a - b).length(); }
constexpr double distanceSq(Vec2 a, Vec2 b) { return (a - b).lengthSq(); }
inline Vec2 rotated(Vec2 v, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
inline Vec2 fromAngle(double angle, double len = 1.0) { return {std::cos(angle) * len, std::sin(angle) * len}; }

/// Axis aligned rectangle stored as min/max corners. A default-constructed
/// rectangle is empty and absorbs the first point added to it.
struct Rect {
    double x0 = std::numeric_limits<double>::infinity();
    double y0 = std::numeric_limits<double>::infinity();
    double x1 = -std::numeric_limits<double>::infinity();
    double y1 = -std::numeric_limits<double>::infinity();

    constexpr Rect() = default;
    constexpr Rect(double ax0, double ay0, double ax1, double ay1) : x0(ax0), y0(ay0), x1(ax1), y1(ay1) {}
    static Rect fromPoints(Vec2 a, Vec2 b)
    {
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
    }
    static Rect fromXYWH(double x, double y, double w, double h) { return {x, y, x + w, y + h}; }

    bool isEmpty() const { return !(x1 >= x0 && y1 >= y0); }
    double width() const { return isEmpty() ? 0.0 : x1 - x0; }
    double height() const { return isEmpty() ? 0.0 : y1 - y0; }
    Vec2 center() const { return {(x0 + x1) * 0.5, (y0 + y1) * 0.5}; }
    Vec2 topLeft() const { return {x0, y0}; }
    Vec2 bottomRight() const { return {x1, y1}; }
    double diagonal() const { return isEmpty() ? 0.0 : std::hypot(x1 - x0, y1 - y0); }

    void include(Vec2 p)
    {
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    }
    void include(const Rect& r)
    {
        if (r.isEmpty()) return;
        x0 = std::min(x0, r.x0); y0 = std::min(y0, r.y0);
        x1 = std::max(x1, r.x1); y1 = std::max(y1, r.y1);
    }
    Rect united(const Rect& r) const { Rect o = *this; o.include(r); return o; }
    Rect inflated(double d) const { return isEmpty() ? *this : Rect{x0 - d, y0 - d, x1 + d, y1 + d}; }
    bool intersects(const Rect& r, double eps = 0.0) const
    {
        return !(r.x0 > x1 + eps || r.x1 < x0 - eps || r.y0 > y1 + eps || r.y1 < y0 - eps);
    }
    bool contains(Vec2 p, double eps = 0.0) const
    {
        return p.x >= x0 - eps && p.x <= x1 + eps && p.y >= y0 - eps && p.y <= y1 + eps;
    }
    bool contains(const Rect& r) const { return r.x0 >= x0 && r.x1 <= x1 && r.y0 >= y0 && r.y1 <= y1; }
    Rect intersected(const Rect& r) const
    {
        Rect o{std::max(x0, r.x0), std::max(y0, r.y0), std::min(x1, r.x1), std::min(y1, r.y1)};
        return o.isEmpty() ? Rect{} : o;
    }
};

} // namespace vx
