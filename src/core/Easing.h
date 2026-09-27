// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — tween easing: Animate's classic ease (-100..100), the preset
// eases of the Classic Ease panel and custom ease curves.
#pragma once

#include "geom/Bezier.h"

#include <string_view>
#include <vector>

namespace vx {

enum class EaseKind {
    None,       ///< linear
    Classic,    ///< Flash classic ease, strength -100 (in) .. 100 (out)
    QuadIn, QuadOut, QuadInOut,
    CubicIn, CubicOut, CubicInOut,
    QuartIn, QuartOut, QuartInOut,
    QuintIn, QuintOut, QuintInOut,
    SineIn, SineOut, SineInOut,
    BackIn, BackOut, BackInOut,
    CircIn, CircOut, CircInOut,
    BounceIn, BounceOut, BounceInOut,
    ElasticIn, ElasticOut, ElasticInOut,
    Custom,
    Count
};

struct Ease {
    EaseKind kind = EaseKind::None;
    int strength = 0; ///< Classic only
    /// Custom: chain of cubics in the unit square from (0,0) to (1,1),
    /// x must be monotone (the Custom Ease editor enforces it).
    std::vector<Cubic> curve;

    double apply(double t) const;
    bool operator==(const Ease&) const = default;
};

std::string_view easeId(EaseKind k);
std::string_view easeLabel(EaseKind k);
EaseKind easeFromId(std::string_view id);

} // namespace vx
