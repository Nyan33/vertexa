// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — blend modes. The first group mirrors Adobe Animate's MovieClip
// blending menu; the second group adds the extra modes known from Krita.
#pragma once

#include <array>
#include <string_view>

namespace vx {

enum class BlendMode {
    // Animate
    Normal,
    Layer,
    Darken,
    Multiply,
    Lighten,
    Screen,
    Overlay,
    HardLight,
    Add,
    Subtract,
    Difference,
    Invert,
    Alpha,
    Erase,
    // Krita / W3C extras
    ColorBurn,
    LinearBurn,
    ColorDodge,
    SoftLight,
    VividLight,
    LinearLight,
    PinLight,
    Exclusion,
    Divide,
    Hue,
    Saturation,
    Color,
    Luminosity,
    Count
};

struct BlendModeInfo {
    BlendMode mode;
    std::string_view id;    ///< stable identifier used in files
    std::string_view label; ///< UI label
    bool animate;           ///< available in Adobe Animate
};

constexpr std::array<BlendModeInfo, int(BlendMode::Count)> kBlendModes{{
    {BlendMode::Normal, "normal", "Normal", true},
    {BlendMode::Layer, "layer", "Layer", true},
    {BlendMode::Darken, "darken", "Darken", true},
    {BlendMode::Multiply, "multiply", "Multiply", true},
    {BlendMode::Lighten, "lighten", "Lighten", true},
    {BlendMode::Screen, "screen", "Screen", true},
    {BlendMode::Overlay, "overlay", "Overlay", true},
    {BlendMode::HardLight, "hardlight", "Hard Light", true},
    {BlendMode::Add, "add", "Add", true},
    {BlendMode::Subtract, "subtract", "Subtract", true},
    {BlendMode::Difference, "difference", "Difference", true},
    {BlendMode::Invert, "invert", "Invert", true},
    {BlendMode::Alpha, "alpha", "Alpha", true},
    {BlendMode::Erase, "erase", "Erase", true},
    {BlendMode::ColorBurn, "colorburn", "Color Burn", false},
    {BlendMode::LinearBurn, "linearburn", "Linear Burn", false},
    {BlendMode::ColorDodge, "colordodge", "Color Dodge", false},
    {BlendMode::SoftLight, "softlight", "Soft Light", false},
    {BlendMode::VividLight, "vividlight", "Vivid Light", false},
    {BlendMode::LinearLight, "linearlight", "Linear Light", false},
    {BlendMode::PinLight, "pinlight", "Pin Light", false},
    {BlendMode::Exclusion, "exclusion", "Exclusion", false},
    {BlendMode::Divide, "divide", "Divide", false},
    {BlendMode::Hue, "hue", "Hue", false},
    {BlendMode::Saturation, "saturation", "Saturation", false},
    {BlendMode::Color, "color", "Color", false},
    {BlendMode::Luminosity, "luminosity", "Luminosity", false},
}};

inline std::string_view blendModeId(BlendMode m) { return kBlendModes[int(m)].id; }
inline std::string_view blendModeLabel(BlendMode m) { return kBlendModes[int(m)].label; }
inline BlendMode blendModeFromId(std::string_view id)
{
    for (const auto& i : kBlendModes)
        if (i.id == id) return i.mode;
    return BlendMode::Normal;
}

} // namespace vx
