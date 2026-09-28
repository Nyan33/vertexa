// SPDX-License-Identifier: GPL-3.0-or-later
#include "LayerCache.h"
#include "Blend.h"
#include "GlRenderer.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace vx {

namespace {

/// Renders a layer that changed this many renders ago from the cache again:
/// layers that change every few frames are simply drawn every time.
constexpr int kSettle = 4;
/// Beyond this many pixels per segment the cache is not worth its memory.
constexpr qint64 kMaxPixels = qint64(8192) * 8192;

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    return h ^ (h >> 33);
}

uint64_t mixDouble(uint64_t h, double d)
{
    uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return mix(h, u);
}

/// Bounding box of the non-transparent pixels.
QRect drawnBounds(const QImage& img)
{
    const int w = img.width(), h = img.height();
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const quint32*>(img.constScanLine(y));
        int first = 0;
        while (first < w && !row[first]) ++first;
        if (first == w) continue;
        int last = w - 1;
        while (!row[last]) --last;
        x0 = std::min(x0, first);
        x1 = std::max(x1, last);
        y0 = std::min(y0, y);
        y1 = y;
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

} // namespace

void LayerCache::clear()
{
    m_global = 0;
    m_units.clear();
    m_segments.clear();
}

void LayerCache::render(QImage& target, const Document& doc, const Timeline& tl, int frame, const Affine& view,
                        const RenderOptions& opts, const QString& context, bool targetEmpty)
{
    // Everything that applies to all layers alike.
    uint64_t g = 1;
    for (double v : {view.a, view.b, view.c, view.d, view.tx, view.ty}) g = mixDouble(g, v);
    g = mix(g, uint64_t(target.width()) << 32 | uint64_t(target.height()));
    g = mix(g, uint64_t(opts.clipFrame));
    g = mix(g, uint64_t(opts.showGuides) | uint64_t(opts.skipHidden) << 1 | uint64_t(opts.masksNeedLock) << 2 |
                   uint64_t(opts.outlineLayers) << 3 | uint64_t(opts.forceOutline) << 4 |
                   uint64_t(GlRenderer::instance() != nullptr) << 5);
    g = mix(g, opts.outlineColor.rgba());
    for (const auto& [id, index] : opts.focusPath) g = mix(mix(g, id), uint64_t(index));
    g = mix(g, std::hash<QString>{}(context));
    g |= 1; // never 0, the value of an empty cache

    RenderOptions o = opts;
    o.onlyLayers.clear();
    const Renderer probe(doc, o);
    const std::vector<int> units = probe.layerUnits(tl);
    m_stats = {int(units.size()), 0, 0};

    struct Unit {
        int index;
        uint32_t id;
        LayerKey key;
        bool cache;
    };
    std::vector<Unit> us;
    us.reserve(units.size());
    std::unordered_map<uint32_t, UnitState> states;
    const bool sameView = g == m_global;
    for (int i : units) {
        Unit u{i, tl.layers[i].id, probe.layerKey(tl, i, frame), false};
        UnitState st{u.key.key, kSettle};
        if (auto it = m_units.find(u.id); it != m_units.end() && sameView)
            st.unchanged = it->second.key == u.key.key ? it->second.unchanged + 1 : 0;
        states[u.id] = st;
        u.cache = sameView && !u.key.backdrop && st.unchanged >= kSettle;
        us.push_back(std::move(u));
    }
    m_units = std::move(states);
    if (!sameView) {
        // Zooming or panning: draw everything; cache once the view rests.
        m_segments.clear();
        m_global = g;
    }
    if (qint64(target.width()) * target.height() > kMaxPixels)
        for (Unit& u : us) u.cache = false;

    auto draw = [&](QImage& dst, const std::vector<int>& layers, bool empty) {
        if (layers.empty()) return;
        o.onlyLayers.assign(tl.layers.size(), 0);
        for (int i : layers) o.onlyLayers[i] = 1;
        renderAccelerated(dst, doc, tl, frame, view, {}, o, empty);
        m_stats.drawn += int(layers.size());
    };
    // An old segment holding exactly the units [from, from + n) for some n.
    auto pieceAt = [&](size_t from, size_t to) -> const Segment* {
        for (const Segment& s : m_segments) {
            if (s.units.empty() || s.units.size() > to - from) continue;
            bool same = true;
            for (size_t k = 0; k < s.units.size() && same; ++k)
                same = s.units[k].first == us[from + k].id && s.units[k].second == us[from + k].key.key;
            if (same) return &s;
        }
        return nullptr;
    };

    std::vector<Segment> segments;
    bool empty = targetEmpty;
    size_t p = 0;
    while (p < us.size()) {
        if (!us[p].cache) {
            std::vector<int> batch;
            while (p < us.size() && !us[p].cache) batch.push_back(us[p++].index);
            draw(target, batch, empty);
            empty = false;
            continue;
        }
        size_t q = p;
        while (q < us.size() && us[q].cache) ++q;
        Segment seg;
        if (const Segment* old = pieceAt(p, q); old && old->units.size() == q - p) {
            seg = *old;
        } else {
            // Build from older segments where possible, drawing the rest.
            QImage img(target.size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(0);
            bool imgEmpty = true;
            for (size_t r = p; r < q;) {
                if (const Segment* piece = pieceAt(r, q)) {
                    if (!piece->image.isNull()) {
                        compositeImage(img, piece->image, piece->offset, BlendMode::Normal, 1.0);
                        imgEmpty = false;
                    }
                    r += piece->units.size();
                    continue;
                }
                std::vector<int> batch;
                while (r < q && (batch.empty() || !pieceAt(r, q))) batch.push_back(us[r++].index);
                draw(img, batch, imgEmpty);
                imgEmpty = false;
            }
            const QRect box = drawnBounds(img);
            if (!box.isEmpty()) {
                seg.image = box == img.rect() ? img : img.copy(box);
                seg.offset = box.topLeft();
            }
            for (size_t k = p; k < q; ++k) {
                seg.units.emplace_back(us[k].id, us[k].key.key);
                seg.pins.insert(seg.pins.end(), us[k].key.pins.begin(), us[k].key.pins.end());
            }
        }
        if (!seg.image.isNull()) {
            compositeImage(target, seg.image, seg.offset, BlendMode::Normal, 1.0);
            empty = false;
        }
        ++m_stats.segments;
        segments.push_back(std::move(seg));
        p = q;
    }
    m_segments = std::move(segments);
}

} // namespace vx
