// SPDX-License-Identifier: GPL-3.0-or-later
#include "Easing.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vx {

namespace {

struct EaseName {
    EaseKind kind;
    std::string_view id, label;
};

constexpr std::array<EaseName, int(EaseKind::Count)> kEaseNames{{
    {EaseKind::None, "none", "No Ease"},
    {EaseKind::Classic, "classic", "Classic Ease"},
    {EaseKind::QuadIn, "quadIn", "Quad In"}, {EaseKind::QuadOut, "quadOut", "Quad Out"},
    {EaseKind::QuadInOut, "quadInOut", "Quad In Out"},
    {EaseKind::CubicIn, "cubicIn", "Cubic In"}, {EaseKind::CubicOut, "cubicOut", "Cubic Out"},
    {EaseKind::CubicInOut, "cubicInOut", "Cubic In Out"},
    {EaseKind::QuartIn, "quartIn", "Quart In"}, {EaseKind::QuartOut, "quartOut", "Quart Out"},
    {EaseKind::QuartInOut, "quartInOut", "Quart In Out"},
    {EaseKind::QuintIn, "quintIn", "Quint In"}, {EaseKind::QuintOut, "quintOut", "Quint Out"},
    {EaseKind::QuintInOut, "quintInOut", "Quint In Out"},
    {EaseKind::SineIn, "sineIn", "Sine In"}, {EaseKind::SineOut, "sineOut", "Sine Out"},
    {EaseKind::SineInOut, "sineInOut", "Sine In Out"},
    {EaseKind::BackIn, "backIn", "Back In"}, {EaseKind::BackOut, "backOut", "Back Out"},
    {EaseKind::BackInOut, "backInOut", "Back In Out"},
    {EaseKind::CircIn, "circIn", "Circ In"}, {EaseKind::CircOut, "circOut", "Circ Out"},
    {EaseKind::CircInOut, "circInOut", "Circ In Out"},
    {EaseKind::BounceIn, "bounceIn", "Bounce In"}, {EaseKind::BounceOut, "bounceOut", "Bounce Out"},
    {EaseKind::BounceInOut, "bounceInOut", "Bounce In Out"},
    {EaseKind::ElasticIn, "elasticIn", "Elastic In"}, {EaseKind::ElasticOut, "elasticOut", "Elastic Out"},
    {EaseKind::ElasticInOut, "elasticInOut", "Elastic In Out"},
    {EaseKind::Custom, "custom", "Custom"},
}};

double bounceOut(double t)
{
    const double n1 = 7.5625, d1 = 2.75;
    if (t < 1.0 / d1) return n1 * t * t;
    if (t < 2.0 / d1) { t -= 1.5 / d1; return n1 * t * t + 0.75; }
    if (t < 2.5 / d1) { t -= 2.25 / d1; return n1 * t * t + 0.9375; }
    t -= 2.625 / d1;
    return n1 * t * t + 0.984375;
}

double powIn(double t, int n) { return std::pow(t, n); }
double powOut(double t, int n) { return 1.0 - std::pow(1.0 - t, n); }
double powInOut(double t, int n)
{
    return t < 0.5 ? std::pow(2.0, n - 1) * std::pow(t, n) : 1.0 - std::pow(-2.0 * t + 2.0, n) / 2.0;
}

double customEase(const std::vector<Cubic>& curve, double t)
{
    if (curve.empty()) return t;
    for (const Cubic& c : curve) {
        if (t < c.p0.x || t > c.p3.x) continue;
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 60; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (c.eval(mid).x < t) lo = mid;
            else hi = mid;
        }
        return c.eval(0.5 * (lo + hi)).y;
    }
    return t <= curve.front().p0.x ? curve.front().p0.y : curve.back().p3.y;
}

} // namespace

