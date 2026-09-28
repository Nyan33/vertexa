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
    /// The keyframe element this item shows: the same pointer on every
    /// evaluation, also for tweened copies (null for shape tween morphs).
    const Element* source = nullptr;
};

/// Whether the tween of keyframe `keyIndex` animates anything: the next
/// keyframe has content and, for a classic tween, elements that pair up
/// (instances, groups, drawing objects), for a shape tween, shapes.
bool tweenAnimates(const Layer& layer, int keyIndex);

/// Elements visible on a layer at `frame` (bottom to top).
std::vector<EvalItem> evaluateLayer(const Document& doc, const Timeline& tl, int layerIndex, int frame);

/// Button states and the matching frames of a button symbol (Up, Over,
/// Down; the fourth frame, Hit, is never shown).
enum class ButtonState { Up = 0, Over = 1, Down = 2 };

/// Frame of the symbol timeline displayed by an instance. Graphic symbols
/// follow their loop options; movie clips show `clipFrame` (0 while
/// authoring, the global playback frame when previewing); buttons show the
/// frame of `button`.
int instanceSymbolFrame(const Document& doc, const InstanceElement& inst, int localFrame, int clipFrame = 0,
                        ButtonState button = ButtonState::Up);

/// Frame of a button symbol that defines where it can be clicked: Hit when
/// it has content, otherwise Up.
int buttonHitFrame(const Document& doc, const Symbol& s);

/// Topmost instance with button behaviour whose hit area contains `p`
/// (timeline space), searched through nested symbols. Returns its
/// `EvalItem::source`, or null.
const Element* buttonAt(const Document& doc, const Timeline& tl, int frame, Vec2 p, double tol, int clipFrame = 0);

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
