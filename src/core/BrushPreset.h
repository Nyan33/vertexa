// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — texture brush presets, modelled on Krita's pixel brush engine:
// a brush tip (auto-generated or an image), spacing, flow/opacity, sensor
// curves (pressure, tilt, rotation, random) and a canvas-anchored texture.
// The strokes stay vector data (samples + preset) and are rasterised at the
// current zoom, so they remain crisp and animatable.
#pragma once

#include "BlendMode.h"
#include "geom/Vec2.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vx {

/// Monotone response curve on [0,1] -> [0,1] (like Krita's sensor curves).
struct ResponseCurve {
    std::vector<Vec2> points{{0.0, 0.0}, {1.0, 1.0}};
    double eval(double x) const;
    bool isLinear() const;
    bool operator==(const ResponseCurve&) const = default;
};

/// 8-bit single channel image (brush tips, textures).
struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
    bool isNull() const { return width <= 0 || height <= 0 || pixels.empty(); }
    uint8_t at(int x, int y) const { return pixels[size_t(y) * width + x]; }
    /// Bilinear sample with wrap-around (for textures).
    double sampleWrapped(double x, double y) const;
    /// Bilinear sample clamped to transparent outside (for tips).
    double sampleClamped(double x, double y) const;
};
using GrayImagePtr = std::shared_ptr<const GrayImage>;

enum class TipType { Auto, Image };
enum class AutoTipShape { Circle, Square };
enum class TextureMode { Multiply, Subtract, Height };

struct BrushPreset {
    std::string id;
    std::string name;
    std::string category;

    // Tip
    TipType tipType = TipType::Auto;
    AutoTipShape autoShape = AutoTipShape::Circle;
    double hardness = 0.85;   ///< 0 = very soft, 1 = hard edge
    double roundness = 1.0;   ///< aspect ratio of the tip
    double angle = 0.0;       ///< degrees
    std::string tipImage;     ///< "builtin:<name>" or a document image id
    double tipDensity = 1.0;  ///< fraction of pixels kept (spray-like tips)

    // Basics
    double size = 12.0;       ///< diameter in document units
    double spacing = 0.08;    ///< fraction of the dab diameter
    double opacity = 1.0;
    double flow = 1.0;
    bool buildUp = false;     ///< false: "wash" (flow builds up to opacity)

    // Dynamics
    bool pressureSize = true;
    ResponseCurve sizeCurve;
    double minSize = 0.05;    ///< fraction of size at zero pressure
    bool pressureOpacity = false;
    ResponseCurve opacityCurve;
    bool pressureFlow = false;
    ResponseCurve flowCurve;
    bool tiltAngle = false;       ///< tip rotation follows the pen tilt
    bool followDirection = false; ///< tip rotation follows the stroke direction
    double rotationJitter = 0.0;  ///< 0..1 of a full turn
    double sizeJitter = 0.0;      ///< 0..1
    double opacityJitter = 0.0;   ///< 0..1
    double scatter = 0.0;         ///< offset perpendicular to the stroke, in diameters
    int count = 1;                ///< dabs per spacing step

    // Texture (Krita "Pattern" option)
    bool textureEnabled = false;
    std::string texture = "builtin:paper";
    double textureScale = 1.0;
    double textureStrength = 0.7;
    double textureBrightness = 0.0; ///< -1..1
    double textureContrast = 1.0;   ///< 0..3
    bool textureInvert = false;
    TextureMode textureMode = TextureMode::Multiply;

    BlendMode blend = BlendMode::Normal;
    double smoothing = 25.0;          ///< input stabiliser 0..100

    bool operator==(const BrushPreset&) const = default;
};
using BrushPresetPtr = std::shared_ptr<const BrushPreset>;

/// Built-in presets (pencil, ink, chalk, charcoal, airbrush, watercolour ...).
const std::vector<BrushPreset>& builtinBrushPresets();

} // namespace vx
