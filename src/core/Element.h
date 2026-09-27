// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — elements that live on keyframes (the XFL <elements> list).
//
// Elements are immutable once placed in a document and shared through
// shared_ptr<const Element>: copying a timeline (undo snapshots, keyframe
// duplication) is cheap and edits always produce new elements.
#pragma once

#include "BlendMode.h"
#include "BrushPreset.h"
#include "Color.h"
#include "ShapeGraph.h"

#include <memory>
#include <string>
#include <vector>

namespace vx {

enum class ElementType { Shape, Instance, Group, Paint, Morph };

class Element;
using ElementPtr = std::shared_ptr<const Element>;

class Element {
public:
    virtual ~Element() = default;
    virtual ElementType type() const = 0;
    virtual std::shared_ptr<Element> clone() const = 0;

    Affine matrix;   ///< element space -> parent space
    Vec2 pivot;      ///< transformation point, element space
    std::string name;

    template <class T>
    std::shared_ptr<T> cloneAs() const { return std::static_pointer_cast<T>(clone()); }
    /// Copy with a different matrix.
    ElementPtr withMatrix(const Affine& m) const
    {
        auto c = clone();
        c->matrix = m;
        return c;
    }
};

/// A vector shape. In merge drawing mode a keyframe has at most one such
/// element (isObject == false, identity matrix) at the bottom; drawing
/// objects (object drawing mode, J) are shapes with isObject == true.
class ShapeElement final : public Element {
public:
    ShapeGraphPtr graph = std::make_shared<ShapeGraph>();
    bool isObject = false;

    ElementType type() const override { return ElementType::Shape; }
    std::shared_ptr<Element> clone() const override { return std::make_shared<ShapeElement>(*this); }
};

enum class SymbolType { MovieClip, Graphic, Button };
enum class LoopMode { Loop, PlayOnce, SingleFrame, LoopReverse, PlayOnceReverse };

/// Instance of a library symbol.
class InstanceElement final : public Element {
public:
    std::string symbolId;
    ColorEffect color;
    BlendMode blend = BlendMode::Normal;
    LoopMode loop = LoopMode::Loop;
    int firstFrame = 0;     ///< graphic symbols
    int lastFrame = -1;     ///< graphic symbols, -1 = last frame of the symbol
    bool visible = true;

    ElementType type() const override { return ElementType::Instance; }
    std::shared_ptr<Element> clone() const override { return std::make_shared<InstanceElement>(*this); }
};

class GroupElement final : public Element {
public:
    std::vector<ElementPtr> children;

    ElementType type() const override { return ElementType::Group; }
    std::shared_ptr<Element> clone() const override { return std::make_shared<GroupElement>(*this); }
};

struct PaintSample {
    Vec2 pos;
    float pressure = 1.0f;
    float tiltX = 0.0f, tiltY = 0.0f; ///< degrees
    float rotation = 0.0f;            ///< degrees
};

struct PaintStroke {
    std::vector<PaintSample> samples;
    BrushPresetPtr brush;
    Color color;
    uint32_t seed = 1;
    bool erase = false; ///< erases previous strokes of the same paint element
};

/// Texture brush painting: a list of dab-rendered strokes composited in their
/// own buffer (so erase strokes only affect this element).
class PaintElement final : public Element {
public:
    std::vector<PaintStroke> strokes;

    ElementType type() const override { return ElementType::Paint; }
    std::shared_ptr<Element> clone() const override { return std::make_shared<PaintElement>(*this); }
    Rect localBounds() const;
};

/// Render-only element produced by shape tweens.
class MorphElement final : public Element {
public:
    std::shared_ptr<const ShapeRenderData> data;

    ElementType type() const override { return ElementType::Morph; }
    std::shared_ptr<Element> clone() const override { return std::make_shared<MorphElement>(*this); }
};

inline const ShapeElement* asShape(const ElementPtr& e)
{
    return e && e->type() == ElementType::Shape ? static_cast<const ShapeElement*>(e.get()) : nullptr;
}
inline const InstanceElement* asInstance(const ElementPtr& e)
{
    return e && e->type() == ElementType::Instance ? static_cast<const InstanceElement*>(e.get()) : nullptr;
}
inline const GroupElement* asGroup(const ElementPtr& e)
{
    return e && e->type() == ElementType::Group ? static_cast<const GroupElement*>(e.get()) : nullptr;
}
inline const PaintElement* asPaint(const ElementPtr& e)
{
    return e && e->type() == ElementType::Paint ? static_cast<const PaintElement*>(e.get()) : nullptr;
}

std::shared_ptr<ShapeElement> makeShapeElement(ShapeGraph g, bool isObject, const Affine& m = {});

} // namespace vx
