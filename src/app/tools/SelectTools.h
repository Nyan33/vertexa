// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — Selection (V), Subselection (A), Free Transform (Q) and Lasso (L).
// The Selection tool follows Animate: click a fill/line of a merged shape to
// select it, drag an unselected edge to bend it, drag a corner to move it,
// marquee cuts shapes, double-click edits symbols in place.
#pragma once

#include "Tool.h"

#include <QPainterPath>

namespace vx::app {

struct StageHit {
    enum class Kind { None, Element, Shape };
    Kind kind = Kind::None;
    int layer = -1;
    int index = -1;   ///< element index in the keyframe
    ShapeHit shape;   ///< merge shape hit (timeline space)
};

/// Topmost hit on the current frame (hidden/locked layers skipped).
StageHit hitStage(const Editor* ed, Vec2 pos, double tol);
bool hitElement(const Document& d, const Element& e, Vec2 p, double tol, int localFrame = 0, int depth = 0);
/// Bounds of an element in its own coordinate space.
Rect localBoundsOf(const Document& d, const Element& e, int localFrame = 0);

class SelectionTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Selection; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void hover(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    void cancel() override;
    QCursor cursor() const override;
    bool busy() const override { return m_mode != Mode::None; }

private:
    enum class Mode { None, Marquee, MoveElements, MoveShape, Bend, Vertex, Hint };
    enum class HoverKind { None, Move, Bend, Corner, Hint };
    struct HintRef {
        int keyStart = -1;
        int index = -1;
        bool end = false;
    };
    bool onPick(Vec2 p, int layer, const ShapeHit& h) const;
    void previewMove(Vec2 delta, bool copy);
    std::vector<std::pair<Vec2, HintRef>> visibleHints() const;
    Vec2 constrained(Vec2 delta, Qt::KeyboardModifiers mods) const;

    Mode m_mode = Mode::None;
    HoverKind m_hover = HoverKind::None;
    Vec2 m_start, m_cur;
    bool m_moved = false;
    int m_layer = -1;
    int m_element = -1;      ///< shape element index (merge shape: 0 or object)
    ShapeGraphPtr m_graph;   ///< graph under edit (element local space)
    Affine m_matrix;         ///< element -> timeline
    int m_edge = -1;
    double m_t = 0.0;
    Vec2 m_vertex;
    ShapeGraph m_rest, m_lifted;
    HintRef m_hint;
};

class SubselectTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Subselection; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    void cancel() override;
    void activate() override;
    bool busy() const override { return m_drag != Drag::None; }

private:
    enum class Drag { None, Vertex, Handle };
    bool pickTarget(Vec2 p);
    ShapeGraphPtr targetGraph() const;
    void commit(const ShapeGraph& g, const QString& label);
    void preview(const ShapeGraph& g);
    int m_layer = -1;
    int m_element = -1;
    Affine m_matrix;
    bool m_hasVertex = false;
    Vec2 m_vertex; ///< element space
    Drag m_drag = Drag::None;
    int m_edge = -1;
    int m_handle = 0;
    Vec2 m_dragPos;
    ShapeGraph m_work;
};

class FreeTransformTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::FreeTransform; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void hover(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    void cancel() override;
    QCursor cursor() const override;
    bool busy() const override { return m_handle != Handle::None; }

private:
    enum class Handle { None, Move, Pivot, Rotate, TL, T, TR, R, BR, B, BL, L, SkewT, SkewB, SkewL, SkewR };
    struct Box {
        bool valid = false;
        Affine toTimeline;
        Rect local;
        Vec2 pivot; ///< timeline space
    };
    Box computeBox() const;
    Handle handleAt(QPointF w, const Box& b) const;
    Affine currentTransform(Vec2 pos, Qt::KeyboardModifiers mods) const;
    void preview(const Affine& t);

    Handle m_handle = Handle::None;
    Handle m_hoverHandle = Handle::None;
    Box m_box;
    Vec2 m_start;
    Affine m_current;
    Vec2 m_pivotDrag;
};

class LassoTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Lasso; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    void cancel() override { m_points.clear(); }
    bool busy() const override { return !m_points.empty(); }

private:
    std::vector<Vec2> m_points;
};

} // namespace vx::app
