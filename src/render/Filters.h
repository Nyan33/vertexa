// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — pixel filters of movie clip instances (Animate's Filters panel).
#pragma once

#include "core/Color.h"
#include "core/Filter.h"

#include <QImage>

namespace vx {

/// Applies `filters` in order to `img` (premultiplied ARGB32, device pixels).
/// `scale` converts the filters' pixel values (blur, distance) to device pixels.
void applyFilters(QImage& img, const FilterList& filters, double scale);

/// Applies a colour transform to every pixel of a premultiplied image.
void applyColorTransform(QImage& img, const ColorTransform& ct);

/// Filters with visible effect (enabled and non-trivial).
bool hasActiveFilters(const FilterList& filters);

} // namespace vx
