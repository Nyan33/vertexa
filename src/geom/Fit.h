// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — least-squares cubic Bezier fitting (Schneider, "An Algorithm for
// Automatically Fitting Digitized Curves", Graphics Gems 1990) with corner
// detection. Used by the pencil, the brush outline and shape recognition.
#pragma once

#include "Bezier.h"

#include <vector>

namespace vx {

struct FitOptions {
    double tolerance = 0.5;      ///< maximum distance between points and the fitted curve
    double cornerAngle = 0.9;    ///< radians; sharper turns are kept as corners
    double cornerRadius = 0.0;   ///< neighbourhood used to measure turns (0 = auto)
    bool closed = false;
};

/// Fits a chain of cubics through the (ordered) points.
std::vector<Cubic> fitCurves(const std::vector<Vec2>& pts, const FitOptions& opt);

/// Remove consecutive duplicates closer than minDist.
std::vector<Vec2> dedupePoints(const std::vector<Vec2>& pts, double minDist);

/// Douglas-Peucker polyline simplification.
std::vector<Vec2> simplifyPolyline(const std::vector<Vec2>& pts, double tolerance);

/// Densely sample a chain of curves (spacing in document units). Curve end
/// points are always included; `isVertex` marks them.
void sampleChain(const std::vector<Cubic>& chain, double spacing, std::vector<Vec2>& out,
                 std::vector<char>* isVertex = nullptr);

} // namespace vx
