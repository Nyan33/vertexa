// SPDX-License-Identifier: GPL-3.0-or-later
#include "Renderer.h"
#include "core/ShapeTween.h"
#include "Blend.h"
#include "Filters.h"
#include "QtConvert.h"
#include "Raster.h"

#include <QCoreApplication>
#include <QPainter>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace vx {

namespace {

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

namespace {

int renderThreads() { return std::clamp(QThread::idealThreadCount(), 1, 16); }

QThreadPool& renderPool()
{
    static QThreadPool pool;
    static const bool init = [] {
        pool.setMaxThreadCount(renderThreads());
        pool.setExpiryTimeout(30000);
        return true;
    }();
    (void)init;
    return pool;
}

} // namespace

Renderer::Renderer(const Document& doc, RenderOptions opts) : m_doc(doc), m_opts(std::move(opts)) {}

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
    // Large frames are drawn in horizontal bands on all cores. Each band is
    // an image over the target's own rows, so nothing drawn for one band
    // (a filter margin, an isolated layer) can reach another; every band
    // walks the document itself and skips what lies outside it.
    const int h = target.height();
    const qint64 pixels = qint64(target.width()) * h;
    int bands = std::min<qint64>(renderThreads(), pixels / (192 * 1024));
    bands = std::min(bands, h / 32);
    const QCoreApplication* app = QCoreApplication::instance();
    if (bands <= 1 || !app || QThread::currentThread() != app->thread()) {
        CpuSurface surface(target);
        render(surface, tl, frame, view, ct);
        return;
    }
    uchar* bits = target.bits(); // detaches before the bands share the rows
    const qsizetype bpl = target.bytesPerLine();
    auto band = [&](int b) {
        const int y0 = int(qint64(h) * b / bands), y1 = int(qint64(h) * (b + 1) / bands);
        QImage rows(bits + y0 * bpl, target.width(), y1 - y0, bpl, target.format());
        CpuSurface surface(rows);
        Renderer(m_doc, m_opts).render(surface, tl, frame, Affine::translate(0, -y0) * view, ct);
    };
    QSemaphore done;
    for (int b = 1; b < bands; ++b)
        renderPool().start([&band, &done, b]() {
            band(b);
            done.release();
        });
    band(0);
    done.acquire(bands - 1);
}

void Renderer::render(Surface& target, const Timeline& tl, int frame, const Affine& view, const ColorTransform& ct)
{
    Ctx c;
    c.m = view;
    c.ct = ct;
    c.clip = target.rect();
    renderTimeline(target, tl, frame, c);
}

void Renderer::renderItems(QImage& target, const std::vector<EvalItem>& items, const Affine& view, const ColorTransform& ct)
{
    CpuSurface surface(target);
    Ctx c;
    c.m = view;
    c.ct = ct;
    c.clip = target.rect();
    c.focus = m_opts.focusPath.size();
    renderList(surface, items, c);
}

Renderer::Unit Renderer::unitOf(const Timeline& tl, int i, int depth) const
{
    const Layer& l = tl.layers[i];
    if (l.type == LayerType::Folder) return Unit::None;
    if (layerHidden(tl, i)) return Unit::None;
    if (l.type == LayerType::Guide && (!m_opts.showGuides || depth > 0)) return Unit::None;
    // Masked layers are drawn together with their (active) mask.
    if (const Layer* mask = tl.maskOf(i)) {
        const bool active = !m_opts.masksNeedLock || mask->locked;
        if (active && mask->visible) return Unit::None;
    }
    if (l.type == LayerType::Mask && (!m_opts.masksNeedLock || l.locked)) return Unit::Mask;
    return Unit::Layer;
}

std::vector<int> Renderer::layerUnits(const Timeline& tl) const
{
    std::vector<int> out;
    for (int i = int(tl.layers.size()) - 1; i >= 0; --i)
        if (unitOf(tl, i, 0) != Unit::None) out.push_back(i);
    return out;
}

// --- Layer keys ---------------------------------------------------------------------

struct Renderer::KeyState {
    uint64_t h = 0x9e3779b97f4a7c15ull;
    bool backdrop = false;
    std::vector<ElementPtr>* pins = nullptr;

