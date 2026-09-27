// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — shape tools (Line, Rectangle, Oval, PolyStar), Pen, fill tools
// (Paint Bucket, Ink Bottle, Eyedropper) and navigation (Hand, Zoom).
#pragma once

#include "Tool.h"

#include <QPainterPath>

namespace vx::app {

class ShapeDragTool : public Tool {
public:
    ShapeDragTool(Editor* e, StageView* v, ToolId id) : Tool(e, v), m_id(id) {}
    ToolId id() const override { return m_id; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    void cancel() override { m_active = false; }
    bool busy() const override { return m_active; }

private:
    /// Region (closed shapes) or open chain (line) for the current drag.
    bool geometry(Region& area, std::vector<Cubic>& openChain) const;
    ToolId m_id;
    bool m_active = false;
    Vec2 m_start, m_end;
    Qt::KeyboardModifiers m_mods;
    int m_layer = -1;
};

class PenTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Pen; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void hover(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;
    bool keyPress(QKeyEvent* e) override;
    void paint(QPainter& p) override;
    void cancel() override;
    void deactivate() override { finish(false); }
    bool busy() const override { return !m_nodes.empty(); }

private:
    struct Node {
        Vec2 p;
        Vec2 out; ///< outgoing handle (absolute); incoming is mirrored
        bool smooth = false;
    };
    void finish(bool close);
    std::vector<Cubic> chain(bool close) const;
    std::vector<Node> m_nodes;
    bool m_dragging = false;
    Vec2 m_hover;
    int m_layer = -1;
};

class PaintBucketTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::PaintBucket; }
    void press(const ToolEvent& e) override;
    void hover(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    QCursor cursor() const override;

private:
    QPainterPath m_previewPath;
    Vec2 m_lastHover{1e300, 1e300};
};

class InkBottleTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::InkBottle; }
    void press(const ToolEvent& e) override;
    QCursor cursor() const override;
};

class EyedropperTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Eyedropper; }
    void press(const ToolEvent& e) override;
    QCursor cursor() const override;
};

class HandTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Hand; }
    QCursor cursor() const override { return Qt::OpenHandCursor; }
};

class ZoomTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Zoom; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    QCursor cursor() const override;

private:
    bool m_drag = false;
    QPointF m_a, m_b;
    Qt::KeyboardModifiers m_mods;
};

} // namespace vx::app
