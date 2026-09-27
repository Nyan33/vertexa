// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — freehand tools: Brush (fill based, like Animate's Brush), Eraser,
// Pencil (stroke based) and Paint Brush (Krita-like texture brushes).
#pragma once

#include "Tool.h"
#include "geom/Smooth.h"

#include <QImage>
#include <QPainterPath>

namespace vx::app {

/// Shared input handling for freehand tools: stabiliser + raw samples.
class FreehandTool : public Tool {
public:
    using Tool::Tool;
    bool busy() const override { return m_active; }
    void cancel() override;

protected:
    void beginStroke(const ToolEvent& e, double smoothing);
    /// Returns the stabilised samples produced by `e`.
    std::vector<InputSample> addSample(const ToolEvent& e);
    std::vector<InputSample> endStroke(double smoothing);
    static InputSample toSample(const ToolEvent& e);

    bool m_active = false;
    Stabilizer m_stab;
    std::vector<InputSample> m_points; ///< stabilised samples so far
    Vec2 m_hover;
    bool m_hasHover = false;
};

class BrushTool : public FreehandTool {
public:
    using FreehandTool::FreehandTool;
    ToolId id() const override { return ToolId::Brush; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void hover(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    QCursor cursor() const override;

private:
    std::vector<BrushPoint> brushPoints(const std::vector<InputSample>& pts) const;
    void rebuildPreview();
    QPainterPath m_preview; ///< timeline space
    std::optional<Region> m_insideMask;
    bool m_insideEmpty = false;
    int m_layer = -1;
};

class EraserTool : public FreehandTool {
public:
    using FreehandTool::FreehandTool;
    ToolId id() const override { return ToolId::Eraser; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void hover(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    QCursor cursor() const override;

private:
    std::vector<BrushPoint> brushPoints(const std::vector<InputSample>& pts) const;
    void faucet(const ToolEvent& e);
    QPainterPath m_preview;
    std::optional<Region> m_mask;
    int m_layer = -1;
};

class PencilTool : public FreehandTool {
public:
    using FreehandTool::FreehandTool;
    ToolId id() const override { return ToolId::Pencil; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    QCursor cursor() const override;

private:
    std::vector<Cubic> buildChain(std::vector<Vec2> pts, bool& closed) const;
    int m_layer = -1;
};

class PaintBrushTool : public FreehandTool {
public:
    using FreehandTool::FreehandTool;
    ToolId id() const override { return ToolId::PaintBrush; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void hover(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    void cancel() override;
    QCursor cursor() const override;

private:
    BrushPresetPtr currentPreset() const;
    void renderIncrement();
    std::vector<PaintSample> m_samples; ///< timeline space
    QImage m_overlay;                   ///< live preview (device pixels)
    size_t m_rendered = 0;
    BrushPresetPtr m_preset;
    int m_layer = -1;
    uint32_t m_seed = 1;
};

/// Commits a finished shape (merge drawing or drawing object) on a layer.
bool commitShape(Editor* ed, int layerIndex, const ShapeGraph& g, const QString& label,
                 const OverlayOptions& opt = {});

} // namespace vx::app
