// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — timelines, layers and keyframes (XFL: DOMTimeline / DOMLayer /
// DOMFrame). Layers are stored top to bottom, like the timeline panel.
#pragma once

#include "Easing.h"
#include "Element.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vx {

enum class TweenType { None, Classic, Shape };
enum class RotateMode { None, Auto, Clockwise, CounterClockwise };
enum class LabelType { Name, Comment, Anchor };
enum class LayerType { Normal, Guide, Mask, Folder };

struct ClassicTweenSettings {
    Ease ease;
    RotateMode rotate = RotateMode::Auto;
    int rotations = 0;
    bool orientToPath = false;
    bool sync = true;
    bool snap = true;
    bool scale = true;
    bool operator==(const ClassicTweenSettings&) const = default;
};

struct ShapeTweenSettings {
    Ease ease;
    bool angular = false; ///< Animate "Blend: Angular" (keeps corners)
    bool operator==(const ShapeTweenSettings&) const = default;
};

/// Shape hint pair (a, b, c ...): position in the start and end shapes.
struct ShapeHint {
    Vec2 start;
    Vec2 end;
    bool operator==(const ShapeHint&) const = default;
};

struct Keyframe {
    int start = 0;
    int duration = 1;
    std::vector<ElementPtr> elements;
    TweenType tween = TweenType::None;
    ClassicTweenSettings classic;
    ShapeTweenSettings shape;
    std::vector<ShapeHint> hints;
    std::string label;
    LabelType labelType = LabelType::Name;

    int end() const { return start + duration; }
    bool isEmpty() const { return elements.empty(); }
    bool contains(int frame) const { return frame >= start && frame < end(); }
};

struct Layer {
    uint32_t id = 0;
    std::string name = "Layer 1";
    LayerType type = LayerType::Normal;
    bool visible = true;
    bool locked = false;
    bool outline = false;
    Color color = Color(0x4f, 0x8c, 0xff);
    uint32_t parentId = 0; ///< mask / guide / folder that owns this layer, 0 = none
    bool expanded = true;  ///< folders
    BlendMode blend = BlendMode::Normal; ///< Vertexa extension (Krita-like)
    double opacity = 1.0;
    std::vector<Keyframe> keys; ///< sorted, contiguous from frame 0

    int length() const { return keys.empty() ? 0 : keys.back().end(); }
    int keyIndexAt(int frame) const;
    const Keyframe* keyAt(int frame) const;
    Keyframe* keyAt(int frame);
    bool isKeyStart(int frame) const;
    /// Ensures a valid key list (at least one blank keyframe, contiguous spans).
    void normalize();
};

struct Timeline {
    std::string name = "Scene 1";
    std::vector<Layer> layers;

    int frameCount() const;
    int layerIndex(uint32_t id) const;
    const Layer* layerById(uint32_t id) const;
    /// Nesting depth (folders / masks / guides).
    int depth(int index) const;
    /// True when the layer is (transitively) inside a collapsed folder.
    bool isCollapsed(int index) const;
    /// Guide layer that guides this layer (a motion guide), or nullptr.
    const Layer* guideOf(int index) const;
    /// Mask layer masking this layer, or nullptr.
    const Layer* maskOf(int index) const;
};

} // namespace vx
