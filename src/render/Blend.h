// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — layer compositing with blend modes (Animate + Krita sets).
#pragma once

#include "core/BlendMode.h"

#include <QImage>
#include <QPoint>

namespace vx {

/// Composite `src` onto `dst` at `offset` (both premultiplied ARGB32).
/// `mask` (optional, same size as src, alpha channel used) multiplies the
/// source coverage — used for mask layers.
void compositeImage(QImage& dst, const QImage& src, QPoint offset, BlendMode mode, double opacity,
                    const QImage* mask = nullptr);

/// Multiply the alpha of `img` by the alpha of `mask` (DestinationIn).
void applyMask(QImage& img, const QImage& mask);

/// Blend of two unpremultiplied channels (0..1), separable modes only.
double blendChannel(BlendMode mode, double cb, double cs);

} // namespace vx
