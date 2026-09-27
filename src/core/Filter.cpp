// SPDX-License-Identifier: GPL-3.0-or-later
#include "Filter.h"

#include "geom/Vec2.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

struct FilterInfo {
    FilterType type;
    std::string_view id;
    std::string_view label;
};

constexpr FilterInfo kFilters[] = {
    {FilterType::DropShadow, "dropShadow", "Drop Shadow"},
    {FilterType::Blur, "blur", "Blur"},
    {FilterType::Glow, "glow", "Glow"},
    {FilterType::Bevel, "bevel", "Bevel"},
    {FilterType::GradientGlow, "gradientGlow", "Gradient Glow"},
    {FilterType::GradientBevel, "gradientBevel", "Gradient Bevel"},
    {FilterType::AdjustColor, "adjustColor", "Adjust Color"},
};

double lerp(double a, double b, double t) { return a + (b - a) * t; }

using Mat = std::array<double, 20>;

Mat identity() { return {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0}; }

/// a ∘ b: apply b first, then a.
Mat multiply(const Mat& a, const Mat& b)
{
    Mat r{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 5; ++col) {
            double v = col == 4 ? a[row * 5 + 4] : 0.0;
            for (int k = 0; k < 4; ++k) v += a[row * 5 + k] * b[k * 5 + col];
            r[row * 5 + col] = v;
        }
    }
    return r;
}

// Contrast response of Animate's Adjust Color (the classic Flash colour
// matrix table, index = contrast 0..100).
constexpr double kDeltaIndex[101] = {
    0,    0.01, 0.02, 0.04, 0.05, 0.06, 0.07, 0.08, 0.1,  0.11, 0.12, 0.14, 0.15, 0.16, 0.17, 0.18, 0.20,
    0.21, 0.22, 0.24, 0.25, 0.27, 0.28, 0.30, 0.32, 0.34, 0.36, 0.38, 0.40, 0.42, 0.44, 0.46, 0.48, 0.5,
    0.53, 0.56, 0.59, 0.62, 0.65, 0.68, 0.71, 0.74, 0.77, 0.80, 0.83, 0.86, 0.89, 0.92, 0.95, 0.98, 1.0,
    1.06, 1.12, 1.18, 1.24, 1.30, 1.36, 1.42, 1.48, 1.54, 1.60, 1.66, 1.72, 1.78, 1.84, 1.90, 1.96, 2.0,
    2.12, 2.25, 2.37, 2.50, 2.62, 2.75, 2.87, 3.0,  3.2,  3.4,  3.6,  3.8,  4.0,  4.3,  4.7,  4.9,  5.0,
    5.5,  6.0,  6.5,  6.8,  7.0,  7.3,  7.5,  7.8,  8.0,  8.4,  8.7,  9.0,  9.4,  9.6,  9.8,  10.0};

} // namespace

Filter Filter::defaults(FilterType t)
{
    Filter f;
    f.type = t;
    switch (t) {
    case FilterType::DropShadow: f.color = Color(0, 0, 0); break;
    case FilterType::Blur: break;
    case FilterType::Glow: f.color = Color(255, 0, 0); break;
    case FilterType::Bevel:
        f.color = Color(0, 0, 0);
        f.highlight = Color(255, 255, 255);
        break;
    case FilterType::GradientGlow:
    case FilterType::GradientBevel:
        f.gradient.stops = {{0.0, Color(255, 255, 255, 0)}, {1.0, Color(0, 0, 0)}};
        if (t == FilterType::GradientBevel)
            f.gradient.stops = {{0.0, Color(255, 255, 255)}, {0.5, Color(255, 0, 0, 0)}, {1.0, Color(0, 0, 0)}};
        break;
    case FilterType::AdjustColor:
    case FilterType::Count: break;
    }
    return f;
}

std::string_view filterId(FilterType t)
{
    for (const FilterInfo& i : kFilters)
        if (i.type == t) return i.id;
    return "dropShadow";
}

std::string_view filterLabel(FilterType t)
{
    for (const FilterInfo& i : kFilters)
        if (i.type == t) return i.label;
    return "Drop Shadow";
}

FilterType filterFromId(std::string_view id)
{
    for (const FilterInfo& i : kFilters)
        if (i.id == id) return i.type;
    return FilterType::DropShadow;
}

