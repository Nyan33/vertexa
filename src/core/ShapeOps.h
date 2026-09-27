// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — editing operations on Flash-style shapes (merge drawing).
// All operations are exact: curves are split at their true intersections and
// face attributes are propagated through the planar arrangement.
#pragma once

#include "ShapeGraph.h"

#include <optional>
#include <vector>

namespace vx {

/// Brush modes of the Animate Brush tool.
enum class PaintMode { Normal, Fills, Behind, Selection, Inside };
/// Eraser modes of the Animate Eraser tool.
enum class EraseMode { Normal, Fills, Lines, SelectedFills, Inside };

// --- construction --------------------------------------------------------
ShapeGraph graphFromRegion(const Region& r, const FillStyle& fill);
/// Same as graphFromRegion for a clean region: no overlaps or crossings, the
/// fill on the same side of every contour (the output of normalizeRegion or
/// booleanOp, optionally with separate holes added). Much faster: no
/// arrangement is built.
ShapeGraph graphFromCleanRegion(const Region& r, const FillStyle& fill);
/// Open or closed chains of curves painted with a stroke style.
ShapeGraph graphFromPaths(const std::vector<std::vector<Cubic>>& chains, const StrokeStyle& stroke);
/// A closed shape (rectangle, oval, polystar...) with optional fill and stroke.
ShapeGraph graphFromShape(const Region& area, const FillStyle* fill, const StrokeStyle* stroke);

// --- merging ---------------------------------------------------------------
struct OverlayOptions {
    PaintMode mode = PaintMode::Normal;
    /// Selection mode: the selected area. Inside mode: the fill face where the
    /// stroke started.
    const Region* mask = nullptr;
    /// Inside mode started in an empty area: only paint empty space.
    bool insideEmpty = false;
    /// Only run the parts of `base` that reach `top` through the arrangement
    /// (same result, much faster on detailed layers; off for testing).
    bool localized = true;
};
/// Paint `top` over `base` (merge drawing).
ShapeGraph overlay(const ShapeGraph& base, const ShapeGraph& top, const OverlayOptions& opt = {});
/// Erase the area of `eraser` from `base`. `mask` restricts SelectedFills/Inside.
ShapeGraph erase(const ShapeGraph& base, const Region& eraser, EraseMode mode, const Region* mask = nullptr,
                 bool localized = true);

// --- fills -----------------------------------------------------------------
/// Paint bucket. `gap` closes gaps up to that size (document units).
/// Returns false when the point is not enclosed or nothing changes.
bool paintBucket(const ShapeGraph& base, Vec2 p, const FillStyle& fill, double gap, ShapeGraph& out);
/// Region the paint bucket would fill at p (for previews), empty if none.
Region bucketRegion(const ShapeGraph& base, Vec2 p, double gap);
/// Ink bottle: add/replace the stroke around the fill (or of the line) at p.
bool inkBottle(const ShapeGraph& base, Vec2 p, const StrokeStyle& stroke, double tol, ShapeGraph& out);
/// Replace the fill style of a face (used by the fill colour swatch).
ShapeGraph recolorFaces(const ShapeGraph& g, const std::vector<int>& faces, const FillStyle& fill);
ShapeGraph restrokeEdges(const ShapeGraph& g, const std::vector<int>& arrEdges, const StrokeStyle& stroke);

// --- hit testing & selection ---------------------------------------------
struct ShapeHit {
    enum class Kind { None, Fill, Stroke };
    Kind kind = Kind::None;
    int face = -1;     ///< topology face (Fill)
    int arrEdge = -1;  ///< topology edge (Stroke)
    int edge = -1;     ///< graph edge index
    double t = 0.0;    ///< parameter on the graph edge
    double distance = 0.0;
};
ShapeHit hitTest(const ShapeGraph& g, Vec2 p, double tol);
/// Nearest graph edge (any edge, stroked or not) within tol.
ShapeHit nearestEdge(const ShapeGraph& g, Vec2 p, double tol);

/// A selection inside a merged shape, in topology indices.
struct ShapeSelection {
    std::vector<int> faces;
    std::vector<int> edges;
    bool isEmpty() const { return faces.empty() && edges.empty(); }
    void add(const ShapeSelection& o);
    bool hasFace(int f) const;
    bool hasEdge(int e) const;
};
/// Line segment under a click: runs until corners and intersections.
ShapeSelection selectStrokeRun(const ShapeGraph& g, int arrEdge);
/// Double click: all connected lines, or a fill with its outline.
ShapeSelection selectConnected(const ShapeGraph& g, const ShapeHit& hit);
ShapeSelection selectFace(const ShapeGraph& g, int face);
ShapeSelection selectAll(const ShapeGraph& g);
/// Selection of everything fully inside a region (topology indices).
ShapeSelection selectInside(const ShapeGraph& g, const Region& r);
/// Region covered by the selected faces (for highlighting / masks).
Region selectionRegion(const ShapeGraph& g, const ShapeSelection& sel);
/// Stroke chains of the selected edges (for highlighting).
std::vector<Cubic> selectionCurves(const ShapeGraph& g, const ShapeSelection& sel);

/// Split a shape into the unselected rest and the lifted selection.
void liftSelection(const ShapeGraph& g, const ShapeSelection& sel, ShapeGraph& rest, ShapeGraph& lifted);
/// Split a shape along a region (marquee / lasso selection of merge shapes).
void cutByRegion(const ShapeGraph& g, const Region& r, ShapeGraph& rest, ShapeGraph& lifted);

// --- direct editing -------------------------------------------------------
/// Selection tool: bend an edge so that the point at t follows `target`.
ShapeGraph bendEdge(const ShapeGraph& g, int edge, double t, Vec2 target);
/// Selection/subselection tool: move a vertex (all edges ending there).
ShapeGraph moveVertex(const ShapeGraph& g, Vec2 vertex, Vec2 target, double tol = 1e-6);
/// Subselection tool: move a control handle of an edge (handle 1 or 2).
ShapeGraph moveHandle(const ShapeGraph& g, int edge, int handle, Vec2 target);
/// Re-split crossings after direct edits and recompute fills.
ShapeGraph normalizeGraph(const ShapeGraph& g);
/// Nearest vertex within tol (returns false if none).
bool nearestVertex(const ShapeGraph& g, Vec2 p, double tol, Vec2& vertex);

/// Drawing-object booleans (Modify > Combine Objects).
ShapeGraph combineUnion(const std::vector<ShapeGraph>& graphs);
ShapeGraph combineIntersect(const ShapeGraph& a, const ShapeGraph& b);
ShapeGraph combinePunch(const ShapeGraph& a, const ShapeGraph& top);
ShapeGraph combineCrop(const ShapeGraph& a, const ShapeGraph& top);

} // namespace vx
