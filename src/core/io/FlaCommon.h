// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — helpers shared by the binary FLA and XFL importers.
#pragma once

#include "core/BlendMode.h"
#include "core/Color.h"
#include "core/Element.h"

#include <algorithm>
#include <cmath>

namespace vx::io {

constexpr double kTwipsPerPixel = 20.0;
/// Flash gradients are defined on the square [-16384, 16384] twips.
constexpr double kGradientHalfSize = 16384.0 / kTwipsPerPixel;

/// SWF / Flash blend mode numbers (0 and 1 are both Normal).
inline BlendMode blendFromFlash(int v)
{
    switch (v) {
    case 2: return BlendMode::Layer;
    case 3: return BlendMode::Multiply;
    case 4: return BlendMode::Screen;
    case 5: return BlendMode::Lighten;
    case 6: return BlendMode::Darken;
    case 7: return BlendMode::Difference;
    case 8: return BlendMode::Add;
    case 9: return BlendMode::Subtract;
    case 10: return BlendMode::Invert;
    case 11: return BlendMode::Alpha;
    case 12: return BlendMode::Erase;
    case 13: return BlendMode::Overlay;
    case 14: return BlendMode::HardLight;
    default: return BlendMode::Normal;
    }
}

inline bool nearlyEqual(const ColorTransform& a, const ColorTransform& b)
{
    auto eq = [](double x, double y, double tol) { return std::abs(x - y) <= tol; };
    return eq(a.rm, b.rm, 1e-3) && eq(a.gm, b.gm, 1e-3) && eq(a.bm, b.bm, 1e-3) && eq(a.am, b.am, 1e-3) &&
           eq(a.ro, b.ro, 0.51) && eq(a.go, b.go, 0.51) && eq(a.bo, b.bo, 0.51) && eq(a.ao, b.ao, 0.51);
}

/// Picks the simplest Animate colour effect that reproduces a transform.
inline ColorEffect colorEffectFrom(const ColorTransform& t)
{
    ColorEffect e;
    if (t.isIdentity()) return e;
    auto eq = [](double a, double b) { return std::abs(a - b) < 1e-3; };
    const bool sameMul = eq(t.rm, t.gm) && eq(t.gm, t.bm);
    const bool noAlphaChange = eq(t.am, 1.0) && eq(t.ao, 0.0);
    if (sameMul && eq(t.ro, 0) && eq(t.go, 0) && eq(t.bo, 0) && eq(t.rm, 1.0) && eq(t.ao, 0.0)) {
        e.kind = ColorEffect::Kind::Alpha;
        e.alpha = std::clamp(t.am, 0.0, 1.0);
        return e;
    }
    if (sameMul && noAlphaChange) {
        const double m = t.rm;
        if (eq(t.ro, 0) && eq(t.go, 0) && eq(t.bo, 0) && m < 1.0) {
            e.kind = ColorEffect::Kind::Brightness;
            e.brightness = m - 1.0;
            return e;
        }
        const double amount = 1.0 - m;
        if (amount > 1e-3) {
            auto ch = [&](double off) { return uint8_t(std::clamp(std::lround(off / amount), 0L, 255L)); };
            const Color tint(ch(t.ro), ch(t.go), ch(t.bo));
            if (tint == Color(255, 255, 255) && eq(t.ro, 255.0 * amount)) {
                e.kind = ColorEffect::Kind::Brightness;
                e.brightness = amount;
                return e;
            }
            e.kind = ColorEffect::Kind::Tint;
            e.tint = tint;
            e.tintAmount = amount;
            if (nearlyEqual(e.toTransform(), t)) return e;
        }
    }
    e = ColorEffect{};
    e.kind = ColorEffect::Kind::Advanced;
    e.advanced = t;
    return e;
}

} // namespace vx::io
