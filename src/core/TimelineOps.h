// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — frame editing commands of the timeline (F5, Shift+F5, F6, F7,
// Shift+F6, tweens, copy/paste frames, reverse, drag keyframes ...).
// All functions keep the layer normalised (contiguous keyframe spans).
#pragma once

#include "Document.h"

#include <vector>

namespace vx {

/// F5 — insert `count` frames at `frame` (extends the layer when past the end).
void insertFrames(Layer& l, int frame, int count = 1);
/// Shift+F5 — remove `count` frames starting at `frame`.
void removeFrames(Layer& l, int frame, int count = 1);
/// F6 / F7 — insert a keyframe (copying the displayed content, tweens baked)
/// or a blank keyframe. Returns the frame where the keyframe was created, or
/// -1. On an existing keyframe the next frame is converted (like Animate).
int insertKeyframe(const Document& doc, Timeline& tl, int layerIndex, int frame, bool blank);
/// Shift+F6 — clear keyframe (the span joins the previous keyframe).
bool clearKeyframe(Layer& l, int frame);
/// Convert every frame of [from, to] to (blank) keyframes.
void convertToKeyframes(const Document& doc, Timeline& tl, int layerIndex, int from, int to, bool blank);
/// Clear Frames — frames in [from, to] become a blank keyframe span.
void clearFrames(Layer& l, int from, int to);
/// Set the tween type on the keyframe span containing `frame`.
bool setTween(Layer& l, int frame, TweenType type);
/// Reverse the keyframes inside [from, to].
void reverseFrames(Layer& l, int from, int to);
/// Copy frames [from, to] as standalone spans (starts relative to `from`).
std::vector<Keyframe> copyFrames(const Layer& l, int from, int to);
/// Paste frames at `at`. `replace` overwrites the same number of frames,
/// otherwise the frames are inserted and later content moves right.
void pasteFrames(Layer& l, int at, const std::vector<Keyframe>& frames, bool replace);
/// Drag a range of frames [from, to] by `delta` frames (keyframe drag).
void moveFrames(Layer& l, int from, int to, int delta);
/// Split the span containing `frame` so that a keyframe starts there
/// (content copied as-is, no baking). Returns the key index.
int splitAt(Layer& l, int frame);
/// Make sure the layer covers `frame` (extends the last span).
void extendTo(Layer& l, int frame);

} // namespace vx
