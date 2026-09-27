// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — vector texture brushes (the Paint Brush tool). Every brush turns
// a stroke into ordinary vector fills, so the result can be filled, erased,
// reshaped and shape-tweened like any other shape:
//
//  * Art      — a vector artwork stretched along the stroke (Animate's art
//               brushes): x of the artwork runs along the path, y across it.
//  * Pattern  — a tile repeated along the stroke, each tile bent to the path.
//  * Textured — the swept area with a rough edge and grain holes taken from a
//               noise field anchored to the canvas (chalk, charcoal, pencil).
//  * Scatter  — vector dabs sprayed along the path.
#pragma once

#include "ShapeGraph.h"
#include "geom/Outline.h"
#include "geom/Smooth.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vx {

/// Monotone response curve on [0,1] -> [0,1] (pressure curves).
struct ResponseCurve {
    std::vector<Vec2> points{{0.0, 0.0}, {1.0, 1.0}};
    double eval(double x) const;
    bool isLinear() const;
    bool operator==(const ResponseCurve&) const = default;
};

enum class VectorBrushKind { Art, Pattern, Textured, Scatter };

struct VectorBrushPreset {
    std::string id;
    std::string name;
    VectorBrushKind kind = VectorBrushKind::Textured;

    double size = 12.0;          ///< stroke width at full pressure (px)
    bool pressureSize = true;
    ResponseCurve sizeCurve;
    double minSize = 0.15;       ///< fraction of the size at zero pressure
    double smoothing = 35.0;

    // Art and pattern brushes.
    std::shared_ptr<const ShapeGraph> art; ///< artwork, fills only
    bool colorize = true;        ///< paint the artwork with the current fill
    double patternGap = 0.0;     ///< space between tiles, fraction of a tile
    bool stretchToFit = true;    ///< pattern: scale tiles to fit a whole number

    // Textured brushes.
    double roughness = 0.3;      ///< edge displacement, fraction of the width
    double roughScale = 6.0;     ///< wavelength of the edge noise (px)
    double grain = 0.4;          ///< density of grain holes, 0..1
    double grainSize = 1.6;      ///< grain hole radius (px)

    // Scatter brushes.
    double dabSize = 0.25;       ///< dab radius, fraction of the width
    double density = 1.5;        ///< dabs per stroke width of path
    double scatter = 0.8;        ///< spread across the path, fraction of the width

    bool builtin = false;
};
using VectorBrushPtr = std::shared_ptr<const VectorBrushPreset>;

/// One coloured piece of a brush stroke.
struct BrushPiece {
    FillStyle fill;
    Region region;
};

/// Centre line of a stroke from input samples: half widths from the preset's
/// size, pressure response and minimum size.
std::vector<BrushPoint> vectorBrushPath(const VectorBrushPreset& p, const std::vector<InputSample>& samples);

/// Vector result of a stroke. `path` is the smoothed centre line with half
/// widths (pressure already applied); `tolerance` is the curve fitting
/// tolerance in document units; `seed` varies scatter brushes per stroke.
std::vector<BrushPiece> vectorBrushStroke(const VectorBrushPreset& p, const std::vector<BrushPoint>& path,
                                          const FillStyle& paint, uint32_t seed, double tolerance);

/// The stroke as a shape graph (later pieces paint over earlier ones).
ShapeGraph vectorBrushGraph(const std::vector<BrushPiece>& pieces);

/// Built-in presets: ink taper, dry brush, chalk, charcoal, pencil, spray,
/// stipple, rope, dashes and vine.
const std::vector<VectorBrushPreset>& builtinVectorBrushes();
const VectorBrushPreset* builtinVectorBrush(const std::string& id);

/// A new art (or pattern) brush from a selected artwork (Animate's "create
/// brush from selection"). Lines are converted to fills; the artwork keeps
/// its colours unless it is plain black, which makes a colourised brush.
VectorBrushPreset makeArtBrush(const ShapeGraph& art, bool pattern, const std::string& name);

/// Lines of a shape turned into fills of the line colour (round caps and
/// joins), painted over the shape's own fills.
ShapeGraph linesToFills(const ShapeGraph& g);

/// Canvas-anchored value noise in [-1, 1] (two octaves).
double brushNoise(Vec2 p, double scale);

} // namespace vx
