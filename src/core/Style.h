// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — fill and stroke styles of vector shapes.
#pragma once

#include "Color.h"
#include "geom/Affine.h"

#include <vector>

namespace vx {

struct GradientStop {
    double pos = 0.0; ///< 0..1
    Color color;
    bool operator==(const GradientStop&) const = default;
};

enum class SpreadMode { Pad, Reflect, Repeat };

struct Gradient {
    std::vector<GradientStop> stops{{0.0, Color(255, 255, 255)}, {1.0, Color(0, 0, 0)}};
    SpreadMode spread = SpreadMode::Pad;
    double focal = 0.0;      ///< radial focal point, -1..1 along the gradient x axis
    bool linearRGB = false;
    /// Maps gradient space to shape space. Gradient space is the unit square
    /// [-1, 1]^2: linear gradients run along x from -1 to 1, radial gradients
    /// have radius 1 around the origin.
    Affine matrix;
    bool operator==(const Gradient&) const = default;

    Color colorAt(double t) const;
};

struct FillStyle {
    enum class Kind { Solid, Linear, Radial };
    Kind kind = Kind::Solid;
    Color color{0, 0, 0};
    Gradient gradient;

    static FillStyle solid(Color c)
    {
        FillStyle f;
        f.color = c;
        return f;
    }
    bool operator==(const FillStyle& o) const
    {
        if (kind != o.kind) return false;
        if (kind == Kind::Solid) return color == o.color;
        return gradient == o.gradient;
    }
    bool isGradient() const { return kind != Kind::Solid; }
    /// Representative colour (solid colour or first stop).
    Color mainColor() const { return kind == Kind::Solid ? color : (gradient.stops.empty() ? color : gradient.stops.front().color); }
    FillStyle transformed(const Affine& m) const
    {
        FillStyle f = *this;
        if (isGradient()) f.gradient.matrix = m * gradient.matrix;
        return f;
    }
    FillStyle withColorTransform(const ColorTransform& ct) const
    {
        FillStyle f = *this;
        f.color = ct.apply(color);
        for (GradientStop& s : f.gradient.stops) s.color = ct.apply(s.color);
        return f;
    }
};

enum class CapStyle { Round, Square, None };
enum class JoinStyle { Round, Miter, Bevel };
enum class StrokePattern { Solid, Dashed, Dotted, Hairline };

struct StrokeStyle {
    FillStyle paint = FillStyle::solid(Color(0, 0, 0));
    double width = 1.0;
    CapStyle cap = CapStyle::Round;
    JoinStyle join = JoinStyle::Round;
    double miterLimit = 3.0;
    StrokePattern pattern = StrokePattern::Solid;
    double dash = 6.0, gap = 4.0;
    bool scaleWithTransform = true; ///< Animate "Scale: Normal"

    bool operator==(const StrokeStyle& o) const
    {
        return paint == o.paint && width == o.width && cap == o.cap && join == o.join && miterLimit == o.miterLimit &&
               pattern == o.pattern && dash == o.dash && gap == o.gap && scaleWithTransform == o.scaleWithTransform;
    }
    StrokeStyle transformed(const Affine& m) const
    {
        StrokeStyle s = *this;
        s.paint = paint.transformed(m);
        if (scaleWithTransform) s.width = width * m.meanScale();
        return s;
    }
};

inline Color Gradient::colorAt(double t) const
{
    if (stops.empty()) return {};
    if (t <= stops.front().pos) return stops.front().color;
    if (t >= stops.back().pos) return stops.back().color;
    for (size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].pos) {
            const double span = stops[i].pos - stops[i - 1].pos;
            const double u = span > 0 ? (t - stops[i - 1].pos) / span : 0.0;
            return lerpColor(stops[i - 1].color, stops[i].color, u);
        }
    }
    return stops.back().color;
}

} // namespace vx
