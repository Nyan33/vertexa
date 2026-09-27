// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — GPU rendering through OpenGL.
//
// The same Renderer walks the document; a GlSurface draws on the GPU:
//  * fills and strokes: stencil-then-cover into a multisampled framebuffer
//    (non-zero winding counted in the stencil buffer, then one quad in the
//    fill's colour or gradient). Neighbouring fills share exactly the same
//    flattened edges, so every sample belongs to exactly one of them and no
//    seam shows between them;
//  * isolated layers, masks, colour transforms and all blend modes: extra
//    framebuffers composited by shaders that port the CPU formulas;
//  * filters: on the CPU for now, on the filtered instance's own pixels.
// Needs OpenGL 3.3 core (or OpenGL ES 3.0) on a hardware driver. Without one
// (or with a software OpenGL, unless VERTEXA_GPU=force) the renderer is
// unavailable and callers draw on the CPU.
#pragma once

#include "Renderer.h"

#include <QImage>
#include <QString>

#include <memory>

namespace vx {

class GlRenderer {
public:
    ~GlRenderer();
    /// The GPU renderer of the GUI thread, or null when there is no usable
    /// OpenGL context or GPU rendering is turned off.
    static GlRenderer* instance();
    /// Turns GPU rendering on or off for the whole application (the
    /// VERTEXA_GPU=0 environment variable turns it off at start).
    static void setEnabled(bool on);
    static bool enabled();

    /// "OpenGL 4.6 — NVIDIA GeForce …".
    QString deviceName() const;
    int samples() const;
    /// CPU time of the last frame before the GPU finished it (walking the
    /// document, preparing geometry, issuing draws), in milliseconds.
    double lastSubmitMs() const;

    /// Renders like Renderer::render into a transparent GPU framebuffer and
    /// composites the result onto `target` (premultiplied ARGB32), or
    /// replaces it when `targetEmpty` says it holds nothing yet.
    bool render(QImage& target, const Document& doc, const Timeline& tl, int frame, const Affine& view,
                const ColorTransform& ct = {}, const RenderOptions& opts = {}, bool targetEmpty = false);

    struct Impl;

private:
    GlRenderer();
    std::unique_ptr<Impl> d;
};

/// Renders on the GPU when available, otherwise on the CPU. True when the
/// GPU drew the frame.
bool renderAccelerated(QImage& target, const Document& doc, const Timeline& tl, int frame, const Affine& view,
                       const ColorTransform& ct = {}, const RenderOptions& opts = {}, bool targetEmpty = false);

} // namespace vx
