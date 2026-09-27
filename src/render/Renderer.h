// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — document renderer: timelines, layers (masks, guides, outline
// mode, layer blending), symbol instances (colour effects, blend modes,
// graphic symbol sync), shapes and morphs.
#pragma once

#include "core/Evaluate.h"

#include <QColor>
#include <QImage>
#include <QRect>

#include <map>
#include <memory>
#include <vector>

namespace vx {

struct RenderOptions {
    int clipFrame = 0;          ///< frame for movie clips (0 while authoring)
    bool showGuides = true;     ///< guide layers are visible while authoring
    bool skipHidden = true;     ///< hidden layers are not drawn
    bool masksNeedLock = true;  ///< Animate shows masking only on locked mask layers
    bool outlineLayers = true;  ///< honour the per-layer outline toggle
    bool forceOutline = false;  ///< draw everything as outlines (onion skin)
    QColor outlineColor;
    /// Edit-in-place: (layer id, element index) pairs locating the edited
    /// instance through nested timelines. The instance itself is not drawn
    /// (the editor draws its timeline at full opacity on top).
    std::vector<std::pair<uint32_t, int>> focusPath;
};

class Renderer {
public:
    Renderer(const Document& doc, RenderOptions opts = {});

    /// Render `frame` of `tl` into `target` (premultiplied ARGB32) through `view`.
    void render(QImage& target, const Timeline& tl, int frame, const Affine& view, const ColorTransform& ct = {});
    /// Render loose elements (e.g. a floating selection) into `target`.
    void renderItems(QImage& target, const std::vector<EvalItem>& items, const Affine& view, const ColorTransform& ct = {});

    /// Seam-free shape rendering (fills through the scanline rasteriser,
    /// strokes through QPainter).
    static void renderShape(QImage& target, const ShapeRenderData& rd, const Affine& m, const ColorTransform& ct,
                            const QRect& clip);
    static void renderOutline(QImage& target, const ShapeRenderData& rd, const Affine& m, const QColor& color,
                              const QRect& clip);

    /// Convenience: render a scene frame at a scale with the stage background.
    static QImage renderFrame(const Document& doc, const Timeline& tl, int frame, double scale, bool transparent,
                              RenderOptions opts = {});


private:
    struct Ctx {
        Affine m;
        ColorTransform ct;
        int depth = 0;
        bool isolated = false;
        size_t focus = 0;
        bool outline = false;
        QColor outlineColor;
        QRect clip;
    };
    void renderTimeline(QImage& target, const Timeline& tl, int frame, const Ctx& c);
    void renderLayerItems(QImage& target, const Timeline& tl, int layerIndex, int frame, const Ctx& c);
    void renderList(QImage& target, const std::vector<EvalItem>& items, const Ctx& c);
    void renderElement(QImage& target, const EvalItem& item, const Ctx& c, bool onPath = false);
    bool layerHidden(const Timeline& tl, int index) const;

    const Document& m_doc;
    RenderOptions m_opts;
};

} // namespace vx