FilterList lerpFilters(const FilterList& a, const FilterList& b, double t)
{
    if (a.size() != b.size()) return a;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].type != b[i].type) return a;
    FilterList out = a;
    for (size_t i = 0; i < a.size(); ++i) {
        const Filter& x = a[i];
        const Filter& y = b[i];
        Filter& o = out[i];
        o.blurX = lerp(x.blurX, y.blurX, t);
        o.blurY = lerp(x.blurY, y.blurY, t);
        o.strength = lerp(x.strength, y.strength, t);
        o.distance = lerp(x.distance, y.distance, t);
        double da = std::fmod(y.angle - x.angle, 360.0);
        if (da > 180) da -= 360;
        if (da < -180) da += 360;
        o.angle = x.angle + da * t;
        o.color = lerpColor(x.color, y.color, t);
        o.highlight = lerpColor(x.highlight, y.highlight, t);
        o.brightness = lerp(x.brightness, y.brightness, t);
        o.contrast = lerp(x.contrast, y.contrast, t);
        o.saturation = lerp(x.saturation, y.saturation, t);
        o.hue = lerp(x.hue, y.hue, t);
        if (x.gradient.stops.size() == y.gradient.stops.size()) {
            for (size_t s = 0; s < o.gradient.stops.size(); ++s) {
                o.gradient.stops[s].pos = lerp(x.gradient.stops[s].pos, y.gradient.stops[s].pos, t);
                o.gradient.stops[s].color = lerpColor(x.gradient.stops[s].color, y.gradient.stops[s].color, t);
            }
        }
        if (t >= 1.0) {
            o.quality = y.quality;
            o.inner = y.inner;
            o.knockout = y.knockout;
            o.hideObject = y.hideObject;
            o.enabled = y.enabled;
        }
    }
    return out;
}

std::array<double, 20> adjustColorMatrix(const Filter& f)
{
    // Brightness: plain offset.
    Mat brightness = identity();
    const double b = std::clamp(f.brightness, -100.0, 100.0);
    brightness[4] = brightness[9] = brightness[14] = b;

    // Contrast: scale around mid grey with Animate's response curve.
    Mat contrast = identity();
    const double c = std::clamp(f.contrast, -100.0, 100.0);
    double x;
    if (c < 0) {
        x = 127 + c / 100 * 127;
    } else {
        const int i = int(c);
        const double frac = c - i;
        const double d = i >= 100 ? kDeltaIndex[100] : kDeltaIndex[i] * (1 - frac) + kDeltaIndex[i + 1] * frac;
        x = d * 127 + 127;
    }
    contrast[0] = contrast[6] = contrast[12] = x / 127;
    contrast[4] = contrast[9] = contrast[14] = 0.5 * (127 - x);

    // Saturation around the luminance axis.
    Mat saturation = identity();
    const double s = std::clamp(f.saturation, -100.0, 100.0);
    const double sx = 1 + (s > 0 ? 3 * s / 100 : s / 100);
    const double lr = 0.3086, lg = 0.6094, lb = 0.0820;
    saturation = {lr * (1 - sx) + sx, lg * (1 - sx),      lb * (1 - sx),      0, 0,
                  lr * (1 - sx),      lg * (1 - sx) + sx, lb * (1 - sx),      0, 0,
                  lr * (1 - sx),      lg * (1 - sx),      lb * (1 - sx) + sx, 0, 0,
                  0,                  0,                  0,                  1, 0};

    // Hue rotation that keeps luminance.
    const double h = std::clamp(f.hue, -180.0, 180.0) * kPi / 180.0;
    const double cv = std::cos(h), sv = std::sin(h);
    const double hr = 0.213, hg = 0.715, hb = 0.072;
    const Mat hue = {hr + cv * (1 - hr) + sv * -hr, hg + cv * -hg + sv * -hg,       hb + cv * -hb + sv * (1 - hb), 0, 0,
                     hr + cv * -hr + sv * 0.143,    hg + cv * (1 - hg) + sv * 0.140, hb + cv * -hb + sv * -0.283,   0, 0,
                     hr + cv * -hr + sv * -(1 - hr), hg + cv * -hg + sv * hg,       hb + cv * (1 - hb) + sv * hb,  0, 0,
                     0,                              0,                             0,                             1, 0};

    return multiply(multiply(multiply(brightness, contrast), saturation), hue);
}

double filterMargin(const FilterList& filters)
{
    double m = 0.0;
    for (const Filter& f : filters) {
        if (!f.enabled) continue;
        const double blur = std::max(f.blurX, f.blurY) * (f.quality >= 3 ? 1.5 : 1.0);
        switch (f.type) {
        case FilterType::DropShadow:
        case FilterType::Bevel:
        case FilterType::GradientBevel: m += blur + std::abs(f.distance); break;
        case FilterType::Blur:
        case FilterType::Glow:
        case FilterType::GradientGlow: m += blur; break;
        case FilterType::AdjustColor:
        case FilterType::Count: break;
        }
    }
    return std::ceil(m) + 2.0;
}

} // namespace vx
