// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrushPreset.h"

#include <algorithm>
#include <cmath>

namespace vx {

double ResponseCurve::eval(double x) const
{
    x = std::clamp(x, 0.0, 1.0);
    if (points.size() < 2) return x;
    if (x <= points.front().x) return std::clamp(points.front().y, 0.0, 1.0);
    if (x >= points.back().x) return std::clamp(points.back().y, 0.0, 1.0);
    // Monotone cubic Hermite interpolation (Fritsch-Carlson), smooth like Krita.
    const size_t n = points.size();
    size_t k = 0;
    while (k + 1 < n && points[k + 1].x < x) ++k;
    auto slope = [&](size_t i) {
        const double dx = points[i + 1].x - points[i].x;
        return dx > 0 ? (points[i + 1].y - points[i].y) / dx : 0.0;
    };
    auto tangent = [&](size_t i) {
        if (i == 0) return slope(0);
        if (i == n - 1) return slope(n - 2);
        const double a = slope(i - 1), b = slope(i);
        if (a * b <= 0) return 0.0;
        return 2.0 / (1.0 / a + 1.0 / b);
    };
    const Vec2 p0 = points[k], p1 = points[k + 1];
    const double h = p1.x - p0.x;
    if (h <= 0) return std::clamp(p1.y, 0.0, 1.0);
    const double t = (x - p0.x) / h;
    const double t2 = t * t, t3 = t2 * t;
    const double y = (2 * t3 - 3 * t2 + 1) * p0.y + (t3 - 2 * t2 + t) * h * tangent(k) + (-2 * t3 + 3 * t2) * p1.y +
                     (t3 - t2) * h * tangent(k + 1);
    return std::clamp(y, 0.0, 1.0);
}

bool ResponseCurve::isLinear() const
{
    return points.size() == 2 && points[0] == Vec2{0, 0} && points[1] == Vec2{1, 1};
}

double GrayImage::sampleWrapped(double x, double y) const
{
    if (isNull()) return 1.0;
    x -= 0.5;
    y -= 0.5;
    const double fx = std::floor(x), fy = std::floor(y);
    const double ax = x - fx, ay = y - fy;
    auto wrap = [](long v, int m) { long r = v % m; return int(r < 0 ? r + m : r); };
    const int x0 = wrap(long(fx), width), y0 = wrap(long(fy), height);
    const int x1 = (x0 + 1) % width, y1 = (y0 + 1) % height;
    const double a = at(x0, y0), b = at(x1, y0), c = at(x0, y1), d = at(x1, y1);
    return ((a * (1 - ax) + b * ax) * (1 - ay) + (c * (1 - ax) + d * ax) * ay) / 255.0;
}

double GrayImage::sampleClamped(double x, double y) const
{
    if (isNull()) return 0.0;
    x -= 0.5;
    y -= 0.5;
    const double fx = std::floor(x), fy = std::floor(y);
    const double ax = x - fx, ay = y - fy;
    auto px = [&](long xx, long yy) -> double {
        if (xx < 0 || yy < 0 || xx >= width || yy >= height) return 0.0;
        return at(int(xx), int(yy));
    };
    const long x0 = long(fx), y0 = long(fy);
    return ((px(x0, y0) * (1 - ax) + px(x0 + 1, y0) * ax) * (1 - ay) +
            (px(x0, y0 + 1) * (1 - ax) + px(x0 + 1, y0 + 1) * ax) * ay) /
           255.0;
}

const std::vector<BrushPreset>& builtinBrushPresets()
{
    static const std::vector<BrushPreset> presets = [] {
        std::vector<BrushPreset> v;
        BrushPreset p;

        p = {};
        p.id = "vx.pencil";
        p.name = "Graphite Pencil";
        p.category = "Sketch";
        p.size = 4.0;
        p.hardness = 0.6;
        p.spacing = 0.05;
        p.pressureOpacity = true;
        p.opacityCurve.points = {{0, 0}, {0.5, 0.35}, {1, 1}};
        p.minSize = 0.5;
        p.textureEnabled = true;
        p.texture = "builtin:paper";
        p.textureStrength = 0.85;
        p.textureScale = 0.5;
        p.flow = 0.9;
        v.push_back(p);

        p = {};
        p.id = "vx.ink";
        p.name = "Ink Pen";
        p.category = "Ink";
        p.size = 8.0;
        p.hardness = 0.98;
        p.spacing = 0.04;
        p.minSize = 0.05;
        p.sizeCurve.points = {{0, 0}, {0.4, 0.55}, {1, 1}};
        p.smoothing = 40;
        v.push_back(p);

        p = {};
        p.id = "vx.chalk";
        p.name = "Chalk";
        p.category = "Dry";
        p.tipType = TipType::Image;
        p.tipImage = "builtin:chalk";
        p.size = 22.0;
        p.spacing = 0.12;
        p.rotationJitter = 1.0;
        p.pressureOpacity = true;
        p.textureEnabled = true;
        p.texture = "builtin:canvas";
        p.textureStrength = 0.8;
        p.minSize = 0.6;
        v.push_back(p);

        p = {};
        p.id = "vx.charcoal";
        p.name = "Charcoal";
        p.category = "Dry";
        p.tipType = TipType::Image;
        p.tipImage = "builtin:charcoal";
        p.size = 28.0;
        p.spacing = 0.1;
        p.tiltAngle = true;
        p.followDirection = true;
        p.pressureOpacity = true;
        p.pressureSize = true;
        p.minSize = 0.4;
        p.textureEnabled = true;
        p.texture = "builtin:grain";
        p.textureStrength = 0.9;
        p.textureScale = 0.8;
        v.push_back(p);

        p = {};
        p.id = "vx.airbrush";
        p.name = "Soft Airbrush";
        p.category = "Paint";
        p.size = 60.0;
        p.hardness = 0.0;
        p.spacing = 0.05;
        p.flow = 0.08;
        p.pressureSize = false;
        p.pressureFlow = true;
        v.push_back(p);

        p = {};
        p.id = "vx.watercolor";
        p.name = "Wet Wash";
        p.category = "Paint";
        p.tipType = TipType::Image;
        p.tipImage = "builtin:blot";
        p.size = 48.0;
        p.spacing = 0.15;
        p.flow = 0.25;
        p.opacity = 0.8;
        p.rotationJitter = 1.0;
        p.sizeJitter = 0.25;
        p.textureEnabled = true;
        p.texture = "builtin:paper";
        p.textureMode = TextureMode::Subtract;
        p.textureStrength = 0.5;
        v.push_back(p);

        p = {};
        p.id = "vx.spray";
        p.name = "Spray";
        p.category = "Paint";
        p.tipType = TipType::Image;
        p.tipImage = "builtin:spray";
        p.size = 40.0;
        p.spacing = 0.2;
        p.scatter = 0.3;
        p.count = 2;
        p.rotationJitter = 1.0;
        p.pressureSize = false;
        p.pressureFlow = true;
        v.push_back(p);

        p = {};
        p.id = "vx.marker";
        p.name = "Flat Marker";
        p.category = "Ink";
        p.autoShape = AutoTipShape::Square;
        p.roundness = 0.3;
        p.angle = 30;
        p.hardness = 0.9;
        p.size = 18.0;
        p.spacing = 0.05;
        p.opacity = 0.75;
        p.pressureSize = false;
        v.push_back(p);
        return v;
    }();
    return presets;
}

} // namespace vx
