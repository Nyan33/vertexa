// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — filters of movie clip and button instances (Animate's Filters
// panel: Drop Shadow, Blur, Glow, Bevel, Gradient Glow, Gradient Bevel and
// Adjust Color). Filters are pixel effects applied to the instance after it
// has been rendered into its own buffer; the instance itself stays vector.
#pragma once

#include "Style.h"

#include <array>
#include <string_view>
#include <vector>

namespace vx {

enum class FilterType { DropShadow, Blur, Glow, Bevel, GradientGlow, GradientBevel, AdjustColor, Count };

enum class BevelKind { Inner, Outer, Full };

struct Filter {
    FilterType type = FilterType::DropShadow;
    bool enabled = true;

    double blurX = 5.0, blurY = 5.0; ///< px
    double strength = 1.0;           ///< 0..255 (Animate shows 0..25500 %)
    int quality = 1;                 ///< 1 low, 2 medium, 3 high (box blur passes)
    double angle = 45.0;             ///< degrees, shadows and bevels
    double distance = 5.0;           ///< px, shadows and bevels
    Color color{0, 0, 0};            ///< shadow / glow colour (with alpha)
    Color highlight{255, 255, 255};  ///< bevel highlight
    bool inner = false;              ///< inner shadow / glow
    bool knockout = false;
    bool hideObject = false;         ///< drop shadow only
    BevelKind bevel = BevelKind::Inner;
    Gradient gradient;               ///< gradient glow / bevel

    // Adjust Color, in Animate's ranges.
    double brightness = 0.0; ///< -100..100
    double contrast = 0.0;   ///< -100..100
    double saturation = 0.0; ///< -100..100
    double hue = 0.0;        ///< -180..180

    bool operator==(const Filter&) const = default;

    static Filter defaults(FilterType t);
};

using FilterList = std::vector<Filter>;

std::string_view filterId(FilterType t);
std::string_view filterLabel(FilterType t);
FilterType filterFromId(std::string_view id);

/// Filters interpolated for classic tweens: lists must match by type, otherwise
/// the list of `a` is kept until the end of the tween (like Animate).
FilterList lerpFilters(const FilterList& a, const FilterList& b, double t);

/// 4x5 colour matrix (row-major, offsets in 0..255) of an Adjust Color filter.
std::array<double, 20> adjustColorMatrix(const Filter& f);

/// How far (px) filters can draw outside the instance bounds.
double filterMargin(const FilterList& filters);

} // namespace vx