    void add(uint64_t v)
    {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
    }
    void add(double v)
    {
        uint64_t u;
        std::memcpy(&u, &v, sizeof u);
        add(u);
    }
    void add(int v) { add(uint64_t(int64_t(v))); }
    void add(bool v) { add(uint64_t(v)); }
    void add(const void* p) { add(uint64_t(reinterpret_cast<uintptr_t>(p))); }
    void add(const std::string& s)
    {
        add(uint64_t(s.size()));
        add(uint64_t(std::hash<std::string>{}(s)));
    }
    void add(const Affine& m)
    {
        add(m.a);
        add(m.b);
        add(m.c);
        add(m.d);
        add(m.tx);
        add(m.ty);
    }
    void add(const Color& c) { add(uint64_t(c.r) | uint64_t(c.g) << 8 | uint64_t(c.b) << 16 | uint64_t(c.a) << 24); }
    void add(const ColorTransform& t)
    {
        for (double v : {t.rm, t.gm, t.bm, t.am, t.ro, t.go, t.bo, t.ao}) add(v);
    }
    void add(const Rect& r)
    {
        for (double v : {r.x0, r.y0, r.x1, r.y1}) add(v);
    }
};

void Renderer::hashElement(KeyState& k, const EvalItem& item, int depth) const
{
    const Element& e = *item.element;
    k.add(int(e.type()));
    k.add(e.matrix);
    switch (e.type()) {
    case ElementType::Shape:
        // Shape graphs are immutable: the (pinned) pointer names the content.
        k.add(static_cast<const ShapeElement&>(e).graph.get());
        k.pins->push_back(item.element);
        return;
    case ElementType::Morph:
        k.add(static_cast<const MorphElement&>(e).data.get());
        k.pins->push_back(item.element);
        return;
    case ElementType::Group: {
        const auto& g = static_cast<const GroupElement&>(e);
        k.add(int(g.children.size()));
        for (const ElementPtr& ch : g.children) hashElement(k, {ch, item.localFrame, ch.get()}, depth);
        return;
    }
    case ElementType::Instance: {
        const auto& in = static_cast<const InstanceElement&>(e);
        k.add(in.visible);
        if (!in.visible) return;
        const Symbol* sym = m_doc.symbol(in.symbolId);
        k.add(in.symbolId);
        if (!sym || depth > 32) return;
        const ButtonState state = item.source && item.source == m_opts.hotButton ? m_opts.hotState : ButtonState::Up;
        const int frame = instanceSymbolFrame(m_doc, in, item.localFrame, m_opts.clipFrame, state);
        k.add(frame);
        k.add(int(sym->type));
        k.add(int(in.behavior));
        k.add(sym->scale9.has_value());
        if (sym->scale9) k.add(*sym->scale9);
        k.add(in.color.toTransform());
        k.add(int(in.blend));
        if (in.blend != BlendMode::Normal && in.blend != BlendMode::Layer && in.behavior != SymbolType::Graphic)
            k.backdrop = true;
        k.add(int(in.filters.size()));
        for (const Filter& f : in.filters) {
            k.add(int(f.type));
            k.add(f.enabled);
            for (double v : {f.blurX, f.blurY, f.strength, f.angle, f.distance, f.brightness, f.contrast, f.saturation, f.hue})
                k.add(v);
            k.add(f.quality);
            k.add(f.color);
            k.add(f.highlight);
            k.add(f.inner);
            k.add(f.knockout);
            k.add(f.hideObject);
            k.add(int(f.bevel));
            k.add(int(f.gradient.stops.size()));
            for (const GradientStop& st : f.gradient.stops) {
                k.add(st.pos);
                k.add(st.color);
            }
        }
        hashTimeline(k, sym->timeline, frame, depth + 1);
        return;
    }
    }
}

void Renderer::hashLayer(KeyState& k, const Timeline& tl, int index, int frame, int depth) const
{
    const Layer& l = tl.layers[index];
    k.add(uint64_t(l.id));
    k.add(int(l.type));
    k.add(l.visible);
    k.add(l.locked);
    k.add(l.outline);
    k.add(l.color);
    k.add(uint64_t(l.parentId));
    k.add(int(l.blend));
    k.add(l.opacity);
    if (l.blend != BlendMode::Normal) k.backdrop = true;
    if (l.type == LayerType::Folder) return;
    const std::vector<EvalItem> items = evaluateLayer(m_doc, tl, index, frame);
    k.add(int(items.size()));
    for (const EvalItem& it : items) hashElement(k, it, depth);
}

