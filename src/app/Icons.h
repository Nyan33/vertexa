// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — procedurally drawn line icons (24px grid, 1.8px strokes).
#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QPainter;
class QRectF;

namespace vx::ui {

/// Draw the named icon into `r` with colour `c`.
void paintIcon(QPainter& p, const QString& name, const QRectF& r, const QColor& c);
/// Icon with normal (text2), active/checked (accent) and disabled variants.
QIcon icon(const QString& name);
QPixmap iconPixmap(const QString& name, int size, const QColor& c, qreal dpr = 2.0);

} // namespace vx::ui