double Ease::apply(double t) const
{
    t = std::clamp(t, 0.0, 1.0);
    constexpr double c1 = 1.70158, c2 = c1 * 1.525, c3 = c1 + 1.0;
    constexpr double c4 = 2.0 * kPi / 3.0, c5 = 2.0 * kPi / 4.5;
    switch (kind) {
    case EaseKind::None: return t;
    case EaseKind::Classic: return t + (std::clamp(strength, -100, 100) / 100.0) * t * (1.0 - t);
    case EaseKind::QuadIn: return powIn(t, 2);
    case EaseKind::QuadOut: return powOut(t, 2);
    case EaseKind::QuadInOut: return powInOut(t, 2);
    case EaseKind::CubicIn: return powIn(t, 3);
    case EaseKind::CubicOut: return powOut(t, 3);
    case EaseKind::CubicInOut: return powInOut(t, 3);
    case EaseKind::QuartIn: return powIn(t, 4);
    case EaseKind::QuartOut: return powOut(t, 4);
    case EaseKind::QuartInOut: return powInOut(t, 4);
    case EaseKind::QuintIn: return powIn(t, 5);
    case EaseKind::QuintOut: return powOut(t, 5);
    case EaseKind::QuintInOut: return powInOut(t, 5);
    case EaseKind::SineIn: return 1.0 - std::cos(t * kPi / 2.0);
    case EaseKind::SineOut: return std::sin(t * kPi / 2.0);
    case EaseKind::SineInOut: return -(std::cos(kPi * t) - 1.0) / 2.0;
    case EaseKind::BackIn: return c3 * t * t * t - c1 * t * t;
    case EaseKind::BackOut: return 1.0 + c3 * std::pow(t - 1.0, 3) + c1 * std::pow(t - 1.0, 2);
    case EaseKind::BackInOut:
        return t < 0.5 ? (std::pow(2 * t, 2) * ((c2 + 1) * 2 * t - c2)) / 2
                       : (std::pow(2 * t - 2, 2) * ((c2 + 1) * (t * 2 - 2) + c2) + 2) / 2;
    case EaseKind::CircIn: return 1.0 - std::sqrt(1.0 - t * t);
    case EaseKind::CircOut: return std::sqrt(1.0 - std::pow(t - 1.0, 2));
    case EaseKind::CircInOut:
        return t < 0.5 ? (1 - std::sqrt(1 - std::pow(2 * t, 2))) / 2 : (std::sqrt(1 - std::pow(-2 * t + 2, 2)) + 1) / 2;
    case EaseKind::BounceIn: return 1.0 - bounceOut(1.0 - t);
    case EaseKind::BounceOut: return bounceOut(t);
    case EaseKind::BounceInOut: return t < 0.5 ? (1 - bounceOut(1 - 2 * t)) / 2 : (1 + bounceOut(2 * t - 1)) / 2;
    case EaseKind::ElasticIn:
        if (t == 0 || t == 1) return t;
        return -std::pow(2.0, 10 * t - 10) * std::sin((t * 10 - 10.75) * c4);
    case EaseKind::ElasticOut:
        if (t == 0 || t == 1) return t;
        return std::pow(2.0, -10 * t) * std::sin((t * 10 - 0.75) * c4) + 1;
    case EaseKind::ElasticInOut:
        if (t == 0 || t == 1) return t;
        return t < 0.5 ? -(std::pow(2.0, 20 * t - 10) * std::sin((20 * t - 11.125) * c5)) / 2
                       : (std::pow(2.0, -20 * t + 10) * std::sin((20 * t - 11.125) * c5)) / 2 + 1;
    case EaseKind::Custom: return customEase(curve, t);
    case EaseKind::Count: break;
    }
    return t;
}

std::string_view easeId(EaseKind k) { return kEaseNames[int(k)].id; }
std::string_view easeLabel(EaseKind k) { return kEaseNames[int(k)].label; }
EaseKind easeFromId(std::string_view id)
{
    for (const auto& e : kEaseNames)
        if (e.id == id) return e.kind;
    return EaseKind::None;
}

} // namespace vx
