// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — drawing targets of the Renderer. The renderer walks the document
// (layers, masks, symbols, tweens) and asks a Surface to draw shapes and to
// combine isolated layers; the Surface decides where the pixels are made:
// on the CPU (CpuSurface, the exact-coverage scanline rasteriser) or on the
// GPU (GlSurface, see GlRenderer.h).
#pragma once

#include "core/BlendMode.h"
#include "core/Color.h"
#include "core/Filter.h"
#include "core/ShapeGraph.h"

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>

#include <memory>

namespace vx {

class Surface {
public:
    virtual ~Surface() = default;
    virtual QSize size() const = 0;
    QRect rect() const { return QRect(QPoint(0, 0), size()); }

    /// Fills and strokes of a shape through `m` (shape -> device pixels).
    /// The render data is shared so that a surface may keep work done for it
    /// (GPU geometry) for as long as it lives.
    virtual void drawShape(const std::shared_ptr<const ShapeRenderData>& rd, const Affine& m, const ColorTransform& ct,
                           const QRect& clip) = 0;
    /// Hairline outlines of a shape (outline mode, onion skin outlines).
    virtual void drawOutline(const ShapeRenderData& rd, const Affine& m, const QColor& color, const QRect& clip) = 0;

    /// A new transparent surface of the same kind, for isolated drawing.
    virtual std::unique_ptr<Surface> makeLayer(QSize size) = 0;
    /// A surface for a filtered instance: filters run on the CPU, so a GPU
    /// surface hands out a CPU one (and composites it by uploading it).
    virtual std::unique_ptr<Surface> makeFilterLayer(QSize size) { return makeLayer(size); }
    /// Composites `layer` (made by makeLayer) at `at` with a blend mode.
    virtual void composite(Surface& layer, QPoint at, BlendMode mode, double opacity) = 0;
    /// Multiplies this surface by the alpha of `mask` (same size).
    virtual void applyMask(Surface& mask) = 0;
    /// Movie clip filters, `scale` converting their pixel values to device pixels.
    virtual void applyFilters(const FilterList& filters, double scale) = 0;
    virtual void applyColorTransform(const ColorTransform& ct) = 0;
};

/// A premultiplied ARGB32 image drawn on the CPU.
class CpuSurface final : public Surface {
public:
    explicit CpuSurface(QImage& image) : m_image(&image) {}
    explicit CpuSurface(QSize size);

    QImage& image() { return *m_image; }
    QSize size() const override { return m_image->size(); }
    void drawShape(const std::shared_ptr<const ShapeRenderData>& rd, const Affine& m, const ColorTransform& ct,
                   const QRect& clip) override;
    void drawOutline(const ShapeRenderData& rd, const Affine& m, const QColor& color, const QRect& clip) override;
    std::unique_ptr<Surface> makeLayer(QSize size) override;
    void composite(Surface& layer, QPoint at, BlendMode mode, double opacity) override;
    void applyMask(Surface& mask) override;
    void applyFilters(const FilterList& filters, double scale) override;
    void applyColorTransform(const ColorTransform& ct) override;

private:
    QImage m_own;
    QImage* m_image;
};

} // namespace vx
