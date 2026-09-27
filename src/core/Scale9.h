// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — 9-slice scaling (Animate's "Enable guides for 9-slice scaling").
//
// A symbol with a scale grid keeps its corners at their size when an instance
// is scaled: only the middle row and column stretch. Shapes are split
// exactly where their edges cross the grid lines, so every piece is mapped by
// one affine map and curves stay exact. Nested symbols scale normally.
#pragma once

#include "Document.h"

#include <optional>

namespace vx {

class Slice9 {
public:
    /// Mapping in symbol space for an instance whose matrix scales the symbol
    /// by (sx, sy). Nothing when the grid is degenerate or there is no scale.
    static std::optional<Slice9> make(const Rect& grid, const Rect& bounds, double sx, double sy);

    Vec2 map(Vec2 p) const { return {m_x.map(p.x), m_y.map(p.y)}; }
    /// `g` moved to symbol space by `toSymbol`, then sliced.
    ShapeGraph apply(const ShapeGraph& g, const Affine& toSymbol = {}) const;

private:
    struct Axis {
        double b0 = 0, g0 = 0, g1 = 0, b1 = 0;
        double corner = 1, middle = 1; ///< local scale of the corner and middle parts
        double map(double v) const;
        double scaleAt(double v) const { return v < g0 || v > g1 ? corner : middle; }
    };
    Axis m_x, m_y;
};

/// The 9-slice mapping of an instance at a frame of its symbol, if any.
std::optional<Slice9> instanceSlice9(const Document& doc, const InstanceElement& in, int symbolFrame);

} // namespace vx
