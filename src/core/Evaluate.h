// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — timeline evaluation: what is visible on a layer at a frame, with
// classic tweens (incl. motion guides) and shape tweens applied.
#pragma once

#include "Document.h"

#include <vector>

namespace vx {

struct EvalItem {
    ElementPtr element;
    /// Frames elapsed since the element's keyframe started (drives graphic
    /// symbol playback).
    int localFrame = 0;
};

/// Elements visible on a layer at `frame` (bottom to top).
std::vector<EvalItem> evaluateLayer(const Document& doc, const Timeline& tl, int layerIndex, int frame);

/// Frame of the symbol timeline displayed by an instance. Graphic symbols
/// follow their loop options; movie clips and buttons show `clipFrame`
/// (0 while authoring, the global playback frame when previewing).
int instanceSymbolFrame(const Document& doc, const InstanceElement& inst, int localFrame, int clipFrame = 0);

/// Stroke chains of a guide layer at a frame, in layer coordinates.
std::vector<std::vector<Cubic>> guideChains(const Timeline& tl, int guideLayerIndex, int frame);

/// Bounds of an element in its parent's coordinates.
Rect elementBounds(const Document& doc, const Element& e, int localFrame = 0, int depth = 0);
/// Bounds of all visible layers of a timeline at a frame.
Rect timelineBounds(const Document& doc, const Timeline& tl, int frame, int depth = 0);

/// Content of a keyframe copied as a new standalone keyframe at `frame`
/// (tweens are baked: used by Insert Keyframe inside a tween span).
std::vector<ElementPtr> bakeFrame(const Document& doc, const Timeline& tl, int layerIndex, int frame);

} // namespace vx
