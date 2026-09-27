// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — dab based painting engine for texture brushes (after Krita's
// pixel brush engine): spacing along the stroke, sensor curves, jitter,
// canvas-anchored texture, flow/opacity "wash" accumulation.
#pragma once

#include "core/Element.h"

#include <QImage>

namespace vx {

struct Document;

struct DabContext {
    Affine toDevice;               ///< element space -> device pixels
    ColorTransform color;          ///< colour effect of the parents
    const Document* doc = nullptr; ///< for imported tips/textures
};

/// Paint one stroke into `buffer` (premultiplied ARGB32, device space).
/// Erase strokes remove paint from the buffer.
void paintStroke(QImage& buffer, const PaintStroke& stroke, const DabContext& ctx);

/// Paint a whole paint element into `buffer`.
void paintElement(QImage& buffer, const PaintElement& element, const DabContext& ctx);

/// Small preview of a brush preset (used by the brush panel).
QImage brushPreview(const BrushPreset& preset, const Color& color, int width, int height,
                    const Document* doc = nullptr);

} // namespace vx
