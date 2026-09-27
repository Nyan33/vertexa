// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — shape tweens (morphing between two vector shapes), including
// shape hints and the Distributive / Angular blend options of Animate.
#pragma once

#include "Element.h"
#include "Timeline.h"

namespace vx {

/// Render data of all shapes (merge shape + drawing objects) of a keyframe,
/// in keyframe (layer) coordinates.
ShapeRenderData collectShapeData(const std::vector<ElementPtr>& elements);

/// Morph from a to b at t (0..1).
ShapeRenderData morphShapes(const ShapeRenderData& a, const ShapeRenderData& b, double t, bool angular,
                            const std::vector<ShapeHint>& hints = {});

/// Converts render data back into an editable shape (used when a keyframe is
/// inserted in the middle of a shape tween).
ShapeGraph graphFromRenderData(const ShapeRenderData& rd);

FillStyle lerpFill(const FillStyle& a, const FillStyle& b, double t);
StrokeStyle lerpStroke(const StrokeStyle& a, const StrokeStyle& b, double t);

} // namespace vx
