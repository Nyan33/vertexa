// SPDX-License-Identifier: GPL-3.0-or-later
#include "GpuGeometry.h"
#include "QtConvert.h"
#include "Raster.h"

#include <QPainterPath>
#include <QPainterPathStroker>

#include <algorithm>
#include <cmath>

namespace vx::gpu {

QRect deviceRect(const Rect& r)
{
    if (r.isEmpty()) return {};
    return QRect(QPoint(int(std::floor(r.x0)) - 1, int(std::floor(r.y0)) - 1),
                 QPoint(int(std::ceil(r.x1)) + 1, int(std::ceil(r.y1)) + 1));
}

double maxScale(const Affine& m) { return std::max({std::hypot(m.a, m.b), std::hypot(m.c, m.d), 1e-9}); }

void appendFan(const std::vector<Vec2>& poly, std::vector<float>& tri, Rect& bounds)
{
    const size_t n = poly.size();
    if (n < 3) return;
    auto push = [&](Vec2 a, Vec2 b, Vec2 c) {
        for (Vec2 p : {a, b, c}) {
            tri.push_back(float(p.x));
            tri.push_back(float(p.y));
        }
    };
    auto at = [&](size_t i) { return poly[i % n]; };
    constexpr size_t kChunk = 24;
    if (n <= 2 * kChunk) {
        for (size_t i = 1; i + 1 < n; ++i) push(poly[0], poly[i], poly[i + 1]);
    } else {
        std::vector<Vec2> skeleton;
        for (size_t s = 0; s < n; s += kChunk) {
            skeleton.push_back(poly[s]);
            const size_t e = std::min(s + kChunk, n); // the last chunk closes on poly[0]
            for (size_t i = s + 1; i + 1 <= e; ++i) push(poly[s], at(i), at(i + 1));
        }
        for (size_t i = 1; i + 1 < skeleton.size(); ++i) push(skeleton[0], skeleton[i], skeleton[i + 1]);
    }
    for (const Vec2& p : poly) bounds.include(p);
}

void appendQPolygons(const QList<QPolygonF>& polys, std::vector<float>& tri, Rect& bounds)
{
    std::vector<Vec2> pts;
    for (const QPolygonF& poly : polys) {
        pts.clear();
        for (const QPointF& p : poly) pts.push_back({p.x(), p.y()});
        appendFan(pts, tri, bounds);
    }
}

void setupStroker(QPainterPathStroker& st, const StrokeStyle& s, double& width, bool& cosmetic)
{
    cosmetic = s.pattern == StrokePattern::Hairline || !s.scaleWithTransform;
    width = s.pattern == StrokePattern::Hairline ? 1.0 : s.width;
    st.setWidth(std::max(0.01, width));
    st.setCapStyle(s.cap == CapStyle::Round ? Qt::RoundCap : s.cap == CapStyle::Square ? Qt::SquareCap : Qt::FlatCap);
    st.setJoinStyle(s.join == JoinStyle::Round ? Qt::RoundJoin : s.join == JoinStyle::Miter ? Qt::MiterJoin : Qt::BevelJoin);
    st.setMiterLimit(s.miterLimit);
    const double w = std::max(0.1, width);
    if (s.pattern == StrokePattern::Dashed) {
        st.setDashPattern(QList<qreal>{std::max(0.1, s.dash / w), std::max(0.1, s.gap / w)});
    } else if (s.pattern == StrokePattern::Dotted) {
        st.setCapStyle(Qt::RoundCap);
        st.setDashPattern(QList<qreal>{0.01, std::max(0.5, (s.gap + s.width) / w)});
    }
}

bool isSoftwareDevice(const QString& name)
{
    static const char* kSoftware[] = {"llvmpipe", "lavapipe", "softpipe", "swrast", "software rasterizer", "swiftshader",
                                      "basic render", "microsoft basic", "warp", "gdi generic", "software renderer"};
    const QString n = name.toLower();
    for (const char* s : kSoftware)
        if (n.contains(QLatin1String(s))) return true;
    return false;
}

Paint::Paint(const FillStyle& f, const Affine& toDevice)
{
    if (f.kind == FillStyle::Kind::Solid) {
        const float a = f.color.a / 255.0f;
        color[0] = f.color.r / 255.0f * a;
        color[1] = f.color.g / 255.0f * a;
        color[2] = f.color.b / 255.0f * a;
        color[3] = a;
        return;
    }
    kind = f.kind == FillStyle::Kind::Linear ? 1 : 2;
    toGradient = (toDevice * f.gradient.matrix).inverted();
    focal = float(std::clamp(f.gradient.focal, -0.98, 0.98));
    spread = int(f.gradient.spread);
    for (int i = 0; i < 256; ++i) {
        const Color c = f.gradient.colorAt(i / 255.0);
        const float a = c.a / 255.0f;
        lut[size_t(i) * 4 + 0] = uint8_t(std::lround(c.r * a));
        lut[size_t(i) * 4 + 1] = uint8_t(std::lround(c.g * a));
        lut[size_t(i) * 4 + 2] = uint8_t(std::lround(c.b * a));
        lut[size_t(i) * 4 + 3] = c.a;
    }
}

int scaleBucket(double scale) { return int(std::floor(std::log2(scale) * 2.0)); }

FlatShape flattenShape(const ShapeRenderData& rd, int bucket)
{
    const double bucketScale = std::exp2((bucket + 1) / 2.0);
    const double tol = 0.2 / bucketScale;
    FlatShape out;
    std::vector<Vec2> pts;
    for (const auto& fp : rd.fills) {
        FlatShape::Part part;
        for (const Contour& c : fp.contours) {
            pts.clear();
            flattenContour(c, Affine{}, tol, pts);
            appendFan(pts, part.tri, part.bounds);
        }
        out.fills.push_back(std::move(part));
    }
    for (const auto& sp : rd.strokes) {
        FlatShape::Part part;
        double width = 1.0;
        QPainterPathStroker st;
        setupStroker(st, sp.style, width, part.cosmetic);
        if (!part.cosmetic) {
            QPainterPath path;
            for (size_t i = 0; i < sp.chains.size(); ++i) appendChain(path, sp.chains[i], sp.closed[i]);
            // Flatten at the bucket's scale, then back to shape space.
            QList<QPolygonF> polys = st.createStroke(path).toSubpathPolygons(QTransform::fromScale(bucketScale, bucketScale));
            for (QPolygonF& poly : polys)
                for (QPointF& p : poly) p /= bucketScale;
            appendQPolygons(polys, part.tri, part.bounds);
        }
        out.strokes.push_back(std::move(part));
    }
    return out;
}

void strokeInDeviceSpace(const ShapeRenderData::StrokePath& sp, const Affine& m, std::vector<float>& tri, Rect& bounds)
{
    QPainterPath path;
    for (size_t k = 0; k < sp.chains.size(); ++k) appendChain(path, sp.chains[k], sp.closed[k]);
    double width = 1.0;
    bool cosmetic = false;
    QPainterPathStroker st;
    setupStroker(st, sp.style, width, cosmetic);
    appendQPolygons(st.createStroke(toQTransform(m).map(path)).toSubpathPolygons(), tri, bounds);
}

void outlineInDeviceSpace(const ShapeRenderData& rd, const Affine& m, std::vector<float>& tri, Rect& bounds)
{
    QPainterPath path;
    for (const auto& fp : rd.fills)
        for (const Contour& c : fp.contours) appendChain(path, c, true);
    for (const auto& sp : rd.strokes)
        for (size_t i = 0; i < sp.chains.size(); ++i) appendChain(path, sp.chains[i], sp.closed[i]);
    QPainterPathStroker st;
    st.setWidth(1.0);
    appendQPolygons(st.createStroke(toQTransform(m).map(path)).toSubpathPolygons(), tri, bounds);
}

} // namespace vx::gpu
