// SPDX-License-Identifier: GPL-3.0-or-later
#include "Renderer.h"
#include "Blend.h"
#include "DabEngine.h"
#include "QtConvert.h"
#include "Raster.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>

namespace vx {

namespace {

// --- texture paint cache ------------------------------------------------------------
struct PaintCacheEntry {
    std::weak_ptr<const Element> owner;
    const Element* key = nullptr;
    Affine m;
    ColorTransform ct;
    QImage img;
    QPoint origin;
};

std::mutex g_cacheMutex;
std::list<PaintCacheEntry> g_paintCache;
constexpr size_t kPaintCacheSize = 64;

QPen penFor(const StrokeStyle& s, const ColorTransform& ct)
{
    const FillStyle paint = ct.isIdentity() ? s.paint : s.paint.withColorTransform(ct);
    QBrush brush;
    if (paint.kind == FillStyle::Kind::Solid) {
        brush = QBrush(toQColor(paint.color));
    } else {
        QGradientStops stops;
        for (const GradientStop& st : paint.gradient.stops) stops.push_back({st.pos, toQColor(st.color)});
        QGradient* g;
        QLinearGradient lin(-1, 0, 1, 0);
        QRadialGradient rad(QPointF(0, 0), 1.0, QPointF(paint.gradient.focal, 0));
        g = paint.kind == FillStyle::Kind::Linear ? static_cast<QGradient*>(&lin) : static_cast<QGradient*>(&rad);
        g->setStops(stops);
        g->setSpread(paint.gradient.spread == SpreadMode::Pad       ? QGradient::PadSpread
                     : paint.gradient.spread == SpreadMode::Reflect ? QGradient::ReflectSpread
                                                                    : QGradient::RepeatSpread);
        brush = QBrush(*g);
        brush.setTransform(toQTransform(paint.gradient.matrix));
    }
    QPen pen(brush, s.width);
    pen.setCapStyle(s.cap == CapStyle::Round ? Qt::RoundCap : s.cap == CapStyle::Square ? Qt::SquareCap : Qt::FlatCap);
    pen.setJoinStyle(s.join == JoinStyle::Round ? Qt::RoundJoin : s.join == JoinStyle::Miter ? Qt::MiterJoin : Qt::BevelJoin);
    pen.setMiterLimit(s.miterLimit);
    if (s.pattern == StrokePattern::Hairline) {
        pen.setWidthF(1.0);
        pen.setCosmetic(true);
    } else if (!s.scaleWithTransform) {
        pen.setCosmetic(true);
    }
    const double w = std::max(0.1, pen.widthF());
    if (s.pattern == StrokePattern::Dashed) pen.setDashPattern({std::max(0.1, s.dash / w), std::max(0.1, s.gap / w)});
    else if (s.pattern == StrokePattern::Dotted) {
        pen.setCapStyle(Qt::RoundCap);
        pen.setDashPattern({0.01, std::max(0.5, (s.gap + s.width) / w)});
    }
    return pen;
}

QRect deviceRect(const Rect& r)
{
    if (r.isEmpty()) return {};
    return QRect(QPoint(int(std::floor(r.x0)) - 1, int(std::floor(r.y0)) - 1),
                 QPoint(int(std::ceil(r.x1)) + 1, int(std::ceil(r.y1)) + 1));
}

} // namespace

Renderer::Renderer(const Document& doc, RenderOptions opts) : m_doc(doc), m_opts(std::move(opts)) {}

void Renderer::clearCache()
{
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_paintCache.clear();
}

void Renderer::renderShape(QImage& target, const ShapeRenderData& rd, const Affine& m, const ColorTransform& ct,
                           const QRect& clip)
{
    if (rd.isEmpty()) return;
    if (!rd.bounds.isEmpty() && !deviceRect(m.mapRect(rd.bounds)).intersects(clip)) return;
    if (!rd.fills.empty()) {
        std::vector<RasterFill> fills;
        fills.reserve(rd.fills.size());
        for (const auto& fp : rd.fills)
            fills.push_back({&fp.contours, ct.isIdentity() ? fp.style : fp.style.withColorTransform(ct)});
        rasterizeFills(target, fills, m, clip);
    }
    if (!rd.strokes.empty()) {
        QPainter p(&target);
        p.setRenderHint(QPainter::Antialiasing);
        p.setClipRect(clip);
        p.setTransform(toQTransform(m));
        p.setBrush(Qt::NoBrush);
        for (const auto& sp : rd.strokes) {
            p.setPen(penFor(sp.style, ct));
            QPainterPath path;
            for (size_t i = 0; i < sp.chains.size(); ++i) appendChain(path, sp.chains[i], sp.closed[i]);
            p.drawPath(path);
        }
    }
}

void Renderer::renderOutline(QImage& target, const ShapeRenderData& rd, const Affine& m, const QColor& color,
                             const QRect& clip)
{
    QPainter p(&target);
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipRect(clip);
    QPen pen(color, 1.0);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.setTransform(toQTransform(m));
    QPainterPath path;
    for (const auto& fp : rd.fills)
        for (const Contour& c : fp.contours) appendChain(path, c, true);
    for (const auto& sp : rd.strokes)
        for (size_t i = 0; i < sp.chains.size(); ++i) appendChain(path, sp.chains[i], sp.closed[i]);
    p.drawPath(path);
}

bool Renderer::layerHidden(const Timeline& tl, int index) const
{
    if (!m_opts.skipHidden) return false;
    if (!tl.layers[index].visible) return true;
    uint32_t p = tl.layers[index].parentId;
    int guard = 0;
    while (p && guard++ < 32) {
        const Layer* l = tl.layerById(p);
        if (!l) break;
        if (l->type == LayerType::Folder && !l->visible) return true;
        p = l->parentId;
    }
    return false;
}

void Renderer::render(QImage& target, const Timeline& tl, int frame, const Affine& view, const ColorTransform& ct)
{
    Ctx c;
    c.m = view;
    c.ct = ct;
    c.clip = target.rect();
    renderTimeline(target, tl, frame, c);
}

void Renderer::renderItems(QImage& target, const std::vector<EvalItem>& items, const Affine& view, const ColorTransform& ct)
{
    Ctx c;
    c.m = view;
    c.ct = ct;
    c.clip = target.rect();
    c.focus = m_opts.focusPath.size();
    renderList(target, items, c);
}

void Renderer::renderTimeline(QImage& target, const Timeline& tl, int frame, const Ctx& c)
{
    if (c.depth > 32) return;
    const int n = int(tl.layers.size());
    for (int i = n - 1; i >= 0; --i) {
        const Layer& l = tl.layers[i];
        if (l.type == LayerType::Folder) continue;
        if (layerHidden(tl, i)) continue;
        if (l.type == LayerType::Guide && (!m_opts.showGuides || c.depth > 0)) continue;

        // Masked layers are drawn together with their (active) mask.
        if (const Layer* mask = tl.maskOf(i)) {
            const bool active = !m_opts.masksNeedLock || mask->locked;
            if (active && mask->visible) continue;
        }
        if (l.type == LayerType::Mask) {
            const bool active = !m_opts.masksNeedLock || l.locked;
            if (active) {
                QImage content(target.size(), QImage::Format_ARGB32_Premultiplied);
                content.fill(0);
                Ctx cc = c;
                for (int j = n - 1; j >= 0; --j) {
                    if (tl.layers[j].parentId != l.id || layerHidden(tl, j)) continue;
                    renderLayerItems(content, tl, j, frame, cc);
                }
                QImage mask(target.size(), QImage::Format_ARGB32_Premultiplied);
                mask.fill(0);
                Ctx mc = c;
                mc.ct = ColorTransform{}; // mask shape: only coverage matters
                renderLayerItems(mask, tl, i, frame, mc);
                applyMask(content, mask);
                compositeImage(target, content, QPoint(0, 0), BlendMode::Normal, 1.0);
                continue;
            }
        }
        renderLayerItems(target, tl, i, frame, c);
    }
}

void Renderer::renderLayerItems(QImage& target, const Timeline& tl, int layerIndex, int frame, const Ctx& c)
{
    const Layer& l = tl.layers[layerIndex];
    const std::vector<EvalItem> items = evaluateLayer(m_doc, tl, layerIndex, frame);
    if (items.empty()) return;
    Ctx lc = c;
    if (m_opts.forceOutline) {
        lc.outline = true;
        lc.outlineColor = m_opts.outlineColor;
    } else if (m_opts.outlineLayers && l.outline) {
        lc.outline = true;
        lc.outlineColor = toQColor(l.color);
    }
    // Edit in place: which item (if any) of this layer lies on the focus path.
    int focusItem = -1;
    if (c.focus < m_opts.focusPath.size() && m_opts.focusPath[c.focus].first == l.id)
        focusItem = m_opts.focusPath[c.focus].second;
    auto draw = [&](QImage& dst, const Ctx& ctx) {
        for (int i = 0; i < int(items.size()); ++i) renderElement(dst, items[i], ctx, i == focusItem);
    };
    if ((l.blend != BlendMode::Normal || l.opacity < 1.0) && !lc.outline) {
        QImage buf(target.size(), QImage::Format_ARGB32_Premultiplied);
        buf.fill(0);
        lc.isolated = true;
        draw(buf, lc);
        compositeImage(target, buf, QPoint(0, 0), l.blend, l.opacity);
        return;
    }
    draw(target, lc);
}

void Renderer::renderList(QImage& target, const std::vector<EvalItem>& items, const Ctx& c)
{
    for (const EvalItem& it : items) renderElement(target, it, c, false);
}

void Renderer::renderElement(QImage& target, const EvalItem& item, const Ctx& c, bool onPath)
{
    const Element& e = *item.element;
    Ctx ec = c;
    // Edit in place: descend along the focus path, skip the edited instance.
    if (onPath) {
        if (c.focus + 1 >= m_opts.focusPath.size()) return;
        ec.focus = c.focus + 1;
    } else {
        ec.focus = m_opts.focusPath.size();
    }
    const Affine m = c.m * e.matrix;
    switch (e.type()) {
    case ElementType::Shape: {
        const auto& s = static_cast<const ShapeElement&>(e);
        if (!s.graph) return;
        if (c.outline) renderOutline(target, s.graph->renderData(), m, c.outlineColor, c.clip);
        else renderShape(target, s.graph->renderData(), m, c.ct, c.clip);
        return;
    }
    case ElementType::Morph: {
        const auto& mo = static_cast<const MorphElement&>(e);
        if (!mo.data) return;
        if (c.outline) renderOutline(target, *mo.data, m, c.outlineColor, c.clip);
        else renderShape(target, *mo.data, m, c.ct, c.clip);
        return;
    }
    case ElementType::Group: {
        const auto& g = static_cast<const GroupElement&>(e);
        Ctx gc = ec;
        gc.m = m;
        for (const ElementPtr& ch : g.children) renderElement(target, {ch, item.localFrame}, gc, false);
        return;
    }
    case ElementType::Paint: {
        Ctx pc = ec;
        pc.m = m;
        renderPaint(target, item.element, pc);
        return;
    }
    case ElementType::Instance: {
        const auto& in = static_cast<const InstanceElement&>(e);
        if (!in.visible) return;
        const Symbol* sym = m_doc.symbol(in.symbolId);
        if (!sym || c.depth > 32) return;
        const int frame = instanceSymbolFrame(m_doc, in, item.localFrame, m_opts.clipFrame);
        Ctx ic = ec;
        ic.m = m;
        ic.ct = c.ct * in.color.toTransform();
        ic.depth = c.depth + 1;
        const bool blends = in.blend != BlendMode::Normal && sym->type != SymbolType::Graphic && !c.outline;
        if (!blends) {
            renderTimeline(target, sym->timeline, frame, ic);
            return;
        }
        if ((in.blend == BlendMode::Alpha || in.blend == BlendMode::Erase) && !c.isolated) return;
        QImage buf(target.size(), QImage::Format_ARGB32_Premultiplied);
        buf.fill(0);
        ic.isolated = true;
        renderTimeline(buf, sym->timeline, frame, ic);
        compositeImage(target, buf, QPoint(0, 0), in.blend, 1.0);
        return;
    }
    }
}

void Renderer::renderPaint(QImage& target, const ElementPtr& owner, const Ctx& c)
{
    const auto& p = static_cast<const PaintElement&>(*owner);
    if (c.outline) {
        QPainter qp(&target);
        qp.setRenderHint(QPainter::Antialiasing);
        qp.setPen(QPen(c.outlineColor, 1.0));
        qp.setTransform(toQTransform(c.m));
        for (const PaintStroke& s : p.strokes)
            for (size_t i = 1; i < s.samples.size(); ++i) qp.drawLine(toQPoint(s.samples[i - 1].pos), toQPoint(s.samples[i].pos));
        return;
    }
    const Rect local = p.localBounds();
    if (local.isEmpty()) return;
    // Split the device transform into an integer offset and a fractional part
    // so panning reuses cached rasterisations.
    const QPoint off(int(std::floor(c.m.tx)), int(std::floor(c.m.ty)));
    Affine frac = c.m;
    frac.tx -= off.x();
    frac.ty -= off.y();
    QRect area = deviceRect(frac.mapRect(local));
    const QRect visible = c.clip.translated(-off);
    const bool huge = double(area.width()) * area.height() > 12e6;
    if (huge) area = area.intersected(visible);
    if (area.isEmpty() || !area.intersects(visible)) return;

    QImage img;
    if (!huge) {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        for (auto it = g_paintCache.begin(); it != g_paintCache.end(); ++it) {
            if (it->key == &p && !it->owner.expired() && it->m == frac && it->ct == c.ct) {
                img = it->img;
                g_paintCache.splice(g_paintCache.begin(), g_paintCache, it);
                break;
            }
        }
    }
    if (img.isNull()) {
        img = QImage(area.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(0);
        DabContext dc;
        dc.toDevice = Affine::translate(-area.left(), -area.top()) * frac;
        dc.color = c.ct;
        dc.doc = &m_doc;
        paintElement(img, p, dc);
        if (!huge) {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            PaintCacheEntry entry;
            entry.key = &p;
            entry.owner = owner;
            entry.m = frac;
            entry.ct = c.ct;
            entry.img = img;
            g_paintCache.push_front(std::move(entry));
            while (g_paintCache.size() > kPaintCacheSize) g_paintCache.pop_back();
        }
    }
    compositeImage(target, img, area.topLeft() + off, BlendMode::Normal, 1.0);
}

QImage Renderer::renderFrame(const Document& doc, const Timeline& tl, int frame, double scale, bool transparent,
                             RenderOptions opts)
{
    const int w = std::max(1, int(std::lround(doc.width * scale)));
    const int h = std::max(1, int(std::lround(doc.height * scale)));
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(transparent ? QColor(0, 0, 0, 0) : toQColor(doc.background));
    Renderer r(doc, std::move(opts));
    r.render(img, tl, frame, Affine::scale(scale));
    return img;
}

} // namespace vx