void Renderer::hashTimeline(KeyState& k, const Timeline& tl, int frame, int depth) const
{
    k.add(int(tl.layers.size()));
    for (int i = 0; i < int(tl.layers.size()); ++i) hashLayer(k, tl, i, frame, depth);
}

LayerKey Renderer::layerKey(const Timeline& tl, int layerIndex, int frame) const
{
    LayerKey out;
    KeyState k;
    k.pins = &out.pins;
    hashLayer(k, tl, layerIndex, frame, 0);
    if (unitOf(tl, layerIndex, 0) == Unit::Mask) {
        const uint32_t id = tl.layers[layerIndex].id;
        for (int j = int(tl.layers.size()) - 1; j >= 0; --j)
            if (tl.layers[j].parentId == id && !layerHidden(tl, j)) hashLayer(k, tl, j, frame, 0);
    }
    out.key = k.h;
    out.backdrop = k.backdrop;
    return out;
}

// --- Rendering ----------------------------------------------------------------------

void Renderer::renderTimeline(Surface& target, const Timeline& tl, int frame, const Ctx& c)
{
    if (c.depth > 32) return;
    const int n = int(tl.layers.size());
    const bool filtered = c.depth == 0 && !m_opts.onlyLayers.empty();
    for (int i = n - 1; i >= 0; --i) {
        const Unit unit = unitOf(tl, i, c.depth);
        if (unit == Unit::None) continue;
        if (filtered && (i >= int(m_opts.onlyLayers.size()) || !m_opts.onlyLayers[i])) continue;
        const Layer& l = tl.layers[i];
        if (unit == Unit::Mask) {
            std::unique_ptr<Surface> content = target.makeLayer(target.size());
            for (int j = n - 1; j >= 0; --j) {
                if (tl.layers[j].parentId != l.id || layerHidden(tl, j)) continue;
                renderLayerItems(*content, tl, j, frame, c);
            }
            std::unique_ptr<Surface> mask = target.makeLayer(target.size());
            Ctx mc = c;
            mc.ct = ColorTransform{}; // mask shape: only coverage matters
            renderLayerItems(*mask, tl, i, frame, mc);
            content->applyMask(*mask);
            target.composite(*content, QPoint(0, 0), BlendMode::Normal, 1.0);
            continue;
        }
        renderLayerItems(target, tl, i, frame, c);
    }
}

void Renderer::renderLayerItems(Surface& target, const Timeline& tl, int layerIndex, int frame, const Ctx& c)
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
    auto draw = [&](Surface& dst, const Ctx& ctx) {
        for (int i = 0; i < int(items.size()); ++i) renderElement(dst, items[i], ctx, i == focusItem);
    };
    if ((l.blend != BlendMode::Normal || l.opacity < 1.0) && !lc.outline) {
        std::unique_ptr<Surface> buf = target.makeLayer(target.size());
        lc.isolated = true;
        draw(*buf, lc);
        target.composite(*buf, QPoint(0, 0), l.blend, l.opacity);
        return;
    }
    draw(target, lc);
}

void Renderer::renderList(Surface& target, const std::vector<EvalItem>& items, const Ctx& c)
{
    for (const EvalItem& it : items) renderElement(target, it, c, false);
}

