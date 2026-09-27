// SPDX-License-Identifier: GPL-3.0-or-later
#include "Surface.h"
#include "Blend.h"
#include "Filters.h"
#include "Renderer.h"

namespace vx {

CpuSurface::CpuSurface(QSize size) : m_own(size, QImage::Format_ARGB32_Premultiplied), m_image(&m_own) { m_own.fill(0); }

void CpuSurface::drawShape(const std::shared_ptr<const ShapeRenderData>& rd, const Affine& m, const ColorTransform& ct,
                           const QRect& clip)
{
    if (rd) Renderer::renderShape(*m_image, *rd, m, ct, clip);
}

void CpuSurface::drawOutline(const ShapeRenderData& rd, const Affine& m, const QColor& color, const QRect& clip)
{
    Renderer::renderOutline(*m_image, rd, m, color, clip);
}

std::unique_ptr<Surface> CpuSurface::makeLayer(QSize size) { return std::make_unique<CpuSurface>(size); }

void CpuSurface::composite(Surface& layer, QPoint at, BlendMode mode, double opacity)
{
    compositeImage(*m_image, static_cast<CpuSurface&>(layer).image(), at, mode, opacity);
}

void CpuSurface::applyMask(Surface& mask) { vx::applyMask(*m_image, static_cast<CpuSurface&>(mask).image()); }

void CpuSurface::applyFilters(const FilterList& filters, double scale) { vx::applyFilters(*m_image, filters, scale); }

void CpuSurface::applyColorTransform(const ColorTransform& ct) { vx::applyColorTransform(*m_image, ct); }

} // namespace vx
