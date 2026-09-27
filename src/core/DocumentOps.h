// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — document level editing: symbols (Convert to Symbol, Break
// Apart), groups, merge-shape helpers and element transforms.
#pragma once

#include "Document.h"
#include "ShapeOps.h"

#include <vector>

namespace vx {

/// The merge-drawing shape of a keyframe (empty graph if none).
ShapeGraph keyframeMergeShape(const Keyframe& k);
/// Replace (or remove, when empty) the merge-drawing shape of a keyframe.
void setKeyframeMergeShape(Keyframe& k, ShapeGraph g);
/// Paint a shape into the keyframe's merge shape.
void mergeIntoKeyframe(Keyframe& k, const ShapeGraph& g, const OverlayOptions& opt = {});

/// Registration point presets of the Convert to Symbol dialog (3x3 grid).
Vec2 registrationPoint(const Rect& bounds, int gridIndex);

/// F8 — move `elements` (in layer coordinates) into a new symbol whose origin
/// is `registration`. Returns the instance that replaces them.
std::shared_ptr<InstanceElement> convertToSymbol(Document& doc, const std::vector<ElementPtr>& elements,
                                                 const std::string& name, SymbolType type, Vec2 registration);

/// Ctrl+B — elements that replace `e` after Break Apart. Shapes returned by
/// this function must be merged into the merge shape by the caller.
std::vector<ElementPtr> breakApart(const Document& doc, const ElementPtr& e, int localFrame = 0);

/// Ctrl+G — group elements (merge shape parts become a drawing inside).
std::shared_ptr<GroupElement> groupElements(const Document& doc, const std::vector<ElementPtr>& elements);
/// Ctrl+Shift+G — children transformed into the parent space.
std::vector<ElementPtr> ungroup(const GroupElement& g);

/// Flattens nested structure into one shape (used by break apart and by the
/// Combine Objects commands).
ShapeGraph flattenToShape(const Document& doc, const std::vector<ElementPtr>& elements, int localFrame = 0);

/// Creates the classic tween on the span containing `frame`: shapes on the
/// two keyframes are turned into graphic symbols ("Tween 1", ...) like
/// Animate does. Returns false when there is no next keyframe.
bool createClassicTween(Document& doc, Timeline& tl, int layerIndex, int frame);
bool createShapeTween(Document& doc, Timeline& tl, int layerIndex, int frame);

/// Duplicate a symbol (library "Duplicate Symbol").
std::string duplicateSymbol(Document& doc, const std::string& symbolId, const std::string& newName);
/// Delete a symbol and all its instances.
void deleteSymbol(Document& doc, const std::string& symbolId);
/// Replace references to one symbol by another (Swap Symbol).
ElementPtr swapSymbol(const ElementPtr& instance, const std::string& newSymbolId);

} // namespace vx
