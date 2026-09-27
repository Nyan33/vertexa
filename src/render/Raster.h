// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — exact-coverage scanline rasteriser for shape fills.
//
// All fill styles of a shape are rasterised in a single pass using signed
// area accumulation (the approach of font-rs / stb_truetype 2), and pixels
// are composited as a coverage-weighted sum of the styles. Two fills that
// share an edge therefore add up to full coverage along that edge — there is
// no "conflation" seam, which is exactly how the Flash player renders shapes.
#pragma once

#include "core/Style.h"
#include "geom/Region.h"

#include <QImage>
#include <QRect>

#include <vector>

namespace vx {

struct RasterFill {
    const std::vector<Contour>* contours = nullptr; ///< shape space
    FillStyle style;
};

/// Rasterise fills (shape space) through `toDevice` onto a premultiplied
/// ARGB32 image, clipped to `clip` (device pixels).
void rasterizeFills(QImage& target, const std::vector<RasterFill>& fills, const Affine& toDevice, const QRect& clip);

/// Flatten a contour into a polyline (device space) with the given tolerance.
void flattenContour(const Contour& c, const Affine& m, double tolerance, std::vector<Vec2>& out);

/// Premultiplied ARGB for a (non-premultiplied) colour.
inline uint32_t premultiply(const Color& c)
{
    const uint32_t a = c.a;
    auto mul = [a](uint32_t v) { return (v * a + 127) / 255; };
    return (a << 24) | (mul(c.r) << 16) | (mul(c.g) << 8) | mul(c.b);
}

} // namespace vx
