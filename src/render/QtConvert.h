// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — conversions between the Qt-free core types and Qt types.
#pragma once

#include "core/Color.h"
#include "geom/Region.h"

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QTransform>

namespace vx {

inline QColor toQColor(const Color& c) { return QColor(c.r, c.g, c.b, c.a); }
inline Color fromQColor(const QColor& c) { return Color(uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), uint8_t(c.alpha())); }
inline QPointF toQPoint(Vec2 p) { return {p.x, p.y}; }
inline Vec2 fromQPoint(const QPointF& p) { return {p.x(), p.y()}; }
inline QTransform toQTransform(const Affine& m) { return QTransform(m.a, m.b, m.c, m.d, m.tx, m.ty); }
inline Affine fromQTransform(const QTransform& t) { return {t.m11(), t.m12(), t.m21(), t.m22(), t.dx(), t.dy()}; }
inline QRectF toQRect(const Rect& r) { return r.isEmpty() ? QRectF() : QRectF(r.x0, r.y0, r.width(), r.height()); }
inline Rect fromQRect(const QRectF& r) { return Rect(r.left(), r.top(), r.right(), r.bottom()); }

inline void appendChain(QPainterPath& path, const std::vector<Cubic>& chain, bool close)
{
    if (chain.empty()) return;
    path.moveTo(toQPoint(chain.front().p0));
    for (const Cubic& c : chain) {
        if (c.isStraight(1e-9)) path.lineTo(toQPoint(c.p3));
        else path.cubicTo(toQPoint(c.p1), toQPoint(c.p2), toQPoint(c.p3));
    }
    if (close) path.closeSubpath();
}

inline QPainterPath toQPath(const Region& r)
{
    QPainterPath p;
    p.setFillRule(Qt::WindingFill);
    for (const Contour& c : r.contours) appendChain(p, c, true);
    return p;
}

inline QPainterPath toQPath(const std::vector<Cubic>& chain, bool close = false)
{
    QPainterPath p;
    appendChain(p, chain, close);
    return p;
}

} // namespace vx
