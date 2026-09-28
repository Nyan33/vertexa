// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — planar arrangement of cubic Bezier curves.
//
// This is the heart of Vertexa's "100% vector" editing model. Every operation
// that Flash/Animate performs on merged shapes (painting over, erasing, paint
// bucket, cutting with a marquee, booleans for drawing objects) is expressed
// as: put all curves into an arrangement, split them at every intersection
// (exactly, no flattening), build the half-edge structure (DCEL), enumerate the
// faces and then decide per face what it becomes.
//
// Face attributes are *propagated topologically* instead of being sampled:
//  * Winding layers — an input curve contributes +1 to the winding number on
//    its left side. Crossing an edge changes the winding by the signed sum of
//    its sources, so the winding of every face is known exactly.
//  * Label layers — an input curve carries a label on its left and right side
//    (e.g. the fill style indices of a Flash edge). Faces inherit labels from
//    the curves that bound them.
//
// The half-edge convention: half-edge 2e runs along edge e (v0 -> v1) and
// 2e+1 runs backwards. face(h) is the face on the left of h; faces bounded by
// a cycle with positive signed area are bounded faces, face 0 is unbounded.
#pragma once

#include "Bezier.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace vx {

enum class LayerKind : uint8_t { Winding, Label };

struct ArrInput {
    Cubic curve;
    int layer = 0;
    int labelLeft = 0;   ///< label layers: value on the left of the curve
    int labelRight = 0;  ///< label layers: value on the right of the curve
    int tag = -1;        ///< free user data, e.g. index of the originating edge
    /// Inputs of one group (>= 0) are known not to cross each other (the
    /// edges of a planar graph meet at their end points only), so they are
    /// not intersected pairwise.
    int group = -1;
};

struct ArrEdgeSource {
    int input = -1;
    bool reversed = false; ///< input curve runs v1 -> v0 relative to the edge
};

struct ArrEdge {
    Cubic curve;
    int v0 = -1, v1 = -1;
    std::vector<ArrEdgeSource> sources;
};

class Arrangement {
public:
    explicit Arrangement(double eps = 1e-7);

    /// Declare the attribute layers. Defaults to a single winding layer.
    void setLayers(std::vector<LayerKind> kinds) { m_kinds = std::move(kinds); }
    int layerCount() const { return int(m_kinds.size()); }

    int add(const ArrInput& in);
    void addCurve(const Cubic& c, int layer, int tag = -1);
    /// Adds a closed contour (winding layer).
    void addContour(const std::vector<Cubic>& contour, int layer, int tag = -1);

    void build();

    // --- topology -------------------------------------------------------
    const std::vector<ArrInput>& inputs() const { return m_inputs; }
    const std::vector<Vec2>& vertices() const { return m_vertices; }
    const std::vector<ArrEdge>& edges() const { return m_edges; }
    int halfEdgeCount() const { return int(m_edges.size() * 2); }
    static int twin(int h) { return h ^ 1; }
    static int edgeOf(int h) { return h >> 1; }
    static bool isForward(int h) { return (h & 1) == 0; }
    int origin(int h) const { return isForward(h) ? m_edges[h >> 1].v0 : m_edges[h >> 1].v1; }
    int dest(int h) const { return isForward(h) ? m_edges[h >> 1].v1 : m_edges[h >> 1].v0; }
    Cubic curve(int h) const { return isForward(h) ? m_edges[h >> 1].curve : m_edges[h >> 1].curve.reversed(); }
    int next(int h) const { return m_next[h]; }
    /// Face on the left of half-edge h.
    int face(int h) const { return m_cycles[m_cycleOf[h]].face; }
    /// True if the source runs in the same direction as half-edge h.
    static bool sourceAlong(int h, const ArrEdgeSource& s) { return isForward(h) != s.reversed; }

    struct Cycle {
        int first = -1;
        int length = 0;
        double area = 0.0;
        Rect bounds;
        int face = 0;
        int component = -1;
    };
    struct Face {
        int outer = -1;          ///< cycle index, -1 for the unbounded face
        std::vector<int> holes;  ///< cycles of components nested in the face
    };
    const std::vector<Cycle>& cycles() const { return m_cycles; }
    int cycleOf(int h) const { return m_cycleOf[h]; }
    const std::vector<Face>& faces() const { return m_faces; }
    int faceCount() const { return int(m_faces.size()); }
    /// Outgoing half-edges of a vertex sorted counter-clockwise.
    const std::vector<int>& fan(int v) const { return m_fans[v]; }

    /// Face containing p (p should not lie on an edge). 0 = unbounded.
    int locate(Vec2 p) const;
    /// All half-edges that have face f on their left.
    std::vector<int> faceHalfEdges(int f) const;
    /// Signed area of a face (outer boundary minus holes).
    double faceArea(int f) const;
    /// Closed contours (outer first) bounding face f.
    std::vector<std::vector<Cubic>> faceContours(int f) const;

    // --- attributes -----------------------------------------------------
    int value(int face, int layer) const { return m_values[size_t(face) * m_kinds.size() + layer]; }

    /// Loops of half-edges bounding the region made of the faces for which
    /// inside(face) is true. The region lies on the left of every half-edge.
    std::vector<std::vector<int>> traceBoundary(const std::function<bool(int)>& inside) const;
    /// Convert a half-edge loop into curves, merging pieces that belong to a
    /// single original cubic.
    std::vector<Cubic> loopCurves(const std::vector<int>& loop, bool join = true) const;

    /// Winding number of p with respect to the curves of the given cycles.
    int cycleWinding(Vec2 p, int cycle) const;

    double eps() const { return m_eps; }

private:
    void computeFans();
    void computeCycles();
    void computeFaces();
    void propagate();

    double m_eps;
    std::vector<LayerKind> m_kinds{LayerKind::Winding};
    std::vector<ArrInput> m_inputs;
    std::vector<Vec2> m_vertices;
    std::vector<ArrEdge> m_edges;
    std::vector<std::vector<int>> m_fans;
    std::vector<int> m_fanIndex;
    std::vector<int> m_next;
    std::vector<int> m_cycleOf;
    std::vector<Cycle> m_cycles;
    std::vector<Face> m_faces;
    std::vector<int> m_values;
};

/// Winding number of p w.r.t. a set of (not necessarily monotone) curves that
/// form closed loops. Positive-area (math CCW) loops give +1 inside.
int windingNumber(Vec2 p, const std::vector<Cubic>& curves);

} // namespace vx
