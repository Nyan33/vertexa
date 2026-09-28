// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — stage rendering that keeps the pixels of layers that do not change.
//
// Every render computes a key of what each top-level layer draws (its
// elements, their matrices and colour effects, nested symbol frames …).
// Consecutive layers whose keys have settled are drawn once into a cached
// image (a "segment") and composited afterwards; layers that change (being
// animated or edited) are drawn every time. Moving the playhead, dragging an
// object or painting on one layer then redraws only the layers that change.
// Segments are rebuilt from older segments where possible, so a layer that
// stops changing joins its neighbours without redrawing them.
#pragma once

#include "Renderer.h"

#include <QImage>
#include <QString>

#include <unordered_map>
#include <vector>

namespace vx {

class LayerCache {
public:
    struct Stats {
        int layers = 0;   ///< top-level layers (a mask counts with its masked layers)
        int drawn = 0;    ///< layers drawn by the last render
        int segments = 0; ///< cached segments composited by the last render
    };

    /// Renders like renderAccelerated() onto `target` (premultiplied ARGB32,
    /// transparent when `targetEmpty`). `context` names the timeline shown
    /// (the scene, or the symbol edited in place): layer ids are per timeline.
    void render(QImage& target, const Document& doc, const Timeline& tl, int frame, const Affine& view,
                const RenderOptions& opts, const QString& context, bool targetEmpty);
    void clear();
    const Stats& stats() const { return m_stats; }

private:
    struct UnitState {
        uint64_t key = 0;
        int unchanged = 0; ///< renders since the key last changed
    };
    struct Segment {
        std::vector<std::pair<uint32_t, uint64_t>> units; ///< layer id, key; bottom to top
        QImage image;                                      ///< cropped to the drawn pixels
        QPoint offset;
        std::vector<ElementPtr> pins;
    };

    uint64_t m_global = 0;
    std::unordered_map<uint32_t, UnitState> m_units;
    std::vector<Segment> m_segments;
    Stats m_stats;
};

} // namespace vx
