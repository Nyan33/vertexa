// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — freehand tools: Brush (fill based, like Animate's Brush), Eraser,
// Pencil (stroke based) and Paint Brush (vector art, pattern, textured and
// scatter brushes; the result is always vector fills).
#pragma once

#include "Tool.h"
#include "core/VectorBrush.h"
#include "geom/Smooth.h"

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QPainterPath>

#include <deque>
#include <functional>
#include <memory>

namespace vx::app {

/// Finished strokes whose shapes are built (and merged) on a worker thread,
/// then committed on the GUI thread in the order they were drawn: letting
/// go of the mouse never waits for the geometry.
class StrokeQueue {
public:
    struct Task {
        std::function<void()> start;  ///< GUI thread, when the task's turn comes
        std::function<void()> work;   ///< worker thread
        std::function<void()> commit; ///< GUI thread, afterwards
    };
    StrokeQueue() = default;
    StrokeQueue(const StrokeQueue&) = delete;
    StrokeQueue& operator=(const StrokeQueue&) = delete;
    ~StrokeQueue() { *m_alive = false; }
    void push(Task task);
    bool pending() const { return !m_tasks.empty(); }

private:
    void next();
    void done();
    std::deque<Task> m_tasks;
    bool m_running = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

/// Ink of the stroke being drawn, painted piece by piece into an image of
/// the view, so a new sample costs only its own piece. The colour's alpha
/// applies to the stroke as a whole (overlapping pieces don't darken).
/// Finished strokes stay shown until they are committed.
class Ink {
public:
    void begin(const StageView* view, const QColor& color);
    /// Paints a piece (timeline space); returns the widget rectangle it touched.
    QRectF fill(const QPainterPath& piece);
    QRectF line(Vec2 a, Vec2 b, double widthPx);
    /// The stroke ends: shown until dropOldest() (a committed stroke).
    void end();
    void dropOldest();
    void cancel();
    void paint(QPainter& p) const;
    bool drawing() const { return m_active.has_value(); }

private:
    struct Sheet {
        QImage image;
        qreal opacity = 1.0;
        Affine xf;  ///< timeline -> image pixels
        QRect used; ///< image pixels painted
    };
    QImage takeImage(QSize px, qreal dpr);
    void recycle(Sheet& s);
    const StageView* m_view = nullptr;
    QColor m_color;
    std::optional<Sheet> m_active;
    std::deque<Sheet> m_pending;
    std::vector<QImage> m_free;
};

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
    /// Moves the hover point; returns the widget area a cursor of
    /// `diameterPx` covered before and covers now.
    QRectF moveCursor(Vec2 pos, double diameterPx);

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
    void cancel() override;
    QCursor cursor() const override;
    bool hasPendingWork() const override { return m_queue.pending(); }

private:
    std::vector<BrushPoint> brushPoints(const std::vector<InputSample>& pts) const;
    /// Inks the samples from `from` on; returns the widget area touched.
    QRectF inkFrom(size_t from);
    Ink m_ink;
    StrokeQueue m_queue;
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
    void cancel() override;
    QCursor cursor() const override;
    bool hasPendingWork() const override { return m_queue.pending(); }

private:
    std::vector<BrushPoint> brushPoints(const std::vector<InputSample>& pts) const;
    QRectF inkFrom(size_t from);
    void faucet(const ToolEvent& e);
    Ink m_ink;
    StrokeQueue m_queue;
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
    void cancel() override;
    QCursor cursor() const override;
    bool hasPendingWork() const override { return m_queue.pending(); }

private:
    QRectF inkFrom(size_t from);
    Ink m_ink;
    StrokeQueue m_queue;
    int m_layer = -1;
};

class PaintBrushTool : public FreehandTool {
public:
    using FreehandTool::FreehandTool;
    ~PaintBrushTool() override;
    ToolId id() const override { return ToolId::PaintBrush; }
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void hover(const ToolEvent& e) override;
    void paint(QPainter& p) override;
    void cancel() override;
    QCursor cursor() const override;
    bool hasPendingWork() const override { return !m_jobs.empty() || m_previewBusy; }

private:
    struct PreviewPiece {
        QPainterPath path; ///< timeline space
        QColor color;
    };
    /// A finished stroke being turned into vector fills and merged on a
    /// worker thread; committed on the GUI thread in order.
    struct Job;
    struct PreviewResult;

    /// Builds the exact preview of the newer samples on a worker thread (one
    /// request at a time); until it arrives they show as a quick outline.
    void requestPreview(bool force);
    void finishPreview(uint64_t generation, PreviewResult& result);
    void rebuildTail();
    FillStyle paintStyle() const;
    Affine overlayTransform() const;
    void startNextJob();
    void finishJob();

    VectorBrushPreset m_preset;
    bool m_chunked = false;             ///< textured brushes preview in chunks
    std::vector<PreviewPiece> m_pieces; ///< exact vector preview (whole stroke)
    QImage m_overlay;                   ///< chunked preview and strokes waiting to be merged (device pixels)
    Affine m_overlayXf;                 ///< timeline -> overlay pixels when it was drawn
    size_t m_covered = 0;               ///< samples the exact preview covers
    QPainterPath m_tail;                ///< quick outline of the newer samples
    QElapsedTimer m_clock;
    qint64 m_lastBuild = 0;
    bool m_previewBusy = false;
    uint64_t m_previewGen = 0; ///< bumped when pending previews become stale
    std::optional<Region> m_insideMask;
    bool m_insideEmpty = false;
    int m_layer = -1;
    uint32_t m_seed = 1;
    std::deque<std::shared_ptr<Job>> m_jobs;
    bool m_running = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

/// Commits a finished shape (merge drawing or drawing object) on a layer.
bool commitShape(Editor* ed, int layerIndex, const ShapeGraph& g, const QString& label,
                 const OverlayOptions& opt = {});

/// Erases `r` (timeline space) from the shapes of a layer's current keyframe.
bool eraseArea(Editor* ed, int layerIndex, const Region& r, EraseMode mode, const Region* mask, const QString& label);

} // namespace vx::app
