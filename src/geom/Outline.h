// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — brush stroke outlines. A brush stroke in Vertexa (like the Brush
// tool of Adobe Animate) is not a centre line with a width: it is a filled
// region equal to the area swept by the brush tip. The region is computed
// exactly (tangent lines between consecutive tip circles, circular joins and
// caps, then a non-zero union through the arrangement) and finally refitted
// into smooth cubic curves within a sub-pixel tolerance.
#pragma once

#include "Region.h"

#include <vector>

namespace vx {

struct BrushPoint {
    Vec2 p;
    double r = 1.0;     ///< half of the tip size at this point
    double angle = 0.0; ///< tip rotation in radians (tilt / bearing driven)
};

enum class TipShape {
    Round,
    Ellipse,    ///< aspect < 1 squashes the circle
    Square,
    Rectangle,
    Slash,      ///< thin line at +45 degrees (Animate brush shape)
    Backslash,  ///< thin line at -45 degrees
    Horizontal, ///< thin horizontal line
    Vertical    ///< thin vertical line
};

struct BrushTip {
    TipShape shape = TipShape::Round;
    double aspect = 1.0;    ///< height / width for Ellipse and Rectangle
    double angle = 0.0;     ///< base rotation (radians)
    bool rotates = false;   ///< true when per-point angles vary (tilt/rotation)
};

/// Exact swept area of a round tip with per-point radius (non-normalised:
/// the returned region may self-overlap; it is correct for non-zero filling).
Region roundSweepOutline(const std::vector<BrushPoint>& pts);

/// Swept area for any tip shape, normalised (no self intersections).
Region sweptRegion(const std::vector<BrushPoint>& pts, const BrushTip& tip, double eps = 1e-7);

/// Resample a stroke at a fixed arc length spacing (linear interpolation of
/// radius and angle).
std::vector<BrushPoint> resampleStroke(const std::vector<BrushPoint>& pts, double spacing);

/// Refit every contour of a region with smooth cubic curves.
Region refitRegion(const Region& r, double tolerance, double cornerAngle = 0.6);

/// Convex polygon of a (non-round) tip at the given radius and rotation.
std::vector<Vec2> tipPolygon(const BrushTip& tip, double r, double angle);

/// Convex hull (counter-clockwise in math orientation).
std::vector<Vec2> convexHull(std::vector<Vec2> pts);

} // namespace vx