void Renderer::renderElement(Surface& target, const EvalItem& item, const Ctx& c, bool onPath)
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
        if (c.slice) {
            const auto rd = std::make_shared<const ShapeRenderData>(c.slice->apply(*s.graph, c.toSymbol * e.matrix).renderData());
            if (c.outline) target.drawOutline(*rd, c.sliceBase, c.outlineColor, c.clip);
            else target.drawShape(rd, c.sliceBase, c.ct, c.clip);
            return;
        }
        if (c.outline) target.drawOutline(s.graph->renderData(), m, c.outlineColor, c.clip);
        else target.drawShape(s.graph->renderDataPtr(), m, c.ct, c.clip);
        return;
    }
    case ElementType::Morph: {
        const auto& mo = static_cast<const MorphElement&>(e);
        if (!mo.data) return;
        if (c.slice) {
            const auto rd = std::make_shared<const ShapeRenderData>(
                c.slice->apply(graphFromRenderData(*mo.data), c.toSymbol * e.matrix).renderData());
            if (c.outline) target.drawOutline(*rd, c.sliceBase, c.outlineColor, c.clip);
            else target.drawShape(rd, c.sliceBase, c.ct, c.clip);
            return;
        }
        if (c.outline) target.drawOutline(*mo.data, m, c.outlineColor, c.clip);
        else target.drawShape(mo.data, m, c.ct, c.clip);
        return;
    }
    case ElementType::Group: {
        const auto& g = static_cast<const GroupElement&>(e);
        Ctx gc = ec;
        gc.m = m;
        gc.toSymbol = c.toSymbol * e.matrix;
        for (const ElementPtr& ch : g.children) renderElement(target, {ch, item.localFrame, ch.get()}, gc, false);
        return;
    }
    case ElementType::Instance: {
        const auto& in = static_cast<const InstanceElement&>(e);
        if (!in.visible) return;
        const Symbol* sym = m_doc.symbol(in.symbolId);
        if (!sym || c.depth > 32) return;
        const ButtonState state = item.source && item.source == m_opts.hotButton ? m_opts.hotState : ButtonState::Up;
        const int frame = instanceSymbolFrame(m_doc, in, item.localFrame, m_opts.clipFrame, state);
        Ctx ic = ec;
        ic.m = m;
        ic.ct = c.ct * in.color.toTransform();
        ic.depth = c.depth + 1;
        // 9-slice applies to this symbol's own shapes; nested symbols scale normally.
        const std::optional<Slice9> slice = instanceSlice9(m_doc, in, frame);
        ic.slice = slice ? &*slice : nullptr;
        ic.sliceBase = m;
        ic.toSymbol = {};
        const bool blends = in.blend != BlendMode::Normal && in.behavior != SymbolType::Graphic && !c.outline;
        const bool filtered = in.behavior != SymbolType::Graphic && !c.outline && hasActiveFilters(in.filters);
        if (!blends && !filtered) {
            renderTimeline(target, sym->timeline, frame, ic);
            return;
        }
        if ((in.blend == BlendMode::Alpha || in.blend == BlendMode::Erase) && !c.isolated) return;
        if (filtered) {
            // Filters work in device pixels on the instance's own buffer; like
            // Animate, they scale with the view and enclosing symbols.
            const double scale = std::sqrt(std::abs(c.m.det()));
            const Rect local = timelineBounds(m_doc, sym->timeline, frame, c.depth + 1);
            if (local.isEmpty()) return;
            const int margin = int(std::ceil(filterMargin(in.filters) * scale));
            const QRect clip = c.clip.isNull() ? target.rect() : c.clip;
            const QRect area = deviceRect(m.mapRect(local)).adjusted(-margin, -margin, margin, margin) &
                               clip.adjusted(-margin, -margin, margin, margin);
            if (area.isEmpty() || double(area.width()) * area.height() > 64e6) return;
            std::unique_ptr<Surface> buf = target.makeLayer(area.size());
            Ctx fc = ic;
            fc.m = Affine::translate(-area.x(), -area.y()) * m;
            fc.sliceBase = fc.m;
            fc.ct = ColorTransform{};
            fc.isolated = true;
            fc.clip = buf->rect();
            renderTimeline(*buf, sym->timeline, frame, fc);
            buf->applyFilters(in.filters, scale);
            buf->applyColorTransform(ic.ct);
            target.composite(*buf, area.topLeft(), blends ? in.blend : BlendMode::Normal, 1.0);
            return;
        }
        std::unique_ptr<Surface> buf = target.makeLayer(target.size());
        ic.isolated = true;
        renderTimeline(*buf, sym->timeline, frame, ic);
        target.composite(*buf, QPoint(0, 0), in.blend, 1.0);
        return;
    }
    }
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
