// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — the Flash/Animate shape model.
//
// A shape is a planar graph of cubic edges. Each edge carries the fill style
// on its left side, the fill style on its right side and an optional stroke
// style — exactly like the edge records of an XFL <DOMShape> (fillStyle0,
// fillStyle1, strokeStyle). Fills are implicit: a filled area is the set of
// faces whose bounding edges carry that fill on the side facing them. This is
// what makes merge drawing work: shapes of the same colour fuse, lines split
// fills, and painting over something replaces what is underneath.
//
// Invariant maintained by all operations in ShapeOps: edges only meet at end
// points (no crossings), and fills are consistent across every face.
#pragma once

#include "Style.h"
#include "geom/Arrangement.h"
#include "geom/Region.h"

#include <memory>
#include <mutex>
#include <vector>

namespace vx {

struct GEdge {
    Cubic c;
    int fillL = 0;  ///< 1-based index into ShapeGraph::fills, 0 = no fill
    int fillR = 0;
    int stroke = 0; ///< 1-based index into ShapeGraph::strokes, 0 = no stroke
    bool operator==(const GEdge&) const = default;
};

/// Render-ready geometry derived from a shape (also produced by shape tweens).
struct ShapeRenderData {
    struct FillPath {
        FillStyle style;
        std::vector<Contour> contours; ///< closed loops, non-zero rule
    };
    struct StrokePath {
        StrokeStyle style;
        std::vector<std::vector<Cubic>> chains;
        std::vector<char> closed;
    };
    std::vector<FillPath> fills;
    std::vector<StrokePath> strokes;
    Rect bounds;
    bool isEmpty() const { return fills.empty() && strokes.empty(); }
};

class ShapeGraph {
public:
    std::vector<FillStyle> fills;
    std::vector<StrokeStyle> strokes;
    std::vector<GEdge> edges;

    int addFill(const FillStyle& f);
    int addStroke(const StrokeStyle& s);
    const FillStyle& fill(int id) const { return fills[id - 1]; }
    const StrokeStyle& stroke(int id) const { return strokes[id - 1]; }

    bool isEmpty() const { return edges.empty(); }
    /// Bounds of the geometry (optionally inflated by half the stroke widths).
    Rect bounds(bool includeStrokeWidth = true) const;
    ShapeGraph transformed(const Affine& m) const;
    ShapeGraph withColorTransform(const ColorTransform& ct) const;

    /// Drops unused styles and merges edges that continue each other.
    void compact();

    /// Render data (chained loops per fill, chains per stroke). Cached.
    const ShapeRenderData& renderData() const;
    /// The same, shared: renderers may cache work per render data (GPU
    /// buffers) and tell from a weak reference when it is gone.
    std::shared_ptr<const ShapeRenderData> renderDataPtr() const;
    /// Planar topology (faces) of this shape. Layer 0 holds the fill labels.
    /// Cached; used for hit testing and selection.
    const Arrangement& topology() const;
    /// Stroke style index of an arrangement edge of topology() (0 = none).
    int topologyStroke(int arrEdge) const;
    /// Original edge index of an arrangement edge of topology().
    int topologySource(int arrEdge) const;

    /// Fill style id at a point (0 = empty).
    int fillAt(Vec2 p) const;

    /// Region covered by a given fill id (or all fills when id == 0).
    Region fillRegion(int fillId = 0) const;

    /// Marks the edges as a planar graph (they meet at their end points only),
    /// as shape operations emit them: merging into the graph then does not
    /// intersect them with each other. Any later change to the edges drops
    /// the mark.
    void markPlanar() { m_planar = edgesHash(); }
    bool isPlanar() const { return m_planar != 0 && m_planar == edgesHash(); }

    /// Must be called after editing edges/styles of a graph whose caches may
    /// already have been built. Copies never inherit caches.
    void invalidate() const
    {
        std::lock_guard lock(m_cache.mutex);
        m_cache.render.reset();
        m_cache.topology.reset();
    }

private:
    /// Built on first use; shared graphs are read (rendered, merged) from
    /// several threads at once.
    struct Cache {
        std::mutex mutex;
        std::shared_ptr<ShapeRenderData> render;
        std::shared_ptr<Arrangement> topology;
        Cache() = default;
        Cache(const Cache&) {}
        Cache& operator=(const Cache&)
        {
            std::lock_guard lock(mutex);
            render.reset();
            topology.reset();
            return *this;
        }
    };
    mutable Cache m_cache;
    uint64_t edgesHash() const;
    uint64_t m_planar = 0; ///< edgesHash() when marked planar
};

using ShapeGraphPtr = std::shared_ptr<const ShapeGraph>;

/// Build render data for a list of edges (shared by shapes and morphs).
ShapeRenderData buildRenderData(const ShapeGraph& g);

/// Stroke outline chains of a graph (only edges with a stroke).
std::vector<std::vector<Cubic>> strokeChains(const ShapeGraph& g, std::vector<int>* styles = nullptr,
                                             std::vector<char>* closed = nullptr);

} // namespace vx
