// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — geometry and paint preparation shared by the GPU renderers
// (OpenGL and RHI): shapes become triangle fans whose winding numbers the
// stencil buffer counts, and fills become a colour or a gradient ramp.
#pragma once

#include "core/ShapeGraph.h"

#include <QList>
#include <QPolygonF>
#include <QRect>
#include <QString>

#include <array>
#include <vector>

class QPainterPathStroker;

namespace vx::gpu {

/// Device pixels touched by a device-space rectangle (with a pixel margin).
QRect deviceRect(const Rect& r);

/// Largest scale factor of a transform (flattening must be fine enough for it).
double maxScale(const Affine& m);

/// Triangles whose winding numbers add up to the polygon's. Long contours are
/// split into chunks fanned from their first point, plus one fan over the
/// chunk starts: the chords cancel, and triangles stay local instead of
/// sweeping across the shape from a single point.
void appendFan(const std::vector<Vec2>& poly, std::vector<float>& tri, Rect& bounds);
void appendQPolygons(const QList<QPolygonF>& polys, std::vector<float>& tri, Rect& bounds);

/// Stroker settings of a stroke style; `cosmetic` strokes keep their width
/// on screen and are stroked in device space.
void setupStroker(QPainterPathStroker& st, const StrokeStyle& s, double& width, bool& cosmetic);

/// Software implementations (llvmpipe, lavapipe, SwiftShader, WARP …) run on
/// the CPU and are slower than the CPU renderer.
bool isSoftwareDevice(const QString& name);

/// Paint of a fill or stroke: a premultiplied colour, or a gradient through
/// a 256-entry premultiplied ramp.
struct Paint {
    int kind = 0; ///< 0 solid, 1 linear, 2 radial
    float color[4] = {0, 0, 0, 0};
    Affine toGradient; ///< device -> gradient space
    float focal = 0;
    int spread = 0;
    std::array<uint8_t, 256 * 4> lut{};

    Paint(const FillStyle& f, const Affine& toDevice);
};

/// Shape geometry flattened in shape space for a scale bucket (half-octave
/// steps, so zooming within a step reuses it).
struct FlatShape {
    struct Part {
        std::vector<float> tri;
        Rect bounds;          ///< shape space
        bool cosmetic = false; ///< strokes drawn in device space instead
    };
    std::vector<Part> fills, strokes;
};
int scaleBucket(double scale);
/// Largest scale of a bucket (flattening for it is fine enough for the whole bucket).
double bucketScale(int bucket);
FlatShape flattenShape(const ShapeRenderData& rd, int bucket);

/// Hairlines and non-scaling strokes keep their width on screen.
bool isCosmetic(const StrokeStyle& s);
/// Outline of a (non-cosmetic) stroke in shape space, flat enough at
/// `scale`; filled with the non-zero rule it covers what the pen paints.
QList<QPolygonF> strokeOutline(const ShapeRenderData::StrokePath& sp, double scale);

/// Triangles of a cosmetic stroke or an outline, in device space.
void strokeInDeviceSpace(const ShapeRenderData::StrokePath& sp, const Affine& m, std::vector<float>& tri, Rect& bounds);
void outlineInDeviceSpace(const ShapeRenderData& rd, const Affine& m, std::vector<float>& tri, Rect& bounds);

} // namespace vx::gpu
